#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

// Files on the card, by paths relative to its root such as "roms/gb/Tetris.gb".
// Paths and names are UTF-8. The firmware's card is the SD card. The PC build
// takes the current directory as the card, with the same roms/, saves/ and
// states/ under it, and also accepts absolute paths. The functions may run on
// any task; each call is complete in itself.
namespace gbx::platform::files {

// The longest path the app keeps, in bytes with the terminating zero. FAT
// limits each name, not the whole path; this bounds the buffers paths are
// built in. A path that does not fit is refused, not cut.
constexpr size_t kMaxPath = 600;

// Makes the card ready. Returns false when there is no usable card; calling
// it again retries, for a card inserted later. The card stays mounted.
bool mount();

// Mounts the card again, for a card taken out and put back: the old mount
// cannot reach it any more. Waits for calls under way on other tasks; a
// Directory open across it ends, as if it had no more names.
bool remount();

// What size() returns for a path with no file. A file that is there but
// cannot be opened, as on a card error, gives -1 instead.
constexpr int64_t kNoFile = -2;

// Size of a file in bytes, kNoFile when there is none, or -1 if it cannot be
// opened.
int64_t size(const char* path);

// Reads the first out.size() bytes of a file. Returns false on any error.
// `done`, when given, counts the bytes read so far.
bool read(const char* path, std::span<uint8_t> out, std::atomic<size_t>* done = nullptr);

// Reads up to out.size() bytes from the start of a file, fewer when the file
// is shorter, and returns the file's size; -1 if it cannot be opened or read.
// One open for a scan that wants a header and the size.
int64_t read_start(const char* path, std::span<uint8_t> out);

// Why a change to the card failed.
enum class Error : uint8_t { none, full, failed };

// Writes `data` as the whole file, creating it if needed, and flushes it to
// the card before returning. A file of the same size is overwritten in place,
// so the file system allocates nothing and only the file's own sectors are
// written. `written`, when given, counts the bytes handed to the card so far.
Error write(const char* path, std::span<const uint8_t> data, std::atomic<size_t>* written = nullptr);

// Creates the directories of `path` up to its last '/'; "saves/gb/x.sav"
// creates saves/ and saves/gb/.
Error make_parents(const char* path);

bool exists(const char* path);
bool remove(const char* path);

// A file kept open for reads at any offset, such as a GBA ROM read page by
// page while a game runs. One task uses an object at a time; reads hold the
// card for their own length only, so other tasks' calls go on between
// them. A remount() ends it: reads then fail until it is opened again.
class File {
public:
    File() = default;
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool open(const char* path);
    void close();
    bool is_open() const { return handle_ != -1; }
    // The size when opened.
    int64_t size() const { return size_; }
    // Reads out.size() bytes from `offset`; false on any error or a short
    // read.
    bool read(uint64_t offset, std::span<uint8_t> out);

private:
    intptr_t handle_ = -1;
    int64_t size_ = -1;
    [[maybe_unused]] uint32_t mount_ = 0;  // the mount it was opened on; the board only
};

// A file written from its start in pieces and then flushed to the card, for
// one not held whole in memory. As with write(), a file of the same size is
// overwritten in place. One task uses an object at a time; each call holds
// the card for its own length only. A remount() ends it: writes then fail.
class Writer {
public:
    Writer() = default;
    ~Writer() { close(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    // Opens `path`, creating it if needed. The path stays the caller's
    // until finish() or close(), for the log.
    Error open(const char* path);
    // Appends `data`.
    Error write(std::span<const uint8_t> data);
    // Cuts the file to what was written, flushes it to the card and closes
    // it.
    Error finish();
    // Closes the file without flushing it, as after a failed write.
    void close();

private:
    intptr_t handle_ = -1;
    uint64_t size_ = 0;  // written
    const char* path_ = "";
    [[maybe_unused]] uint32_t mount_ = 0;  // the mount it was opened on; the board only
};

// One name in a directory.
struct Entry {
    char name[256];
    bool directory;
};

// The names in a directory, in the order the file system keeps them,
// without "." and "..".
class Directory {
public:
    Directory() = default;
    ~Directory() { close(); }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    bool open(const char* path);
    // False at the end or on an error.
    bool next(Entry& entry);
    void close();

private:
    void* handle_ = nullptr;
    [[maybe_unused]] uint32_t mount_ = 0;  // the mount it was opened on; the board only
};

}  // namespace gbx::platform::files
