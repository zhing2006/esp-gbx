#include "library.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "gbx/files.h"
#include "gbx/platform.h"

namespace gbx::app {
namespace {

constexpr const char* kTag = "library";
constexpr size_t kPath = platform::files::kMaxPath;
// A batch: the scan publishes after this many ROM headers or this long.
constexpr int kBatchRoms = 32;
constexpr uint64_t kBatchUs = 250'000;

char fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

// Case is ignored for ASCII letters and digit runs compare as numbers, so
// "Game 2" comes before "Game 10". Names equal that way keep a fixed order.
int natural_compare(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            size_t ei = i, ej = j;
            while (ei < a.size() && digit(a[ei])) ++ei;
            while (ej < b.size() && digit(b[ej])) ++ej;
            if (ei - i != ej - j) return ei - i < ej - j ? -1 : 1;
            if (const int c = a.substr(i, ei - i).compare(b.substr(j, ej - j))) return c;
            i = ei;
            j = ej;
            continue;
        }
        const char ca = fold(a[i]), cb = fold(b[j]);
        if (ca != cb) return static_cast<uint8_t>(ca) < static_cast<uint8_t>(cb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size()) return i < a.size() ? 1 : -1;
    return a.compare(b);
}

// .gb, .gbc and .gba in any case; the header decides the machine.
bool rom_name(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    char ext[4] = {};
    const std::string_view e = name.substr(dot + 1);
    if (e.size() < 2 || e.size() > 3) return false;
    for (size_t i = 0; i < e.size(); ++i) ext[i] = fold(e[i]);
    const std::string_view x(ext, e.size());
    return x == "gb" || x == "gbc" || x == "gba";
}

// FNV-1a of a name with ASCII case folded, for matching saves to ROMs.
uint32_t name_hash(std::string_view t) {
    uint32_t h = 2166136261u;
    for (char c : t) h = (h ^ static_cast<uint8_t>(fold(c))) * 16777619u;
    return h;
}

}  // namespace

bool Library::start() {
    entries_ = static_cast<Entry*>(platform::allocate_external(sizeof(Entry) * kMaxEntries));
    names_ = static_cast<char*>(platform::allocate_external(kNamePool));
    if (!entries_ || !names_) {
        platform::log(platform::LogLevel::error, kTag, "no memory for the ROM list");
        return false;
    }
    return wake_.initialize(1) &&
           task_.start(run_entry, this, 0, "gbx-library", 6144, 2, platform::StackMemory::external);
}

void Library::stop() {
    stopping_.store(true);
    wake_.signal();
    task_.join();
    platform::free_external(entries_);
    platform::free_external(names_);
    entries_ = nullptr;
    names_ = nullptr;
    count_ = 0;
    stopping_.store(false);
    state_.store(State::idle);
}

void Library::scan() {
    const State s = state();
    if (s == State::idle || s == State::no_card) {
        requested_.store(true);
        wake_.signal();
    }
}

uint32_t Library::roms() const {
    std::lock_guard lock(mutex_);
    return roms_;
}

void Library::run() {
    while (!stopping_.load()) {
        wake_.wait();
        if (requested_.exchange(false) && !stopping_.load()) scan_all();
    }
}

void Library::scan_all() {
    {
        std::lock_guard lock(mutex_);
        count_ = 0;
        roms_ = 0;
        names_used_ = 0;
        full_.store(false);
        generation_.fetch_add(1, std::memory_order_release);
    }
    state_.store(State::scanning, std::memory_order_release);
    if (!platform::files::mount()) {
        platform::log(platform::LogLevel::warn, kTag, "no card");
        state_.store(State::no_card, std::memory_order_release);
        return;
    }
    const uint64_t start = platform::monotonic_us();
    scan_folder(kRoot);
    // Breadth first: the folders found are scanned in the order found. Only
    // this task changes count_.
    int folders = 1;
    for (int i = 0; i < count_ && !stopping_.load(); ++i) {
        if (entries_[i].directory) {
            scan_folder(i);
            ++folders;
        }
    }
    state_.store(State::done, std::memory_order_release);
    platform::log(platform::LogLevel::info, kTag, "%u ROMs in %d folders, %llu ms%s", static_cast<unsigned>(roms()),
                  folders, static_cast<unsigned long long>((platform::monotonic_us() - start) / 1000),
                  full() ? ", list full" : "");
}

