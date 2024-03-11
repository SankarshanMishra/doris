// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "spill_sort_sink_operator.h"

#include "pipeline/exec/sort_sink_operator.h"
#include "vec/spill/spill_stream_manager.h"

namespace doris::pipeline {
SpillSortSinkLocalState::SpillSortSinkLocalState(DataSinkOperatorXBase* parent, RuntimeState* state)
        : Base(parent, state) {
    _finish_dependency = std::make_shared<FinishDependency>(
            parent->operator_id(), parent->node_id(), parent->get_name() + "_FINISH_DEPENDENCY",
            state->get_query_ctx());
}
Status SpillSortSinkLocalState::init(doris::RuntimeState* state,
                                     doris::pipeline::LocalSinkStateInfo& info) {
    RETURN_IF_ERROR(Base::init(state, info));
    SCOPED_TIMER(exec_time_counter());
    SCOPED_TIMER(_open_timer);

    _init_counters();

    RETURN_IF_ERROR(setup_in_memory_sort_op(state));

    auto& parent = Base::_parent->template cast<Parent>();
    Base::_shared_state->enable_spill = parent._enable_spill;
    Base::_shared_state->in_mem_shared_state->sorter->set_enable_spill(parent._enable_spill);
    if (parent._enable_spill) {
        _finish_dependency->block();
    }
    return Status::OK();
}
void SpillSortSinkLocalState::_init_counters() {
    _internal_runtime_profile = std::make_unique<RuntimeProfile>("internal_profile");

    _partial_sort_timer = ADD_TIMER(_profile, "PartialSortTime");
    _merge_block_timer = ADD_TIMER(_profile, "MergeBlockTime");
    _sort_blocks_memory_usage =
            ADD_CHILD_COUNTER_WITH_LEVEL(_profile, "SortBlocks", TUnit::BYTES, "MemoryUsage", 1);

    _spill_merge_sort_timer =
            ADD_CHILD_TIMER_WITH_LEVEL(_profile, "SpillMergeSortTime", "Spill", 1);
}
#define UPDATE_PROFILE(counter, name)                           \
    do {                                                        \
        auto* child_counter = child_profile->get_counter(name); \
        if (child_counter != nullptr) {                         \
            COUNTER_SET(counter, child_counter->value());       \
        }                                                       \
    } while (false)

void SpillSortSinkLocalState::update_profile(RuntimeProfile* child_profile) {
    UPDATE_PROFILE(_partial_sort_timer, "PartialSortTime");
    UPDATE_PROFILE(_merge_block_timer, "MergeBlockTime");
    UPDATE_PROFILE(_sort_blocks_memory_usage, "SortBlocks");
}
Status SpillSortSinkLocalState::open(RuntimeState* state) {
    RETURN_IF_ERROR(Base::open(state));
    return Status::OK();
}
Status SpillSortSinkLocalState::close(RuntimeState* state, Status execsink_status) {
    {
        std::unique_lock<std::mutex> lk(_spill_lock);
        if (_is_spilling) {
            _spill_cv.wait(lk);
        }
    }
    return Status::OK();
}
Status SpillSortSinkLocalState::setup_in_memory_sort_op(RuntimeState* state) {
    _runtime_state = RuntimeState::create_unique(
            nullptr, state->fragment_instance_id(), state->query_id(), state->fragment_id(),
            state->query_options(), TQueryGlobals {}, state->exec_env(), state->get_query_ctx());
    _runtime_state->set_query_mem_tracker(state->query_mem_tracker());
    _runtime_state->set_task_execution_context(state->get_task_execution_context().lock());
    _runtime_state->set_be_number(state->be_number());

    _runtime_state->set_desc_tbl(&state->desc_tbl());
    _runtime_state->set_pipeline_x_runtime_filter_mgr(state->local_runtime_filter_mgr());

    auto& parent = Base::_parent->template cast<Parent>();
    Base::_shared_state->in_mem_shared_state_sptr =
            parent._sort_sink_operator->create_shared_state();
    Base::_shared_state->in_mem_shared_state =
            static_cast<SortSharedState*>(Base::_shared_state->in_mem_shared_state_sptr.get());

    LocalSinkStateInfo info {0,  _internal_runtime_profile.get(),
                             -1, Base::_shared_state->in_mem_shared_state,
                             {}, {}};
    RETURN_IF_ERROR(parent._sort_sink_operator->setup_local_state(_runtime_state.get(), info));
    auto* sink_local_state = _runtime_state->get_sink_local_state();
    DCHECK(sink_local_state != nullptr);

    _profile->add_info_string("TOP-N", *sink_local_state->profile()->get_info_string("TOP-N"));

    return sink_local_state->open(state);
}

SpillSortSinkOperatorX::SpillSortSinkOperatorX(ObjectPool* pool, int operator_id,
                                               const TPlanNode& tnode, const DescriptorTbl& descs)
        : DataSinkOperatorX(operator_id, tnode.node_id) {
    _sort_sink_operator = std::make_unique<SortSinkOperatorX>(pool, operator_id, tnode, descs);
}

Status SpillSortSinkOperatorX::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(DataSinkOperatorX::init(tnode, state));
    _name = "SPILL_SORT_SINK_OPERATOR";

