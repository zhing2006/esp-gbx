#include "slots.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

#include "gbx/files.h"
#include "rom_info.h"

// A slot file, little endian:
//
//     0     8  magic "GBXSLOT\0"
//     8     2  version, 2
//    10     2  header size, 64
//    12     4  flags: bit 0, the board's time below was trusted
//    16     8  the board's time when the state was taken, microseconds
//              since 1970 (platform::wall_time())
//    24    24  the ROM's identity, as in the state, see gb::rom_identity()
//    48     4  cartridge RAM bytes, as in the state
//    52     4  state bytes, n
//    56     1  run mode, as in the state: 0 GB mode, 1 CGB mode, 2 DMG
//              compatibility mode; 3 a GBA's state (gba::Snapshot)
//    57     7  zero
//    64     t  the frame the state was taken on, 160 x 144 pixels: in GB
//              mode shades, four a byte, the leftmost pixel in bits 1:0,
//              5760 bytes; in the CGB's modes BGR555 colors, 46080 bytes.
//              A GBA's is 240 x 160 BGR555 colors, 76800 bytes.
//  64+t     n  the state, see gb::Snapshot, with its own version and check
//  64+t+n      zeros up to the file's size
//  end-4    4  CRC-32 (the zlib polynomial) of everything before it
//
// The header repeats the identity, RAM size and mode so that a slot of
// another ROM or mode can be named without its state. Every slot file of a
// ROM in a run mode has the size of its largest state, so a write overwrites
// the file in place: the file system allocates nothing and writes only the
// file's own sectors. A write cut off leaves its slot failing the check; the
// other slots and the cartridge save are other files. Each slot has one
// file, so such a write loses that slot. The check of the whole file, not
// the state's alone, also covers the header and the thumbnail the list
// shows. A game that can run in GB mode and in color has slots of both sizes
// when PALETTE changed between them; the list reads either whole.
//
// A GBA's states have one size for every game, the largest save memory
// included, so all of its slot files have the same size, about 1 MiB.
namespace gbx::app {
namespace {

using platform::files::Error;

#ifdef GBX_PROFILER
constexpr const char* kTag = "slots";
#endif
constexpr char kMagic[8] = {'G', 'B', 'X', 'S', 'L', 'O', 'T', '\0'};
constexpr uint16_t kVersion = 2;
constexpr size_t kHeader = 64;
constexpr size_t kTrailer = 4;
constexpr size_t kColorFrameBytes = gb::kFramePixels * 2;
constexpr size_t kGbaFrameBytes = gba::kFramePixels * 2;
constexpr uint8_t kGbaByte = 3;
// Where the state starts in a file of `mode`, or a GBA's.
constexpr size_t state_offset(gb::Mode mode, bool gba = false) {
    return kHeader + (gba ? kGbaFrameBytes : mode == gb::Mode::dmg ? gb::kPackedFrameBytes : kColorFrameBytes);
}
constexpr uint8_t mode_byte(gb::Mode mode) { return mode == gb::Mode::dmg ? 0 : mode == gb::Mode::cgb ? 1 : 2; }
constexpr bool mode_of(uint8_t byte, gb::Mode& mode, bool& gba) {
    if (byte > kGbaByte) return false;
    gba = byte == kGbaByte;
    mode = byte == 1 ? gb::Mode::cgb : byte == 2 ? gb::Mode::compat : gb::Mode::dmg;
    return true;
}
constexpr uint32_t kWallTrusted = 1;
// Slots::infos_: the slots, the write under way and the task's scratch.
constexpr int kPending = Slots::kCount;
constexpr int kScratch = Slots::kCount + 1;
constexpr int kInfos = Slots::kCount + 2;

uint32_t get16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }
uint64_t get64(const uint8_t* p) { return get32(p) | uint64_t{get32(p + 4)} << 32; }
void put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
void put64(uint8_t* p, uint64_t v) { put32(p, static_cast<uint32_t>(v)); put32(p + 4, static_cast<uint32_t>(v >> 32)); }

const char* reason(Error e) {
    return e == Error::full ? "the SD card is full." : "the SD card reported an error.";
}

}  // namespace

void state_path(const char* rom_path, int slot, char* out, size_t size) {
    const std::string_view stem = rom_stem(rom_path);
    std::snprintf(out, size, "states/%.*s.%d.state", static_cast<int>(stem.size()), stem.data(), slot);
}