void Library::scan_folder(int folder) {
    char path[kPath];
    {
        std::lock_guard lock(mutex_);
        path_locked(folder, path, sizeof path);
    }
    platform::files::Directory dir;
    if (!dir.open(path)) return;
    // Names first, closing the folder before the headers are read.
    const int first = count_;
    int end = first;
    platform::files::Entry item;
    while (dir.next(item)) {
        // Hidden names, such as the ._ files macOS writes on FAT cards.
        if (item.name[0] == '.' || (!item.directory && !rom_name(item.name))) continue;
        const size_t size = std::strlen(item.name);
        // A name whose path does not fit is left out, as a cut path names no
        // file. The folder's own path fits, or it would not be listed.
        if (std::strlen(path) + 1 + size >= kPath) continue;
        if (end == kMaxEntries || names_used_ + size > kNamePool) {
            full_.store(true);
            break;
        }
        Entry& e = entries_[end++];
        e = {};
        e.parent = folder;
        e.name = static_cast<uint32_t>(names_used_);
        e.name_size = static_cast<uint16_t>(size);
        e.directory = item.directory;
        std::memcpy(names_ + names_used_, item.name, size);
        names_used_ += size;
    }
    dir.close();
    if (end == first) return;

    // saves/ mirrors roms/: a ROM has a save when <stem>.0.sav or .1.sav is there.
    std::vector<uint32_t> saved;
    char saves[kPath + 1];
    std::snprintf(saves, sizeof saves, "saves%s", path + 4);
    if (dir.open(saves)) {
        while (dir.next(item)) {
            const std::string_view n(item.name);
            if (!item.directory && n.size() > 6 && (n.ends_with(".0.sav") || n.ends_with(".1.sav"))) {
                saved.push_back(name_hash(n.substr(0, n.size() - 6)));
            }
        }
        dir.close();
        std::sort(saved.begin(), saved.end());
    }

    const int length = static_cast<int>(std::strlen(path));
    std::array<uint8_t, kRomHeaderBytes> header;
    int published = first, batch = 0;
    uint64_t batch_start = platform::monotonic_us();
    for (int i = first; i < end && !stopping_.load(); ++i) {
        Entry& e = entries_[i];
        if (e.directory) continue;
        const std::string_view n(names_ + e.name, e.name_size);
        std::snprintf(path + length, sizeof path - length, "/%.*s", static_cast<int>(n.size()), n.data());
        const int64_t size = platform::files::read_start(path, header);
        const size_t got = size < 0 ? 0 : static_cast<size_t>(std::min<int64_t>(size, header.size()));
        e.info = parse_rom(std::span(header).first(got), size);
        e.save = std::binary_search(saved.begin(), saved.end(), name_hash(n.substr(0, n.rfind('.'))));
        if (++batch >= kBatchRoms || platform::monotonic_us() - batch_start >= kBatchUs) {
            publish(i + 1);
            published = i + 1;
            batch = 0;
            batch_start = platform::monotonic_us();
        }
    }
    path[length] = '\0';
    if (published < end) publish(end);
}

void Library::publish(int end) {
    std::lock_guard lock(mutex_);
    for (int i = count_; i < end; ++i) {
        if (entries_[i].directory) continue;
        ++roms_;
        for (int p = entries_[i].parent; p != kRoot; p = entries_[p].parent) ++entries_[p].roms;
    }
    count_ = end;
    generation_.fetch_add(1, std::memory_order_release);
}

int Library::list(int folder, std::span<int32_t> out) const {
    std::lock_guard lock(mutex_);
    int n = 0;
    for (int i = 0; i < count_ && n < static_cast<int>(out.size()); ++i) {
        if (entries_[i].parent == folder) out[n++] = i;
    }
    std::sort(out.begin(), out.begin() + n, [this](int32_t a, int32_t b) {
        const Entry& x = entries_[a];
        const Entry& y = entries_[b];
        if (x.directory != y.directory) return x.directory;
        return natural_compare({names_ + x.name, x.name_size}, {names_ + y.name, y.name_size}) < 0;
    });
    return n;
}

Library::Entry Library::entry(int index) const {
    std::lock_guard lock(mutex_);
    return index >= 0 && index < count_ ? entries_[index] : Entry{};
}

void Library::name(int index, char* out, size_t size) const {
    std::lock_guard lock(mutex_);
    if (index < 0 || index >= count_) {
        std::snprintf(out, size, "%s", "");
        return;
    }
    const Entry& e = entries_[index];
    std::snprintf(out, size, "%.*s", static_cast<int>(e.name_size), names_ + e.name);
}

void Library::path(int index, char* out, size_t size) const {
    std::lock_guard lock(mutex_);
    path_locked(index, out, size);
}

void Library::path_locked(int index, char* out, size_t size) const {
    // Folders nest at most as deep as paths fit, so a fixed chain does.
    std::array<int, 64> chain;
    int depth = 0;
    for (int i = index; i >= 0 && i < count_ && depth < static_cast<int>(chain.size()); i = entries_[i].parent) {
        chain[depth++] = i;
    }
    int n = std::snprintf(out, size, "roms");
    while (depth > 0 && n >= 0 && static_cast<size_t>(n) < size) {
        const Entry& e = entries_[chain[--depth]];
        n += std::snprintf(out + n, size - n, "/%.*s", static_cast<int>(e.name_size), names_ + e.name);
    }
}

int Library::find(const char* path) const {
    std::string_view rest(path);
    if (!rest.starts_with("roms/")) return -1;
    rest.remove_prefix(5);
    std::lock_guard lock(mutex_);
    int parent = kRoot;
    while (!rest.empty()) {
        const size_t slash = std::min(rest.find('/'), rest.size());
        const std::string_view part = rest.substr(0, slash);
        rest.remove_prefix(std::min(slash + 1, rest.size()));
        int found = -1;
        for (int i = 0; i < count_; ++i) {
            const Entry& e = entries_[i];
            if (e.parent == parent && std::string_view(names_ + e.name, e.name_size) == part) {
                found = i;
                break;
            }
        }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

void Library::set_save(int index, bool save) {
    std::lock_guard lock(mutex_);
    if (index >= 0 && index < count_ && entries_[index].save != save) {
        entries_[index].save = save;
        generation_.fetch_add(1, std::memory_order_release);
    }
}

}  // namespace gbx::app
