// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include <runtime/decimalv3.h>

#include <cstdint>

#include "column/binary_column.h"
#include "column/column.h"
#include "column/nullable_column.h"
#include "gutil/casts.h"
#include "roaring/roaring.hh"
#include "storage/rowset/bitmap_index_reader.h"
#include "storage/rowset/bloom_filter.h"
#include "storage/types.h"
#include "storage/vectorized/column_predicate.h"
#include "storage/vectorized/range.h"

namespace starrocks::vectorized {

template <FieldType field_type>
class ColumnEqPredicate : public ColumnPredicate {
    using ValueType = typename CppTypeTraits<field_type>::CppType;

public:
    ColumnEqPredicate(const TypeInfoPtr& type_info, ColumnId id, ValueType value)
            : ColumnPredicate(type_info, id), _value(value) {}

    ~ColumnEqPredicate() override = default;

    template <typename Op>
    inline void t_evaluate(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const {
        auto* v = reinterpret_cast<const ValueType*>(column->raw_data());
        auto* sel = selection;
        if (!column->has_null()) {
            for (size_t i = from; i < to; i++) {
                sel[i] = Op::apply(sel[i], (uint8_t)(v[i] == _value));
            }
        } else {
            /* must use const uint8_t* to make vectorized effect, vector<uint8_t> not work */
            const uint8_t* is_null = down_cast<const NullableColumn*>(column)->immutable_null_column_data().data();
            for (size_t i = from; i < to; i++) {
                sel[i] = Op::apply(sel[i], (uint8_t)((!is_null[i]) & (v[i] == _value)));
            }
        }
    }

    void evaluate(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateAssignOp>(column, selection, from, to);
    }

    void evaluate_and(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateAndOp>(column, selection, from, to);
    }

    void evaluate_or(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateOrOp>(column, selection, from, to);
    }

    bool zone_map_filter(const ZoneMapDetail& detail) const override {
        const auto& min = detail.min_or_null_value();
        const auto& max = detail.max_value();
        const auto type_info = this->type_info();
        return type_info->cmp(Datum(_value), min) >= 0 && type_info->cmp(Datum(_value), max) <= 0;
    }

    Status seek_bitmap_dictionary(BitmapIndexIterator* iter, SparseRange* range) const override {
        range->clear();
        bool exact_match = false;
        Status s = iter->seek_dictionary(&_value, &exact_match);
        if (s.ok()) {
            if (exact_match) {
                rowid_t ordinal = iter->current_ordinal();
                range->add(Range(ordinal, ordinal + 1));
            }
        } else if (!s.is_not_found()) {
            return s;
        }
        return Status::OK();
    }

    bool support_bloom_filter() const override { return true; }

    bool bloom_filter(const BloomFilter* bf) const override {
        static_assert(field_type != OLAP_FIELD_TYPE_HLL, "TODO");
        static_assert(field_type != OLAP_FIELD_TYPE_OBJECT, "TODO");
        static_assert(field_type != OLAP_FIELD_TYPE_PERCENTILE, "TODO");
        return bf->test_bytes(reinterpret_cast<const char*>(&_value), sizeof(_value));
    }

    PredicateType type() const override { return PredicateType::kEQ; }

    Datum value() const override { return Datum(_value); }

    std::vector<Datum> values() const override { return std::vector<Datum>{Datum(_value)}; }

    bool can_vectorized() const override { return true; }

    Status convert_to(const ColumnPredicate** output, const TypeInfoPtr& target_type_info,
                      ObjectPool* obj_pool) const override {
        return predicate_convert_to<field_type>(*this, _value, new_column_eq_predicate, output, target_type_info,
                                                obj_pool);
    }

    std::string debug_string() const override {
        std::stringstream ss;
        ss << "(columnId(" << _column_id << ")==" << this->type_info()->to_string(&_value) << ")";
        return ss.str();
    }

private:
    ValueType _value;
};

template <FieldType field_type>
class BinaryColumnEqPredicate : public ColumnPredicate {
    using ValueType = Slice;

public:
    BinaryColumnEqPredicate(const TypeInfoPtr& type_info, ColumnId id, ValueType value)
            : ColumnPredicate(type_info, id), _zero_padded_str(value.data, value.size), _value(_zero_padded_str) {}

    ~BinaryColumnEqPredicate() override = default;

