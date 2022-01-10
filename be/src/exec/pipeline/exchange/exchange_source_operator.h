// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <atomic>

#include "exec/pipeline/source_operator.h"

namespace starrocks {
class DataStreamRecvr;
class RowDescriptor;
namespace pipeline {
class ExchangeSourceOperator : public SourceOperator {
public:
    ExchangeSourceOperator(OperatorFactory* factory, int32_t id, int32_t plan_node_id)
            : SourceOperator(factory, id, "exchange_source", plan_node_id) {}

    ~ExchangeSourceOperator() override = default;

    Status prepare(RuntimeState* state) override;

    bool has_output() const override;

    bool is_finished() const override;

    void set_finishing(RuntimeState* state) override;

    StatusOr<vectorized::ChunkPtr> pull_chunk(RuntimeState* state) override;

private:
    std::shared_ptr<DataStreamRecvr> _stream_recvr = nullptr;
    std::atomic<bool> _is_finishing = false;
};

class ExchangeSourceOperatorFactory final : public SourceOperatorFactory {
public:
    ExchangeSourceOperatorFactory(int32_t id, int32_t plan_node_id, int32_t num_sender, const RowDescriptor& row_desc)
            : SourceOperatorFactory(id, "exchange_source", plan_node_id),
              _num_sender(num_sender),
              _row_desc(row_desc) {}

    ~ExchangeSourceOperatorFactory() override = default;

    OperatorPtr create(int32_t degree_of_parallelism, int32_t driver_sequence) override {
        ++_stream_recvr_cnt;
        return std::make_shared<ExchangeSourceOperator>(this, _id, _plan_node_id);
    }

    std::shared_ptr<DataStreamRecvr> create_stream_recvr(RuntimeState* state,
                                                         const std::shared_ptr<RuntimeProfile>& profile);
    void close_stream_recvr();

private:
    int32_t _num_sender;
    const RowDescriptor& _row_desc;
    std::shared_ptr<DataStreamRecvr> _stream_recvr = nullptr;
    std::atomic<int64_t> _stream_recvr_cnt = 0;
};

} // namespace pipeline
} // namespace starrocks
