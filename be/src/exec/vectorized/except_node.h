// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <unordered_set>

#include "column/chunk.h"
#include "column/column_hash.h"
#include "column/column_helper.h"
#include "column/type_traits.h"
#include "exec/olap_common.h"
#include "exec/pipeline/operator.h"
#include "exec/vectorized/except_hash_set.h"
#include "exprs/expr_context.h"
#include "gutil/casts.h"
#include "runtime/mem_pool.h"
#include "util/hash_util.hpp"
#include "util/phmap/phmap.h"
#include "util/slice.h"

namespace starrocks {
class DescriptorTbl;
class SlotDescriptor;
class TupleDescriptor;
} // namespace starrocks

namespace starrocks::vectorized {
class ExceptNode : public ExecNode {
public:
    ExceptNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);

    ~ExceptNode() override {
        if (runtime_state() != nullptr) {
            close(runtime_state());
        }
    }

    Status init(const TPlanNode& tnode, RuntimeState* state = nullptr) override;
    Status prepare(RuntimeState* state) override;
    Status open(RuntimeState* state) override;
    Status get_next(RuntimeState* state, ChunkPtr* row_batch, bool* eos) override;
    Status close(RuntimeState* state) override;

    pipeline::OpFactories decompose_to_pipeline(pipeline::PipelineBuilderContext* context) override;

    int64_t mem_usage() const {
        int64_t usage = 0;
        if (_hash_set != nullptr) {
            usage += _hash_set->mem_usage();
        }
        if (_build_pool != nullptr) {
            usage += _build_pool->total_reserved_bytes();
        }
        return usage;
    }

private:
    /// Tuple id resolved in Prepare() to set tuple_desc_;
    const int _tuple_id;
    /// Descriptor for tuples this union node constructs.
    const TupleDescriptor* _tuple_desc;
    // Exprs materialized by this node. The i-th result expr list refers to the i-th child.
    std::vector<std::vector<ExprContext*>> _child_expr_lists;

    struct ExceptColumnTypes {
        TypeDescriptor result_type;
        bool is_nullable;
        bool is_constant;
    };
    std::vector<ExceptColumnTypes> _types;

    std::unique_ptr<ExceptHashSerializeSet> _hash_set;
    ExceptHashSerializeSet::Iterator _hash_set_iterator;
    ExceptHashSerializeSet::KeyVector _remained_keys;

    // pool for allocate key.
    std::unique_ptr<MemPool> _build_pool;

    RuntimeProfile::Counter* _build_set_timer = nullptr; // time to build hash set
    RuntimeProfile::Counter* _erase_duplicate_row_timer = nullptr;
    RuntimeProfile::Counter* _get_result_timer = nullptr;
};

} // namespace starrocks::vectorized
