// Columnar binary event log. Schemas are generated from mog::Reflected
// record declarations, so a file can only be written or read through the
// same member-pointer table that defines its layout - drift is impossible
// by construction.
//
// File layout (all integers little-endian):
//   magic       8 bytes  "MOGLOG\x01\x00"
//   u32         stream count
//   per stream: u8 name length, name, u32 field count,
//               per field: u8 column type code, u8 name length, name
//   then any number of row groups:
//               u32 stream index, u32 row count, columns concatenated
//
// Row groups are an IO batching unit only; readers concatenate them, so
// grouping may vary with producer scheduling without affecting content.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Reflect.hpp>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mog {

// Explicit extent: strlen semantics would truncate at the embedded NUL.
inline constexpr std::string_view kLogMagic{"MOGLOG\x01\x00", 8};

struct FieldSpec {
    std::string name;
    ColumnType type;
};

[[nodiscard]] inline std::string schema_fingerprint(std::span<const FieldSpec> fs) {
    std::string s;
    for (const auto& f : fs) {
        s += f.name;
        s += ':';
        s += column_type_name(f.type);
        s += ',';
    }
    return s;
}

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

template <Reflected S, class Fn>
constexpr void write_cols(const S& rec, Fn&& put) {
    for_each_field<S>([&](const auto& f) {
        using T = std::remove_reference_t<decltype(field_of(rec, f))>;
        if constexpr (std::is_enum_v<T>)
            put(static_cast<std::underlying_type_t<T>>(field_of(rec, f)));
        else
            put(field_of(rec, f));
    });
}

template <Reflected S>
constexpr void field_widths(std::vector<std::uint64_t>& out) {
    for_each_field<S>([&](const auto& f) {
        using T = std::remove_reference_t<decltype(field_of(*(S*)nullptr, f))>;
        if constexpr (std::is_enum_v<T>)
            out.push_back(sizeof(std::underlying_type_t<T>));
        else
            out.push_back(sizeof(T));
    });
}

} // namespace detail

class ColumnLogWriter {
public:
    explicit ColumnLogWriter(std::FILE* f) : f_(f) {
        MOG_PRE(f != nullptr);
        static_assert(std::endian::native == std::endian::little);
        std::fwrite(kLogMagic.data(), 1, kLogMagic.size(), f_);
        put_u32(0); // placeholder stream count; patched as streams are added
    }

    void close() {
        if (f_ != nullptr) {
            flush();
            f_ = nullptr;
        }
    }

    ~ColumnLogWriter() { close(); }

    ColumnLogWriter(const ColumnLogWriter&) = delete;
    ColumnLogWriter& operator=(const ColumnLogWriter&) = delete;

    template <Reflected S>
    std::uint32_t add_stream(std::string_view name) {
        MOG_PRE(!streams_sealed_);
        const std::uint32_t id = static_cast<std::uint32_t>(streams_.size());
        streams_.push_back({});
        Stream& st = streams_.back();
        std::fputc(static_cast<int>(name.size()), f_);
        std::fwrite(name.data(), 1, name.size(), f_);
        put_u32(static_cast<std::uint32_t>(field_count<S>()));
        for_each_field<S>([&](const auto& fld) {
            using T = std::remove_reference_t<decltype(field_of(*(S*)nullptr, fld))>;
            const auto ct = column_type_of<T>();
            std::fputc(static_cast<int>(ct), f_);
            std::fputc(static_cast<int>(fld.name.size()), f_);
            std::fwrite(fld.name.data(), 1, fld.name.size(), f_);
            st.specs.push_back(FieldSpec{std::string(fld.name), ct});
        });
        detail::field_widths<S>(st.widths);
        st.col_pending.resize(st.specs.size());
        patch_count();
        return id;
    }

    template <Reflected S>
    void append(std::uint32_t stream, const S& rec) {
        MOG_PRE(stream < streams_.size());
        Stream& st = streams_[stream];
        std::size_t cfi = 0;
        detail::write_cols(rec, [&](const auto& v) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
            auto& col_buf = st.col_pending[cfi++];
            col_buf.insert(col_buf.end(), p, p + sizeof(v));
        });
        ++st.rows;
    }

    // Writes every buffered row as one row group per stream.
    void flush() {
        for (std::uint32_t si = 0; si < streams_.size(); ++si)
            flush_stream(si);
        std::fflush(f_);
    }

    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }
    [[nodiscard]] std::uint64_t rows_written(std::uint32_t stream) const noexcept {
        return streams_[stream].rows_total;
    }

