#include "gbx/files.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "gbx/platform.h"

// The current directory is the card, so card paths and paths given on the
// command line are both opened as they are. Paths are UTF-8; on Windows they
// go through std::filesystem and the wide-character API, since fopen() would
// read them in the ANSI code page.
namespace gbx::platform::files {
namespace {

namespace fs = std::filesystem;

constexpr const char* kTag = "files";
constexpr size_t kChunk = 4096;
constexpr size_t kReadChunk = 64 * 1024;

fs::path native(const char* path) {
#ifdef _WIN32
    // std::filesystem throws on text that is not UTF-8, which ends the
    // program as exceptions are off. Such a path names no file.
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), n);
    wide.pop_back();
    return fs::path(std::move(wide));
#else
    const std::string_view text(path);
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#endif
}

FILE* open_file(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t wide_mode[8] = {};
    for (size_t i = 0; mode[i] && i + 1 < std::size(wide_mode); ++i) wide_mode[i] = static_cast<wchar_t>(mode[i]);
    return _wfopen(native(path).c_str(), wide_mode);
#else
    return std::fopen(path, mode);
#endif
}

int flush_to_disk(FILE* file) {
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

Error error_from(int error, const char* operation, const char* path) {
    log(LogLevel::warn, kTag, "%s %s: %s", operation, path, std::strerror(error));
    return error == ENOSPC ? Error::full : Error::failed;
}

}  // namespace

bool mount() {
    return true;
}

bool remount() {
    return true;
}

int64_t size(const char* path) {
    FILE* file = open_file(path, "rb");
    if (!file) return errno == ENOENT ? kNoFile : -1;
    int64_t result = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) result = std::ftell(file);
    std::fclose(file);
    return result;
}

bool read(const char* path, std::span<uint8_t> out, std::atomic<size_t>* done) {
    FILE* file = open_file(path, "rb");
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
    FILE* file = open_file(path, "rb");
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
    // As on the firmware, an existing file is overwritten in place.
    FILE* file = open_file(path, "r+b");
    if (!file) file = open_file(path, "wb");
    if (!file) return error_from(errno, "open", path);
    size_t done = 0;
    while (done < data.size()) {
        const size_t n = std::fwrite(data.data() + done, 1, std::min(data.size() - done, kChunk), file);
        if (n == 0) {
            const int error = errno;
            std::fclose(file);
            return error_from(error, "write", path);
        }
        done += n;
        if (written) written->store(done, std::memory_order_relaxed);
    }
    if (std::fflush(file) != 0 || flush_to_disk(file) != 0) {
        const int error = errno;
        std::fclose(file);
        return error_from(error, "flush", path);
    }
    if (std::fclose(file) != 0) return error_from(errno, "close", path);
    std::error_code ec;
    if (fs::file_size(native(path), ec) > data.size() && !ec) {
        fs::resize_file(native(path), data.size(), ec);
        if (ec) return error_from(ec.value(), "truncate", path);
    }
    return Error::none;
}

Error make_parents(const char* path) {
    const fs::path parent = native(path).parent_path();
    if (parent.empty()) return Error::none;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (!ec) return Error::none;
    const std::string_view file(path);
    return error_from(ec.value(), "mkdir", std::string(file.substr(0, file.rfind('/'))).c_str());
}

bool File::open(const char* path) {
    close();
    FILE* file = open_file(path, "rb");
    if (!file) return false;
#ifdef _WIN32
    const bool sized = _fseeki64(file, 0, SEEK_END) == 0;
    const int64_t end = sized ? _ftelli64(file) : -1;
#else
    const bool sized = fseeko(file, 0, SEEK_END) == 0;
    const int64_t end = sized ? static_cast<int64_t>(ftello(file)) : -1;
#endif
    if (end < 0) {
        std::fclose(file);
        return false;
    }
    handle_ = reinterpret_cast<intptr_t>(file);
    size_ = end;
    return true;
}

void File::close() {
    if (handle_ == -1) return;
    std::fclose(reinterpret_cast<FILE*>(handle_));
    handle_ = -1;
    size_ = -1;
}

bool File::read(uint64_t offset, std::span<uint8_t> out) {
    if (handle_ == -1) return false;
    auto* file = reinterpret_cast<FILE*>(handle_);
#ifdef _WIN32
    if (_fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(out.data(), 1, out.size(), file) == out.size();
}

bool exists(const char* path) {
    std::error_code ec;
    return fs::exists(native(path), ec);
}

bool remove(const char* path) {
    std::error_code ec;
    return fs::remove(native(path), ec);
}

bool Directory::open(const char* path) {
    close();
    std::error_code ec;
    auto* it = new fs::directory_iterator(native(*path ? path : "."), ec);
    if (ec) {
        delete it;
        return false;
    }
    handle_ = it;
    return true;
}

bool Directory::next(Entry& entry) {
    auto* it = static_cast<fs::directory_iterator*>(handle_);
    if (!it) return false;
    std::error_code ec;
    while (*it != fs::directory_iterator()) {
        const auto& item = **it;
        const std::u8string name = item.path().filename().u8string();
        const bool directory = item.is_directory(ec);
        it->increment(ec);
        if (ec) *it = fs::directory_iterator();
        if (name.size() >= sizeof entry.name) continue;
        std::memcpy(entry.name, name.data(), name.size());
        entry.name[name.size()] = '\0';
        entry.directory = directory;
        return true;
    }
    return false;
}

void Directory::close() {
    delete static_cast<fs::directory_iterator*>(handle_);
    handle_ = nullptr;
}

}  // namespace gbx::platform::files
