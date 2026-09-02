// Arrow C Data Interface (Phase 1): zero-copy in-memory interoperability.
// Exposes market replay, trades, and simulation telemetry directly to Python,
// Polars, DuckDB, and PyArrow without intermediate serialization.
#pragma once

#include <mog/ColumnLog.hpp>
#include <mog/Simulate.hpp>
#include <mog/Tearsheet.hpp>
#include <mog/Trades.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
    const char* format = nullptr;
    const char* name = nullptr;
    const char* metadata = nullptr;
    int64_t flags = 0;
    int64_t n_children = 0;
    struct ArrowSchema** children = nullptr;
    struct ArrowSchema* dictionary = nullptr;

    void (*release)(struct ArrowSchema*) = nullptr;
    void* private_data = nullptr;
};

struct ArrowArray {
    int64_t length = 0;
    int64_t null_count = 0;
    int64_t offset = 0;
    int64_t n_buffers = 0;
    int64_t n_children = 0;
    const void** buffers = nullptr;
    struct ArrowArray** children = nullptr;
    struct ArrowArray* dictionary = nullptr;

    void (*release)(struct ArrowArray*) = nullptr;
    void* private_data = nullptr;
};

#endif // ARROW_C_DATA_INTERFACE

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
    int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out) = nullptr;
    int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out) = nullptr;
    const char* (*get_last_error)(struct ArrowArrayStream*) = nullptr;
    void (*release)(struct ArrowArrayStream*) = nullptr;
    void* private_data = nullptr;
};

#endif // ARROW_C_STREAM_INTERFACE

namespace mog::arrow {

namespace detail {

template <class T>
struct underlying_or_self {
    using type = T;
};
template <class T>
    requires std::is_enum_v<T>
struct underlying_or_self<T> {
    using type = std::underlying_type_t<T>;
};
template <class T>
using underlying_or_self_t = typename underlying_or_self<T>::type;

struct SchemaHolder {
    std::string format;
    std::string name;
    std::vector<ArrowSchema> child_schemas;
    std::vector<ArrowSchema*> child_ptrs;
};

inline void release_schema(ArrowSchema* schema) noexcept {
    if (schema == nullptr || schema->release == nullptr)
        return;
    for (int64_t i = 0; i < schema->n_children; ++i) {
        if (schema->children != nullptr && schema->children[i] != nullptr &&
            schema->children[i]->release != nullptr) {
            schema->children[i]->release(schema->children[i]);
        }
    }
    delete static_cast<SchemaHolder*>(schema->private_data);
    schema->release = nullptr;
    schema->private_data = nullptr;
}

inline void init_schema(ArrowSchema* out, std::string format, std::string name,
                        std::vector<ArrowSchema> children = {}) {
    auto* holder = new SchemaHolder();
    holder->format = std::move(format);
    holder->name = std::move(name);
    holder->child_schemas = std::move(children);
    holder->child_ptrs.resize(holder->child_schemas.size());
    for (std::size_t i = 0; i < holder->child_schemas.size(); ++i) {
        holder->child_ptrs[i] = &holder->child_schemas[i];
    }

    out->format = holder->format.c_str();
    out->name = holder->name.empty() ? nullptr : holder->name.c_str();
    out->metadata = nullptr;
    out->flags = 0;
    out->n_children = static_cast<int64_t>(holder->child_ptrs.size());
    out->children = holder->child_ptrs.empty() ? nullptr : holder->child_ptrs.data();
    out->dictionary = nullptr;
    out->release = &release_schema;
    out->private_data = holder;
}

struct ArrayHolder {
    std::vector<const void*> buffer_ptrs;
    std::vector<ArrowArray> child_arrays;
    std::vector<ArrowArray*> child_ptrs;
    std::vector<std::vector<std::uint8_t>> owned_buffers;
};

inline void release_array(ArrowArray* array) noexcept {
    if (array == nullptr || array->release == nullptr)
        return;
    for (int64_t i = 0; i < array->n_children; ++i) {
        if (array->children != nullptr && array->children[i] != nullptr &&
            array->children[i]->release != nullptr) {
            array->children[i]->release(array->children[i]);
        }
    }
    delete static_cast<ArrayHolder*>(array->private_data);
    array->release = nullptr;
    array->private_data = nullptr;
}

template <typename T, typename Producer>
inline ArrowArray make_primitive_array_direct(std::size_t n, Producer&& prod) {
    auto* holder = new ArrayHolder();
    holder->owned_buffers.emplace_back(n * sizeof(T));
    auto* dst = reinterpret_cast<T*>(holder->owned_buffers[0].data());
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<T>(prod(i));
    }
    holder->buffer_ptrs.resize(2);
    holder->buffer_ptrs[0] = nullptr;
    holder->buffer_ptrs[1] = holder->owned_buffers[0].data();