private:
    struct Stream {
        std::vector<FieldSpec> specs;
        std::vector<std::uint64_t> widths;
        std::vector<std::vector<std::uint8_t>> col_pending; // direct columnar buffers
        std::uint32_t rows = 0;
        std::uint64_t rows_total = 0;
    };

    void flush_stream(std::uint32_t si) {
        Stream& st = streams_[si];
        if (st.rows == 0)
            return;
        streams_sealed_ = true;
        put_u32(si);
        put_u32(st.rows);
        for (auto& col_buf : st.col_pending) {
            std::fwrite(col_buf.data(), 1, col_buf.size(), f_);
            bytes_written_ += col_buf.size();
            col_buf.clear();
        }
        st.rows_total += st.rows;
        st.rows = 0;
    }

    void put_u32(std::uint32_t v) {
        std::fwrite(&v, 1, sizeof(v), f_);
        bytes_written_ += sizeof(v);
    }
    void patch_count() {
        const long pos = std::ftell(f_);
        std::fseek(f_, static_cast<long>(kLogMagic.size()), SEEK_SET);
        const auto n = static_cast<std::uint32_t>(streams_.size());
        std::fwrite(&n, 1, sizeof(n), f_);
        std::fseek(f_, pos, SEEK_SET);
    }

    std::FILE* f_;
    std::vector<Stream> streams_;
    std::uint64_t bytes_written_ = 0;
    bool streams_sealed_ = false;
};