bool Slots::start(Faults& faults, platform::Notification& done) {
    faults_ = &faults;
    done_ = &done;
    infos_ = static_cast<Info*>(platform::allocate_external(sizeof(Info) * kInfos));
    // External memory: four color thumbnails are 180 KiB.
    thumbnails_ = static_cast<Thumbnail*>(platform::allocate_external(sizeof(Thumbnail) * kInfos));
    if (!infos_ || !thumbnails_) return false;
    std::uninitialized_default_construct_n(infos_, kInfos);
    std::uninitialized_default_construct_n(thumbnails_, kInfos);
    return wake_.initialize(1) &&
           task_.start(run_entry, this, 0, "gbx-slots", 6144, 2, platform::StackMemory::external);
}

void Slots::stop() {
    close();
    stopping_.store(true);
    wake_.signal();
    task_.join();
    stopping_.store(false);
    platform::free_external(infos_);
    infos_ = nullptr;
    platform::free_external(thumbnails_);
    thumbnails_ = nullptr;
}

bool Slots::open(const char* rom_path, const gb::System& system) {
    close();
    const size_t ram_size = system.cartridge_ram().size();
    const gb::Mode mode = system.memory.mode();
    if (!snapshot_.allocate(ram_size, mode)) return false;
    gba_ = false;
    mode_ = mode;
    const size_t file_size = state_offset(mode) + snapshot_.max_encoded() + kTrailer;
    // A file of the other kind of mode, for the list: GB mode's, or a CGB's.
    const gb::Mode other = mode == gb::Mode::dmg ? gb::Mode::cgb : gb::Mode::dmg;
    const size_t buffer_size = std::max(file_size, state_offset(other) + snapshot_.max_encoded(other) + kTrailer);
    if (begin(rom_path, gb::rom_identity(system.memory.rom()), file_size, buffer_size)) return true;
    snapshot_.release();
    return false;
}

#if GBX_HAS_GBA
bool Slots::open(const char* rom_path, gba::System& system) {
    close();
    if (!system.memory.rom || !gba_snapshot_.allocate()) return false;
    gba_ = true;
    mode_ = gb::Mode::dmg;
    const size_t file_size = state_offset(mode_, true) + gba_snapshot_.max_encoded() + kTrailer;
    if (begin(rom_path, gba::rom_identity(*system.memory.rom), file_size, file_size)) return true;
    gba_snapshot_.release();
    return false;
}
#endif

// The buffers and paths of the slots, and their listing.
bool Slots::begin(const char* rom_path, const gb::RomIdentity& identity, size_t file_size, size_t buffer_size) {
    auto* file = static_cast<uint8_t*>(platform::allocate_external(buffer_size));
    if (!file) return false;
    const std::string_view path(rom_path);
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    {
        std::lock_guard lock(mutex_);
        for (int slot = 1; slot <= kCount; ++slot) state_path(rom_path, slot, paths_[slot - 1], sizeof paths_[0]);
        std::snprintf(name_, sizeof name_, "%.*s", static_cast<int>(name.size()), name.data());
        identity_ = identity;
        file_ = file;
        file_size_ = file_size;
        buffer_size_ = buffer_size;
        error_[0] = '\0';
        listed_ = false;
        active_ = true;
        work_ = Work::listing;
        requested_ = true;
    }
    generation_.fetch_add(1, std::memory_order_release);
    wake_.signal();
    return true;
}

void Slots::close() {
    // A write under way finishes first: the slot would be left damaged.
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (!active_) return;
            if (work_ == Work::none) {
                active_ = false;
                listed_ = false;
                break;
            }
        }
        platform::sleep_ms(5);
    }
    platform::free_external(file_);
    file_ = nullptr;
    file_size_ = buffer_size_ = 0;
    snapshot_.release();
#if GBX_HAS_GBA
    gba_snapshot_.release();
#endif
    generation_.fetch_add(1, std::memory_order_release);
}

bool Slots::save(int slot, gb::System& system) {
    return reserve(slot) && submit(slot, !gba_ && system.capture(snapshot_));
}

#if GBX_HAS_GBA
bool Slots::save(int slot, gba::System& system) {
    return reserve(slot) && submit(slot, gba_ && system.capture(gba_snapshot_));
}
#endif

