// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <memory>

#include "column/chunk.h"
#include "column/schema.h"
#include "storage/vectorized/row_source_mask.h"
#include "storage/vectorized/tablet_reader_params.h"
#include "util/runtime_profile.h"

namespace starrocks {
class Status;
} // namespace starrocks

namespace starrocks::vectorized {

class Chunk;

class ChunkIterator {
public:
    // |schema| is the output fields.
    ChunkIterator(vectorized::Schema schema) : _schema(std::move(schema)) {}

    ChunkIterator(vectorized::Schema schema, int chunk_size) : _schema(std::move(schema)), _chunk_size(chunk_size) {}

    virtual ~ChunkIterator() = default;

    // Fetch records from |this| iterator into |chunk|.
    //
    // REQUIRES: |chunk| is not null and is empty. the type of each column in |chunk| must
    // correspond to each field in `schema()`, in the same order.
    //
    // if the returned status is `OK`, at least on record is appended to |chunk|,
    // i.e, size of |chunk| must be greater than zero.
    // if the returned status is `EndOfFile`, size of |chunk| must be zero;
    // otherwise, the size of |chunk| is undefined.
    Status get_next(Chunk* chunk) {
        Status st = do_get_next(chunk);
        DCHECK_CHUNK(chunk);
        return st;
    }

    // like get_next(Chunk* chunk), but also returns each row's rowid(ordinal id)
    Status get_next(Chunk* chunk, vector<uint32_t>* rowid) {
        Status st = do_get_next(chunk, rowid);
        DCHECK_CHUNK(chunk);
        return st;
    }

    // like get_next(Chunk* chunk), but also returns each row source mask
    // row source mask sequence will be generated by HeapMergeIterator or be used by MaskMergeIterator.
    Status get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) {
        Status st = do_get_next(chunk, source_masks);
        DCHECK_CHUNK(chunk);
        return st;
    }

    // Release resources associated with this iterator, e.g, deallocate memory.
    // This routine can be called at most once.
    virtual void close() = 0;

    virtual std::size_t merged_rows() const { return 0; }

    const Schema& schema() const { return _schema; }

    // Returns the Schema of the result.
    // If a Field uses the global dictionary strategy, the field will be rewritten as INT
    const Schema& encoded_schema() const { return _encoded_schema; }

    virtual Status init_encoded_schema(ColumnIdToGlobalDictMap& dict_maps) {
        _encoded_schema.reserve(schema().num_fields());
        for (const auto& field : schema().fields()) {
            const auto cid = field->id();
            if (dict_maps.count(cid)) {
                _encoded_schema.append(Field::convert_to_dict_field(*field));
            } else {
                _encoded_schema.append(field);
            }
        }
        return Status::OK();
    }

    virtual Status init_output_schema(const std::unordered_set<uint32_t>& unused_output_column_ids) {
        if (_is_init_output_schema) {
            return Status::OK();
        }
        for (const auto& field : encoded_schema().fields()) {
            const auto cid = field->id();
            if (!unused_output_column_ids.count(cid)) {
                _output_schema.append(field);
            }
        }
        DCHECK(_output_schema.num_fields() > 0);
        _is_init_output_schema = true;
        return Status::OK();
    }

    const Schema& output_schema() const {
        if (_is_init_output_schema) {
            return _output_schema;
        } else {
            return _schema;
        }
    }

    int chunk_size() const { return _chunk_size; }

protected:
    virtual Status do_get_next(Chunk* chunk) = 0;
    virtual Status do_get_next(Chunk* chunk, vector<uint32_t>* rowid) {
        return Status::NotSupported("Chunk* chunk, vector<uint32_t>* rowid) not supported");
    }
    virtual Status do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) {
        return Status::NotSupported("get chunk with sources not supported");
    }

    vectorized::Schema _schema;
    vectorized::Schema _encoded_schema;
    vectorized::Schema _output_schema;
    bool _is_init_output_schema = false;

    int _chunk_size = DEFAULT_CHUNK_SIZE;
};

using ChunkIteratorPtr = std::shared_ptr<ChunkIterator>;

class TimedChunkIterator final : public ChunkIterator {
public:
    TimedChunkIterator(ChunkIteratorPtr iter, RuntimeProfile::Counter* counter)
            : ChunkIterator(iter->schema(), iter->chunk_size()), _iter(std::move(iter)), _cost(0), _counter(counter) {}

    ~TimedChunkIterator() override = default;

    void close() override {
        COUNTER_UPDATE(_counter, _cost);
        _iter->close();
        _iter.reset();
    }

    size_t merged_rows() const override { return _iter->merged_rows(); }

    virtual Status init_encoded_schema(ColumnIdToGlobalDictMap& dict_maps) override {
        ChunkIterator::init_encoded_schema(dict_maps);
        _iter->init_encoded_schema(dict_maps);
        return Status::OK();
    }

    virtual Status init_output_schema(const std::unordered_set<uint32_t>& unused_output_column_ids) override {
        ChunkIterator::init_output_schema(unused_output_column_ids);
        _iter->init_output_schema(unused_output_column_ids);
        return Status::OK();
    }

private:
    Status do_get_next(Chunk* chunk) override {
        SCOPED_RAW_TIMER(&_cost);
        return _iter->get_next(chunk);
    }

    Status do_get_next(Chunk* chunk, vector<uint32_t>* rowid) override {
        SCOPED_RAW_TIMER(&_cost);
        return _iter->get_next(chunk, rowid);
    }

    Status do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) override {
        SCOPED_RAW_TIMER(&_cost);
        return _iter->get_next(chunk, source_masks);
    }

    ChunkIteratorPtr _iter;
    int64_t _cost;
    RuntimeProfile::Counter* _counter;
};

inline ChunkIteratorPtr timed_chunk_iterator(const ChunkIteratorPtr& iter, RuntimeProfile::Counter* counter) {
    return std::make_shared<TimedChunkIterator>(iter, counter);
}

} // namespace starrocks::vectorized
