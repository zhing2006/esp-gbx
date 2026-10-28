#pragma once

// The byte encoding of the GB's and the GBA's states, see gb/snapshot.cpp:
// little-endian integers written to and read from pieces, with a running
// CRC-32 of everything before the check at the end.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "gbx/common/pieces.h"

namespace gbx::encoding {

constexpr std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}();
// CRC-32 with the zlib polynomial, going on from `crc`, the value of the
// bytes before, as zlib's crc32() does.
inline uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0) {
    uint32_t c = crc ^ 0xffffffffu;
    for (uint8_t b : data) c = kCrcTable[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
inline uint32_t get16(const uint8_t* p) { return p[0] | p[1] << 8; }
inline uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }
inline void put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

template<class T> constexpr size_t width() {
    if constexpr (std::is_enum_v<T>) return 1;
    else return sizeof(T);
}

// Writes an encoding into `buffer`, handing the buffer to the sink each time
// it is full and once at the end; without a sink the whole encoding has to
// fit. finish() appends the check.
class Output {
public:
    Output(std::span<uint8_t> buffer, PieceSink sink, void* context)
        : buffer_(buffer), sink_(sink), context_(context) {}
    template<class T> void operator()(const T& value) {
        uint64_t v;
        if constexpr (std::is_enum_v<T>) v = static_cast<uint8_t>(value);
        else if constexpr (std::is_same_v<T, bool>) v = value;
        else v = static_cast<std::make_unsigned_t<T>>(value);
        for (size_t i = 0; i < width<T>(); ++i) put(static_cast<uint8_t>(v >> (i * 8)));
    }
    void bytes(std::span<const uint8_t> data) {
        while (!data.empty()) {
            if (at_ == buffer_.size() && !flush(false)) return;
            const size_t n = std::min(data.size(), buffer_.size() - at_);
            std::memcpy(buffer_.data() + at_, data.data(), n);
            at_ += n;
            written_ += n;
            data = data.subspan(n);
        }
    }
    void words(std::span<const uint16_t> data) {
        for (uint16_t w : data) { put(static_cast<uint8_t>(w)); put(static_cast<uint8_t>(w >> 8)); }
    }
    bool present(bool flag) const { return flag; }
    size_t limit(size_t count, size_t) const { return count; }
    // The bytes written so far.
    size_t written() const { return written_; }
    // Appends the check of everything before it and hands over the rest.
    // False when the sink stopped the encoding or it did not fit.
    bool finish() {
        crc_ = crc32(buffer_.subspan(folded_, at_ - folded_), crc_);
        folded_ = at_;
        sealed_ = true;
        uint8_t check[4];
        put32(check, crc_);
        bytes(check);
        return flush(true);
    }

protected:
    void put(uint8_t byte) {
        if (at_ == buffer_.size() && !flush(false)) return;
        buffer_[at_++] = byte;
        ++written_;
    }

private:
    // Hands over the buffer; `last` at the end, where a buffer without a
    // sink holds the whole encoding.
    bool flush(bool last) {
        if (!ok_ || buffer_.empty()) return ok_ = false;
        if (!sealed_) crc_ = crc32(buffer_.subspan(folded_, at_ - folded_), crc_);
        folded_ = at_;
        if (!sink_) return ok_ = last;
        ok_ = at_ == 0 || sink_(context_, buffer_.first(at_));
        at_ = folded_ = 0;
        return ok_;
    }

    std::span<uint8_t> buffer_;
    PieceSink sink_;
    void* context_;
    size_t at_ = 0;      // in the buffer
    size_t folded_ = 0;  // of the buffer, in crc_
    size_t written_ = 0;
    uint32_t crc_ = 0;
    bool sealed_ = false;  // crc_ is complete
    bool ok_ = true;
};

// Reads an encoding of `size` bytes from a source, keeping the check of what
// it has read. After the first thing wrong it reads no more.
class Input {
public:
    Input(PieceSource source, void* context, size_t size) : source_(source), context_(context), left_(size) {}
    template<class T> void operator()(T& value) {
        uint8_t b[8];
        if (!take(b, width<T>())) { value = T{}; return; }
        uint64_t v = 0;
        for (size_t i = 0; i < width<T>(); ++i) v |= uint64_t{b[i]} << (i * 8);
        if constexpr (std::is_same_v<T, bool>) {
            ok_ &= v <= 1;
            value = v;
        } else value = static_cast<T>(v);
    }
    void bytes(std::span<uint8_t> data) { take(data.data(), data.size()); }
    void words(std::span<uint16_t> data) {
        for (auto& w : data) {
            uint8_t b[2];
            if (!take(b, 2)) return;
            w = static_cast<uint16_t>(b[0] | b[1] << 8);
        }
    }
    bool present(bool flag) const { return flag; }
    // A count beyond its array makes the state invalid; read no more.
    size_t limit(size_t count, size_t capacity) {
        if (count > capacity) { ok_ = false; return 0; }
        return count;
    }
    // Reads `n` bytes into `out`, or with no `out` only for the check.
    bool take(uint8_t* out, size_t n) {
        while (ok_ && n) {
            if (piece_.empty() && !next()) break;
            const size_t k = std::min(n, piece_.size());
            if (out) {
                std::memcpy(out, piece_.data(), k);
                out += k;
            }
            crc_ = crc32(piece_.first(k), crc_);
            piece_ = piece_.subspan(k);
            at_ += k;
            n -= k;
        }
        return ok_;
    }
    // Reads the check at the end: true when it is the check of everything
    // before it and the encoding ends there.
    bool check() {
        const uint32_t crc = crc_;
        uint8_t stored[4];
        return take(stored, sizeof stored) && get32(stored) == crc && piece_.empty() && left_ == 0;
    }
    bool ok() const { return ok_; }
    size_t position() const { return at_; }

protected:
    void fail() { ok_ = false; }

private:
    bool next() {
        piece_ = left_ ? source_(context_, left_) : std::span<const uint8_t>{};
        if (piece_.empty() || piece_.size() > left_) {
            piece_ = {};
            return ok_ = false;
        }
        left_ -= piece_.size();
        return true;
    }

    PieceSource source_;
    void* context_;
    std::span<const uint8_t> piece_;
    size_t left_;  // what the source has still to give
    size_t at_ = 0;
    uint32_t crc_ = 0;
    bool ok_ = true;
};

// A source of the bytes of a span, for an encoding held whole.
struct Whole {
    std::span<const uint8_t> rest;
    static std::span<const uint8_t> next(void* context, size_t most) {
        auto& whole = *static_cast<Whole*>(context);
        const auto piece = whole.rest.first(std::min(most, whole.rest.size()));
        whole.rest = whole.rest.subspan(piece.size());
        return piece;
    }
};

}  // namespace gbx::encoding
