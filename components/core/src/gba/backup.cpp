#include "gbx/gba/backup.h"

#include <algorithm>
#include <cstring>

#include "gbx/platform.h"

namespace gbx::gba {
namespace {

constexpr size_t kStorage = 128 * 1024;
// GBATEK "GBA Cart Backup EEPROM": about 108368 cycles from the end of a
// write's bitstream until the chip is ready.
constexpr Tick kEepromWrite = 108368;
// Manufacturer and device IDs, see Backup.
constexpr uint8_t kPanasonic[2] = {0x32, 0x1b}, kSanyo[2] = {0x62, 0x13};

}  // namespace

// --- BackupScanner -------------------------------------------------------------

// A string at `at`, of which `available` bytes are there.
void BackupScanner::check(const uint8_t* at, size_t available) {
    struct Id { const char* text; BackupType type; };
    static constexpr Id kIds[] = {{"EEPROM_V", BackupType::eeprom}, {"SRAM_V", BackupType::sram},
                                  {"FLASH1M_V", BackupType::flash128}, {"FLASH512_V", BackupType::flash64},
                                  {"FLASH_V", BackupType::flash64}};
    for (const auto& id : kIds) {
        const size_t n = std::strlen(id.text);
        if (n <= available && std::memcmp(at, id.text, n) == 0) {
            type_ = id.type;
            return;
        }
    }
}

void BackupScanner::feed(std::span<const uint8_t> bytes) {
    if (type_ != BackupType::unknown || bytes.empty()) return;
    // Strings that start in the kept tail and end in this piece.
    if (tail_size_) {
        uint8_t joined[sizeof tail_ + kLongest];
        const size_t extra = std::min<size_t>(bytes.size(), kLongest);
        std::memcpy(joined, tail_, tail_size_);
        std::memcpy(joined + tail_size_, bytes.data(), extra);
        const uint64_t base = offset_ - tail_size_;
        for (size_t i = 0; i < tail_size_ && type_ == BackupType::unknown; ++i)
            if (((base + i) & 3) == 0) check(joined + i, tail_size_ + extra - i);
    }
    for (size_t i = (4 - (offset_ & 3)) & 3; i < bytes.size() && type_ == BackupType::unknown; i += 4)
        check(bytes.data() + i, bytes.size() - i);
    // The last bytes, where a string may continue into the next piece.
    uint8_t kept[sizeof tail_ + kLongest];
    std::memcpy(kept, tail_, tail_size_);
    const size_t take = std::min<size_t>(bytes.size(), sizeof tail_);
    std::memcpy(kept + tail_size_, bytes.data() + bytes.size() - take, take);
    const size_t total = tail_size_ + take;
    tail_size_ = static_cast<unsigned>(std::min<size_t>(total, sizeof tail_));
    std::memcpy(tail_, kept + total - tail_size_, tail_size_);
    offset_ += bytes.size();
}

// --- Backup ---------------------------------------------------------------------

Backup::Backup() = default;
Backup::~Backup() { platform::free_external(data_); }

bool Backup::allocate() {
    if (!data_) data_ = static_cast<uint8_t*>(platform::allocate_external(kStorage));
    return data_ != nullptr;
}

void Backup::configure(BackupType type, size_t rom_size) {
    type_ = type;
    rom_size_ = rom_size;
}

size_t Backup::size_of(BackupType type) {
    switch (type) {
    case BackupType::sram: return 32 * 1024;
    case BackupType::flash64: return 64 * 1024;
    case BackupType::flash128: return 128 * 1024;
    case BackupType::eeprom512: return 512;
    case BackupType::eeprom8k: return 8 * 1024;
    default: return 0;
    }
}

void Backup::reset() {
    if (data_) std::memset(data_, 0xff, kStorage);
    written_ = 0;
    reset_state();
}

void Backup::load(BackupType type, std::span<const uint8_t> contents) {
    type_ = type;
    std::memset(data_, 0xff, kStorage);
    std::memcpy(data_, contents.data(), std::min(contents.size(), size()));
    written_ = 0;
}

void Backup::reset_state() {
    flash_state_ = FlashState::ready;
    flash_id_ = flash_erase_ = false;
    flash_bank_ = 0;
    eeprom_shift_ = 0;
    eeprom_count_ = 0;
    eeprom_read_bit_ = 68;
    eeprom_address_ = 0;
    eeprom_ready_ = 0;
    eeprom_writing_ = false;
    eeprom_buffer_ = 0;
}

uint8_t Backup::read(uint32_t address) {
    switch (type_) {
    case BackupType::sram: return data_[address & 0x7fff];
    case BackupType::flash64:
    case BackupType::flash128: {
        const uint32_t offset = address & 0xffff;
        if (flash_id_ && offset < 2) return (type_ == BackupType::flash64 ? kPanasonic : kSanyo)[offset];
        return data_[(flash_bank_ << 16) | offset];
    }
    default: return 0xff;
    }
}

void Backup::write(uint32_t address, uint8_t value) {
    const uint32_t offset = address & 0xffff;
    if (type_ == BackupType::unknown) {
        // The first write decides between Flash and SRAM.
        type_ = offset == 0x5555 && value == 0xaa ? BackupType::flash64 : BackupType::sram;
    }
    switch (type_) {
    case BackupType::sram:
        data_[address & 0x7fff] = value;
        mark(address & 0x7fff);
        return;
    case BackupType::flash64:
    case BackupType::flash128: flash_command(offset, value); return;
    default: return;
    }
}

// GBATEK "GBA Cart Backup Flash ROM": commands are AAh to 5555h, 55h to
// 2AAAh, then the command to 5555h, or for erase, a second such sequence.
void Backup::flash_command(uint32_t offset, uint8_t value) {
    switch (flash_state_) {
    case FlashState::write:
        // Programming clears bits; an erased byte takes any value.
        data_[(flash_bank_ << 16) | offset] &= value;
        mark((flash_bank_ << 16) | offset);
        flash_state_ = FlashState::ready;
        return;
    case FlashState::bank:
        if (offset == 0 && type_ == BackupType::flash128) flash_bank_ = value & 1;
        flash_state_ = FlashState::ready;
        return;
    case FlashState::ready:
        flash_state_ = offset == 0x5555 && value == 0xaa ? FlashState::first : FlashState::ready;
        return;
    case FlashState::first:
        flash_state_ = offset == 0x2aaa && value == 0x55 ? FlashState::second : FlashState::ready;
        return;
    case FlashState::second:
        break;
    default:
        flash_state_ = FlashState::ready;
        return;
    }
    flash_state_ = FlashState::ready;
    if (flash_erase_) {
        flash_erase_ = false;
        if (offset == 0x5555 && value == 0x10) {
            std::memset(data_, 0xff, size());
            mark_written();
        } else if (value == 0x30) {
            std::memset(data_ + ((flash_bank_ << 16) | (offset & 0xf000)), 0xff, 0x1000);
            mark((flash_bank_ << 16) | (offset & 0xf000));
        }
        return;
    }
    if (offset != 0x5555) return;
    switch (value) {
    case 0x90: flash_id_ = true; break;
    case 0xf0: flash_id_ = false; break;
    case 0x80: flash_erase_ = true; break;
    case 0xa0: flash_state_ = FlashState::write; break;
    case 0xb0: flash_state_ = FlashState::bank; break;
    default: break;
    }
}

// A read request is 2 + n + 1 bits, a write 2 + n + 64 + 1, with n = 6 or
// 14 address bits (GBATEK "GBA Cart Backup EEPROM").
void Backup::eeprom_dma(unsigned units, bool write) {
    if (type_ == BackupType::unknown) type_ = BackupType::eeprom;
    if (type_ != BackupType::eeprom || !write) return;
    if (units == 9 || units == 73) type_ = BackupType::eeprom512;
    else if (units == 17 || units == 81) type_ = BackupType::eeprom8k;
}

uint16_t Backup::eeprom_read(Tick now) {
    if (eeprom_read_bit_ < 68) {
        const unsigned bit = eeprom_read_bit_++;
        if (bit < 4) return 0;
        return static_cast<uint16_t>((eeprom_buffer_ >> (63 - (bit - 4))) & 1);
    }
    return eeprom_writing_ && now < eeprom_ready_ ? 0 : 1;
}

void Backup::eeprom_write(uint16_t value, Tick now) {
    if (type_ != BackupType::eeprom512 && type_ != BackupType::eeprom8k) return;
    const unsigned n = eeprom_bits();
    eeprom_shift_ = (eeprom_shift_ << 1) | (value & 1);
    ++eeprom_count_;
    if (eeprom_count_ == 2) {
        eeprom_command_ = static_cast<uint8_t>(eeprom_shift_ & 3);
        eeprom_shift_ = 0;
        // Neither a read (11) nor a write (10): start over.
        if (eeprom_command_ < 2) eeprom_count_ = 0;
        return;
    }
    if (eeprom_count_ == 2 + n) {
        // An 8 KiB chip uses the low 10 of its 14 address bits.
        eeprom_address_ = static_cast<uint32_t>(eeprom_shift_) & static_cast<uint32_t>(size() / 8 - 1);
        eeprom_shift_ = 0;
        return;
    }
    if (eeprom_command_ == 3 && eeprom_count_ == 2 + n + 1) {
        uint64_t data = 0;
        for (unsigned i = 0; i < 8; ++i) data = (data << 8) | data_[eeprom_address_ * 8 + i];
        eeprom_buffer_ = data;
        eeprom_read_bit_ = 0;
        eeprom_count_ = 0;
        return;
    }
    if (eeprom_command_ == 2 && eeprom_count_ == 2 + n + 64) {
        eeprom_buffer_ = eeprom_shift_;
        return;
    }
    if (eeprom_command_ == 2 && eeprom_count_ == 2 + n + 65) {
        for (unsigned i = 0; i < 8; ++i)
            data_[eeprom_address_ * 8 + i] = static_cast<uint8_t>(eeprom_buffer_ >> (56 - 8 * i));
        mark(eeprom_address_ * 8);
        eeprom_writing_ = true;
        eeprom_ready_ = now + kEepromWrite;
        eeprom_count_ = 0;
    }
}

}  // namespace gbx::gba
