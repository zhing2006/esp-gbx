#pragma once

#include <memory>

#include "gbx/common/rom.h"
#include "gbx/gb/rtc.h"
#include "gbx/platform.h"

namespace gbx::gb {
// Controller state belongs to the CPU owner. Registers resolve to stable ROM
// windows and a RAM window when written, never on the instruction read path.
// A selected clock register has no window: its accesses reach read() and
// write_ram(), and `now` is the access time the clock counts up to.
class Cartridge {
public:
    // Whether load() takes a ROM, from its header (0100-014F) and its size,
    // and why not.
    enum class Support : uint8_t { ok, type, size, ram };
    static Support supports(std::span<const uint8_t> header, size_t size);
    // A failed load preserves the current ROM, RAM and controller state.
    bool load(std::span<const uint8_t> bytes);
    // The clock starts from `clock` at time `now`.
    void reset(const RtcState& clock = {}, Tick now = 0);
    bool control(uint16_t address, uint8_t value, Tick now);
    uint8_t read(uint16_t address) const;
    void write_ram(uint16_t address, uint8_t value, Tick now);
    bool has_rtc() const { return has_rtc_; }
    Rtc& rtc() { return rtc_; }
    const uint8_t* rom_window(unsigned slot) const { return rom_.window(rom_bank[slot] * 16384, 16384); }
    uint8_t* ram_window() const { return ram_enabled_ && ram_select_ < 8 + (kind_ == Kind::mbc5 ? 8 : 0) && ram_size_ ? ram_.get() + ram_offset_ : nullptr; }
    std::span<const uint8_t> rom() const { return rom_.bytes(); }
    std::span<const uint8_t> ram() const { return {ram_.get(), ram_size_}; }
    // Fills RAM from a save, up to the shorter of the two.
    void load_ram(std::span<const uint8_t> bytes);
    // A battery keeps the RAM and the clock (Pan Docs, The Cartridge Header, 0147).
    bool battery() const { return battery_; }
    // RAM banks a program may have written since the previous call: bit n
    // for the 8 KiB from n * 8192, set for each bank mapped while RAM was
    // enabled. Writes to an open window need no access of their own, so the
    // owner calls this at frame boundaries and compares only these banks.
    uint32_t take_ram_written() {
        const uint32_t banks = ram_written_;
        ram_written_ = ram_window() ? 1u << (ram_offset_ / 8192) : 0;
        return banks;
    }
    // A program wrote a clock register since the previous call.
    bool take_rtc_written() { const bool written = rtc_written_; rtc_written_ = false; return written; }
    size_t ram_window_size() const { return ram_size_ < 8192 ? ram_size_ : 8192; }
    bool nibble_ram() const { return kind_ == Kind::mbc2; }
    uint16_t ram_bank() const { return ram_window() ? static_cast<uint16_t>(ram_offset_ / 8192) : 0x3ff; }
    uint16_t rom_bank[2]{0, 1};
private:
    friend struct SnapshotAccess;
    enum class Kind : uint8_t { plain, mbc1, mbc2, mbc3, mbc5 };
    struct Layout {
        Kind kind;
        size_t ram_size;
    };
    static Support layout(std::span<const uint8_t> header, size_t size, Layout& out);
    Kind kind_ = Kind::plain;
    Rom rom_;
    struct Release { void operator()(uint8_t* ram) const { platform::free_external(ram); } };
    std::unique_ptr<uint8_t[], Release> ram_;
    size_t ram_size_ = 0, ram_offset_ = 0;
    uint16_t rom_mask_ = 1, bank1_ = 1;
    uint8_t bank2_ = 0, mode_ = 0, ram_select_ = 0;
    bool ram_enabled_ = false, multicart_ = false, rumble_ = false;
    bool wide_ = false;  // MBC30: an eight-bit ROM bank number.
    bool has_rtc_ = false, battery_ = false;
    Rtc rtc_;
    uint32_t ram_written_ = 0;
    bool rtc_written_ = false;
    // The clock register selected at A000-BFFF, or -1.
    int rtc_register() const {
        return has_rtc_ && ram_enabled_ && ram_select_ >= 8 && ram_select_ <= 12 ? ram_select_ - 8 : -1;
    }
    void resolve();
};
}  // namespace gbx::gb