// Reserves the snapshot for a save to `slot`, so that it is this task's
// while the state is copied.
bool Slots::reserve(int slot) {
    if (slot < 1 || slot > kCount) return false;
    const char* refused = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!active_) {
            refused = "the slots are not open.";
        } else if (work_ != Work::none) {
            refused = "another SLOT is being written.";
        } else {
            work_ = Work::writing;
            slot_ = slot;
            written_.store(0, std::memory_order_relaxed);
        }
    }
    if (refused) refuse(slot, refused);
    return !refused;
}

// Hands a captured state to the task, which writes it.
bool Slots::submit(int slot, bool captured) {
    const auto wall = platform::wall_time();
    {
        std::lock_guard lock(mutex_);
        if (captured) {
            wall_ = wall;
            requested_ = true;
        } else {
            work_ = Work::none;
        }
    }
    if (!captured) {
        refuse(slot, "the state could not be taken.");
        return false;
    }
    generation_.fetch_add(1, std::memory_order_release);
    wake_.signal();
    return true;
}

// A save() that writes nothing ends as a failed write does.
void Slots::refuse(int slot, const char* why) {
    char name[sizeof name_];
    {
        std::lock_guard lock(mutex_);
        std::snprintf(error_, sizeof error_, "%s", why);
        std::memcpy(name, name_, sizeof name);
    }
    faults_->add("Can't save SLOT %d of %s: %s", slot, name, why);
    writes_.fetch_add(1, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
}

bool Slots::read(int slot) {
    if (slot < 1 || slot > kCount) return false;
    {
        std::lock_guard lock(mutex_);
        if (!active_ || work_ != Work::none) return false;
        work_ = Work::reading;
        slot_ = slot;
        requested_ = true;
    }
    generation_.fetch_add(1, std::memory_order_release);
    wake_.signal();
    return true;
}

Slots::Kind Slots::read_kind() const {
    std::lock_guard lock(mutex_);
    return read_kind_;
}

platform::WallTime Slots::read_wall() const {
    std::lock_guard lock(mutex_);
    return read_wall_;
}

bool Slots::active() const {
    std::lock_guard lock(mutex_);
    return active_;
}

Slots::Work Slots::work() const {
    std::lock_guard lock(mutex_);
    return work_;
}

int Slots::work_slot() const {
    std::lock_guard lock(mutex_);
    return slot_;
}

int Slots::progress() const {
    std::lock_guard lock(mutex_);
    return file_size_ ? static_cast<int>(written_.load(std::memory_order_relaxed) * 100 / file_size_) : 0;
}

bool Slots::info(int slot, Info& out) const {
    std::lock_guard lock(mutex_);
    if (!active_ || !listed_ || slot < 1 || slot > kCount) return false;
    out = pending_ && slot == slot_ ? infos_[kPending] : infos_[slot - 1];
    return true;
}

bool Slots::thumbnail(int slot, Thumbnail& out) const {
    std::lock_guard lock(mutex_);
    if (!active_ || !listed_ || slot < 1 || slot > kCount) return false;
    const int at = pending_ && slot == slot_ ? kPending : slot - 1;
    if (infos_[at].kind != Kind::ok && infos_[at].kind != Kind::mode) return false;
    out = thumbnails_[at];
    return true;
}

void Slots::error(char* out, size_t size) const {
    std::lock_guard lock(mutex_);
    std::snprintf(out, size, "%s", error_);
}

void Slots::rom_title(char (&out)[17]) const {
    std::lock_guard lock(mutex_);
    if (gba_) app::gba_title(std::span<const uint8_t, 12>(identity_.data(), 12), out);
    else app::rom_title(std::span<const uint8_t, 16>(identity_.data(), 16), out);
}

void Slots::run() {
    while (!stopping_.load()) {
        Work work;
        int slot;
        {
            std::lock_guard lock(mutex_);
            work = requested_ ? work_ : Work::none;
            requested_ = false;
            slot = slot_;
        }
        if (work == Work::none) {
            wake_.wait();
            continue;
        }
        if (work == Work::listing) list();
        else if (work == Work::writing) write_slot(slot);
        else read_slot(slot);
        finish(work);
    }
}

void Slots::finish(Work work) {
    {
        std::lock_guard lock(mutex_);
        if (work_ == work) work_ = Work::none;
    }
    generation_.fetch_add(1, std::memory_order_release);
    done_->signal();
}

// The file of `slot` as the list shows it. It stays in file_, so an ok one
// can be decoded from there.
Slots::Kind Slots::examine(int slot, Info& info, Thumbnail& thumbnail) {
    // Field by field: `info = {}` would build an Info on the stack.
    info.kind = Kind::empty;
    info.mode = gb::Mode::dmg;
    info.gba = false;
    info.other_rom = false;
    info.title[0] = '\0';
    info.wall = {};
    char path[kSavePathSize];
    {
        std::lock_guard lock(mutex_);
        std::memcpy(path, paths_[slot - 1], sizeof path);
    }
    const int64_t size = platform::files::size(path);
    if (size == platform::files::kNoFile) return Kind::empty;
    if (size < 0) return Kind::unreadable;
    // A file of this ROM, in either kind of mode, fits the buffer and is
    // read whole. A larger one, of a ROM with more RAM, gives the buffer its
    // header and the start of its state, which say what it is.
    const bool whole = static_cast<size_t>(size) <= buffer_size_;
    if (whole ? !platform::files::read(path, {file_, static_cast<size_t>(size)},
                                       static_cast<size_t>(size) == file_size_ ? &written_ : nullptr)
              : platform::files::read_start(path, {file_, buffer_size_}) < 0)
        return Kind::unreadable;
    const size_t have = std::min<size_t>(static_cast<size_t>(size), buffer_size_);
    const uint8_t* h = file_;
    if (have < kHeader || std::memcmp(h, kMagic, sizeof kMagic) != 0) return Kind::damaged;
    if (get16(h + 8) != kVersion) return Kind::version;
    gb::Mode mode;
    bool gba;
    if (get16(h + 10) != kHeader || !mode_of(h[56], mode, gba) || have < state_offset(mode, gba) + kTrailer)
        return Kind::damaged;
    // A GB's state in a GBA's slots, or the other way round.
    if (gba != gba_) return Kind::system;
    const size_t at = state_offset(mode, gba);
    const size_t state_size = get32(h + 52);
    const std::span<const uint8_t> state(h + at, std::min(state_size, have - at));
    if (!whole) {
        // The state's own header still tells another version or system. A
        // GBA's files all fit the buffer.
        const auto status = gb::Snapshot::inspect(state);
        if (status == gb::Snapshot::Status::version) return Kind::version;
        if (status == gb::Snapshot::Status::system) return Kind::system;
        return get32(h + 48) != snapshot_.ram_size() ? Kind::incompatible : Kind::damaged;
    }
    const size_t file_size = static_cast<size_t>(size);
    if (crc32({h, file_size - kTrailer}) != get32(h + file_size - kTrailer) || state_size > file_size - at - kTrailer)
        return Kind::damaged;
    gb::RomIdentity identity{};
#if GBX_HAS_GBA
    if (gba) {
        switch (gba::Snapshot::inspect(state, &identity)) {
        case gba::Snapshot::Status::ok: break;
        case gba::Snapshot::Status::version: return Kind::version;
        case gba::Snapshot::Status::system: return Kind::system;
        default: return Kind::damaged;
        }
        if (file_size != file_size_) return Kind::damaged;
        info.kind = Kind::ok;
    } else
#endif
    {
        size_t ram_size = 0;
        gb::Mode state_mode;
        switch (gb::Snapshot::inspect(state, &identity, &ram_size, &state_mode)) {
        case gb::Snapshot::Status::ok: break;
        case gb::Snapshot::Status::version: return Kind::version;
        case gb::Snapshot::Status::system: return Kind::system;
        default: return Kind::damaged;
        }
        if (ram_size != snapshot_.ram_size()) return Kind::incompatible;
        if (state_mode != mode) return Kind::damaged;
        // The same ROM in the same mode makes files of the same size.
        if (mode == mode_ && file_size != file_size_) return Kind::damaged;
        info.kind = mode == mode_ ? Kind::ok : Kind::mode;
    }
    info.mode = mode;
    info.gba = gba;
    {
        std::lock_guard lock(mutex_);
        info.other_rom = identity != identity_;
    }
    if (gba) app::gba_title(std::span<const uint8_t, 12>(identity.data(), 12), info.title);
    else app::rom_title(std::span<const uint8_t, 16>(identity.data(), 16), info.title);
    info.wall = {(get32(h + 12) & kWallTrusted) != 0, static_cast<int64_t>(get64(h + 16))};
    thumbnail.mode = mode;
    thumbnail.gba = gba;
    if (!gba && mode == gb::Mode::dmg) {
        std::copy_n(h + kHeader, thumbnail.shades.size(), thumbnail.shades.begin());
    } else {
        // Only the slots of a GBA game take a GBA's states, see above.
        const size_t pixels = GBX_HAS_GBA && gba ? gba::kFramePixels : gb::kFramePixels;
        for (size_t i = 0; i < pixels; ++i) thumbnail.colors[i] = static_cast<uint16_t>(get16(h + kHeader + 2 * i));
    }
    return info.kind;
}

void Slots::list() {
    Info& info = infos_[kScratch];
    for (int slot = 1; slot <= kCount; ++slot) {
        info.kind = examine(slot, info, thumbnails_[kScratch]);
        std::lock_guard lock(mutex_);
        if (!active_) return;
        infos_[slot - 1] = info;
        thumbnails_[slot - 1] = thumbnails_[kScratch];
    }
    std::lock_guard lock(mutex_);
    listed_ = true;
}

void Slots::write_slot(int slot) {
    char path[kSavePathSize];
    platform::WallTime wall;
    gb::RomIdentity identity;
    {
        std::lock_guard lock(mutex_);
        std::memcpy(path, paths_[slot - 1], sizeof path);
        wall = wall_;
        identity = identity_;
    }
#ifdef GBX_PROFILER
    const uint64_t start = platform::monotonic_us();
#endif
    uint8_t* h = file_;
    const size_t at = state_offset(mode_, gba_);
    std::memset(h, 0, file_size_);
    std::memcpy(h, kMagic, sizeof kMagic);
    put16(h + 8, kVersion);
    put16(h + 10, kHeader);
    put32(h + 12, wall.trusted ? kWallTrusted : 0);
    put64(h + 16, static_cast<uint64_t>(wall.us));
#if GBX_HAS_GBA
    const auto& state_identity = gba_ ? gba_snapshot_.identity() : snapshot_.identity();
    std::copy(state_identity.begin(), state_identity.end(), h + 24);
    put32(h + 48, static_cast<uint32_t>(gba_ ? gba_snapshot_.backup().size() : snapshot_.ram_size()));
#else
    std::copy(snapshot_.identity().begin(), snapshot_.identity().end(), h + 24);
    put32(h + 48, static_cast<uint32_t>(snapshot_.ram_size()));
#endif
    h[56] = gba_ ? kGbaByte : mode_byte(mode_);
    Thumbnail& thumbnail = thumbnails_[kScratch];
    thumbnail.mode = mode_;
    thumbnail.gba = gba_;
#if GBX_HAS_GBA
    if (gba_) {
        const auto frame = gba_snapshot_.frame();
        for (size_t i = 0; i < frame.size(); ++i) put16(h + kHeader + 2 * i, frame[i]);
        std::copy(frame.begin(), frame.end(), thumbnail.colors.begin());
    } else
#endif
    if (mode_ == gb::Mode::dmg) {
        const auto frame = snapshot_.frame();
        for (size_t i = 0; i + 3 < frame.size(); i += 4) {
            h[kHeader + i / 4] = static_cast<uint8_t>((frame[i] & 3) | (frame[i + 1] & 3) << 2 |
                                                      (frame[i + 2] & 3) << 4 | (frame[i + 3] & 3) << 6);
        }
        std::copy_n(h + kHeader, thumbnail.shades.size(), thumbnail.shades.begin());
    } else {
        const auto frame = snapshot_.color_frame();
        for (size_t i = 0; i < frame.size(); ++i) put16(h + kHeader + 2 * i, frame[i]);
        std::copy(frame.begin(), frame.end(), thumbnail.colors.begin());
    }
    {
        // The list shows the state being written in its slot.
        std::lock_guard lock(mutex_);
        Info& info = infos_[kPending];
        info.kind = Kind::ok;
        info.mode = mode_;
        info.gba = gba_;
        info.other_rom = false;
        if (gba_) app::gba_title(std::span<const uint8_t, 12>(identity.data(), 12), info.title);
        else app::rom_title(std::span<const uint8_t, 16>(identity.data(), 16), info.title);
        info.wall = wall;
        thumbnails_[kPending] = thumbnail;
        pending_ = true;
    }
    generation_.fetch_add(1, std::memory_order_release);
    const std::span<uint8_t> room(h + at, file_size_ - at - kTrailer);
#if GBX_HAS_GBA
    const size_t state_size = gba_ ? gba_snapshot_.encode(room) : snapshot_.encode(room);
#else
    const size_t state_size = snapshot_.encode(room);
#endif
    put32(h + 52, static_cast<uint32_t>(state_size));
    put32(h + file_size_ - kTrailer, crc32({h, file_size_ - kTrailer}));
#ifdef GBX_PROFILER
    const uint64_t encoded = platform::monotonic_us();
#endif
    Error error = state_size ? Error::none : Error::failed;
    if (error == Error::none) error = platform::files::make_parents(path);
    if (error == Error::none) error = platform::files::write(path, {file_, file_size_}, &written_);
    if (error == Error::none) {
        {
            std::lock_guard lock(mutex_);
            infos_[slot - 1] = infos_[kPending];
            thumbnails_[slot - 1] = thumbnails_[kPending];
            pending_ = false;
            error_[0] = '\0';
        }
        writes_.fetch_add(1, std::memory_order_release);
#ifdef GBX_PROFILER
        platform::log(platform::LogLevel::warn, kTag, "wrote SLOT %d: state %u bytes, encode %llu us, write %llu ms",
                      slot, static_cast<unsigned>(state_size), static_cast<unsigned long long>(encoded - start),
                      static_cast<unsigned long long>((platform::monotonic_us() - encoded) / 1000));
#endif
        return;
    }
    char name[sizeof name_];
    {
        std::lock_guard lock(mutex_);
        std::snprintf(error_, sizeof error_, "%s", reason(error));
        std::memcpy(name, name_, sizeof name);
    }
    faults_->add("Can't write SLOT %d of %s: %s", slot, name, reason(error));
    // The file may be half written; the list shows what is there now.
    Info& info = infos_[kScratch];
    info.kind = examine(slot, info, thumbnails_[kScratch]);
    {
        std::lock_guard lock(mutex_);
        infos_[slot - 1] = info;
        thumbnails_[slot - 1] = thumbnails_[kScratch];
        pending_ = false;
    }
    writes_.fetch_add(1, std::memory_order_release);
}

void Slots::read_slot(int slot) {
    written_.store(0, std::memory_order_relaxed);
#ifdef GBX_PROFILER
    const uint64_t start = platform::monotonic_us();
#endif
    Info& info = infos_[kScratch];
    Kind kind = examine(slot, info, thumbnails_[kScratch]);
#ifdef GBX_PROFILER
    const uint64_t read = platform::monotonic_us();
#endif
    if (kind == Kind::ok) {
        const std::span<const uint8_t> state(file_ + state_offset(mode_, gba_), get32(file_ + 52));
#if GBX_HAS_GBA
        const bool decoded = gba_ ? gba_snapshot_.decode(state) == gba::Snapshot::Status::ok
                                  : snapshot_.decode(state) == gb::Snapshot::Status::ok;
#else
        const bool decoded = snapshot_.decode(state) == gb::Snapshot::Status::ok;
#endif
        if (!decoded) kind = Kind::damaged;
    }
#ifdef GBX_PROFILER
    platform::log(platform::LogLevel::warn, kTag, "read SLOT %d: kind %d, read %llu ms, decode %llu us", slot,
                  static_cast<int>(kind), static_cast<unsigned long long>((read - start) / 1000),
                  static_cast<unsigned long long>(platform::monotonic_us() - read));
#endif
    {
        std::lock_guard lock(mutex_);
        read_kind_ = kind;
        read_wall_ = info.wall;
        // The list follows what the card holds now.
        if (kind != Kind::ok) {
            info.kind = kind;
            infos_[slot - 1] = info;
            thumbnails_[slot - 1] = thumbnails_[kScratch];
        }
    }
}

}  // namespace gbx::app
