#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/common/pieces.h"
#include "gbx/gba/backup.h"

namespace gbx::gba {
struct SnapshotState;
class RomCache;

// The ROM a state or save belongs to, from its header (GBATEK "GBA
// Cartridge Header"): 0A0h-0AFh (title and game code), 0B0h-0B1h (maker
// code), 0BCh (software version), 0BDh (complement check) and four zero
// bytes. The cartridge save files use the same 24 bytes.
using RomIdentity = std::array<uint8_t, 24>;
RomIdentity rom_identity(RomCache& rom);

// A complete state of a GBA at an instruction boundary: the CPU, the
// timeline and its pending events, every device, all memory including the
// cartridge's save memory, GPIO and clock, the prefetch buffer, and the
// renderer's and mixer's state on the other core, so that running on from
// it gives what an uninterrupted run gives. It holds nothing of a backend:
// no JIT code, register allocation, trace or counter, so either backend can
// restore it.
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
    // Allocates room for any state, the largest save memory included, in
    // external memory, so that capturing allocates nothing.
    bool allocate();
    void release();
    bool allocated() const { return state_ != nullptr; }
    // Holds a state, captured or decoded.
    bool valid() const { return valid_; }
    const RomIdentity& identity() const { return identity_; }
    // The save memory's type and contents in the state.
    BackupType backup_type() const;
    std::span<const uint8_t> backup() const;
    // The frame the renderer held when the state was taken, BGR555 row by
    // row: the last completed frame when taken at the start of VBlank, as
    // the app does. Empty without a state.
    std::span<const uint16_t> frame() const;

    // The largest encoding, for the buffer that encode() fills; 0 before
    // allocate().
    size_t max_encoded() const;
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
        unknown,  // not a state
        system,   // of another system, such as a GB
        version,  // another format version, which is never migrated
        damaged,  // fails its length, checksum or a check of its values
    };
    // Checks an encoding without decoding it. `identity`, when given,
    // receives the state's unless it is unknown, of another system or of
    // another version.
    static Status inspect(std::span<const uint8_t> bytes, RomIdentity* identity = nullptr);
    // Reads an encoding. A failure leaves the snapshot without a state.
    Status decode(std::span<const uint8_t> bytes);
    // inspect() and decode() of an encoding of `size` bytes that `source`
    // gives in order. They stop at the first thing wrong, which can leave
    // bytes the source has not given; decode() checks the checksum only at
    // the end, after it has written the state.
    static Status inspect(size_t size, PieceSource source, void* context, RomIdentity* identity = nullptr);
    Status decode(size_t size, PieceSource source, void* context);

private:
    friend class System;
    void* block_ = nullptr;
    SnapshotState* state_ = nullptr;
    RomIdentity identity_{};
    bool valid_ = false;
};

}  // namespace gbx::gba
