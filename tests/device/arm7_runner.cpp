#include "arm7_runner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <unistd.h>

#include "arm7_execution.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "execution.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_private/esp_clk.h"
#include "freertos/task.h"
#include "gbx/gba/arm7.h"
#include "gbx/gba/bus.h"
#include "gbx/gba/jit/arm7_jit.h"
#include "transport.h"

namespace gbx::test {
namespace {

using namespace arm7_execution;
using protocol::load16;
using protocol::load32;
using protocol::store16;
using protocol::store32;

// Records are copied as they are stored: little-endian words, and transactions
// in BusRecord's own layout.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(gba::BusRecord) == 12 && offsetof(gba::BusRecord, address) == 4 &&
              offsetof(gba::BusRecord, data) == 8);
static_assert(sizeof(gba::Arm7Registers) == 37 * 4 && std::is_trivially_copyable_v<gba::Arm7Registers>);

// Reads start on a sector and cover whole sectors of cache-line-aligned PSRAM,
// so FATFS passes them to the SDMMC DMA directly; other buffers make
// sdmmc_read_sectors() copy through a bounce buffer.
constexpr uint32_t kSector = 512, kChunk = 128 * 1024, kBuffers = 4, kAlignment = 128;
constexpr uint32_t kMaxFiles = 64;
constexpr uint32_t kMinCase = 4 * kStateWords + 8 + 9;
constexpr uint32_t kMaxCase = kMinCase + 4 * kStateWords + 12 * kMaxRecords;
constexpr uint8_t kNone = 0xff;

struct File { uint32_t cases, table, records, size; };
struct Job { uint32_t start, end; };
// A block of the stream, or its end when buffer is kNone.
struct Block {
    uint8_t buffer;
    bool failed;
    uint32_t position, size;
};

struct Context {
    int fd = -1;
    std::array<File, kMaxFiles> files{};
    uint32_t file_count = 0;
    uint32_t* table = nullptr;  // A range's record offsets and the end of its last one.
    std::array<uint8_t*, kBuffers> buffers{};
    QueueHandle_t jobs = nullptr, free = nullptr, full = nullptr;
    std::atomic<bool> cancel{false};
    std::atomic<uint32_t> read_us{0};
    Block current{kNone, false, 0, 0};
    bool ended = true, failed = false;
    std::array<uint32_t, kMaxIndices> selected{};
    // One case. The object is in PSRAM; these stay in the data cache.
    alignas(4) std::array<uint8_t, kMaxCase> stitch{};
    std::array<uint32_t, kStateWords> initial{}, expected{}, actual{}, jit_actual{};
    std::array<gba::BusRecord, kMaxRecords> wanted{}, records{}, jit_records{};
    uint32_t cycle_differences = 0;
    // CPU cycles of each step of a request, see DONE in arm7_execution.h.
    uint64_t interpreter_cycles = 0, compile_cycles = 0, publish_cycles = 0, jit_cycles = 0;
    // Its code pool is unused: single steps run from the slot after the
    // gateway.
    gba::Arm7Jit jit;
};
Context* context = nullptr;

// PSRAM, like the other protocol buffers, leaving internal RAM to the JIT.
EXT_RAM_BSS_ATTR std::array<uint8_t, protocol::kMaxPayload> batch;

bool error(std::string_view message) {
    return transport::send(protocol::Kind::error,
        {reinterpret_cast<const uint8_t*>(message.data()), message.size()});
}

// Streams each job's [start, end) to the consumer in blocks, then an end block.
void reader(void*) {
    auto& c = *context;
    for (;;) {
        Job job;
        xQueueReceive(c.jobs, &job, portMAX_DELAY);
        uint32_t position = job.start & ~(kSector - 1);
        const uint32_t end = (job.end + kSector - 1) & ~(kSector - 1);
        bool failed = lseek(c.fd, static_cast<off_t>(position), SEEK_SET) != static_cast<off_t>(position);
        while (!failed && position < job.end && !c.cancel.load(std::memory_order_relaxed)) {
            uint8_t index;
            xQueueReceive(c.free, &index, portMAX_DELAY);
            const int64_t start = esp_timer_get_time();
            const ssize_t size = read(c.fd, c.buffers[index], std::min(kChunk, end - position));
            c.read_us.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start), std::memory_order_relaxed);
            if (size <= 0) {
                xQueueSend(c.free, &index, portMAX_DELAY);
                failed = true;
                break;
            }
            const Block block{index, false, position, static_cast<uint32_t>(size)};
            xQueueSend(c.full, &block, portMAX_DELAY);
            position += static_cast<uint32_t>(size);
        }
        const Block done{kNone, failed, 0, 0};
        xQueueSend(c.full, &done, portMAX_DELAY);
    }
}

