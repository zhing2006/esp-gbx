#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/common/pieces.h"
#include "gbx/gb/mode.h"

namespace gbx::gb {
struct SnapshotState;

// The ROM a state or save belongs to, from its header: 0134-0143 (title,
// manufacturer code, CGB flag), 0147-0149 (type, ROM and RAM size), 014D-014F
// (header and global checksums) and two zero bytes. The cartridge save files
// use the same 24 bytes.
using RomIdentity = std::array<uint8_t, 24>;
RomIdentity rom_identity(std::span<const uint8_t> rom);

// A complete state of a DMG-C, or of a CPU CGB E in either of its modes, at
// an instruction boundary: the CPU, the timeline and its pending events,
// every device, all memory including the cartridge RAM and clock, and the
// renderer's and mixer's state on the other core, so that running on from it
// gives what an uninterrupted run gives. A CGB's adds its WRAM and VRAM
// banks, palette RAM, VRAM DMA, speed and color frames. It holds nothing of
// a backend: no JIT code, register allocation, trace or counter, so either
// backend can restore it. A state restores only into a system of its mode.
//
// System::capture() and System::restore() only copy and run on the owner;
// encode() and decode(), which turn it into the versioned byte format and
// check it, may run on another task. Each step needs the snapshot to itself.
class Snapshot {
public:
    Snapshot();
    ~Snapshot();
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    // Allocates room for a system with `ram_size` bytes of cartridge RAM, in
    // external memory, so that capturing allocates nothing; `mode` is the
    // system's, which max_encoded() sizes for. Allocating again with the
    // same size keeps the buffer.
    bool allocate(size_t ram_size, Mode mode = Mode::dmg);
    // Frees the buffer.
    void release();
    bool allocated() const { return state_ != nullptr; }
    // Holds a state, captured or decoded.
    bool valid() const { return valid_; }
    size_t ram_size() const { return ram_size_; }
    const RomIdentity& identity() const { return identity_; }
    // The mode of the state held, or allocate()'s before there is one.
    Mode mode() const { return mode_; }
    // The last completed frame when the state was taken, row by row: in GB
    // mode kFramePixels shades, 0 lightest, and in the CGB's modes its
    // BGR555 colors, as palette RAM gave them. Empty in the other modes.
    std::span<const uint8_t> frame() const;
    std::span<const uint16_t> color_frame() const;

    // The largest encoding of a state of this size in `mode`, by default
    // allocate()'s, for the buffer that encode() fills; 0 before allocate().
    size_t max_encoded() const { return max_encoded(mode_); }
    size_t max_encoded(Mode mode) const;
    // The size of the encoding of the state held; 0 without one.
    size_t encoded_size() const;
    // Writes the state; returns its size, or 0 when `out` is too small.
    size_t encode(std::span<uint8_t> out) const;
    // Writes the state through `buffer`, handing it to `sink` each time it
    // is full and once at the end; returns its size, or 0 when the sink
    // stopped it or there is no state.
    size_t encode(std::span<uint8_t> buffer, PieceSink sink, void* context) const;
    enum class Status : uint8_t {
        ok,
        unknown,       // not a state
        version,       // another format version, which is never migrated
        system,        // of a system this version does not know, such as a GBA
        damaged,       // fails its length or checksum
        incompatible,  // another cartridge RAM size than allocate() set
    };
    // Checks an encoding without decoding it. `identity`, `ram_size` and
    // `mode`, when given, receive the state's unless it is unknown, of
    // another version or of another system.
    static Status inspect(std::span<const uint8_t> bytes, RomIdentity* identity = nullptr,
                          size_t* ram_size = nullptr, Mode* mode = nullptr);
    // Reads an encoding, of any mode. A failure leaves the snapshot without
    // a state.
    Status decode(std::span<const uint8_t> bytes);
    // inspect() and decode() of an encoding of `size` bytes that `source`
    // gives in order. They stop at the first thing wrong, which can leave
    // bytes the source has not given; decode() checks the checksum only at
    // the end, after it has written the state.
    static Status inspect(size_t size, PieceSource source, void* context, RomIdentity* identity = nullptr,
                          size_t* ram_size = nullptr, Mode* mode = nullptr);
    Status decode(size_t size, PieceSource source, void* context);

private:
    friend class System;
    void* block_ = nullptr;  // One allocation holds both.
    SnapshotState* state_ = nullptr;
    uint8_t* ram_ = nullptr;
    size_t ram_size_ = 0;
    RomIdentity identity_{};
    Mode mode_ = Mode::dmg;
    bool valid_ = false;
};
}  // namespace gbx::gb