    _sort_sink_operator->set_dests_id(DataSinkOperatorX<LocalStateType>::dests_id());
    RETURN_IF_ERROR(_sort_sink_operator->set_child(DataSinkOperatorX<LocalStateType>::_child_x));
    return _sort_sink_operator->init(tnode, state);
}

Status SpillSortSinkOperatorX::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(DataSinkOperatorX<LocalStateType>::prepare(state));
    RETURN_IF_ERROR(_sort_sink_operator->prepare(state));
    _enable_spill = _sort_sink_operator->is_full_sort();
    LOG(INFO) << "spill sort sink, enable spill: " << _enable_spill;
    return Status::OK();
}
Status SpillSortSinkOperatorX::open(RuntimeState* state) {
    RETURN_IF_ERROR(DataSinkOperatorX<LocalStateType>::open(state));
    return _sort_sink_operator->open(state);
}
Status SpillSortSinkOperatorX::close(RuntimeState* state) {
    RETURN_IF_ERROR(DataSinkOperatorX<LocalStateType>::close(state));
    return _sort_sink_operator->close(state);
}
Status SpillSortSinkOperatorX::revoke_memory(RuntimeState* state) {
    if (!_enable_spill) {
        return Status::OK();
    }
    auto& local_state = get_local_state(state);
    return local_state.revoke_memory(state);
}
size_t SpillSortSinkOperatorX::revocable_mem_size(RuntimeState* state) const {
    if (!_enable_spill) {
        return 0;
    }
    auto& local_state = get_local_state(state);
    if (!local_state.Base::_shared_state->sink_status.ok()) {
        return UINT64_MAX;
    }
    return _sort_sink_operator->get_revocable_mem_size(local_state._runtime_state.get());
}
Status SpillSortSinkOperatorX::sink(doris::RuntimeState* state, vectorized::Block* in_block,
                                    bool eos) {
    auto& local_state = get_local_state(state);
    SCOPED_TIMER(local_state.exec_time_counter());
    RETURN_IF_ERROR(local_state.Base::_shared_state->sink_status);
    COUNTER_UPDATE(local_state.rows_input_counter(), (int64_t)in_block->rows());
    if (in_block->rows() > 0) {
        local_state._shared_state->update_spill_block_batch_row_count(in_block);
    }
    local_state._eos = eos;
    RETURN_IF_ERROR(_sort_sink_operator->sink(local_state._runtime_state.get(), in_block, false));
    local_state._mem_tracker->set_consumption(
            local_state._shared_state->in_mem_shared_state->sorter->data_size());
    if (eos) {
        if (_enable_spill) {
            if (revocable_mem_size(state) > 0) {
                RETURN_IF_ERROR(revoke_memory(state));
            } else {
                local_state._dependency->set_ready_to_read();
            }
        } else {
            RETURN_IF_ERROR(
                    local_state._shared_state->in_mem_shared_state->sorter->prepare_for_read());
            local_state._dependency->set_ready_to_read();
        }
    }
    return Status::OK();
}
Status SpillSortSinkLocalState::revoke_memory(RuntimeState* state) {
    DCHECK(!_is_spilling);
    _is_spilling = true;

    LOG(INFO) << "sort node " << Base::_parent->id() << " revoke_memory"
              << ", eos: " << _eos;
    RETURN_IF_ERROR(Base::_shared_state->sink_status);

    auto status = ExecEnv::GetInstance()->spill_stream_mgr()->register_spill_stream(
            state, _spilling_stream, print_id(state->query_id()), "sort", _parent->id(),
            _shared_state->spill_block_batch_row_count,
            SpillSortSharedState::SORT_BLOCK_SPILL_BATCH_BYTES, profile());
    RETURN_IF_ERROR(status);

    _spilling_stream->set_write_counters(Base::_spill_serialize_block_timer,
                                         Base::_spill_block_count, Base::_spill_data_size,
                                         Base::_spill_write_disk_timer);

    status = _spilling_stream->prepare_spill();
    RETURN_IF_ERROR(status);
    _shared_state->sorted_streams.emplace_back(_spilling_stream);

    auto& parent = Base::_parent->template cast<Parent>();

    // TODO: spill thread may set_ready before the task::execute thread put the task to blocked state
    if (!_eos) {
        Base::_dependency->Dependency::block();
    }
    status =
            ExecEnv::GetInstance()
                    ->spill_stream_mgr()
                    ->get_spill_io_thread_pool(_spilling_stream->get_spill_root_dir())
                    ->submit_func([this, state, &parent] {
                        SCOPED_ATTACH_TASK(state);
                        Defer defer {[&]() {
                            if (!_shared_state->sink_status.ok()) {
                                LOG(WARNING)
                                        << "sort node " << _parent->id()
                                        << " revoke memory error: " << _shared_state->sink_status;
                            } else {
                                LOG(INFO)
                                        << "sort node " << _parent->id() << " revoke memory finish";
                            }

                            _spilling_stream->end_spill(_shared_state->sink_status);
                            if (!_shared_state->sink_status.ok()) {
                                _shared_state->clear();
                            }

                            {
                                std::unique_lock<std::mutex> lk(_spill_lock);
                                _spilling_stream.reset();
                                _is_spilling = false;
                                if (_eos) {
                                    _dependency->set_ready_to_read();
                                    _finish_dependency->set_ready();
                                } else {
                                    _dependency->Dependency::set_ready();
                                }
                                _spill_cv.notify_one();
                            }
                        }};

                        _shared_state->sink_status =
                                parent._sort_sink_operator->prepare_for_spill(_runtime_state.get());
                        RETURN_IF_ERROR(_shared_state->sink_status);

                        auto* sink_local_state = _runtime_state->get_sink_local_state();
                        update_profile(sink_local_state->profile());

                        bool eos = false;
                        vectorized::Block block;
                        while (!eos && !state->is_cancelled()) {
                            {
                                SCOPED_TIMER(_spill_merge_sort_timer);
                                _shared_state->sink_status =
                                        parent._sort_sink_operator->merge_sort_read_for_spill(
                                                _runtime_state.get(), &block,
                                                _shared_state->spill_block_batch_row_count, &eos);
                            }
                            RETURN_IF_ERROR(_shared_state->sink_status);
                            {
                                SCOPED_TIMER(Base::_spill_timer);
                                _shared_state->sink_status =
                                        _spilling_stream->spill_block(block, eos);
                            }
                            RETURN_IF_ERROR(_shared_state->sink_status);
                            block.clear_column_data();
                        }
                        parent._sort_sink_operator->reset(_runtime_state.get());

                        return Status::OK();
                    });
    if (!status.ok()) {
        _is_spilling = false;
        _spilling_stream->end_spill(status);

        if (!_eos) {
            Base::_dependency->Dependency::set_ready();
        }
    }
    return status;
}
} // namespace doris::pipeline