// Returns the held block and takes the next; false at the end of the stream.
bool next_block(Context& c, uint64_t& wait_us) {
    if (c.current.buffer != kNone) xQueueSend(c.free, &c.current.buffer, portMAX_DELAY);
    const int64_t start = esp_timer_get_time();
    xQueueReceive(c.full, &c.current, portMAX_DELAY);
    wait_us += esp_timer_get_time() - start;
    if (c.current.buffer != kNone) return true;
    c.ended = true;
    c.failed = c.current.failed;
    return false;
}

// The record at [position, position + size) of the stream; records come in
// increasing order. One that spans two blocks is assembled in stitch.
const uint8_t* stream_record(Context& c, uint32_t position, uint32_t size, uint64_t& wait_us) {
    while (c.current.buffer == kNone || position >= c.current.position + c.current.size)
        if (c.ended || !next_block(c, wait_us)) return nullptr;
    if (position + size <= c.current.position + c.current.size)
        return c.buffers[c.current.buffer] + (position - c.current.position);
    for (uint32_t copied = 0; copied < size;) {
        if (position + copied >= c.current.position + c.current.size && (c.ended || !next_block(c, wait_us)))
            return nullptr;
        const uint32_t from = position + copied - c.current.position;
        const uint32_t count = std::min(size - copied, c.current.size - from);
        std::memcpy(c.stitch.data() + copied, c.buffers[c.current.buffer] + from, count);
        copied += count;
    }
    return c.stitch.data();
}

// Drains the stream to its end block, stopping the reader first if asked.
bool end_stream(Context& c, bool cancel) {
    if (cancel) c.cancel.store(true, std::memory_order_relaxed);
    uint64_t ignored = 0;
    while (!c.ended) next_block(c, ignored);
    c.cancel.store(false, std::memory_order_relaxed);
    return !c.failed;
}

// A read of the offset table while the reader is idle; its time counts as
// both read and wait.
bool read_at(Context& c, uint32_t position, void* data, uint32_t size, uint64_t& wait_us) {
    const int64_t start = esp_timer_get_time();
    const bool ok = lseek(c.fd, static_cast<off_t>(position), SEEK_SET) == static_cast<off_t>(position) &&
                    read(c.fd, data, size) == static_cast<ssize_t>(size);
    const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
    c.read_us.fetch_add(elapsed, std::memory_order_relaxed);
    wait_us += elapsed;
    return ok;
}

bool condition_passes(uint32_t condition, uint32_t flags) {
    const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
    switch (condition) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    case 14: return true;
    default: return false;
    }
}

// The class of arm7_execution.h, decoding as src/gba/executor.h does.
unsigned classify(uint32_t op, uint32_t cpsr) {
    if ((cpsr & gba::kFlagT) || !condition_passes(op >> 28, cpsr >> 28)) return kJudged;
    const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
    const bool base_write = rn == 15 && writeback && !((op & (1u << 20)) && rd == 15);
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x90) == 0x90) return (op & 0x60) && base_write ? kR15Write : kJudged;
        if ((op & 0x01900000) == 0x01000000 && (op & 0x0ffffff0) != 0x012fff10)
            return !(op & (1u << 21)) && rd == 15 ? kR15Write : kJudged;
        return kJudged;
    case 1: return (op & 0x01b00000) == 0x01000000 && rd == 15 ? kR15Write : kJudged;
    case 2: return base_write ? kR15Write : kJudged;
    case 3: return !(op & 0x10) && base_write ? kR15Write : kJudged;
    case 7: return !(op & (1u << 24)) && (op & 0x10) && ((op >> 8) & 15) == 14 ? kCp14 : kJudged;
    default: return kJudged;
    }
}

