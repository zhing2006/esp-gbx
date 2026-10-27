#include "gbx/files.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "gbx/esp/storage.h"

namespace gbx::platform::files {
namespace {

constexpr const char* kTag = "files";
// Writes go to the card in pieces of this size, so `written` moves on.
constexpr size_t kChunk = 4096;
// Reads too, in larger pieces, which the card reads faster.
constexpr size_t kReadChunk = 64 * 1024;

// Every call holds this while it uses the card, so remount() never takes the
// mount away under one. FatFs locks the volume per operation as well; this
// only adds the mount's lifetime.
std::mutex card;
// Counts mounts; a Directory from an older one is stale.
uint32_t mount_count = 1;

// Card paths under the mount point, such as /sdcard/roms/gb/Tetris.gb.
class CardPath {
public:
    explicit CardPath(const char* path) {
        n_ = std::snprintf(text_, sizeof text_, "%s/%s", storage::kSdMountPath, path);
    }
    // False when the path did not fit.
    bool ok() const { return n_ > 0 && static_cast<size_t>(n_) < sizeof text_; }
    char* c_str() { return text_; }

private:
    // The mount point and a path somewhat longer than kMaxPath, as the app's
    // save files have longer names than their ROMs.
    char text_[kMaxPath + 64];
    int n_ = 0;
};

// FatFs reports a full card as ENOSPC, see vfs_fat_write().
Error error_from(int error, const char* operation, const char* path) {
    ESP_LOGW(kTag, "%s %s: %s", operation, path, std::strerror(error));
    return error == ENOSPC ? Error::full : Error::failed;
}

}  // namespace

bool mount() {
    std::lock_guard lock(card);
    return storage::mount_sd();
}

bool remount() {
    std::lock_guard lock(card);
    storage::unmount_sd();
    ++mount_count;
    return storage::mount_sd();
}

int64_t size(const char* path) {
    std::lock_guard lock(card);
    CardPath full(path);
    if (!full.ok()) return -1;
    FILE* file = std::fopen(full.c_str(), "rb");
    // FatFs reports a missing file or folder as ENOENT, see fresult_to_errno().
    if (!file) return errno == ENOENT ? kNoFile : -1;
    int64_t result = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) result = std::ftell(file);
    std::fclose(file);
    return result;
}

bool read(const char* path, std::span<uint8_t> out, std::atomic<size_t>* done) {
    std::lock_guard lock(card);
    CardPath full(path);
    FILE* file = full.ok() ? std::fopen(full.c_str(), "rb") : nullptr;
    if (!file) return false;
    size_t at = 0;
    while (at < out.size()) {
        const size_t n = std::fread(out.data() + at, 1, std::min(out.size() - at, kReadChunk), file);
        if (n == 0) break;
        at += n;
        if (done) done->store(at, std::memory_order_relaxed);
    }
    std::fclose(file);
    return at == out.size();
}

int64_t read_start(const char* path, std::span<uint8_t> out) {
    std::lock_guard lock(card);
    CardPath full(path);
    FILE* file = full.ok() ? std::fopen(full.c_str(), "rb") : nullptr;
    if (!file) return -1;
    int64_t result = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) result = std::ftell(file);
    const auto want = static_cast<size_t>(std::clamp<int64_t>(result, 0, static_cast<int64_t>(out.size())));
    if (result >= 0 && (std::fseek(file, 0, SEEK_SET) != 0 || std::fread(out.data(), 1, want, file) != want)) {
        result = -1;
    }
    std::fclose(file);
    return result;
}

Error write(const char* path, std::span<const uint8_t> data, std::atomic<size_t>* written) {
    std::lock_guard lock(card);
    CardPath full(path);
    if (!full.ok()) return Error::failed;
    // No O_TRUNC: a file of the same size keeps its clusters, see files.h.
    const int fd = ::open(full.c_str(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return error_from(errno, "open", path);
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, std::min(data.size() - done, kChunk));
        if (n <= 0) {
            const int error = n == 0 ? ENOSPC : errno;
            ::close(fd);
            return error_from(error, "write", path);
        }
        done += static_cast<size_t>(n);
        if (written) written->store(done, std::memory_order_relaxed);
    }
    struct stat info{};
    if (::fstat(fd, &info) == 0 && info.st_size > static_cast<off_t>(data.size()) &&
        ::ftruncate(fd, static_cast<off_t>(data.size())) != 0) {
        const int error = errno;
        ::close(fd);
        return error_from(error, "truncate", path);
    }
    // f_sync() writes the data, the FAT and the directory entry to the card.
    if (::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        return error_from(error, "flush", path);
    }
    if (::close(fd) != 0) return error_from(errno, "close", path);
    return Error::none;
}