    ArrowArray arr{};
    arr.length = static_cast<int64_t>(n);
    arr.null_count = 0;
    arr.offset = 0;
    arr.n_buffers = 2;
    arr.n_children = 0;
    arr.buffers = holder->buffer_ptrs.data();
    arr.children = nullptr;
    arr.dictionary = nullptr;
    arr.release = &release_array;
    arr.private_data = holder;
    return arr;
}

template <typename Producer>
inline ArrowArray make_bool_array_direct(std::size_t n, Producer&& prod) {
    auto* holder = new ArrayHolder();
    const std::size_t n_bytes = (n + 7) / 8;
    holder->owned_buffers.emplace_back(n_bytes, 0);
    auto* dst = holder->owned_buffers[0].data();
    for (std::size_t i = 0; i < n; ++i) {
        if (prod(i)) {
            dst[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
    }
    holder->buffer_ptrs.resize(2);
    holder->buffer_ptrs[0] = nullptr;
    holder->buffer_ptrs[1] = holder->owned_buffers[0].data();

    ArrowArray arr{};
    arr.length = static_cast<int64_t>(n);
    arr.null_count = 0;
    arr.offset = 0;
    arr.n_buffers = 2;
    arr.n_children = 0;
    arr.buffers = holder->buffer_ptrs.data();
    arr.children = nullptr;
    arr.dictionary = nullptr;
    arr.release = &release_array;
    arr.private_data = holder;
    return arr;
}

inline ArrowArray make_primitive_array(std::span<const std::uint8_t> data) {
    return make_primitive_array_direct<std::uint8_t>(data.size(),
                                                     [&](std::size_t i) { return data[i]; });
}

template <typename T>
inline ArrowArray make_primitive_array(std::span<const T> data, const char* = nullptr) {
    return make_primitive_array_direct<T>(data.size(), [&](std::size_t i) { return data[i]; });
}

inline ArrowArray make_bool_array(std::span<const std::uint8_t> data) {
    return make_bool_array_direct(data.size(), [&](std::size_t i) { return data[i] != 0; });
}

inline void assemble_struct_array(ArrowArray* out, int64_t length,
                                  std::vector<ArrowArray> children) {
    auto* holder = new ArrayHolder();
    holder->child_arrays = std::move(children);
    holder->child_ptrs.resize(holder->child_arrays.size());
    for (std::size_t i = 0; i < holder->child_arrays.size(); ++i) {
        holder->child_ptrs[i] = &holder->child_arrays[i];
    }
    holder->buffer_ptrs.resize(1);
    holder->buffer_ptrs[0] = nullptr; // struct null bitmap

    out->length = length;
    out->null_count = 0;
    out->offset = 0;
    out->n_buffers = 1;
    out->n_children = static_cast<int64_t>(holder->child_ptrs.size());
    out->buffers = holder->buffer_ptrs.data();
    out->children = holder->child_ptrs.empty() ? nullptr : holder->child_ptrs.data();
    out->dictionary = nullptr;
    out->release = &release_array;
    out->private_data = holder;
}

} // namespace detail

// Exports reconstructed trade records into Apache Arrow C Data Interface structures.
inline void export_trades(std::span<const trades::Print> records, ArrowArray* out_array,
                          ArrowSchema* out_schema) {
    const std::size_t n = records.size();

    if (out_schema != nullptr) {
        std::vector<ArrowSchema> children(7);
        detail::init_schema(&children[0], "L", "match_number");
        detail::init_schema(&children[1], "I", "locate");
        detail::init_schema(&children[2], "L", "ts_ns");
        detail::init_schema(&children[3], "l", "price_ticks");
        detail::init_schema(&children[4], "I", "shares");
        detail::init_schema(&children[5], "b", "printable");
        detail::init_schema(&children[6], "b", "from_execute_with_price");
        detail::init_schema(out_schema, "+s", "", std::move(children));
    }

    if (out_array != nullptr) {
        std::vector<ArrowArray> children;
        children.reserve(7);
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return records[i].match_number; }));
        children.push_back(detail::make_primitive_array_direct<std::uint32_t>(
            n, [&](std::size_t i) { return records[i].locate; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return records[i].ts_ns; }));
        children.push_back(detail::make_primitive_array_direct<std::int64_t>(
            n, [&](std::size_t i) { return records[i].price_ticks; }));
        children.push_back(detail::make_primitive_array_direct<std::uint32_t>(
            n, [&](std::size_t i) { return records[i].shares; }));
        children.push_back(
            detail::make_bool_array_direct(n, [&](std::size_t i) { return records[i].printable; }));
        children.push_back(detail::make_bool_array_direct(
            n, [&](std::size_t i) { return records[i].from_execute_with_price; }));
        detail::assemble_struct_array(out_array, static_cast<int64_t>(n), std::move(children));
    }
}