// The CPU state of a case's state words.
gba::Arm7State load_state(const std::array<uint32_t, kStateWords>& words) {
    gba::Arm7Registers registers;
    std::memcpy(static_cast<void*>(&registers), words.data(), sizeof(registers));
    gba::Arm7State state;
    gba::arm7_set_registers(state, registers);
    state.pipeline = {words[37], words[38]};
    state.sequential = words[39] & 1;
    return state;
}

void store_state(const gba::Arm7State& state, std::array<uint32_t, kStateWords>& words) {
    const auto registers = gba::arm7_registers(state);
    std::memcpy(words.data(), &registers, sizeof(registers));
    words[37] = state.pipeline[0];
    words[38] = state.pipeline[1];
    words[39] = state.sequential ? 3 : 2;
}

// The JIT compiles the instruction a fetch would find. In a case's state it
// is in the pipeline, which the flat bus does not hold.
uint32_t pipeline_code(const void* context, uint32_t address, bool thumb) {
    const auto& state = *static_cast<const gba::Arm7State*>(context);
    const uint32_t length = thumb ? 2 : 4;
    if (address == state.r[15] - 2 * length) return state.pipeline[0];
    return address == state.r[15] - length ? state.pipeline[1] : 0;
}

// The actual of one backend at output: the state words that differ from the
// initial ones, then the bus records.
uint8_t* store_actual(const Context& c, const std::array<uint32_t, kStateWords>& actual,
                      const std::array<gba::BusRecord, kMaxRecords>& records, size_t count, uint8_t* output) {
    uint8_t* next = output + 8;
    uint64_t changed = 0;
    for (unsigned i = 0; i < kStateWords; ++i) {
        if (actual[i] == c.initial[i]) continue;
        changed |= uint64_t{1} << i;
        store32(next, actual[i]);
        next += 4;
    }
    std::memcpy(output, &changed, 8);
    *next++ = static_cast<uint8_t>(count);
    std::memcpy(next, records.data(), 12 * count);
    return next + 12 * count;
}

