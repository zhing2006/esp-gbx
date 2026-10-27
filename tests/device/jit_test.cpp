#include "jit_test.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "gbx/executable.h"
#include "gbx/jit/assembler.h"
#include "gbx/jit/cache.h"
#include "gbx/jit/store.h"
#include "transport.h"
#include "sm83_blocks.h"
#include "arm7_blocks.h"
#include "gba_blocks.h"
#include "mapped_blocks.h"
#include "mapper_dma.h"
#include "cpu_peripherals.h"

extern "C" uint32_t gbx_jit_abi_probe(gbx::jit::BlockEntry entry, void* context);
extern "C" uint32_t gbx_rv32_probe(const void* code, uint32_t* io);
extern "C" const void* const gbx_rv32_cases[];
extern "C" const uint32_t gbx_rv32_case_count;

namespace gbx::test::jit_test {
namespace {
using namespace jit;
using namespace jit::rv32;

// These are native-code execution fixtures, not an implementation of an emulated
// CPU. Expected results follow RV32I/M and psABI; the PC runner judges them.
class Checks {
public:
    explicit Checks(uint32_t request, uint16_t expected = kCheckCount) : request_(request), expected_(expected) {}
    bool start() { return cache_.initialize(); }
    BlockEntry compile(const Assembler& code) {
        if (!code.valid()) { ok_ = false; return nullptr; }
        cache_.clear();
        const auto block = cache_.install(0, code.code());
        if (block.status != InstallStatus::ok) ok_ = false;
        return block.entry;
    }
    bool result(uint32_t actual) {
        if (!ok_) return false;
        uint8_t data[12];
        protocol::store16(data, kVersion);
        protocol::store16(data + 2, count_++);
        protocol::store32(data + 4, request_);
        protocol::store32(data + 8, actual);
        ok_ = transport::send_data(data);
        return ok_;
    }
    bool execute(const Assembler& code, void* context = nullptr) {
        const auto entry = compile(code);
        return entry && result(entry(context));
    }
    bool finish() {
        if (!ok_ || count_ != expected_ || !transport::finish()) return false;
        uint8_t done[8];
        protocol::store16(done, kVersion);
        protocol::store16(done + 2, count_);
        protocol::store32(done + 4, request_);
        return transport::send(protocol::Kind::done, done);
    }
private:
    CodeCache cache_;
    uint32_t request_;
    uint16_t expected_;
    uint16_t count_ = 0;
    bool ok_ = true;
};

struct Context { uint32_t input[2]; uint32_t output, calls, stack_mod16, returned; };
static_assert(offsetof(Context, returned) == 20);

__attribute__((noinline)) uint32_t helper(Context* context, uint32_t a, uint32_t b) {
    uintptr_t sp;
    asm volatile("mv %0, sp" : "=r"(sp));
    context->stack_mod16 = sp & 15;
    ++context->calls;
    context->output = a * b + context->input[0];
    // Force the generated caller to obey caller-saved register rules, even in
    // debug builds whose C++ register allocation may otherwise leave them alone.
    asm volatile("li t0, 91\nli t1, 92\nli t2, 93\nli t3, 94\nli t4, 95\nli t5, 96\nli t6, 97"
                 ::: "t0", "t1", "t2", "t3", "t4", "t5", "t6");
    return context->output;
}

uint32_t cache_erase() {
    CodeCache cache;
    if (!cache.initialize(256, 8)) return 0;
    // These keys form a cluster wrapping from bucket 14 through 3, plus a
    // separate cluster at 7. Some entries are at home inside the first cluster.
    constexpr std::array<uint64_t, 8> keys{19, 14, 0, 33, 5, 28, 7, 21};
    std::array<unsigned, 8> values{10, 11, 12, 13, 14, 15, 16, 17};
    unsigned live = 0;
    auto install = [&](unsigned i) {
        std::array<uint32_t, 4> words{};
        Assembler code(words);
        code.li(Reg::a0, values[i]); code.emit(ret());
        const auto result = cache.install_replacing(keys[i], code.code());
        if (!result.entry || result.evicted) return false;
        live |= 1u << i;
        return true;
    };
    auto verify = [&] {
        unsigned count = 0;
        for (unsigned i = 0; i < keys.size(); ++i) {
            const auto entry = cache.find(keys[i]);
            if (live & (1u << i)) {
                if (!entry || entry(nullptr) != values[i]) return false;
                ++count;
            } else if (entry) return false;
        }
        return cache.used_bytes() == count * 8;
    };
    for (unsigned i = 0; i < keys.size(); ++i) if (!install(i)) return 0;
    if (!verify()) return 0;
    constexpr std::array<unsigned, 8> order{0, 2, 5, 7, 1, 6, 3, 4};
    for (const auto i : order) {
        if (!cache.erase(keys[i])) return 0;
        live &= ~(1u << i);
        if (!verify()) return 0;
        values[i] += 16;
        if (!install(i) || !verify()) return 0;
    }
    for (const auto i : order) {
        if (!cache.erase(keys[i])) return 0;
        live &= ~(1u << i);
        if (!verify()) return 0;
    }
    return 1;
}

// The second level: blocks stored over three segments and past the end of
// the lap run their relocated calls into their segment's copy of the shared
// code, including blocks written where dropped ones ran before; the oldest
// records are dropped first, each retired once with its code range, and
// the calls report the targets they were stored with.
constexpr unsigned kStoreBlocks = 4000, kStoreWords = 256;
uint32_t code_store() {
    CodeCache cache;
    if (!cache.initialize(1024, 4)) return 0;
    std::array<uint32_t, 2> shared_words{};
    Assembler shared(shared_words);
    shared.emit(addi(Reg::a0, Reg::a0, 1000)); shared.emit(ret());
    const auto origin = cache.install(0, shared.code());
    if (!origin.entry) return 0;
    const auto origin_address = reinterpret_cast<uintptr_t>(origin.entry);
    CodeStore store;
    if (!store.initialize(3 * CodeStore::kSegmentBytes, kStoreBlocks, shared.code(), origin_address)) return 0;
    struct Retired { unsigned count; bool sizes; } retired{0, true};
    const auto on_retire = [](void* context, uintptr_t begin, uintptr_t end) {
        auto& r = *static_cast<Retired*>(context);
        ++r.count;
        r.sizes &= end - begin == kStoreWords * 4;
    };
    std::array<uint32_t, kStoreWords> words{};
    uintptr_t first = 0;
    bool reused = false;
    for (unsigned i = 0; i < kStoreBlocks; ++i) {
        Assembler code(words);
        code.emit(addi(Reg::a5, Reg::ra, 0));
        const Relocation call{static_cast<uint32_t>(code.offset()), origin_address};
        code.emit(jal(Reg::ra, 0));
        code.emit(addi(Reg::a0, Reg::a0, static_cast<int32_t>(i % 1024)));
        code.emit(jalr(Reg::zero, Reg::a5, 0));
        while (code.offset() < kStoreWords * 4) code.emit(nop());
        const auto copy = store.store(i, code.code(), {&call, 1}, {}, on_retire, &retired);
        if (copy.empty()) return 0;
        const auto at = reinterpret_cast<uintptr_t>(copy.data());
        if (!first) first = at;
        else reused |= at <= first;
    }
    bool runs = true, order = true, targets = true;
    unsigned live = 0;
    for (unsigned i = 0; i < kStoreBlocks; ++i) {
        CodeStore::Record record;
        const bool found = store.find(i, record);
        if (found) {
            ++live;
            runs &= reinterpret_cast<BlockEntry>(record.code.data())(nullptr) == 1000 + i % 1024;
            targets &= record.relocations.size() == 1 && store.target(record, 0) == origin_address;
        }
        order &= found == (i >= kStoreBlocks - store.records());
    }
    uint32_t result = 0;
    if (runs && live) result |= 1;
    if (order && live == store.records() && retired.count == store.drops() &&
        retired.count + live == kStoreBlocks && retired.sizes) result |= 2;
    if (targets) result |= 4;
    if (reused) result |= 8;
    return result;
}

// The Zcb, Zba, Zbb and Zbs cases in rv32_extensions.S, then the same
// instructions from the encoders that code generation uses: the result, or
// 0xbad00000 | the exception code if the instruction trapped.
bool extensions(Checks& checks) {
    if (gbx_rv32_case_count + 14 != kExtensionCheckCount) return false;
    const auto probe = [&](const void* code) {
        uint32_t io[4]{0x0400f0a8, 0xffffffda, 0x8001aa80, 0};
        const auto result = gbx_rv32_probe(code, io);
        return checks.result(io[3] ? 0xbad00000 | (io[3] & 0xfff) : result);
    };
    for (uint32_t i = 0; i < gbx_rv32_case_count; ++i) if (!probe(gbx_rv32_cases[i])) return false;
    for (const auto instruction : {sh3add(Reg::a0, Reg::a1, Reg::a2), zext_h(Reg::a0, Reg::a1),
                                   sext_b(Reg::a0, Reg::a1), bexti(Reg::a0, Reg::a1, 7),
                                   andn(Reg::a0, Reg::a1, Reg::a2), ror(Reg::a0, Reg::a1, Reg::a2),
                                   rori(Reg::a0, Reg::a1, 12), maxu(Reg::a0, Reg::a1, Reg::a2),
                                   clz(Reg::a0, Reg::a1), sh1add(Reg::a0, Reg::a1, Reg::a2),
                                   sh2add(Reg::a0, Reg::a1, Reg::a2), bclri(Reg::a0, Reg::a1, 15),
                                   binvi(Reg::a0, Reg::a1, 31), bseti(Reg::a0, Reg::a1, 9)}) {
        std::array<uint32_t, 2> words{};
        Assembler code(words);
        code.emit(instruction); code.emit(ret());
        const auto entry = checks.compile(code);
        if (!entry || !probe(reinterpret_cast<const void*>(entry))) return false;
    }
    return true;
}

bool execute_checks(uint32_t request_id, uint16_t group) {
    if (group == 2) {
        Checks checks(request_id, kExtensionCheckCount);
        return checks.start() && extensions(checks) && checks.finish();
    }
    const bool memory_only = group == 1;
    // Release the program fixture's pool before allocating the backend pool.
    std::array<uint32_t, 8> programs{};
    if (!memory_only && !sm83_blocks(programs)) return false;
    std::array<uint32_t, 5> arm7{};
    if (!memory_only && !arm7_blocks(arm7)) return false;
    std::array<uint32_t, 18> mapped{};
    if (!mapped_blocks(mapped)) return false;
    std::array<uint32_t, 10> peripherals{};
    if (!cpu_peripherals(peripherals)) return false;
    std::array<uint32_t, 10> mapper{};
    if (!mapper_dma(mapper)) return false;
    std::array<uint32_t, 2> gba{};
    if (!gba_blocks(gba)) return false;
    const auto erased = cache_erase();
    Checks checks(request_id, memory_only ? kMemoryCheckCount : kCheckCount);
    if (memory_only) {
        for (const auto result : mapped) if (!checks.result(result)) return false;
        if (!checks.result(erased)) return false;
        for (const auto result : peripherals) if (!checks.result(result)) return false;
        for (const auto result : mapper) if (!checks.result(result)) return false;
        for (const auto result : gba) if (!checks.result(result)) return false;
        return checks.finish();
    }
    if (!checks.start()) return false;
    std::array<uint32_t, 256> storage{};
    constexpr uint32_t constants[]{0, 1, 0x7ff, 0x800, 0xfffff800, 0xfffff7ff,
                                  0xffffffff, 0x80000000, 0x7fffffff, 0x12345800, 0xfff, 0x1000};
    for (auto value : constants) {
        Assembler code(storage);
        code.li(Reg::a0, value);
        code.emit(ret());
        if (!checks.execute(code)) return false;
    }
    using Binary = Instruction (*)(Reg, Reg, Reg);
    constexpr Binary operations[]{add, sub, sll, slt, sltu, xor_, srl, sra, or_, and_,
                                  mul, mulh, mulhsu, mulhu, rv32::div, divu, rem, remu};
    uint32_t operands[]{0x80000003, 5};
    for (auto operation : operations) {
        Assembler code(storage);
        code.emit(lw(Reg::t0, Reg::a0, 0));
        code.emit(lw(Reg::t1, Reg::a0, 4));
        code.emit(operation(Reg::a0, Reg::t0, Reg::t1));
        code.emit(ret());
        if (!checks.execute(code, operands)) return false;
    }
    operands[0] = 0x80000000;
    for (auto divisor : {0u, 0xffffffffu}) {
        operands[1] = divisor;
        for (Binary operation : {rv32::div, rem}) {
            Assembler code(storage);
            code.emit(lw(Reg::t0, Reg::a0, 0));
            code.emit(lw(Reg::t1, Reg::a0, 4));
            code.emit(operation(Reg::a0, Reg::t0, Reg::t1));
            code.emit(ret());
            if (!checks.execute(code, operands)) return false;
        }
    }
    using Branch = Instruction (*)(Reg, Reg, int32_t);
    constexpr Branch branches[]{beq, bne, blt, bge, bltu, bgeu};
    for (auto branch : branches) {
        Assembler code(storage);
        code.emit(lw(Reg::t0, Reg::a0, 0));
        code.emit(lw(Reg::t1, Reg::a0, 4));
        code.emit(branch(Reg::t0, Reg::t1, 12));
        code.li(Reg::a0, 0);
        code.emit(ret());
        code.li(Reg::a0, 1);
        code.emit(ret());
        const auto entry = checks.compile(code);
        if (!entry) return false;
        uint32_t mask = 0;
        constexpr uint32_t values[][2]{{0xffffffff, 1}, {1, 0xffffffff}, {1, 1}};
        for (unsigned i = 0; i < 3; ++i) {
            operands[0] = values[i][0]; operands[1] = values[i][1];
            mask |= entry(operands) << i;
        }
        if (!checks.result(mask)) return false;
    }
    {
        Assembler code(storage);
        code.li(Reg::a0, 0); code.li(Reg::t0, 10);
        const auto loop = code.offset();
        code.emit(add(Reg::a0, Reg::a0, Reg::t0));
        code.emit(addi(Reg::t0, Reg::t0, -1));
        code.emit(bne(Reg::t0, Reg::zero, static_cast<int32_t>(loop) - static_cast<int32_t>(code.offset())));
        const auto skip = code.offset();
        code.emit(nop());
        code.li(Reg::a0, 99);
        code.patch(skip, jal(Reg::zero, static_cast<int32_t>(code.offset() - skip)));
        code.emit(ret());
        if (!checks.execute(code)) return false;
    }
    // C extension: each sequence must compress in full, or it reports zero.
    const auto compressed = [&](const Assembler& code, size_t bytes, void* context = nullptr) {
        return code.offset() == bytes ? checks.execute(code, context) : checks.result(0);
    };
    {
        Assembler code(storage, true);
        code.emit(addi(Reg::a0, Reg::zero, -5)); code.emit(addi(Reg::a0, Reg::a0, 17));
        code.emit(lui(Reg::a1, 31)); code.emit(lui(Reg::a2, 0xfffe0));
        code.emit(add(Reg::a0, Reg::a0, Reg::a1)); code.emit(add(Reg::a0, Reg::a2, Reg::a0));
        code.emit(ret());
        if (!compressed(code, 14)) return false;
    }
    {
        Assembler code(storage, true);
        code.emit(addi(Reg::a1, Reg::zero, 29)); code.emit(addi(Reg::a2, Reg::zero, 6));
        code.emit(addi(Reg::a3, Reg::a1, 0)); code.emit(sub(Reg::a3, Reg::a3, Reg::a2));
        code.emit(xor_(Reg::a3, Reg::a3, Reg::a1)); code.emit(or_(Reg::a3, Reg::a2, Reg::a3));
        code.emit(and_(Reg::a3, Reg::a3, Reg::a1)); code.emit(andi(Reg::a3, Reg::a3, -8));
        code.emit(add(Reg::a0, Reg::zero, Reg::a3)); code.emit(ret());
        if (!compressed(code, 20)) return false;
    }
    {
        Assembler code(storage, true);
        code.emit(lui(Reg::a0, 0xfffe0)); code.emit(addi(Reg::a0, Reg::a0, 5));
        code.emit(srai(Reg::a0, Reg::a0, 4)); code.emit(srli(Reg::a0, Reg::a0, 8));
        code.emit(slli(Reg::a0, Reg::a0, 3)); code.emit(ret());
        if (!compressed(code, 12)) return false;
    }
    {
        Assembler code(storage, true);
        code.emit(addi(Reg::sp, Reg::sp, -64)); code.emit(addi(Reg::a0, Reg::zero, 21));
        code.emit(sw(Reg::a0, Reg::sp, 60)); code.emit(addi(Reg::a1, Reg::sp, 32));
        code.emit(addi(Reg::a2, Reg::zero, -7)); code.emit(sw(Reg::a2, Reg::a1, 4));
        code.emit(lw(Reg::a4, Reg::sp, 36)); code.emit(lw(Reg::a5, Reg::a1, 28));
        code.emit(sub(Reg::a5, Reg::a5, Reg::a4)); code.emit(addi(Reg::sp, Reg::sp, 64));
        code.emit(addi(Reg::a0, Reg::a5, 0)); code.emit(ret());
        if (!compressed(code, 24)) return false;
    }
    {
        Assembler code(storage, true);
        code.emit(lw(Reg::a1, Reg::a0, 0)); code.emit(addi(Reg::a0, Reg::zero, 0));
        code.emit(beq(Reg::a1, Reg::zero, 4)); code.emit(addi(Reg::a0, Reg::a0, 1));
        code.emit(bne(Reg::a1, Reg::zero, 4)); code.emit(addi(Reg::a0, Reg::a0, 2));
        code.emit(ret());
        uint32_t mask = 0;
        if (const auto entry = code.offset() == 14 ? checks.compile(code) : nullptr) {
            constexpr uint32_t values[]{0, 1, 0x80000000};
            for (unsigned i = 0; i < 3; ++i) { operands[0] = values[i]; mask |= entry(operands) << (i * 2); }
        }
        if (!checks.result(mask)) return false;
    }
    {
        // A loop, a call to the code at 16 by c.jal and by c.jalr, and c.j
        // over that code.
        Assembler code(storage, true);
        code.emit(addi(Reg::a5, Reg::ra, 0));
        code.emit(addi(Reg::a0, Reg::zero, 0)); code.emit(addi(Reg::a1, Reg::zero, 5));
        code.emit(addi(Reg::a0, Reg::a0, 3)); code.emit(addi(Reg::a1, Reg::a1, -1));
        code.emit(bne(Reg::a1, Reg::zero, -4));
        code.emit(jal(Reg::ra, 4)); code.emit(jal(Reg::zero, 6));
        code.emit(addi(Reg::a0, Reg::a0, 7)); code.emit(ret());
        code.emit(auipc(Reg::a2, 0)); code.emit(addi(Reg::a2, Reg::a2, -4));
        code.emit(jalr(Reg::ra, Reg::a2, 0)); code.emit(jalr(Reg::zero, Reg::a5, 0));
        if (!compressed(code, 30)) return false;
    }
    {
        // A relocated call and 32-bit instructions two bytes past a word.
        CodeCache cache;
        if (!cache.initialize(1024, 4)) return false;
        Assembler callee(storage); callee.li(Reg::a0, 5); callee.emit(ret());
        const auto target = cache.install(0, callee.code());
        std::array<uint32_t, 16> words{};
        Assembler code(words, true);
        code.emit(addi(Reg::a5, Reg::ra, 0));
        const Relocation call{static_cast<uint32_t>(code.offset()), reinterpret_cast<uintptr_t>(target.entry)};
        code.emit_exact(jal(Reg::ra, 0));
        code.emit(addi(Reg::a0, Reg::a0, 300)); code.emit(lui(Reg::a1, 0x12345));
        code.emit(add(Reg::a0, Reg::a0, Reg::a1)); code.emit(jalr(Reg::zero, Reg::a5, 0));
        const auto caller = target.entry && code.offset() == 18 ? cache.install(1, code.code(), {&call, 1}) : InstallResult{};
        if (!checks.result(caller.entry ? caller.entry(nullptr) : 0)) return false;
    }
    if (!extensions(checks)) return false;
    using Load = Instruction (*)(Reg, Reg, int32_t);
    uint32_t memory = 0x8001aa80;
    for (auto load : std::array<Load, 4>{lb, lbu, lh, lhu}) {
        Assembler code(storage);
        code.emit(load(Reg::a0, Reg::a0, (load == lh || load == lhu) ? 2 : 0));
        code.emit(ret());
        if (!checks.execute(code, &memory)) return false;
    }
    {
        memory = 0x11223344;
        Assembler code(storage);
        code.li(Reg::t0, 0xaa); code.emit(sb(Reg::t0, Reg::a0, 1));
        code.li(Reg::t0, 0x8001); code.emit(sh(Reg::t0, Reg::a0, 2));
        code.emit(lw(Reg::a0, Reg::a0, 0)); code.emit(ret());
        if (!checks.execute(code, &memory)) return false;
    }
    {
        Context context{{5, 0}, 0, 0, 0, 0};
        Assembler code(storage);
        code.begin_function();
        code.li(Reg::a1, 7); code.li(Reg::a2, 9);
        code.call(reinterpret_cast<uintptr_t>(&helper));
        code.emit(sw(Reg::a0, Reg::s0, offsetof(Context, returned)));
        code.li(Reg::a0, 0x81);
        code.end_function();
        const auto entry = checks.compile(code);
        if (!entry || !checks.result(entry(&context)) || !checks.result(context.returned) ||
            !checks.result(context.calls) || !checks.result(context.stack_mod16) ||
            !checks.result(gbx_jit_abi_probe(entry, &context))) return false;
    }
    {
        CodeCache cache;
        if (!cache.initialize(1024, 4)) return false;
        BlockEntry first = nullptr;
        for (auto value : {0x12345678u, 0x89abcdefu}) {
            cache.clear();
            Assembler code(storage);
            // Warm several cache lines, then rewrite the same published range.
            for (unsigned i = 0; i < 64; ++i) code.emit(nop());
            code.li(Reg::a0, value); code.emit(ret());
            const auto block = cache.install(value, code.code());
            if (!block.entry || !checks.result(block.entry(nullptr))) return false;
            if (!first) first = block.entry;
            else if (!checks.result(first == block.entry) || !checks.result(cache.find(0x12345678) == nullptr)) return false;
        }
    }
    {
        // The same rewrite in PSRAM, where the JIT's second level runs code.
        auto memory = platform::allocate_executable(1024, platform::CodeMemory::external);
        for (auto value : {0x12345678u, 0x89abcdefu}) {
            Assembler code(storage);
            for (unsigned i = 0; i < 64; ++i) code.emit(nop());
            code.li(Reg::a0, value); code.emit(ret());
            uint32_t actual = 0;
            if (memory.words && code.valid()) {
                volatile uint32_t* out = memory.words;
                for (size_t i = 0; i < code.code().size(); ++i) out[i] = code.code()[i];
                if (platform::publish_executable(memory, 0, code.code().size_bytes()))
                    actual = reinterpret_cast<BlockEntry>(memory.words)(nullptr);
            }
            if (!checks.result(actual)) { platform::free_executable(memory); return false; }
        }
        platform::free_executable(memory);
    }
    if (!checks.result(code_store())) return false;
    {
        CodeCache cache;
        if (!cache.initialize(128, 4)) return false;
        Assembler full(storage);
        for (size_t i = 0; i < cache.capacity_bytes() / 4; ++i) full.emit(nop());
        const auto block = cache.install(0, full.code());
        const uint32_t instruction = ret().bits;
        if (!checks.result(block.entry && cache.install(1, {&instruction, 1}).status == InstallStatus::full)) return false;
        cache.clear();
        Assembler code(storage); code.li(Reg::a0, 42); code.emit(ret());
        const auto recovered = cache.install(1, code.code());
        if (!recovered.entry || !checks.result(recovered.entry(nullptr))) return false;
    }
    {
        CodeCache cache;
        if (!cache.initialize(1024, 2)) return false;
        Assembler code(storage); code.li(Reg::a0, 7); code.emit(ret());
        if (!cache.install(0, code.code()).entry || !cache.install(1, code.code()).entry ||
            !checks.result(cache.install(2, code.code()).status == InstallStatus::full)) return false;
    }
    {
        CodeCache cache;
        if (!cache.initialize(1024, 4)) return false;
        constexpr uint64_t keys[]{0, UINT64_C(1) << 32, UINT64_MAX};
        bool good = true;
        for (unsigned i = 0; i < 3; ++i) {
            Assembler code(storage); code.li(Reg::a0, 17 * (i + 1)); code.emit(ret());
            if (!cache.install(keys[i], code.code()).entry) return false;
        }
        for (unsigned i = 0; i < 3; ++i) {
            const auto entry = cache.find(keys[i]);
            good &= entry && entry(nullptr) == 17 * (i + 1);
        }
        if (!checks.result(good)) return false;
        cache.clear();
        if (!checks.result(cache.find(0) == nullptr && cache.find(UINT64_MAX) == nullptr)) return false;
    }
    for (const auto result : programs) if (!checks.result(result)) return false;
    for (const auto result : arm7) if (!checks.result(result)) return false;
    for (const auto result : mapped) if (!checks.result(result)) return false;
    if (!checks.result(erased)) return false;
    {
        CodeCache cache;
        if (!cache.initialize(256, 4)) return false;
        auto install = [&](uint64_t key, unsigned value, unsigned count = 16) {
            std::array<uint32_t, 64> words{};
            Assembler code(words);
            code.li(Reg::a0, value); code.emit(ret());
            while (code.offset() < count * 4) code.emit(nop());
            return cache.install_replacing(key, code.code());
        };
        bool good = true;
        for (unsigned i = 0; i < 4; ++i) good &= install(UINT64_MAX - i, i + 10).entry != nullptr;
        const auto first = install(4, 14);
        good &= first.entry && first.evicted == 1 && !cache.find(UINT64_MAX);
        auto hot = cache.find(UINT64_MAX - 1);
        good &= hot && hot(nullptr) == 11;
        const auto second = install(5, 15);
        good &= second.entry && second.evicted == 1 && !cache.find(UINT64_MAX - 2);
        for (const auto key : {UINT64_MAX - 1, UINT64_MAX - 3, uint64_t{4}, uint64_t{5}}) {
            const auto entry = cache.find(key);
            const auto expected = key > 5 ? UINT64_MAX - key + 10 : key + 10;
            good &= entry && entry(nullptr) == expected;
        }
        good &= cache.used_bytes() == 256;
        cache.clear();
        good &= install(0, 10, 12).entry && install(1, 11, 20).entry &&
            install(2, 12).entry && install(3, 13).entry;
        const auto merged = install(4, 14, 24);
        good &= merged.entry && merged.evicted == 2 && !cache.find(0) && !cache.find(1);
        const auto gap = install(5, 15, 8);
        good &= gap.entry && !gap.evicted && cache.used_bytes() == 256;
        for (unsigned i = 2; i < 6; ++i) {
            const auto entry = cache.find(i);
            good &= entry && entry(nullptr) == i + 10;
        }
        if (!checks.result(good)) return false;
    }
    for (const auto result : peripherals) if (!checks.result(result)) return false;
    for (const auto result : mapper) if (!checks.result(result)) return false;
    for (const auto result : gba) if (!checks.result(result)) return false;
    return checks.finish();
}
}  // namespace

bool run(uint32_t request_id, uint16_t group) {
    if (execute_checks(request_id, group)) return true;
    constexpr std::string_view message = "JIT backend check could not complete";
    transport::send(protocol::Kind::error,
        {reinterpret_cast<const uint8_t*>(message.data()), message.size()});
    return false;
}

}  // namespace gbx::test::jit_test