inline void export_trade_records(std::span<const trades::Print> records, ArrowArray* out_array,
                                 ArrowSchema* out_schema) {
    export_trades(records, out_array, out_schema);
}

// Exports simulation fill reports into Apache Arrow C Data Interface structures.
inline void export_fill_reports(std::span<const SimFillReport> reports, ArrowArray* out_array,
                                ArrowSchema* out_schema) {
    const std::size_t n = reports.size();

    if (out_schema != nullptr) {
        std::vector<ArrowSchema> children(7);
        detail::init_schema(&children[0], "L", "visible_ts");
        detail::init_schema(&children[1], "L", "ref");
        detail::init_schema(&children[2], "l", "price_ticks");
        detail::init_schema(&children[3], "I", "qty");
        detail::init_schema(&children[4], "L", "seq");
        detail::init_schema(&children[5], "l", "fee");
        detail::init_schema(&children[6], "C", "side");
        detail::init_schema(out_schema, "+s", "", std::move(children));
    }

    if (out_array != nullptr) {
        std::vector<ArrowArray> children;
        children.reserve(7);
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return reports[i].visible_ts; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return reports[i].ref; }));
        children.push_back(detail::make_primitive_array_direct<std::int64_t>(
            n, [&](std::size_t i) { return reports[i].price_ticks; }));
        children.push_back(detail::make_primitive_array_direct<std::uint32_t>(
            n, [&](std::size_t i) { return reports[i].qty; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return reports[i].seq; }));
        children.push_back(detail::make_primitive_array_direct<std::int64_t>(
            n, [&](std::size_t i) { return reports[i].fee; }));
        children.push_back(detail::make_primitive_array_direct<std::uint8_t>(
            n, [&](std::size_t i) { return static_cast<std::uint8_t>(reports[i].side); }));
        detail::assemble_struct_array(out_array, static_cast<int64_t>(n), std::move(children));
    }
}