    template <typename Op>
    inline void t_evaluate(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const {
        auto* v = reinterpret_cast<const ValueType*>(column->raw_data());
        auto* sel = selection;
        if (!column->has_null()) {
            for (size_t i = from; i < to; i++) {
                sel[i] = Op::apply(sel[i], (uint8_t)(v[i] == _value));
            }
        } else {
            /* must use const uint8_t* to make vectorized effect, vector<uint8_t> not work */
            const uint8_t* is_null = down_cast<const NullableColumn*>(column)->immutable_null_column_data().data();
            for (size_t i = from; i < to; i++) {
                sel[i] = Op::apply(sel[i], (uint8_t)((!is_null[i]) && (v[i] == _value)));
            }
        }
    }

    void evaluate(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateAssignOp>(column, selection, from, to);
    }

    void evaluate_and(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateAndOp>(column, selection, from, to);
    }

    void evaluate_or(const Column* column, uint8_t* selection, uint16_t from, uint16_t to) const override {
        t_evaluate<ColumnPredicateOrOp>(column, selection, from, to);
    }

    uint16_t evaluate_branchless(const Column* column, uint16_t* sel, uint16_t sel_size) const override {
        // Get BinaryColumn
        const BinaryColumn* binary_column;
        if (column->is_nullable()) {
            // This is NullableColumn, get its data_column
            binary_column =
                    down_cast<const BinaryColumn*>(down_cast<const NullableColumn*>(column)->data_column().get());
        } else {
            binary_column = down_cast<const BinaryColumn*>(column);
        }

        uint16_t new_size = 0;
        if (!column->has_null()) {
            for (uint16_t i = 0; i < sel_size; ++i) {
                uint16_t data_idx = sel[i];
                sel[new_size] = data_idx;
                new_size += binary_column->get_slice(data_idx) == _value;
            }
        } else {
            /* must use uint8_t* to make vectorized effect */
            const uint8_t* is_null = down_cast<const NullableColumn*>(column)->immutable_null_column_data().data();
            for (uint16_t i = 0; i < sel_size; ++i) {
                uint16_t data_idx = sel[i];
                sel[new_size] = data_idx;
                new_size += !is_null[data_idx] && binary_column->get_slice(data_idx) == _value;
            }
        }
        return new_size;
    }

    bool zone_map_filter(const ZoneMapDetail& detail) const override {
        const auto& min = detail.min_or_null_value();
        const auto& max = detail.max_value();
        const auto type_info = this->type_info();
        return type_info->cmp(Datum(_value), min) >= 0 && type_info->cmp(Datum(_value), max) <= 0;
    }

    PredicateType type() const override { return PredicateType::kEQ; }

    Datum value() const override { return Datum(Slice(_zero_padded_str)); }

    std::vector<Datum> values() const override { return std::vector<Datum>{Datum(_value)}; }

    bool can_vectorized() const override { return false; }

    Status seek_bitmap_dictionary(BitmapIndexIterator* iter, SparseRange* range) const override {
        // see the comment in `predicate_parser.cpp`.
        Slice padded_value(_zero_padded_str);
        range->clear();
        bool exact_match = false;
        Status s = iter->seek_dictionary(&padded_value, &exact_match);
        if (s.ok()) {
            if (exact_match) {
                rowid_t ordinal = iter->current_ordinal();
                range->add(Range(ordinal, ordinal + 1));
            }
        } else if (!s.is_not_found()) {
            return s;
        }
        return Status::OK();
    }

    bool support_bloom_filter() const override { return true; }

    bool bloom_filter(const BloomFilter* bf) const override {
        Slice padded(_zero_padded_str);
        return bf->test_bytes(padded.data, padded.size);
    }

    Status convert_to(const ColumnPredicate** output, const TypeInfoPtr& target_type_info,
                      ObjectPool* obj_pool) const override {
        const auto to_type = target_type_info->type();
        if (to_type == field_type) {
            *output = this;
            return Status::OK();
        }
        CHECK(false) << "Not support, from_type=" << field_type << ", to_type=" << to_type;
        return Status::OK();
    }

    std::string debug_string() const override {
        std::stringstream ss;
        ss << "(columnId(" << _column_id << ")=" << _zero_padded_str << ")";
        return ss.str();
    }

    bool padding_zeros(size_t len) override {
        size_t old_sz = _zero_padded_str.size();
        _zero_padded_str.append(len > old_sz ? len - old_sz : 0, '\0');
        _value = Slice(_zero_padded_str.data(), old_sz);
        return true;
    }

private:
    std::string _zero_padded_str;
    ValueType _value;
};

// declared in column_predicate.h.
ColumnPredicate* new_column_eq_predicate(const TypeInfoPtr& type_info, ColumnId id, const Slice& operand) {
    return new_column_predicate<ColumnEqPredicate, BinaryColumnEqPredicate>(type_info, id, operand);
}

} // namespace starrocks::vectorized
