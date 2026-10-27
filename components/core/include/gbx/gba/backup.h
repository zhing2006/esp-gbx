#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx::gba {

// The cartridge's save memory (GBATEK "GBA Cart Backup IDs" and the
// sections after it).
enum class BackupType : uint8_t {
    unknown,    // Decided by the program's first access, see Backup.
    none,
    sram,       // 32 KiB
    flash64,
    flash128,
    eeprom,     // EEPROM_V without a size: decided by the first bitstream.
    eeprom512,
    eeprom8k,
};

// The type an ID string in the ROM names, scanning word-aligned positions
// as a ROM is read: GBATEK "GBA Cart Backup IDs". The first string found
// wins.
class BackupScanner {
public:
    // Bytes in order from the start of the ROM.
    void feed(std::span<const uint8_t> bytes);
    BackupType type() const { return type_; }
private:
    BackupType type_ = BackupType::unknown;
    // The longest ID is 10 bytes; the last 9 of a piece may start one.
    static constexpr unsigned kLongest = 10;
    uint8_t tail_[kLongest - 1]{};
    unsigned tail_size_ = 0;
    uint64_t offset_ = 0;
    void check(const uint8_t* at, size_t available);
};

// SRAM, Flash and EEPROM, with the type from an ID string, the caller (a
// save file) or the program's first access (product design, "GBA 存档类型识
// 别"): a Flash command at E005555h makes it 64 KiB Flash, a DMA bitstream
// to the EEPROM range an EEPROM sized by the address bits of that first
// stream, any other write to the SRAM area SRAM. Until then the SRAM area
// reads FFh.
//
// Flash IDs: Panasonic 1B32h for 64 KiB and Sanyo 1362h for 128 KiB, both
// of GBATEK's table; neither needs Atmel's 128-byte sector writes nor
// Macronix's extra stop command. Erasing and writing finish at once,
// which GBATEK's timeouts allow; programs poll the data until it is there.
//
// An EEPROM write keeps the chip busy for 108368 cycles (GBATEK "GBA Cart
// Backup EEPROM"); reads return bit 0 = 0 meanwhile.
class Backup {
public:
    Backup();
    ~Backup();
    Backup(const Backup&) = delete;
    Backup& operator=(const Backup&) = delete;
    bool allocate();
    // The type and the ROM's size, which decides where the EEPROM answers.
    void configure(BackupType type, size_t rom_size);
    // Fresh contents (FFh) and command state, for a new cartridge.
    void reset();
    // A save's type and contents, the rest of the type's size erased.
    void load(BackupType type, std::span<const uint8_t> contents);
    // The command state, at power-on; the contents stay.
    void reset_state();
    BackupType type() const { return type_; }
    // A Flash or EEPROM command under way: a sequence started, a bitstream
    // or a read going, or a write the chip is still busy with.
    bool busy(Tick now) const {
        return flash_state_ != FlashState::ready || flash_erase_ || eeprom_count_ || eeprom_read_bit_ < 68 ||
               (eeprom_writing_ && now < eeprom_ready_);
    }
    size_t size() const { return size_of(type_); }
    // The bytes of a type, 0 for one not decided yet.
    static size_t size_of(BackupType type);
    std::span<uint8_t> data() { return {data_, size()}; }
    // The 8 KiB banks of data() the program may have changed since the last
    // call, a bit each.
    uint32_t take_written() { const uint32_t w = written_; written_ = 0; return w; }
    // Every bank, as after the data was replaced.
    void mark_written() { written_ = (1u << ((size() + kBankBytes - 1) / kBankBytes)) - 1; }
    static constexpr size_t kBankBytes = 8192;

    // The SRAM area, E000000h-FFFFFFFh, by byte.
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t value);
    // Whether an address in D000000h-DFFFFFFh reaches the EEPROM: anywhere
    // there with a ROM of 16 MiB or less, from DFFFF00h with a larger one.
    bool eeprom_at(uint32_t address) const {
        return (type_ == BackupType::eeprom || type_ == BackupType::eeprom512 || type_ == BackupType::eeprom8k ||
                type_ == BackupType::unknown) &&
               (address & 0x0f000000) == 0x0d000000 && (rom_size_ <= (16u << 20) || (address & 0xffffff) >= 0xffff00);
    }
    // A DMA of `units` halfwords starts to (write) or from the EEPROM: the
    // first one decides an unknown type and size.
    void eeprom_dma(unsigned units, bool write);
    uint16_t eeprom_read(Tick now);
    void eeprom_write(uint16_t value, Tick now);

private:
    friend struct SnapshotAccess;
    BackupType type_ = BackupType::unknown;
    size_t rom_size_ = 0;
    uint8_t* data_ = nullptr;  // 128 KiB, external memory.
    uint32_t written_ = 0;
    void mark(uint32_t offset) { written_ |= 1u << (offset / kBankBytes); }
    // Flash.
    enum class FlashState : uint8_t { ready, first, second, erase, erase_first, erase_second, write, bank };
    FlashState flash_state_ = FlashState::ready;
    bool flash_id_ = false;
    bool flash_erase_ = false;  // The command after 80h.
    uint8_t flash_bank_ = 0;
    void flash_command(uint32_t offset, uint8_t value);
    // EEPROM.
    unsigned eeprom_bits() const { return type_ == BackupType::eeprom8k ? 14 : 6; }
    uint64_t eeprom_shift_ = 0;   // Bits received since the last field.
    unsigned eeprom_count_ = 0;   // Bits of the bitstream received.
    uint8_t eeprom_command_ = 0;
    unsigned eeprom_read_bit_ = 68;  // Bits of a read sent, 68 = none pending.
    uint32_t eeprom_address_ = 0;
    Tick eeprom_ready_ = 0;
    bool eeprom_writing_ = false;
    uint64_t eeprom_buffer_ = 0;
};

}  // namespace gbx::gba