// Exports simulation decision records into Apache Arrow C Data Interface structures.
inline void export_decisions(std::span<const SimDecision> decisions, ArrowArray* out_array,
                             ArrowSchema* out_schema) {
    const std::size_t n = decisions.size();

    if (out_schema != nullptr) {
        std::vector<ArrowSchema> children(9);
        detail::init_schema(&children[0], "C", "kind");
        detail::init_schema(&children[1], "I", "filled_qty");
        detail::init_schema(&children[2], "I", "cancelled_qty");
        detail::init_schema(&children[3], "L", "filled_notional");
        detail::init_schema(&children[4], "L", "visible_ts");
        detail::init_schema(&children[5], "L", "ref");
        detail::init_schema(&children[6], "C", "side");
        detail::init_schema(&children[7], "L", "seq");
        detail::init_schema(&children[8], "l", "fee");
        detail::init_schema(out_schema, "+s", "", std::move(children));
    }

    if (out_array != nullptr) {
        std::vector<ArrowArray> children;
        children.reserve(9);
        children.push_back(detail::make_primitive_array_direct<std::uint8_t>(
            n, [&](std::size_t i) { return static_cast<std::uint8_t>(decisions[i].kind); }));
        children.push_back(detail::make_primitive_array_direct<std::uint32_t>(
            n, [&](std::size_t i) { return decisions[i].filled_qty; }));
        children.push_back(detail::make_primitive_array_direct<std::uint32_t>(
            n, [&](std::size_t i) { return decisions[i].cancelled_qty; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return decisions[i].filled_notional; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return decisions[i].visible_ts; }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return decisions[i].ref; }));
        children.push_back(detail::make_primitive_array_direct<std::uint8_t>(
            n, [&](std::size_t i) { return static_cast<std::uint8_t>(decisions[i].side); }));
        children.push_back(detail::make_primitive_array_direct<std::uint64_t>(
            n, [&](std::size_t i) { return decisions[i].seq; }));
        children.push_back(detail::make_primitive_array_direct<std::int64_t>(
            n, [&](std::size_t i) { return decisions[i].fee; }));
        detail::assemble_struct_array(out_array, static_cast<int64_t>(n), std::move(children));
    }
}

inline const char* arrow_format_of(ColumnType t) noexcept {
    switch (t) {
    case ColumnType::u8:
        return "C";
    case ColumnType::u32:
        return "I";
    case ColumnType::u64:
        return "L";
    case ColumnType::i64:
        return "l";
    case ColumnType::f64:
        return "g";
    }
    return "z";
}

// Automatically exports any Reflected record sequence into Apache Arrow C Data Interface.
template <Reflected S>
inline void export_reflected_records(std::span<const S> records, ArrowArray* out_array,
                                     ArrowSchema* out_schema) {
    const std::size_t n = records.size();
    constexpr std::size_t nf = field_count<S>();

    if (out_schema != nullptr) {
        std::vector<ArrowSchema> children;
        children.reserve(nf);
        for_each_field<S>([&](const auto& fld) {
            using T = std::remove_reference_t<decltype(field_of(*(S*)nullptr, fld))>;
            ArrowSchema sch{};
            detail::init_schema(&sch, arrow_format_of(column_type_of<T>()), std::string(fld.name));
            children.push_back(std::move(sch));
        });
        detail::init_schema(out_schema, "+s", "", std::move(children));
    }

    if (out_array != nullptr) {
        std::vector<ArrowArray> children;
        children.reserve(nf);
        for_each_field<S>([&](const auto& fld) {
            using T = std::remove_reference_t<decltype(field_of(*(S*)nullptr, fld))>;
            using Raw = detail::underlying_or_self_t<T>;
            children.push_back(detail::make_primitive_array_direct<Raw>(n, [&](std::size_t i) {
                if constexpr (std::is_enum_v<T>)
                    return static_cast<Raw>(field_of(records[i], fld));
                else
                    return field_of(records[i], fld);
            }));
        });
        detail::assemble_struct_array(out_array, static_cast<int64_t>(n), std::move(children));
    }
}

} // namespace mog::arrow