Error make_parents(const char* path) {
    std::lock_guard lock(card);
    CardPath full(path);
    if (!full.ok()) return Error::failed;
    char* text = full.c_str();
    // Skip the mount point, which exists.
    for (char* slash = std::strchr(text + std::strlen(storage::kSdMountPath) + 1, '/'); slash;
         slash = std::strchr(slash + 1, '/')) {
        *slash = '\0';
        struct stat info{};
        if (::stat(text, &info) != 0 && ::mkdir(text, 0777) != 0 && errno != EEXIST) {
            // The folder that failed, as a card path like `path`.
            const Error error = error_from(errno, "mkdir", text + std::strlen(storage::kSdMountPath) + 1);
            *slash = '/';
            return error;
        }
        *slash = '/';
    }
    return Error::none;
}

bool File::open(const char* path) {
    close();
    std::lock_guard lock(card);
    mount_ = mount_count;
    CardPath full(path);
    const int fd = full.ok() ? ::open(full.c_str(), O_RDONLY) : -1;
    if (fd < 0) return false;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0) {
        ::close(fd);
        return false;
    }
    handle_ = fd;
    size_ = end;
    return true;
}

void File::close() {
    if (handle_ == -1) return;
    std::lock_guard lock(card);
    // A remount closed the volume under it; there is nothing left to close.
    if (mount_ == mount_count) ::close(static_cast<int>(handle_));
    handle_ = -1;
    size_ = -1;
}

// FatFs reads whole sectors straight into the destination, so one read()
// per piece with no stdio buffer in between.
bool File::read(uint64_t offset, std::span<uint8_t> out) {
    std::lock_guard lock(card);
    if (handle_ == -1 || mount_ != mount_count) return false;
    const int fd = static_cast<int>(handle_);
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(offset)) return false;
    size_t at = 0;
    while (at < out.size()) {
        const ssize_t n = ::read(fd, out.data() + at, out.size() - at);
        if (n <= 0) return false;
        at += static_cast<size_t>(n);
    }
    return true;
}

bool exists(const char* path) {
    std::lock_guard lock(card);
    CardPath full(path);
    struct stat info{};
    return full.ok() && ::stat(full.c_str(), &info) == 0;
}

bool remove(const char* path) {
    std::lock_guard lock(card);
    CardPath full(path);
    return full.ok() && ::unlink(full.c_str()) == 0;
}

bool Directory::open(const char* path) {
    close();
    std::lock_guard lock(card);
    mount_ = mount_count;
    CardPath full(path);
    // opendir() of the mount point needs no trailing slash.
    char* text = full.c_str();
    if (const size_t n = std::strlen(text); n > 1 && text[n - 1] == '/') text[n - 1] = '\0';
    handle_ = full.ok() ? ::opendir(text) : nullptr;
    return handle_ != nullptr;
}

bool Directory::next(Entry& entry) {
    std::lock_guard lock(card);
    if (!handle_ || mount_ != mount_count) return false;
    while (const dirent* d = ::readdir(static_cast<DIR*>(handle_))) {
        if (std::strcmp(d->d_name, ".") == 0 || std::strcmp(d->d_name, "..") == 0) continue;
        std::snprintf(entry.name, sizeof entry.name, "%s", d->d_name);
        entry.directory = d->d_type == DT_DIR;
        return true;
    }
    return false;
}

void Directory::close() {
    std::lock_guard lock(card);
    // A stream from an older mount went with it.
    if (handle_ && mount_ == mount_count) ::closedir(static_cast<DIR*>(handle_));
    handle_ = nullptr;
}

}  // namespace gbx::platform::files