// Executes one record on the interpreter, judged by the suite, then on the
// JIT from the same state, compared with the interpreter in every state word
// and every field of every bus record. Returns the size of its item at
// output, 0 for a pass without one, or -1 for a malformed record.
// The caller has checked that size is at least kMinCase.
int execute(Context& c, const uint8_t* p, uint32_t size, uint8_t mode, uint8_t* output) {
    std::memcpy(c.initial.data(), p, 4 * kStateWords);
    p += 4 * kStateWords;
    uint64_t mask;
    std::memcpy(&mask, p, 8);
    p += 8;
    const uint32_t changed_words = static_cast<uint32_t>(std::popcount(mask));
    if ((mask >> kStateWords) || kMinCase + 4 * changed_words > size) return -1;
    c.expected = c.initial;
    for (; mask; mask &= mask - 1, p += 4) c.expected[std::countr_zero(mask)] = load32(p);
    const uint32_t opcode = load32(p), base = load32(p + 4);
    const unsigned count = p[8];
    p += 9;
    if (count > kMaxRecords || kMinCase + 4 * changed_words + 12 * count != size) return -1;
    std::memcpy(c.wanted.data(), p, 12 * count);

    const gba::Arm7State initial = load_state(c.initial);
    gba::Arm7State state = initial;
    gba::TestBus bus(std::span(c.wanted.data(), count), opcode, base, c.records);
    uint32_t start = esp_cpu_get_cycle_count();
    gba::arm7_step(state, bus.bus());
    uint32_t now = esp_cpu_get_cycle_count();
    c.interpreter_cycles += now - start;
    store_state(state, c.actual);

    // Compiling, publishing and running are timed apart.
    state = initial;
    gba::TestBus jit_bus(std::span(c.wanted.data(), count), opcode, base, c.jit_records);
    start = esp_cpu_get_cycle_count();
    c.jit.compile_single(initial, {&initial, pipeline_code});
    now = esp_cpu_get_cycle_count();
    c.compile_cycles += now - start;
    start = now;
    c.jit.publish_single();
    now = esp_cpu_get_cycle_count();
    c.publish_cycles += now - start;
    start = now;
    const auto fallback = static_cast<uint8_t>(c.jit.execute_single(state, jit_bus.bus()).fallback);
    c.jit_cycles += esp_cpu_get_cycle_count() - start;
    store_state(state, c.jit_actual);

    const bool overflowed = bus.overflowed();
    const size_t recorded = std::min(bus.size(), kMaxRecords);
    bool same = !overflowed && recorded == count;
    if (same) {
        bool cycles = true;
        for (unsigned i = 0; i < count; ++i) {
            const auto& a = c.records[i];
            const auto& e = c.wanted[i];
            same &= a.kind == e.kind && a.size == e.size && a.address == e.address && a.data == e.data &&
                    !((a.access ^ e.access) & 1);
            cycles &= a.cycle == e.cycle;
        }
        c.cycle_differences += !cycles;
    }
    // R15's undefined low bits, by the expected CPSR's T (TRM 2.6.1).
    uint32_t difference = (c.actual[15] ^ c.expected[15]) & ((c.expected[31] & gba::kFlagT) ? ~1u : ~3u);
    for (unsigned i = 0; i < kStateWords; ++i) difference |= i == 15 ? 0 : c.actual[i] ^ c.expected[i];
    same &= difference == 0;

    // The pair: everything the interpreter observed, Lock and cycle too.
    const size_t jit_recorded = std::min(jit_bus.size(), kMaxRecords);
    const bool differs = jit_bus.size() != bus.size() || c.jit_actual != c.actual ||
                         std::memcmp(c.jit_records.data(), c.records.data(), 12 * recorded) != 0;

    const unsigned type = classify(opcode, c.initial[31]);
    if (mode == kDeviceJudge && type == kJudged && same && !differs && !fallback) return 0;
    const uint8_t outcome = mode == kDiagnostic ? kUnjudged : (type != kJudged || same) ? kPass : kFail;
    const bool pair_failed = mode != kDiagnostic && differs;
    const bool attach = mode == kDiagnostic || outcome == kFail || differs || fallback;
    output[0] = static_cast<uint8_t>(0x80 | outcome | (type << 2) | (overflowed ? 0x10 : 0) | (attach ? 0x20 : 0) |
                                     (pair_failed ? 0x40 : 0));
    output[1] = static_cast<uint8_t>(fallback | (jit_bus.overflowed() ? 8 : 0));
    if (!attach) return 2;
    uint8_t* next = store_actual(c, c.actual, c.records, recorded, output + 2);
    next = store_actual(c, c.jit_actual, c.jit_records, jit_recorded, next);
    return static_cast<int>(next - output);
}

// The header and the index of tools/prepare_gba_tests.py; files follow each
// other, and each offset table directly precedes its records.
bool load_index(Context& c, uint32_t& largest) {
    uint8_t header[16];
    if (read(c.fd, header, sizeof(header)) != sizeof(header) || std::memcmp(header, "GBXARM7\0", 8) ||
        load32(header + 8) != 1)
        return false;
    c.file_count = load32(header + 12);
    if (!c.file_count || c.file_count > kMaxFiles) return false;
    uint64_t end = 16 + 64ull * c.file_count;
    largest = 0;
    for (uint32_t i = 0; i < c.file_count; ++i) {
        uint8_t entry[64];
        if (read(c.fd, entry, sizeof(entry)) != sizeof(entry)) return false;
        uint64_t table, records, size;
        std::memcpy(&table, entry + 40, 8);
        std::memcpy(&records, entry + 48, 8);
        std::memcpy(&size, entry + 56, 8);
        const uint32_t cases = load32(entry + 32);
        if (!cases || load32(entry + 36) || table != end || records != table + 4ull * cases ||
            size < uint64_t{kMinCase} * cases || records + size > 0x7fffffff)
            return false;
        c.files[i] = {cases, static_cast<uint32_t>(table), static_cast<uint32_t>(records),
                      static_cast<uint32_t>(size)};
        end = records + size;
        largest = std::max(largest, cases);
    }
    return lseek(c.fd, 0, SEEK_END) == static_cast<off_t>(end);
}

}  // namespace

