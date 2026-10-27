#include "sm83_runner.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "esp_attr.h"
#include "gbx/gb/sm83.h"
#include "transport.h"

namespace gbx::test {
namespace {

using namespace execution;
using protocol::load16;
using protocol::load32;
using protocol::store16;
using protocol::store32;

// Static storage belongs to the single transport task, not its small stack.
// The reply batch is in PSRAM, like the other test protocol buffers, so the
// JIT code pool keeps its internal RAM.
EXT_RAM_BSS_ATTR std::array<uint8_t, protocol::kMaxPayload> batch;
std::array<std::array<gb::BusSample, kMaxSamples>, 2> samples;
std::array<uint16_t, kMaxObservedRam> addresses;

bool error(std::string_view message) {
    return transport::send(protocol::Kind::error,
        {reinterpret_cast<const uint8_t*>(message.data()), message.size()});
}

struct Reader {
    std::span<const uint8_t> bytes;
    bool valid = true;

    std::span<const uint8_t> take(size_t size) {
        if (!valid || size > bytes.size()) {
            valid = false;
            return {};
        }
        auto result = bytes.first(size);
        bytes = bytes.subspan(size);
        return result;
    }
    uint16_t u16() { const auto p = take(2); return valid ? load16(p.data()) : 0; }
    uint32_t u32() { const auto p = take(4); return valid ? load32(p.data()) : 0; }
};

struct State {
    gb::Sm83State cpu;
    bool has_ei = false;
    std::span<const uint8_t> ram;
};

bool state(Reader& reader, State& result) {
    const auto values = reader.take(14);
    if (!reader.valid || (values[13] & ~3)) return false;
    auto& cpu = result.cpu;
    cpu = {};
    cpu.pc = load16(values.data()); cpu.sp = load16(values.data() + 2);
    cpu.a = values[4]; cpu.b = values[5]; cpu.c = values[6];
    cpu.d = values[7]; cpu.e = values[8]; cpu.f = values[9];
    cpu.h = values[10]; cpu.l = values[11]; cpu.ime = values[12] != 0;
    if (values[13] & 1) reader.take(1);  // IE is not modeled by the flat bus.
    result.has_ei = (values[13] & 2) != 0;
    if (result.has_ei) {
        const auto ei = reader.take(1);
        if (reader.valid) cpu.ei_pending = ei[0] != 0;
    }
    const auto count = reader.u16();
    if (count > kMaxRam) return false;
    result.ram = reader.take(count * 3);
    return reader.valid;
}

std::array<uint16_t, 13> registers(const gb::Sm83State& cpu) {
    return {cpu.pc, cpu.sp, cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.f, cpu.h, cpu.l,
        static_cast<uint16_t>(cpu.ime), static_cast<uint16_t>(cpu.ei_pending),
        static_cast<uint16_t>(cpu.mode)};
}

bool matches(const State& expected, std::span<const uint8_t> expected_samples,
             const gb::Sm83State& cpu, const uint8_t* memory, size_t sample_count) {
    // Apply the external suite's criteria independently of the host judge.
    // README.MD ignores IE/IME and disconnected pins, but not provided EI.
    if (expected_samples.size() != sample_count * 4) return false;
    for (size_t i = 0; i < sample_count; ++i) {
        const auto* wanted = expected_samples.data() + i * 4;
        const auto& actual = samples[0][i];
        if (((wanted[3] & 1) && load16(wanted) != actual.address) ||
            ((wanted[3] & 2) && wanted[2] != actual.data) ||
            (wanted[3] >> 2) != actual.signals) return false;
    }
    const auto wanted = registers(expected.cpu), actual = registers(cpu);
    for (size_t i = 0; i < 10; ++i) if (wanted[i] != actual[i]) return false;
    if (expected.has_ei && expected.cpu.ei_pending != cpu.ei_pending) return false;
    for (size_t i = 0; i < expected.ram.size(); i += 3) {
        const auto* ram = expected.ram.data() + i;
        if (memory[load16(ram)] != ram[2]) return false;
    }
    return true;
}

struct Actual {
    gb::Sm83State cpu;
    uint8_t* memory;
    uint16_t mcycles = 0;
    size_t count = 0;
    bool exceeded = false;
};

uint8_t* encode(const gb::Sm83State& initial, const Actual& actual,
                std::span<const gb::BusSample> trace, size_t ram_count, uint8_t* next) {
    *next++ = actual.exceeded;
    auto* mask_data = next; next += 2;
    const auto before = registers(initial), after = registers(actual.cpu);
    uint16_t mask = 0;
    for (size_t i = 0; i < after.size(); ++i) {
        if (before[i] == after[i]) continue;
        mask |= 1u << i;
        if (i < 2) { store16(next, after[i]); next += 2; }
        else *next++ = static_cast<uint8_t>(after[i]);
    }
    store16(mask_data, mask);
    store16(next, actual.mcycles); next += 2;
    *next++ = static_cast<uint8_t>(actual.count);
    store16(next, static_cast<uint16_t>(ram_count)); next += 2;
    for (size_t i = 0; i < ram_count; ++i) {
        store16(next, addresses[i]); next[2] = actual.memory[addresses[i]]; next += 3;
    }
    for (const auto& sample : trace.first(actual.count)) {
        store16(next, sample.address); next[2] = sample.data; next[3] = sample.signals; next += 4;
    }
    return next;
}

bool same(const Actual& a, const Actual& b, size_t ram_count,
          std::span<const uint8_t> expected_samples) {
    if (registers(a.cpu) != registers(b.cpu) || a.mcycles != b.mcycles || a.count != b.count) return false;
    for (size_t i = 0; i < ram_count; ++i)
        if (a.memory[addresses[i]] != b.memory[addresses[i]]) return false;
    for (size_t i = 0; i < a.count; ++i) {
        const auto& x = samples[0][i]; const auto& y = samples[1][i];
        // Transfer pins are meaningful even when the expected observation did
        // not describe a transfer. Ignore only disconnected idle/hold pins.
        const auto valid = i * 4 < expected_samples.size() ? expected_samples[i * 4 + 3] & 3 : 0;
        if (x.signals != y.signals || (((x.signals & 3) || (valid & 1)) && x.address != y.address) ||
            (((x.signals & 3) || (valid & 2)) && x.data != y.data)) return false;
    }
    return true;
}

// Returns zero for malformed input. Both memories remain available until the
// pair is compared; only observed addresses are inspected, not a full RAM scan.
size_t execute(std::span<const uint8_t> input, uint8_t* memory, gb::Sm83Jit& jit,
               uint8_t mode, uint8_t oracle, uint16_t limit, uint8_t* output) {
    Reader reader{input};
    reader.take(reader.u16());
    State initial{}, expected{};
    if (!state(reader, initial) || !state(reader, expected)) return 0;
    const auto observations = reader.u16();
    const auto expected_samples = reader.take(static_cast<size_t>(observations) * 4);
    if (!reader.valid || !reader.bytes.empty()) return 0;
    std::memset(memory, 0, 2 * 65536);
    for (size_t i = 0; i < initial.ram.size(); i += 3) {
        const auto* ram = initial.ram.data() + i;
        memory[load16(ram)] = memory[65536 + load16(ram)] = ram[2];
    }
    // One-instruction code is opcode-keyed and contains no per-case state.
    // Keep it across cases; CPU, RAM, bus pins and clocks are reset independently.
    Actual actual[2]{{initial.cpu, memory}, {initial.cpu, memory + 65536}};
    gb::JitFallback fallback = gb::JitFallback::none;
    for (unsigned backend = 0; backend < 2; ++backend) {
        auto& a = actual[backend];
        gb::FlatBus flat{std::span<uint8_t, 65536>(a.memory, 65536)};
        gb::TraceBus trace(flat.bus(), samples[backend]);
        const auto bus = trace.bus();
        if (backend) fallback = jit.run_block(a.cpu, bus, 1).fallback;
        else gb::sm83_step(a.cpu, bus);
        while (a.cpu.mode != gb::CpuMode::running && trace.size() < observations && trace.size() < limit)
            gb::sm83_wait(a.cpu, bus);
        a.exceeded = observations > limit || trace.size() > limit || trace.overflowed();
        a.count = std::min(trace.size(), samples[backend].size());
        a.mcycles = static_cast<uint16_t>(flat.mcycles());
    }
    size_t ram_count = 0;
    for (size_t i = 0; i < expected.ram.size(); i += 3) addresses[ram_count++] = load16(expected.ram.data() + i);
    for (unsigned backend = 0; backend < 2; ++backend)
        for (size_t i = 0; i < actual[backend].count; ++i)
            if (samples[backend][i].signals & 2) addresses[ram_count++] = samples[backend][i].address;
    std::sort(addresses.begin(), addresses.begin() + ram_count);
    ram_count = std::unique(addresses.begin(), addresses.begin() + ram_count) - addresses.begin();
    const auto oracle_result = actual[0].exceeded ? kLimit : (mode == kDiagnostic || oracle == kReferenceOracle) ? kUnjudged :
        matches(expected, expected_samples, actual[0].cpu, memory, actual[0].count) ? kPass : kFail;
    const auto pair_result = (actual[0].exceeded || actual[1].exceeded) ? kLimit : mode == kDiagnostic ? kUnjudged :
        same(actual[0], actual[1], ram_count, expected_samples) ? kPass : kFail;
    const bool has_actual = mode == kDiagnostic || (oracle == kSuiteOracle && oracle_result != kPass) ||
        pair_result != kPass || fallback != gb::JitFallback::none;
    output[0] = oracle_result | (pair_result << 2) | (static_cast<uint8_t>(fallback) << 4) | (has_actual << 7);
    if (!has_actual) return 1;
    auto* next = encode(initial.cpu, actual[0], samples[0], ram_count, output + 1);
    next = encode(initial.cpu, actual[1], samples[1], ram_count, next);
    return static_cast<size_t>(next - output);
}

}  // namespace

Sm83Runner::~Sm83Runner() {
    if (file_) std::fclose(file_);
    std::free(memory_);
    std::free(group_data_);
}

bool Sm83Runner::open(const char* path) {
    if (file_) return false;
    file_ = std::fopen(path, "rb");
    uint8_t header[24];
    if (!file_ || std::fread(header, 1, 16, file_) != 16 ||
        std::memcmp(header, "GBXSM83\0", 8) || load32(header + 8) != 1) return false;
    group_count_ = load32(header + 12);
    if (!group_count_ || group_count_ > groups_.size()) return false;
    uint64_t end = 16 + group_count_ * 24;
    for (size_t i = 0; i < group_count_; ++i) {
        if (std::fread(header, 1, 24, file_) != 24) return false;
        auto& group = groups_[i];
        group = {load16(header), load32(header + 4), load32(header + 8), load32(header + 16)};
        if (load16(header + 2) || load32(header + 12) || load32(header + 20) ||
            group.offset != end || !group.count || !group.size || group.size > 256 * 1024 ||
            (i && group.opcode <= groups_[i - 1].opcode)) return false;
        end += group.size;
        capacity_ = std::max(capacity_, static_cast<size_t>(group.size));
    }
    if (std::fseek(file_, 0, SEEK_END) || std::ftell(file_) != static_cast<long>(end)) return false;
    memory_ = static_cast<uint8_t*>(std::malloc(2 * 65536));
    group_data_ = static_cast<uint8_t*>(std::malloc(capacity_));
    return memory_ && group_data_;
}

bool Sm83Runner::run(std::span<const uint8_t> request) {
    if (!file_ || !memory_ || !group_data_) return error("SM83 data is unavailable");
    // The JIT takes its internal code pool on first use, so a session that
    // runs only ROM tests leaves internal RAM to the larger pool of a ROM job.
    if (!jit_.available()) jit_.initialize(std::span<const uint8_t, 65536>(memory_ + 65536, 65536));
    if (request.size() < 20 || load16(request.data()) != kVersion ||
        request[2] != kPaired || request[3] != kSm83Flat || !jit_.available())
        return error("Paired SM83 execution is unavailable or incompatible");
    const auto id = load32(request.data() + 4);
    const auto opcode = load16(request.data() + 8);
    const auto limit = load16(request.data() + 10);
    const auto count = load16(request.data() + 12);
    const auto mode = request[14], selection = request[15], oracle = request[16];
    if (!id || !limit || limit > kMaxSamples || !count || count > selected_.size() ||
        mode > kDiagnostic || selection > 1 || oracle > kReferenceOracle ||
        request[17] || request[18] || request[19] ||
        request.size() != 20 + (selection ? 4u : count * 4u))
        return error("Invalid execution limits or selection");
    const Group* group = nullptr;
    for (size_t i = 0; i < group_count_; ++i) if (groups_[i].opcode == opcode) group = &groups_[i];
    if (!group) return error("Opcode group is missing");
    // Copy before sending: transport sends invalidate the incoming payload view.
    const auto first = load32(request.data() + 20);
    if (selection && (first >= group->count || count > group->count - first))
        return error("Invalid case range");
    for (size_t i = 0; i < count; ++i) {
        selected_[i] = selection ? first + i : load32(request.data() + 20 + i * 4);
        if (selected_[i] >= group->count || (i && selected_[i] <= selected_[i - 1]))
            return error("Invalid case index");
    }
    if (std::fseek(file_, group->offset, SEEK_SET) ||
        std::fread(group_data_, 1, group->size, file_) != group->size) return error("Cannot read opcode group");
    Reader reader{{group_data_, group->size}};
    size_t size = kBatchHeader;
    uint16_t records = 0;
    size_t selected = 0;
    auto flush = [&]() {
        store16(batch.data(), kVersion); batch[2] = mode; batch[3] = oracle;
        store32(batch.data() + 4, id);
        store16(batch.data() + 8, static_cast<uint16_t>(selected - records));
        store16(batch.data() + 10, records);
        const bool ok = transport::send_data(std::span(batch).first(size));
        records = 0; size = kBatchHeader;
        return ok;
    };
    for (uint32_t index = 0; index < group->count; ++index) {
        const auto length = reader.u32();
        const auto input = reader.take(length);
        if (!reader.valid) return error("Truncated SM83 case");
        if (selected == count || index != selected_[selected]) continue;
        if (size + kMaxRecord > batch.size() && !flush()) return false;
        const auto result_size = execute(input, memory_, jit_, mode, oracle, limit, batch.data() + size);
        if (!result_size) return error("Invalid SM83 case");
        size += result_size; ++records; ++selected;
    }
    if (!reader.bytes.empty() || selected != count) return error("Opcode group length mismatch");
    if ((records && !flush()) || !transport::finish()) return false;
    uint8_t done[12]{};
    store16(done, kVersion); store32(done + 4, id); store32(done + 8, count);
    return transport::send(protocol::Kind::done, done);
}

}  // namespace gbx::test