class ColumnLogReader {
public:
    explicit ColumnLogReader(std::FILE* f) : f_(f) {
        MOG_PRE(f != nullptr);
        char magic[8];
        if (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, kLogMagic.data(), 8) != 0) {
            error_ = "bad magic";
            return;
        }
        std::uint32_t n = 0;
        if (!get(n)) {
            error_ = "truncated header";
            return;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            StreamMeta sm;
            unsigned char nl = 0;
            if (!std::fread(&nl, 1, 1, f_)) {
                error_ = "truncated";
                return;
            }
            sm.name.resize(static_cast<std::size_t>(nl));
            if (nl && !std::fread(sm.name.data(), 1, nl, f_)) {
                error_ = "truncated";
                return;
            }
            if (!get(sm.field_count)) {
                error_ = "truncated";
                return;
            }
            for (std::uint32_t k = 0; k < sm.field_count; ++k) {
                unsigned char tc = 0, fnl = 0;
                if (!std::fread(&tc, 1, 1, f_) || !std::fread(&fnl, 1, 1, f_)) {
                    error_ = "truncated";
                    return;
                }
                FieldSpec fs{};
                fs.type = static_cast<ColumnType>(tc);
                fs.name.resize(static_cast<std::size_t>(fnl));
                if (fnl && !std::fread(fs.name.data(), 1, fnl, f_)) {
                    error_ = "truncated";
                    return;
                }
                sm.specs.push_back(std::move(fs));
            }
            streams_.push_back(std::move(sm));
        }
        data_begin_ = std::ftell(f_);
    }

    // Rewinds to the first row group so another read pass can start over.
    void seek_to_data() { std::fseek(f_, data_begin_, SEEK_SET); }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t stream_count() const noexcept {
        return static_cast<std::uint32_t>(streams_.size());
    }
    [[nodiscard]] const std::string& stream_name(std::uint32_t i) const noexcept {
        return streams_[i < streams_.size() ? i : 0].name;
    }
    [[nodiscard]] std::span<const FieldSpec> schema(std::uint32_t i) const noexcept {
        return i < streams_.size() ? streams_[i].specs : std::span<const FieldSpec>{};
    }

    // True when S's reflected schema equals the stored one.
    template <Reflected S>
    [[nodiscard]] bool schema_matches(std::uint32_t stream) const {
        std::vector<FieldSpec> want;
        for_each_field<S>([&](const auto& fld) {
            using T = std::remove_reference_t<decltype(field_of(*(S*)nullptr, fld))>;
            want.push_back(FieldSpec{std::string(fld.name), column_type_of<T>()});
        });
        return schema_fingerprint(want) == schema_fingerprint(schema(stream));
    }

    // Reads every row group belonging to `stream`, invoking sink per record.
    // Storage is column-major, so a whole group is slurped and records are
    // assembled by striding the columns.
    template <Reflected S, class Fn>
    bool read_stream(std::uint32_t stream, Fn&& sink) {
        if (!schema_matches<S>(stream))
            return false;
        const std::size_t nf = schema(stream).size();
        const std::vector<FieldSpec> specs(schema(stream).begin(), schema(stream).end());
        std::vector<std::uint64_t> col_widths(nf, 0);
        for (std::size_t i = 0; i < nf; ++i)
            col_widths[i] = width(specs[i].type);
        std::vector<std::uint64_t> col_off(nf, 0);
        std::vector<char> buf;
        while (true) {
            std::uint32_t si = 0, rows = 0;
            if (!get(si) || !get(rows))
                break; // clean EOF
            if (si != stream || rows == 0) {
                const auto sz = static_cast<long>(group_payload(si, rows));
                if (sz > 0 && std::fseek(f_, sz, SEEK_CUR) != 0) {
                    error_ = "truncated group";
                    return false;
                }
                continue;
            }
            const auto total = group_payload(stream, rows);
            buf.resize(static_cast<std::size_t>(total));
            if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), f_) != buf.size()) {
                error_ = "truncated group";
                return false;
            }
            // Column-major layout: column c starts at rows * sum(width[0..c))
            // and holds width[c]-byte elements row after row.
            std::uint64_t acc = 0;
            for (std::size_t i = 0; i < nf; ++i) {
                col_off[i] = acc;
                acc += col_widths[i] * static_cast<std::uint64_t>(rows);
            }
            for (std::uint32_t r = 0; r < rows; ++r) {
                S rec{};
                bool got = true;
                std::size_t cfi = 0;
                for_each_field<S>([&](const auto& fld) {
                    using T = std::remove_reference_t<decltype(field_of(rec, fld))>;
                    using Raw = detail::underlying_or_self_t<T>;
                    const auto w = col_widths[cfi];
                    const auto* src = buf.data() + static_cast<std::size_t>(col_off[cfi]) +
                                      static_cast<std::size_t>(static_cast<std::uint64_t>(r) * w);
                    if (w != sizeof(Raw)) {
                        got = false;
                        return;
                    }
                    Raw v{};
                    std::memcpy(&v, src, sizeof(Raw));
                    if constexpr (std::is_enum_v<T>)
                        field_of(rec, fld) = static_cast<T>(v);
                    else
                        field_of(rec, fld) = v;
                    ++cfi;
                });
                if (!got) {
                    error_ = "field width mismatch";
                    return false;
                }
                sink(rec);
            }
        }
        return true;
    }

    bool skip_group() {
        std::uint32_t si = 0, rows = 0;
        if (!get(si) || !get(rows))
            return false;
        const auto sz = static_cast<long>(group_payload(si, rows));
        return std::fseek(f_, sz, SEEK_CUR) == 0;
    }

    [[nodiscard]] std::uint64_t group_payload(std::uint32_t stream, std::uint32_t rows) const {
        std::uint64_t total = 0;
        for (const auto& fs : schema(stream))
            total += width(fs.type) * rows;
        return total;
    }

private:
    struct StreamMeta {
        std::string name;
        std::uint32_t field_count = 0;
        std::vector<FieldSpec> specs;
    };

    template <class T>
    bool get(T& v) {
        return std::fread(&v, 1, sizeof(T), f_) == sizeof(T);
    }

    [[nodiscard]] static std::uint64_t width(ColumnType t) noexcept {
        switch (t) {
        case ColumnType::u8:
            return 1;
        case ColumnType::u32:
            return 4;
        case ColumnType::u64:
            return 8;
        case ColumnType::i64:
            return 8;
        case ColumnType::f64:
            return 8;
        }
        return 0;
    }

    std::FILE* f_;
    std::vector<StreamMeta> streams_;
    std::string error_;
    long data_begin_ = 0;
};

} // namespace mog