bool Arm7Runner::open(const char* path) {
    if (context) return false;
    void* storage = heap_caps_malloc(sizeof(Context), MALLOC_CAP_SPIRAM);
    if (!storage) return false;
    auto* c = new (storage) Context;
    uint32_t largest = 0;
    c->fd = ::open(path, O_RDONLY);
    bool ok = c->fd >= 0 && load_index(*c, largest);
    if (ok) {
        c->table = static_cast<uint32_t*>(heap_caps_malloc(4 * (largest + 1), MALLOC_CAP_SPIRAM));
        for (auto& buffer : c->buffers)
            buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(kAlignment, kChunk, MALLOC_CAP_SPIRAM));
        c->jobs = xQueueCreate(1, sizeof(Job));
        c->free = xQueueCreate(kBuffers, 1);
        c->full = xQueueCreate(kBuffers + 1, sizeof(Block));
        ok = c->table && c->jobs && c->free && c->full &&
             std::all_of(c->buffers.begin(), c->buffers.end(), [](auto* buffer) { return buffer != nullptr; });
    }
    if (ok) {
        for (uint8_t i = 0; i < kBuffers; ++i) xQueueSend(c->free, &i, 0);
        context = c;
        // The stack is in PSRAM, as components/platform/src/esp/worker.cpp
        // places them, so the JIT code pool keeps the internal RAM.
        ok = xTaskCreatePinnedToCoreWithCaps(reader, "arm7-read", 4096, nullptr, 5, nullptr, 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    if (!ok) {
        // A failed open leaves the data unavailable for this boot.
        context = nullptr;
        if (c->fd >= 0) close(c->fd);
        for (auto* buffer : c->buffers) heap_caps_free(buffer);
        heap_caps_free(c->table);
        if (c->jobs) vQueueDelete(c->jobs);
        if (c->free) vQueueDelete(c->free);
        if (c->full) vQueueDelete(c->full);
        c->~Context();
        heap_caps_free(storage);
    }
    return ok;
}

bool Arm7Runner::run(std::span<const uint8_t> request) {
    if (!context) return error("ARM7TDMI data is unavailable");
    auto& c = *context;
    if (request.size() < kRequestHeader || load16(request.data()) != execution::kVersion ||
        request[2] != kPaired)
        return error("ARM7TDMI execution is unavailable or incompatible");
    // The pool holds no blocks; the shared code takes the gateway and the
    // single-step slot.
    if (!c.jit.initialize(1024, 4)) return error("ARM7TDMI JIT code memory is unavailable");
    const uint8_t mode = request[3], selection = request[10];
    const uint32_t id = load32(request.data() + 4), count = load32(request.data() + 12);
    const uint16_t file_index = load16(request.data() + 8);
    if (!id || mode > kDiagnostic || selection > 1 || request[11] || file_index >= c.file_count || !count ||
        (!selection && count > kMaxIndices) || request.size() != kRequestHeader + (selection ? 4 : 4 * count))
        return error("Invalid ARM7TDMI request");
    const File& file = c.files[file_index];
    const uint32_t first = selection ? load32(request.data() + 16) : 0;
    if (selection && (first >= file.cases || count > file.cases - first)) return error("Invalid case range");
    // Copy before sending: transport sends invalidate the request's payload view.
    for (uint32_t i = 0; !selection && i < count; ++i) {
        c.selected[i] = load32(request.data() + 16 + 4 * i);
        if (c.selected[i] >= file.cases || (i && c.selected[i] <= c.selected[i - 1]))
            return error("Invalid case index");
    }

    // Both selections stream the records from the first selected case to the
    // last and skip the others: seeking back and forth would make FATFS walk
    // the cluster chain from the start of the file for every case.
    const int64_t started = esp_timer_get_time();
    uint64_t wait_us = 0, send_us = 0;
    c.read_us.store(0, std::memory_order_relaxed);
    c.cycle_differences = 0;
    c.interpreter_cycles = c.compile_cycles = c.publish_cycles = c.jit_cycles = 0;
    const uint32_t low = selection ? first : c.selected[0];
    const uint32_t span = (selection ? first + count : c.selected[count - 1] + 1) - low;
    const uint32_t entries = span + (low + span < file.cases);
    if (!read_at(c, file.table + 4 * low, c.table, 4 * entries, wait_us))
        return error("Cannot read ARM7TDMI offsets");
    if (entries == span) c.table[span] = file.size;
    for (uint32_t i = 0; i < span; ++i)
        if (c.table[i + 1] < c.table[i] + kMinCase || c.table[i + 1] - c.table[i] > kMaxCase ||
            c.table[i + 1] > file.size)
            return error("Invalid ARM7TDMI offsets");
    c.ended = c.failed = false;
    const Job job{file.records + c.table[0], file.records + c.table[span]};
    xQueueSend(c.jobs, &job, portMAX_DELAY);

    size_t size = kBatchHeader;
    uint32_t sent = 0, records = 0;
    uint8_t* run = nullptr;  // The run item that a pass can extend.
    auto flush = [&] {
        store16(batch.data(), execution::kVersion);
        batch[2] = mode;
        batch[3] = 0;
        store32(batch.data() + 4, id);
        store32(batch.data() + 8, sent);
        store32(batch.data() + 12, records);
        const int64_t start = esp_timer_get_time();
        const bool ok = transport::send_data(std::span(batch).first(size));
        send_us += esp_timer_get_time() - start;
        sent += records;
        records = 0;
        size = kBatchHeader;
        run = nullptr;
        return ok;
    };
    const char* failure = nullptr;
    bool sending = true;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = (selection ? first + i : c.selected[i]) - low;
        const uint32_t record_size = c.table[at + 1] - c.table[at];
        const uint8_t* record = stream_record(c, file.records + c.table[at], record_size, wait_us);
        if (!record) {
            failure = "Cannot read ARM7TDMI case";
            break;
        }
        if (size + kMaxItem > batch.size() && !(sending = flush())) break;
        const int written = execute(c, record, record_size, mode, batch.data() + size);
        if (written < 0) {
            failure = "Invalid ARM7TDMI case";
            break;
        }
        if (written) {
            size += written;
            run = nullptr;
        } else if (run && *run < 0x7f) {
            ++*run;
        } else {
            run = batch.data() + size++;
            *run = 0;
        }
        ++records;
    }
    if (!end_stream(c, failure || !sending) && !failure && sending)
        failure = "Cannot read ARM7TDMI cases";
    if (!sending) return false;
    if (failure) return error(failure);
    if (records && !flush()) return false;
    const int64_t start = esp_timer_get_time();
    if (!transport::finish()) return false;
    send_us += esp_timer_get_time() - start;
    uint8_t done[kDoneSize]{};
    store16(done, execution::kVersion);
    store32(done + 4, id);
    store32(done + 8, count);
    store32(done + 12, c.cycle_differences);
    store32(done + 16, c.read_us.load(std::memory_order_relaxed));
    store32(done + 20, static_cast<uint32_t>(wait_us));
    store32(done + 24, static_cast<uint32_t>(send_us));
    store32(done + 28, static_cast<uint32_t>(esp_timer_get_time() - started));
    const uint64_t mhz = esp_clk_cpu_freq() / 1000000;
    store32(done + 32, static_cast<uint32_t>(c.interpreter_cycles / mhz));
    store32(done + 36, static_cast<uint32_t>(c.compile_cycles / mhz));
    store32(done + 40, static_cast<uint32_t>(c.publish_cycles / mhz));
    store32(done + 44, static_cast<uint32_t>(c.jit_cycles / mhz));
    return transport::send(protocol::Kind::done, done);
}

}  // namespace gbx::test
