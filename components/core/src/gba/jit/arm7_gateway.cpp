// The gateway of Arm7Jit: shared code before the code pool that blocks
// reach with `jal t6` (kLink). The entry is a psABI function; the native
// frame it opens (Frame in arm7_jit_runtime.h) holds the context for the
// whole run.

#include <bit>
#include <iterator>

#include "arm7_compiler.h"

namespace gbx::gba {

using namespace arm7_compiler;

bool Arm7Jit::generate_gateway(const Memory* memory, const StubCycles& cycles, Stubs& s, size_t& bytes) {
    constexpr int kContextCpu = offsetof(Context, cpu), kContextRemaining = offsetof(Context, remaining),
                  kContextMemory = offsetof(Context, memory),
                  kContextPrefetchBase = offsetof(Context, prefetch_base),
                  kContextPrefetchFlags = offsetof(Context, prefetch_flags),
                  kContextDispatch = offsetof(Context, dispatch), kContextTags = offsetof(Context, tags),
                  kContextLink = offsetof(Context, link), kContextSpent = offsetof(Context, spent);
    constexpr int kStateR = offsetof(Arm7State, r), kStateSequentialFlag = offsetof(Arm7State, sequential);
    jit::Assembler out(output_, true);
    const auto emit = [&](rv::Instruction instruction) { out.emit(instruction); };
    const auto mv = [&](R d, R s) { emit(rv::addi(d, s, 0)); };
    const auto at = [&] { return out.offset(); };
    const auto distance = [](size_t from, size_t to) {
        return static_cast<int32_t>(static_cast<int64_t>(to) - static_cast<int64_t>(from));
    };
    // A forward branch, and its target here.
    const auto forward = [&] {
        const auto from = at();
        out.emit_exact(rv::nop());
        return from;
    };
    const auto land = [&](size_t from, Branch op, R a, R b) { out.patch(from, op(a, b, distance(from, at()))); };
    constexpr R kCallerSaved[]{R::ra, R::t0, R::t1, R::t2, R::a0, R::a1, R::a2, R::a3,
                               R::a4, R::a5, R::a6, R::a7, R::t3, R::t4, R::t5, R::t6};
    constexpr R kCalleeSaved[]{R::ra, R::s0, R::s1, R::s2, R::s3, R::s4, R::s5,
                               R::s6, R::s7, R::s8, R::s9, R::s10, R::s11};
    const auto save = [&](R r) { emit(rv::sw(r, R::sp, host_slot(r))); };
    const auto restore = [&](R r) { emit(rv::lw(r, R::sp, host_slot(r))); };
    // Back to the block that called, `past` bytes after the call.
    const auto back = [&](int32_t past = 0) { emit(rv::jalr(R::zero, kLink, past)); };
    // C++ with the frame in a0, through the psABI's ra; the caller saved
    // ra and kLink.
    const auto call_c = [&](uintptr_t function) {
        emit(rv::addi(R::a0, R::sp, 0));
        out.li(R::t6, static_cast<uint32_t>(function));
        emit(rv::jalr(R::ra, R::t6, 0));
    };
    // A stub that calls a runtime helper with the frame in a0 and the
    // block's arguments in a1-a5, keeping every register but `result`, which
    // receives the helper's result.
    const auto helper = [&](uintptr_t function, R result) {
        for (const R r : kCallerSaved)
            if (r != result) save(r);
        call_c(function);
        // Events may have run: a wait loop measures its cycles afresh.
        emit(rv::sw(R::zero, R::sp, kFrameSpinSite));
        if (result != R::a0) mv(result, R::a0);
        for (const R r : kCallerSaved)
            if (r != result) restore(r);
        back();
    };
    // NZCV into bits 31-28 of `cpsr`, using `scratch`.
    const auto pack = [&](R cpsr, R scratch) {
        emit(rv::slli(cpsr, cpsr, 4));
        emit(rv::srli(cpsr, cpsr, 4));
        emit(rv::srli(scratch, kN, 31));
        emit(rv::slli(scratch, scratch, 31));
        emit(rv::or_(cpsr, cpsr, scratch));
        emit(rv::sltiu(scratch, kZ, 1));
        emit(rv::slli(scratch, scratch, 30));
        emit(rv::or_(cpsr, cpsr, scratch));
        emit(rv::slli(scratch, kC, 29));
        emit(rv::or_(cpsr, cpsr, scratch));
        emit(rv::srli(scratch, kV, 31));
        emit(rv::slli(scratch, scratch, 28));
        emit(rv::or_(cpsr, cpsr, scratch));
    };
    // The guest registers and NZCV to the Arm7State at `state`.
    const auto store_state = [&](R state, R cpsr, R scratch) {
        for (unsigned i = 0; i < kGuest.size(); ++i)
            emit(rv::sw(kGuest[i], state, kStateR + 4 * static_cast<int32_t>(i)));
        emit(rv::lw(cpsr, state, kStateCpsr));
        pack(cpsr, scratch);
        emit(rv::sw(cpsr, state, kStateCpsr));
    };
    s = {};

    // enter(context, block): the frame, the guest registers, NZCV and the
    // cycles left, then the block.
    s.enter = at();
    emit(rv::addi(R::sp, R::sp, -static_cast<int32_t>(sizeof(Frame))));
    for (unsigned i = 0; i < std::size(kCalleeSaved); ++i)
        emit(rv::sw(kCalleeSaved[i], R::sp, kFrameSaved + 4 * static_cast<int32_t>(i)));
    emit(rv::sw(R::a0, R::sp, kFrameContext));
    emit(rv::sw(R::zero, R::sp, kFrameSpinSite));
    emit(rv::lw(R::a2, R::a0, kContextCpu));
    emit(rv::sw(R::a2, R::sp, kFrameCpu));
    // The guest registers and NZCV from the Arm7State at `state`, through
    // `cpsr`.
    const auto load_state = [&](R state, R cpsr) {
        for (unsigned i = 0; i < kGuest.size(); ++i)
            emit(rv::lw(kGuest[i], state, kStateR + 4 * static_cast<int32_t>(i)));
        emit(rv::lw(cpsr, state, kStateCpsr));
        mv(kN, cpsr);
        out.extract_bit(kZ, cpsr, 30);
        emit(rv::xori(kZ, kZ, 1));
        out.extract_bit(kC, cpsr, 29);
        emit(rv::slli(kV, cpsr, 3));
    };
    load_state(R::a2, R::a3);
    emit(rv::lw(kRemaining, R::a0, kContextRemaining));
    // Mapped runs: the memory and the prefetch buffer native code owns.
    for (const auto& [from, to] : {std::pair{kContextMemory, kFrameMemory}, std::pair{kContextPrefetchBase, kFramePrefetchBase},
                                   std::pair{kContextPrefetchFlags, kFramePrefetchFlags}}) {
        emit(rv::lw(R::a3, R::a0, from));
        emit(rv::sw(R::a3, R::sp, to));
    }
    mv(R::t6, R::a1);
    emit(rv::jalr(R::zero, R::a1, 0));

    // The end of a run: the caller's registers back; a0 holds the exit.
    const auto epilogue = at();
    for (unsigned i = 0; i < std::size(kCalleeSaved); ++i)
        emit(rv::lw(kCalleeSaved[i], R::sp, kFrameSaved + 4 * static_cast<int32_t>(i)));
    emit(rv::addi(R::sp, R::sp, static_cast<int32_t>(sizeof(Frame))));
    emit(rv::ret());

    // exit: `jal t6, exit`, then R15 and the exit bits (exit_bits).
    // exit_synced does not write the guest registers and NZCV. Both keep a1
    // until they have read the S of the next fetch.
    s.exit = at();
    emit(rv::lw(R::a4, R::sp, kFrameCpu));
    store_state(R::a4, R::a2, R::a3);
    s.exit_synced = at();
    emit(rv::lw(R::a4, R::sp, kFrameCpu));
    emit(rv::lw(R::a5, kLink, 4));
    emit(rv::andi(R::a3, R::a5, static_cast<int32_t>(exit_bits::dynamic)));
    const auto dynamic = at();
    out.emit_exact(rv::nop());
    emit(rv::lw(R::a0, kLink, 0));
    out.patch(dynamic, rv::bne(R::a3, R::zero, distance(dynamic, at())));
    emit(rv::sw(R::a0, R::a4, kStateR15));
    out.extract_bit(R::a3, R::a5, 7);
    out.extract_bit(R::a2, R::a5, exit_bits::sequential_request);
    const auto requested = at();
    out.emit_exact(rv::nop());
    out.extract_bit(R::a3, R::a1, 4);
    out.patch(requested, rv::beq(R::a2, R::zero, distance(requested, at())));
    emit(rv::sb(R::a3, R::a4, kStateSequentialFlag));
    emit(rv::lw(R::a1, R::sp, kFrameContext));
    emit(rv::sw(kRemaining, R::a1, kContextRemaining));
    for (const auto& [from, to] : {std::pair{kFramePrefetchBase, kContextPrefetchBase},
                                   std::pair{kFramePrefetchFlags, kContextPrefetchFlags}}) {
        emit(rv::lw(R::a3, R::sp, from));
        emit(rv::sw(R::a3, R::a1, to));
    }
    // A linkable exit leaves the address of its link word for C++.
    out.extract_bit(R::a3, R::a5, std::countr_zero(exit_bits::linkable));
    const auto unlinkable = at();
    out.emit_exact(rv::nop());
    emit(rv::addi(R::a3, kLink, 8));
    out.patch(unlinkable, rv::beq(R::a3, R::zero, distance(unlinkable, at())));
    emit(rv::sw(R::a3, R::a1, kContextLink));
    emit(rv::andi(R::a3, R::a5, 127));
    emit(rv::lw(R::a2, R::a1, kContextInstructions));
    emit(rv::add(R::a2, R::a2, R::a3));
    emit(rv::sw(R::a2, R::a1, kContextInstructions));
    emit(rv::srli(R::a0, R::a5, exit_bits::reason));
    emit(rv::andi(R::a0, R::a0, 3));
    emit(rv::jal(R::zero, distance(at(), epilogue)));

    // side_exit: `jal t6, side_exit` and its descriptor (side_bits). C++
    // reads the registers from the frame and finishes the run.
    s.side_exit = at();
    for (unsigned r = 1; r < 32; ++r)
        if (r < 2 || r > 4) save(static_cast<R>(r));  // Not sp, gp or tp.
    mv(R::a1, kLink);
    call_c(reinterpret_cast<uintptr_t>(&side_exit));
    emit(rv::jal(R::zero, distance(at(), epilogue)));

    // m_boundary: mapped_boundary() at Frame::pc, a0 nonzero to stop.
    s.m_boundary = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_boundary), R::a0);
    // The same before a linked or dispatched block: mapped_link_boundary().
    const auto link_boundary = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_link_boundary), R::a0);
    // m_stop: where a boundary inside a mapped block finds no cycles left,
    // `jal t6, m_stop`, a jump back into the block, then the words of
    // side_exit: m_boundary at the boundary's R15, then the jump, or the
    // side exit when the run must stop. Keeps a1, which may hold the
    // request of the next fetch.
    s.m_stop = at();
#ifdef GBX_PROFILER
    emit(rv::lw(R::a0, R::sp, kFrameContext));
    emit(rv::lw(R::a3, R::a0, static_cast<int32_t>(offsetof(Context, counts)) + 16));
    emit(rv::addi(R::a3, R::a3, 1));
    emit(rv::sw(R::a3, R::a0, static_cast<int32_t>(offsetof(Context, counts)) + 16));
#endif
    mv(R::a3, kLink);
    emit(rv::lw(R::a0, kLink, 4));
    emit(rv::sw(R::a0, R::sp, kFramePc));
    emit(rv::jal(kLink, distance(at(), s.m_boundary)));
    mv(kLink, R::a3);
    const auto stop = forward();
    back();
    land(stop, rv::bne, R::a0, R::zero);
    emit(rv::addi(kLink, kLink, 4));
    emit(rv::jal(R::zero, distance(at(), s.side_exit)));

    // link: `jal t6, link`, R15, the exit bits and the link word, which
    // holds the target block's code once C++ linked the exit. The run goes
    // on there as at a boundary inside a block: with no cycles left, the
    // boundary's catch-up runs first, and the run leaves when it must stop.
    // Leaving through it, flags are computed and held registers free, so
    // that ra keeps the target's code until the block is entered. A direct
    // exit (exit_bits::direct_jump) comes here only without cycles left or
    // unlinked.
    // The boundary before the next block's first instruction, whose R15 is
    // in `r15`: branches to `stop` when the run must leave. Keeps kLink and
    // ra.
    const auto boundary = [&](R r15) {
        emit(rv::sw(r15, R::sp, kFramePc));
        mv(R::a5, kLink);
        emit(rv::jal(kLink, distance(at(), link_boundary)));
        mv(kLink, R::a5);
    };
    // Instructions completed by the exit at kLink, less those in a4 with
    // `skips`, and the S of the next fetch, to Arm7State at a4, through
    // a1-a3 and a5: S, the exit's bit or, with `requested`, a0.
    const auto complete = [&](bool sequential, bool requested = false, bool skips = false) {
        emit(rv::lw(R::a5, kLink, 4));
        emit(rv::andi(R::a3, R::a5, 127));
        emit(rv::lw(R::a1, R::sp, kFrameContext));
        emit(rv::lw(R::a2, R::a1, kContextInstructions));
        emit(rv::add(R::a2, R::a2, R::a3));
        if (skips) emit(rv::sub(R::a2, R::a2, R::a4));
        emit(rv::sw(R::a2, R::a1, kContextInstructions));
#ifdef GBX_PROFILER
        const int32_t count = static_cast<int32_t>(offsetof(Context, counts)) + (sequential ? 4 : 0);
        emit(rv::lw(R::a2, R::a1, count));
        emit(rv::addi(R::a2, R::a2, 1));
        emit(rv::sw(R::a2, R::a1, count));
#endif
        emit(rv::lw(R::a4, R::sp, kFrameCpu));
        if (sequential) emit(rv::addi(R::a3, R::zero, 1));
        else if (!requested) out.extract_bit(R::a3, R::a5, 7);
        emit(rv::sb(requested ? R::a0 : R::a3, R::a4, kStateSequentialFlag));
    };
    // link_requested is the same for an exit after a conditional
    // instruction whose paths fetch next with different S: a1 holds the
    // request of that fetch, as for exit, and the boundary's C++ keeps it.
    for (const bool requested : {false, true}) {
        (requested ? s.link_requested : s.link) = at();
        emit(rv::lw(R::ra, kLink, 8));
        const auto unlinked = at();
        out.emit_exact(rv::nop());
        const auto has_cycles = at();
        out.emit_exact(rv::nop());
        emit(rv::lw(R::a0, kLink, 0));
        boundary(R::a0);
        const auto stopped = at();
        out.emit_exact(rv::nop());
        out.patch(has_cycles, rv::blt(R::zero, kRemaining, distance(has_cycles, at())));
        if (requested) out.extract_bit(R::a0, R::a1, 4);
        complete(false, requested);
        mv(R::t6, R::ra);
        emit(rv::jalr(R::zero, R::t6, 0));
        out.patch(unlinked, rv::beq(R::ra, R::zero, distance(unlinked, s.exit)));
        out.patch(stopped, rv::bne(R::a0, R::zero, distance(stopped, s.exit)));
    }

    // dispatch: `jal t6, dispatch`, then words as for exit, with R15 in a0:
    // the block of the target from the dispatch table, Arm7Jit::kDispatch
    // entries of {R15, key's high word, code}. The high word is CPSR's T and
    // mode with the timing tag of R15's region (Arm7Jit::key()); from bit
    // 24 on, the entry's instructions that the run skips, for a boundary
    // inside a block. The run goes on there as through a link, but a
    // boundary's entry leaves for C++ when no cycles remain.
    s.dispatch = at();
#ifdef GBX_PROFILER
    // csrr a2, mcycle
    out.emit(rv::Instruction{0xb0000073u | 2u << 12 | static_cast<uint32_t>(R::a2) << 7, true});
    emit(rv::sw(R::a2, R::sp, static_cast<int32_t>(offsetof(Frame, profile))));
    emit(rv::lw(R::a1, R::sp, kFrameContext));
    emit(rv::lw(R::a2, R::a1, static_cast<int32_t>(offsetof(Context, counts)) + 12));
    emit(rv::addi(R::a2, R::a2, 1));
    emit(rv::sw(R::a2, R::a1, static_cast<int32_t>(offsetof(Context, counts)) + 12));
#endif
    emit(rv::lw(R::a4, R::sp, kFrameCpu));
    emit(rv::lw(R::a3, R::a4, kStateCpsr));
    emit(rv::andi(R::a3, R::a3, 0x3f));
    emit(rv::lw(R::a1, R::sp, kFrameContext));
    emit(rv::lw(R::a2, R::a1, kContextTags));
    emit(rv::srli(R::a5, R::a0, 24));
    emit(rv::add(R::a2, R::a2, R::a5));
    emit(rv::lbu(R::a2, R::a2, 0));
    emit(rv::slli(R::a2, R::a2, 6));
    emit(rv::or_(R::a3, R::a3, R::a2));
    emit(rv::srli(R::a5, R::a0, 1));
    emit(rv::add(R::a5, R::a5, R::a3));
    emit(rv::andi(R::a5, R::a5, static_cast<int32_t>(kDispatch - 1)));
    emit(rv::slli(R::a2, R::a5, 3));
    emit(rv::slli(R::a5, R::a5, 2));
    emit(rv::add(R::a5, R::a5, R::a2));
    emit(rv::lw(R::a2, R::a1, kContextDispatch));
    emit(rv::add(R::a5, R::a5, R::a2));
    emit(rv::lw(R::a2, R::a5, 0));
    const auto miss_pc = at();
    out.emit_exact(rv::nop());
    emit(rv::lw(R::a2, R::a5, 4));
    emit(rv::xor_(R::a3, R::a2, R::a3));
    emit(rv::slli(R::a3, R::a3, 8));
    const auto miss_key = at();
    out.emit_exact(rv::nop());
    emit(rv::srli(R::a4, R::a2, 24));
    emit(rv::lw(R::ra, R::a5, 8));
#ifdef GBX_PROFILER
    {
        const int32_t cycles = static_cast<int32_t>(offsetof(Context, counts)) + 20;
        out.emit(rv::Instruction{0xb0000073u | 2u << 12 | static_cast<uint32_t>(R::a2) << 7, true});
        emit(rv::lw(R::a3, R::sp, static_cast<int32_t>(offsetof(Frame, profile))));
        emit(rv::sub(R::a2, R::a2, R::a3));
        emit(rv::lw(R::a3, R::a1, cycles));
        emit(rv::add(R::a3, R::a3, R::a2));
        emit(rv::sw(R::a3, R::a1, cycles));
    }
#endif
    const auto dispatch_cycles = at();
    out.emit_exact(rv::nop());
    const auto boundary_entry = at();
    out.emit_exact(rv::nop());
    boundary(R::a0);
    emit(rv::addi(R::a2, R::a0, 0));
    emit(rv::lw(R::a0, R::sp, kFramePc));
    const auto dispatch_stopped = at();
    out.emit_exact(rv::nop());
    emit(rv::addi(R::a4, R::zero, 0));
    out.patch(dispatch_cycles, rv::blt(R::zero, kRemaining, distance(dispatch_cycles, at())));
    complete(true, false, true);
    mv(R::t6, R::ra);
    emit(rv::jalr(R::zero, R::t6, 0));
    out.patch(miss_pc, rv::bne(R::a2, R::a0, distance(miss_pc, s.exit)));
    out.patch(miss_key, rv::bne(R::a3, R::zero, distance(miss_key, s.exit)));
    out.patch(boundary_entry, rv::bne(R::a4, R::zero, distance(boundary_entry, s.exit)));
    out.patch(dispatch_stopped, rv::bne(R::a2, R::zero, distance(dispatch_stopped, s.exit)));

    // link_synced and dispatch_synced: the same after a helper that wrote
    // Arm7State and changed the mode or CPSR: the guest registers and NZCV
    // come from Arm7State first.
    for (const bool dynamic : {false, true}) {
        (dynamic ? s.dispatch_synced : s.link_synced) = at();
        emit(rv::lw(R::a4, R::sp, kFrameCpu));
        load_state(R::a4, R::a3);
        emit(rv::jal(R::zero, distance(at(), dynamic ? s.dispatch : s.link)));
    }

    // count_entry: a block start in the second level, where `jal t6,
    // count_entry` takes the place of the first word of the CLOCK mark, so
    // that t6 points at the second, which the stub skips with the third, at
    // kCode from the block's code. The entry is counted down in the last header word; once
    // the count runs out, the run leaves before the block, with
    // Context::spent and the count left at 1, so that C++ moves it into the
    // pool.
    s.count_entry = at();
    constexpr int kCode = -4, kHeader = kCode - 4 * static_cast<int32_t>(kHeaderWords);
    emit(rv::lw(R::a0, kLink, kCode - 4));
    emit(rv::addi(R::a0, R::a0, -1));
    const auto spent = at();
    out.emit_exact(rv::nop());
    emit(rv::sw(R::a0, kLink, kCode - 4));
    back(8);
    out.patch(spent, rv::bge(R::zero, R::a0, distance(spent, at())));
    emit(rv::lw(R::a4, R::sp, kFrameCpu));
    store_state(R::a4, R::a2, R::a3);
    emit(rv::lw(R::a0, kLink, kHeader));
    emit(rv::sw(R::a0, R::a4, kStateR15));
    emit(rv::lw(R::a1, R::sp, kFrameContext));
    emit(rv::sw(kRemaining, R::a1, kContextRemaining));
    for (const auto& [from, to] : {std::pair{kFramePrefetchBase, kContextPrefetchBase},
                                   std::pair{kFramePrefetchFlags, kContextPrefetchFlags}}) {
        emit(rv::lw(R::a3, R::sp, from));
        emit(rv::sw(R::a3, R::a1, to));
    }
    emit(rv::sw(R::zero, R::a1, kContextLink));
    emit(rv::addi(R::a3, kLink, kHeader));
    emit(rv::sw(R::a3, R::a1, kContextSpent));
    emit(rv::addi(R::a0, R::zero, static_cast<int32_t>(Arm7JitExit::limit)));
    emit(rv::jal(R::zero, distance(at(), epilogue)));

    // Mapped accesses and catch-ups: C++ runs MappedBus's paths.
    // m_catch_fetch_at, m_catch_at and m_idle_at take Frame::pc from the
    // word after the call and return past it.
    const auto pc_word = [&] {
        emit(rv::lw(R::a0, kLink, 0));
        emit(rv::sw(R::a0, R::sp, kFramePc));
        emit(rv::addi(kLink, kLink, 4));
    };
    s.m_catch_fetch_at = at();
    pc_word();
    s.m_catch_fetch = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_catch_fetch), R::a0);
    s.m_catch_at = at();
    pc_word();
    s.m_catch = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_catch), R::a0);
    s.m_idle_at = at();
    pc_word();
    s.m_idle = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_idle), R::a0);
    s.m_read = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_read), R::a0);
    s.m_write = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_write), R::a0);
    s.m_fetch = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_fetch), R::a0);
    s.m_refill = at();
    helper(reinterpret_cast<uintptr_t>(&mapped_refill), R::a0);

    // The cycles below are immediates of fixed size, so that new wait
    // states regenerate the gateway with the same layout (refresh_gateway()).
    const auto fixed = [&](rv::Instruction instruction) { out.emit_exact(instruction); };
    const auto charge_fixed = [&](unsigned value) { fixed(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(value))); };

    // m_rom_fetch: a code fetch from the ROM with the prefetch buffer, the
    // rules of Memory::rom_fetch() as Compiler::m_rom_fetch() emits them for
    // a fetch the buffer can hold 8 halfwords for. Returns past the word
    // after the call; uses a0-a3. A fetch native code does not time goes
    // to mapped_fetch().
    for (unsigned stub = 0; stub < s.m_rom_fetch.size(); ++stub) {
        s.m_rom_fetch[stub] = at();
        const bool thumb = stub < 3;
        const unsigned ws = stub % 3, halfword = cycles.s16[ws];
        const int32_t halves = thumb ? 1 : 2;
        // An S fetch, and the halfwords the buffer brings for it; an N fetch.
        const unsigned step = thumb ? halfword : cycles.s32[ws], n = thumb ? cycles.n16[ws] : cycles.n32[ws];
        // arrival - now of a fetch that finds the buffer full: at most this.
        const int32_t full = (halves - 8) * static_cast<int32_t>(halfword);
        const auto done = [&] { back(4); };
        emit(rv::lw(R::a3, R::sp, kFramePrefetchFlags));
        emit(rv::lw(R::a2, R::sp, kFramePrefetchBase));
        // Owned and running, the stalled mark of an ARM fetch aside.
        if (thumb) {
            emit(rv::addi(R::a0, R::a3, -static_cast<int32_t>(kPrefetchFast | kPrefetchRunning)));
        } else {
            emit(rv::andi(R::a0, R::a3, ~static_cast<int32_t>(kPrefetchStalled)));
            emit(rv::addi(R::a0, R::a0, -static_cast<int32_t>(kPrefetchFast | kPrefetchRunning)));
        }
        const auto not_running = forward();
        const auto running = at();
        // a0 = arrival - now.
        fixed(rv::addi(R::a2, R::a2, static_cast<int32_t>(step)));
        emit(rv::add(R::a0, R::a2, kRemaining));
        fixed(rv::slti(R::a1, R::a0, full + 1));
        const auto filled = forward();
        emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
        if (!thumb) {
            // An ARM fetch that waits for the buffer marks it stalled.
            emit(rv::slt(R::a1, R::zero, R::a0));
            emit(rv::slli(R::a1, R::a1, 2));
            emit(rv::andi(R::a3, R::a3, ~static_cast<int32_t>(kPrefetchStalled)));
            emit(rv::or_(R::a3, R::a3, R::a1));
            emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        }
        const auto at_once = forward();
        emit(rv::sub(kRemaining, R::zero, R::a2));
        done();
        land(at_once, rv::bge, R::zero, R::a0);
        emit(rv::addi(kRemaining, kRemaining, -1));
        done();
        // Full: the buffer stops, and this fetch takes from what it holds.
        land(filled, rv::bne, R::a1, R::zero);
        out.li(R::a3, kPrefetchFast | static_cast<uint32_t>(8 - halves) << kPrefetchPausedShift);
        emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        emit(rv::addi(kRemaining, kRemaining, -1));
        done();
        land(not_running, rv::bne, R::a0, R::zero);
        // Owned, and neither stopped nor full: 1, 3, 5 or 7.
        emit(rv::addi(R::a0, R::a3, -1));
        emit(rv::sltiu(R::a0, R::a0, 7));
        const auto other = forward();
        emit(rv::andi(R::a0, R::a3, kPrefetchRunning));
        emit(rv::bne(R::a0, R::zero, distance(at(), running)));
        // Right after the CPU's own access the buffer has not begun: this
        // fetch is the CPU's own S access, and the buffer starts after it.
        emit(rv::add(R::a0, R::a2, kRemaining));
        const auto later = forward();
        charge_fixed(step);
        emit(rv::sub(R::a2, R::zero, kRemaining));
        emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
        done();
        land(later, rv::bne, R::a0, R::zero);
        emit(rv::ori(R::a3, R::a3, kPrefetchRunning));
        emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        emit(rv::jal(R::zero, distance(at(), running)));
        land(other, rv::beq, R::a0, R::zero);
        emit(rv::andi(R::a0, R::a3, kPrefetchFast));
        const auto not_fast = forward();
        emit(rv::andi(R::a0, R::a3, kPrefetchEmpty));
        const auto empty = forward();
        // Stopped full: the fetch takes from the halfwords held, and the
        // buffer stays stopped once it has none.
        emit(rv::srli(R::a0, R::a3, kPrefetchPausedShift));
        size_t last = 0;
        if (!thumb) {
            emit(rv::addi(R::a1, R::zero, 1));
            last = forward();
        }
        emit(rv::addi(R::a0, R::a0, -halves));
        emit(rv::addi(kRemaining, kRemaining, -1));
        const auto drained = forward();
        emit(rv::slli(R::a0, R::a0, kPrefetchPausedShift));
        emit(rv::ori(R::a3, R::a0, kPrefetchFast));
        emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        done();
        if (!thumb) {
            // An ARM fetch takes the last halfword held and waits for the
            // restarted buffer to bring the next.
            land(last, rv::beq, R::a0, R::a1);
            charge_fixed(cycles.n16[ws]);
        }
        land(drained, rv::beq, R::a0, R::zero);
        out.li(R::a3, kPrefetchFast | kPrefetchEmpty | kPrefetchDrained);
        emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        done();
        // Stopped and empty: an N fetch, after which the buffer starts.
        land(empty, rv::bne, R::a0, R::zero);
        charge_fixed(n);
        emit(rv::sub(R::a2, R::zero, kRemaining));
        emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
        out.li(R::a3, kPrefetchFast);
        emit(rv::sw(R::a3, R::sp, kFramePrefetchFlags));
        done();
        // Memory holds the buffer: mapped_fetch() with the request and the
        // address from the word after the call, returning past it.
        land(not_fast, rv::beq, R::a0, R::zero);
        emit(rv::lw(R::a2, kLink, 0));
        emit(rv::srli(R::a1, R::a2, kRomFetchRequestShift));
        emit(rv::andi(R::a1, R::a1, 0xf0));
        emit(rv::ori(R::a1, R::a1, static_cast<int32_t>(kFetch | (thumb ? kHalf : kWord))));
        emit(rv::slli(R::a2, R::a2, 4));
        emit(rv::srli(R::a2, R::a2, 4));
        emit(rv::sw(R::a2, R::sp, kFramePc));
        emit(rv::addi(kLink, kLink, 4));
        emit(rv::jal(R::zero, distance(at(), s.m_fetch)));
    }

    // m_rom_refill: a static refill within a ROM block's region pair, as
    // Compiler::m_rom_refill() describes it. Two words follow the call: the
    // target, and Frame::pc for the fetch of the instruction that refills.
    // m_rom_refill_dynamic: the same with the target in a2, aligned, and
    // the next fetch's address in the first word: a refill it does not
    // time goes to mapped_refill(). Both return past the words with the
    // target in a2; they use a0-a3, the target kept in Frame's slot for tp.
    std::array<size_t, 6> refill_body{};
    for (unsigned stub = 0; stub < s.m_rom_refill.size(); ++stub) {
        s.m_rom_refill[stub] = at();
        const bool thumb = stub < 3;
        const unsigned ws = stub % 3, halfword = cycles.s16[ws];
        const unsigned step = thumb ? halfword : cycles.s32[ws], n = thumb ? cycles.n16[ws] : cycles.n32[ws];
        const int32_t length = thumb ? 2 : 4;
        const auto call_stub = [&](uintptr_t stub_at) { emit(rv::jal(kLink, distance(at(), stub_at))); };
        const auto done = [&] {
            emit(rv::lw(R::a2, R::sp, kFrameRefillTarget));
            emit(rv::jalr(R::zero, R::a3, 8));
        };
        // Frame::pc for a fetch at the target: kAfterPrefetch, in the block's state.
        const auto target_pc = [&] {
            emit(rv::lw(R::a2, R::sp, kFrameRefillTarget));
            emit(rv::ori(R::a0, R::a2, kAfterPrefetch));
            if (thumb) {
                out.li(kLink, kPcThumb);
                emit(rv::or_(R::a0, R::a0, kLink));
            }
            emit(rv::sw(R::a0, R::sp, kFramePc));
        };
        emit(rv::lw(R::a2, kLink, 0));
        emit(rv::sw(R::a2, R::sp, kFrameRefillTarget));
        refill_body[stub] = at();
        mv(R::a3, kLink);
        // With no cycles left, the fetch catches up first.
        const auto has_cycles = forward();
        emit(rv::lw(R::a0, R::a3, 4));
        emit(rv::sw(R::a0, R::sp, kFramePc));
        call_stub(s.m_catch_fetch);
        land(has_cycles, rv::blt, R::zero, kRemaining);
        emit(rv::lw(R::a1, R::sp, kFramePrefetchFlags));
        emit(rv::andi(R::a0, R::a1, kPrefetchFast));
        const auto slow = forward();
        // The wait for the halfword the buffer is fetching: none while it
        // is stopped, before it begins and once it is full.
        emit(rv::sltiu(R::a0, R::a1, 8));
        const auto stopped = forward();
        emit(rv::lw(R::a2, R::sp, kFramePrefetchBase));
        emit(rv::add(R::a0, R::a2, kRemaining));
        const auto not_begun = forward();
        fixed(rv::slti(R::a1, R::a0, 1 - 8 * static_cast<int32_t>(halfword)));
        const auto was_full = forward();
        emit(rv::sub(R::a0, R::zero, R::a0));
        if (!(halfword & (halfword - 1))) {
            fixed(rv::andi(R::a0, R::a0, static_cast<int32_t>(halfword - 1)));
            fixed(rv::nop());
        } else {
            fixed(rv::addi(R::a2, R::zero, static_cast<int32_t>(halfword)));
            fixed(rv::remu(R::a0, R::a0, R::a2));
        }
        const auto arrived = forward();
        emit(rv::add(kRemaining, kRemaining, R::a0));
        charge_fixed(halfword);
        land(stopped, rv::beq, R::a0, R::zero);
        land(not_begun, rv::bge, R::a0, R::zero);
        land(was_full, rv::bne, R::a1, R::zero);
        land(arrived, rv::beq, R::a0, R::zero);
        // The target's N fetch, after which the buffer starts at the next.
        charge_fixed(n);
        emit(rv::sub(R::a2, R::zero, kRemaining));
        emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
        out.li(R::a0, kPrefetchFast);
        emit(rv::sw(R::a0, R::sp, kFramePrefetchFlags));
        // The CPU's own S fetch of the next, with cycles left.
        const auto none_left = forward();
        charge_fixed(step);
        emit(rv::sub(R::a2, R::zero, kRemaining));
        emit(rv::sw(R::a2, R::sp, kFramePrefetchBase));
        done();
        land(none_left, rv::bge, R::zero, kRemaining);
        target_pc();
        call_stub(s.m_catch_fetch);
        target_pc();
        emit(rv::addi(R::a2, R::a2, length));
        out.li(R::a1, kFetch | (thumb ? kHalf : kWord) | kSequential);
        call_stub(s.m_fetch);
        done();
        // Memory holds the buffer: mapped_refill() times both fetches.
        land(slow, rv::beq, R::a0, R::zero);
        out.li(R::a1, static_cast<uint32_t>(length) | kRefillChecked);
        emit(rv::lw(R::a2, R::sp, kFrameRefillTarget));
        emit(rv::lw(R::a0, R::a3, 4));
        emit(rv::sw(R::a0, R::sp, kFramePc));
        call_stub(s.m_refill);
        done();
    }
    for (unsigned stub = 0; stub < s.m_rom_refill_dynamic.size(); ++stub) {
        s.m_rom_refill_dynamic[stub] = at();
        const bool thumb = stub < 3;
        const int32_t length = thumb ? 2 : 4;
        std::array<size_t, 6> other{};
        unsigned others = 0;
        // Within the block's region pair, not next to a 128 KiB boundary,
        // and neither the halfword the buffer holds next nor, in ARM state,
        // the word after.
        emit(rv::srli(R::a0, R::a2, 25));
        emit(rv::addi(R::a0, R::a0, -static_cast<int32_t>(4 + stub % 3)));
        other[others++] = forward();
        for (const int32_t ahead : {length, 2 * length}) {
            emit(rv::addi(R::a0, R::a2, ahead));
            emit(rv::slli(R::a0, R::a0, 15));
            other[others++] = forward();
        }
        emit(rv::lw(R::a0, kLink, 0));
        other[others++] = forward();
        if (!thumb) {
            emit(rv::addi(R::a0, R::a0, 4));
            other[others++] = forward();
        }
        emit(rv::sw(R::a2, R::sp, kFrameRefillTarget));
        emit(rv::jal(R::zero, distance(at(), refill_body[stub])));
        land(other[0], rv::bne, R::a0, R::zero);
        land(other[1], rv::beq, R::a0, R::zero);
        land(other[2], rv::beq, R::a0, R::zero);
        land(other[3], rv::beq, R::a0, R::a2);
        if (!thumb) land(other[4], rv::beq, R::a0, R::a2);
        out.li(R::a1, static_cast<uint32_t>(length));
        emit(rv::lw(R::a0, kLink, 4));
        emit(rv::sw(R::a0, R::sp, kFramePc));
        emit(rv::addi(kLink, kLink, 8));
        emit(rv::jal(R::zero, distance(at(), s.m_refill)));
    }

    // m_ram_refill: a dynamic refill from an IWRAM or EWRAM block, the
    // target aligned in a2 and Frame::pc in the word after the call. One
    // into IWRAM or EWRAM costs what m_refill_static() charges for it,
    // while both fetches find cycles left; mapped_refill() times the rest.
    // Returns past the word with a2 kept; uses a0 and a1.
    for (unsigned stub = 0; stub < s.m_ram_refill.size(); ++stub) {
        s.m_ram_refill[stub] = at();
        const bool thumb = stub == 0;
        emit(rv::srli(R::a0, R::a2, 24));
        emit(rv::addi(R::a0, R::a0, -3));
        const auto not_iwram = forward();
        // a0 = the cycles of each fetch.
        emit(rv::addi(R::a0, R::zero, 1));
        const auto timed = at();
        const auto has_cycles = forward();
        const auto slow = at();
        out.li(R::a1, thumb ? 2 : 4);
        emit(rv::lw(R::a0, kLink, 0));
        emit(rv::sw(R::a0, R::sp, kFramePc));
        emit(rv::addi(kLink, kLink, 4));
        emit(rv::jal(R::zero, distance(at(), s.m_refill)));
        land(not_iwram, rv::bne, R::a0, R::zero);
        emit(rv::addi(R::a0, R::a0, 1));
        emit(rv::bne(R::a0, R::zero, distance(at(), slow)));
        fixed(rv::addi(R::a0, R::zero, static_cast<int32_t>(cycles.ewram[!thumb])));
        emit(rv::jal(R::zero, distance(at(), timed)));
        land(has_cycles, rv::blt, R::a0, kRemaining);
        emit(rv::sub(kRemaining, kRemaining, R::a0));
        emit(rv::sub(kRemaining, kRemaining, R::a0));
        back(4);
    }

    // m_load and m_store: a data access at a2 of the stub's width, a3
    // holding a write's data, a read returning in a0. With no cycles left
    // it catches up first (mapped_catch()). IWRAM, EWRAM and VRAM run here
    // with MappedBus's paths, and so do halfword and word writes of palette
    // RAM and OAM, and reads of the ROM's resident pages while Memory holds
    // the prefetch buffer stopped, as code in RAM and the BIOS reads it:
    // Memory::read_slow() with no halfword to wait for. A byte write to
    // video memory, a write to a line of translated code and the rest go
    // to C++. The word after the call holds Frame::pc with
    // the request's kSequential and kLock in bits 28-29. Keeps a2 and a3;
    // returns past the word.
    const auto split = [](uintptr_t address) {
        const auto value = static_cast<uint32_t>(address);
        return std::pair{(value + 0x800u) >> 12, static_cast<int32_t>(value << 20) >> 20};
    };
    const auto address = [](const void* data) { return reinterpret_cast<uintptr_t>(data); };
    const uintptr_t iwram = memory ? address(memory->iwram.data()) : 0,
                    iwram_code = memory ? address(memory->iwram_code.data()) : 0,
                    ewram = memory ? address(memory->ewram) : 0, ewram_code = memory ? address(memory->ewram_code) : 0,
                    vram = memory ? address(memory->vram) : 0, dirty = memory ? address(memory->dirty.data()) : 0,
                    palette = memory ? address(memory->palette.data()) : 0,
                    oam = memory ? address(memory->oam.data()) : 0,
                    io = memory ? address(memory->io_.data()) : 0,
                    display_quiet = memory ? address(&memory->display_quiet) : 0,
                    io_reads = memory ? address(memory->io_reads.data()) : 0,
                    vcount_from = memory ? address(&memory->vcount_from) : 0,
                    deadline = memory ? address(&memory->deadline) : 0,
                    spin_epoch = memory ? address(&memory->spin_epoch) : 0,
                    rom_table = memory ? address(&memory->rom_table) : 0,
                    prefetch_head = memory ? address(&memory->prefetch_.head) : 0,
                    gamepak_next = memory ? address(&memory->gamepak_next_) : 0,
                    cycles_n = memory ? address(memory->cycles_n.data()) : 0;
    // cycles_s is 48 bytes after cycles_n, which an andi reaches.
    const int32_t cycles_s = memory ? static_cast<int32_t>(address(memory->cycles_s.data()) - cycles_n) : 0;
    if (cycles_s < 0 || cycles_s > 2047) return false;
    // Frame::pc into a0 from the word at kLink, the request's bits into a1.
    const auto access_word = [&] {
        emit(rv::lw(R::a0, kLink, 0));
        emit(rv::lui(R::a1, 0x30000));
        emit(rv::and_(R::a1, R::a0, R::a1));
        emit(rv::xor_(R::a0, R::a0, R::a1));
    };
    // Jumps to the C++ path of the store stub being generated, patched in
    // once its place is known.
    struct {
        std::array<size_t, 8> at{};
        unsigned count = 0;
    } to_cpp_io;
    const auto to_cpp = [&] { to_cpp_io.at[to_cpp_io.count++] = forward(); };
    // A write to the I/O registers after the display's, from the branch at
    // `from` with a0 = the offset - 8. The registers that Memory::
    // io_write16() only stores take any width: the DMA channels' addresses
    // and counts, the serial data and the unused words E0h-FFh and
    // 110h-11Fh, a bit each by word from B0h. A halfword or word to IE, IF
    // or IME, or a byte to IME's low byte, as programs clear and set IME
    // around their critical sections, runs here when update_irq() would
    // only record the new line:
    // with relaxed timing, when IE & IF does not rise and no stop follows,
    // as the line rising with CPSR's I clear asks for; otherwise when
    // neither IE & IF nor IME changes, which starts no watch of the line.
    // A write to IF also leaves the keypad's interrupt off. The rest goes
    // to C++. IE & IF and IME before the write are what irq_request_ and
    // irq_master_ last recorded, as update_irq() runs at every change.
    constexpr uint32_t kPlainWords = 0x3f0ff6db;
    const auto io_writes = [&](unsigned width, size_t from) {
        land(from, rv::beq, R::a1, R::zero);
        if (!memory) {
            to_cpp();
            return true;
        }
        const auto epoch = [&] {
            const auto [high, low] = split(spin_epoch);
            emit(rv::lui(R::a1, high));
            emit(rv::lw(R::a0, R::a1, low));
            emit(rv::addi(R::a0, R::a0, 1));
            emit(rv::sw(R::a0, R::a1, low));
        };
        const int32_t align = -(1 << width);
        const auto aligned_offset = [&] {
            emit(rv::slli(R::a0, R::a2, 8));
            emit(rv::srli(R::a0, R::a0, 8));
            if (width) emit(rv::andi(R::a0, R::a0, align));
        };
        emit(rv::addi(R::a1, R::a0, -(0xb0 - 8)));
        emit(rv::srli(R::a1, R::a1, 2));
        emit(rv::sltiu(R::a0, R::a1, 30));
        const auto beyond = forward();
        out.li(R::a0, kPlainWords);
        emit(rv::srl(R::a0, R::a0, R::a1));
        emit(rv::andi(R::a0, R::a0, 1));
        const auto not_plain = forward();
        aligned_offset();
        {
            const auto [high, low] = split(io);
            emit(rv::lui(R::a1, high));
            emit(rv::add(R::a0, R::a0, R::a1));
            emit(width == 2 ? rv::sw(R::a3, R::a0, low) : width == 1 ? rv::sh(R::a3, R::a0, low) : rv::sb(R::a3, R::a0, low));
        }
        epoch();
        emit(rv::addi(kRemaining, kRemaining, -1));
        back(4);
        land(beyond, rv::beq, R::a0, R::zero);
        land(not_plain, rv::beq, R::a0, R::zero);
        aligned_offset();
        emit(rv::addi(R::a1, R::a0, -0x208));
        const auto ime = forward();
        size_t ie = 0, flags = 0;
        if (width) {
            emit(rv::addi(R::a1, R::a0, -0x200));
            ie = forward();
        }
        if (width == 1) {
            emit(rv::addi(R::a1, R::a0, -0x202));
            flags = forward();
        }
        to_cpp();
        // a4 = IE's address, through which the registers and the line's
        // records are reached; a5 = IE and t6 = IF before the write.
        const uintptr_t base = io + 0x200;
        const auto relative = [&](uintptr_t at_address) { return static_cast<int64_t>(at_address) - static_cast<int64_t>(base); };
        const int64_t if_flags = relative(address(&memory->irq_flags_)),
                      count = relative(address(&memory->irq_request_.count)),
                      before = relative(address(&memory->irq_request_.before)), keypad = relative(io + 0x132);
        for (const int64_t offset : {if_flags, count, before, keypad})
            if (offset < -2048 || offset > 2047) return false;
        struct Slow { size_t at; Branch op; R a, b; };
        std::array<Slow, 8> slow_io{};
        unsigned slows = 0;
        const auto slow_if = [&](Branch op, R a, R b) { slow_io[slows++] = {forward(), op, a, b}; };
        const auto open = [&] {
            emit(rv::sw(kLink, R::sp, kFrameStubLink));
            save(R::a4);
            save(R::a5);
            const auto [high, low] = split(base);
            emit(rv::lui(R::a4, high));
            emit(rv::addi(R::a4, R::a4, low));
            emit(rv::lhu(R::a5, R::a4, 0));
            emit(rv::lhu(R::t6, R::a4, 2));
        };
        const auto close = [&] {
            restore(R::a4);
            restore(R::a5);
            emit(rv::lw(kLink, R::sp, kFrameStubLink));
            epoch();
            emit(rv::addi(kRemaining, kRemaining, -1));
            back(4);
        };
        // A write to IF leaves the keypad's interrupt off: KEYCNT's bit 14.
        const auto keypad_off = [&] {
            emit(rv::lhu(R::a0, R::a4, static_cast<int32_t>(keypad)));
            emit(rv::slli(R::a0, R::a0, 17));
            slow_if(rv::blt, R::a0, R::zero);
        };
        const auto bool_of = [&](R r) { emit(rv::sltu(r, R::zero, r)); };
        const auto store_if = [&] {
            emit(rv::sh(R::t6, R::a4, 2));
            emit(rv::sh(R::t6, R::a4, static_cast<int32_t>(if_flags)));
        };
        const auto new_ie = [&] {
            emit(rv::slli(R::a5, R::a3, 18));
            emit(rv::srli(R::a5, R::a5, 18));
        };
        // IF & ~(a3 >> shift) into t6.
        const auto new_if = [&](unsigned shift) {
            if (shift) {
                emit(rv::srli(R::a0, R::a3, static_cast<int32_t>(shift)));
                emit(rv::xori(R::a0, R::a0, -1));
            } else {
                emit(rv::xori(R::a0, R::a3, -1));
            }
            emit(rv::and_(R::t6, R::t6, R::a0));
        };
#ifdef GBX_GBA_RELAXED_TIMING
        // The new line from IE & IF in a1 and IME, as update_irq() sets
        // it, unless it stops the run: a0 = the line, irq_request_ reset.
        const auto line = [&] {
            bool_of(R::a1);
            emit(rv::lhu(R::a0, R::a4, 8));
            emit(rv::and_(R::a0, R::a0, R::a1));
            const auto down = forward();
            emit(rv::lw(R::a0, R::sp, kFrameCpu));
            emit(rv::lw(R::a0, R::a0, kStateCpsr));
            emit(rv::andi(R::a0, R::a0, kFlagI));
            slow_if(rv::beq, R::a0, R::zero);
            emit(rv::addi(R::a0, R::zero, 1));
            land(down, rv::beq, R::a0, R::zero);
            emit(rv::sb(R::zero, R::a4, static_cast<int32_t>(count)));
            emit(rv::sb(R::a1, R::a4, static_cast<int32_t>(before)));
            const auto [high, low] = split(address(&memory->irq_line));
            emit(rv::lui(R::a1, high));
            emit(rv::sb(R::a0, R::a1, low));
        };
        // IE & IF before (a0) and after (a1) a write to IE: no rise.
        const auto no_rise = [&] {
            const auto none = forward();
            slow_if(rv::beq, R::a0, R::zero);
            land(none, rv::beq, R::a1, R::zero);
        };
#endif
        // IME.
        land(ime, rv::beq, R::a1, R::zero);
        open();
        emit(rv::and_(R::a5, R::a5, R::t6));
        bool_of(R::a5);
        emit(rv::andi(R::a0, R::a3, 1));
#ifdef GBX_GBA_RELAXED_TIMING
        emit(rv::and_(R::a1, R::a0, R::a5));
        const auto down = forward();
        emit(rv::lw(R::a1, R::sp, kFrameCpu));
        emit(rv::lw(R::a1, R::a1, kStateCpsr));
        emit(rv::andi(R::a1, R::a1, kFlagI));
        slow_if(rv::beq, R::a1, R::zero);
        emit(rv::addi(R::a1, R::zero, 1));
        land(down, rv::beq, R::a1, R::zero);
        emit(rv::sh(R::a0, R::a4, 8));
        if (width == 2) {
            emit(rv::srli(R::a0, R::a3, 16));
            emit(rv::sh(R::a0, R::a4, 10));
        }
        emit(rv::sb(R::zero, R::a4, static_cast<int32_t>(count)));
        emit(rv::sb(R::a5, R::a4, static_cast<int32_t>(before)));
        {
            const auto [high, low] = split(address(&memory->irq_line));
            emit(rv::lui(R::a0, high));
            emit(rv::sb(R::a1, R::a0, low));
        }
#else
        emit(rv::lhu(R::a1, R::a4, 8));
        slow_if(rv::bne, R::a0, R::a1);
        if (width == 2) {
            emit(rv::srli(R::a0, R::a3, 16));
            emit(rv::sh(R::a0, R::a4, 10));
        }
#endif
        close();
        if (!width) {
            for (unsigned i = 0; i < slows; ++i) land(slow_io[i].at, slow_io[i].op, slow_io[i].a, slow_io[i].b);
            restore(R::a4);
            restore(R::a5);
            emit(rv::lw(kLink, R::sp, kFrameStubLink));
            to_cpp();
            return true;
        }
        // IE, and with a word IF too, in that order as io_write() does.
        land(ie, rv::beq, R::a1, R::zero);
        open();
        if (width == 2) keypad_off();
        emit(rv::and_(R::a0, R::a5, R::t6));
        new_ie();
        emit(rv::and_(R::a1, R::a5, R::t6));
#ifdef GBX_GBA_RELAXED_TIMING
        no_rise();
        if (width == 2) {
            // The line after IE alone must not stop the run either.
            const auto none = forward();
            emit(rv::lhu(R::a0, R::a4, 8));
            const auto off = forward();
            emit(rv::lw(R::a0, R::sp, kFrameCpu));
            emit(rv::lw(R::a0, R::a0, kStateCpsr));
            emit(rv::andi(R::a0, R::a0, kFlagI));
            slow_if(rv::beq, R::a0, R::zero);
            land(none, rv::beq, R::a1, R::zero);
            land(off, rv::beq, R::a0, R::zero);
            new_if(16);
            emit(rv::and_(R::a1, R::a5, R::t6));
        }
        line();
#else
        bool_of(R::a0);
        bool_of(R::a1);
        slow_if(rv::bne, R::a0, R::a1);
        if (width == 2) {
            new_if(16);
            emit(rv::and_(R::a1, R::a5, R::t6));
            bool_of(R::a1);
            slow_if(rv::bne, R::a0, R::a1);
        }
#endif
        emit(rv::sh(R::a5, R::a4, 0));
        if (width == 2) store_if();
        close();
        if (width == 1) {
            // IF.
            land(flags, rv::beq, R::a1, R::zero);
            open();
            keypad_off();
#ifndef GBX_GBA_RELAXED_TIMING
            emit(rv::and_(R::a1, R::a5, R::t6));
            bool_of(R::a1);
#endif
            new_if(0);
#ifdef GBX_GBA_RELAXED_TIMING
            emit(rv::and_(R::a1, R::a5, R::t6));
            line();
#else
            emit(rv::and_(R::a0, R::a5, R::t6));
            bool_of(R::a0);
            slow_if(rv::bne, R::a0, R::a1);
#endif
            store_if();
            close();
        }
        // Back to C++ with the registers as they were.
        for (unsigned i = 0; i < slows; ++i) land(slow_io[i].at, slow_io[i].op, slow_io[i].a, slow_io[i].b);
        restore(R::a4);
        restore(R::a5);
        emit(rv::lw(kLink, R::sp, kFrameStubLink));
        to_cpp();
        return true;
    };
    for (unsigned width = 0; width < 3; ++width) {
        for (const bool write : {false, true}) {
            (write ? s.m_store : s.m_load)[width] = at();
#ifdef GBX_PROFILER
            emit(rv::lw(R::a0, R::sp, kFrameContext));
            emit(rv::lw(R::a1, R::a0, static_cast<int32_t>(offsetof(Context, counts)) + 8));
            emit(rv::addi(R::a1, R::a1, 1));
            emit(rv::sw(R::a1, R::a0, static_cast<int32_t>(offsetof(Context, counts)) + 8));
            {
                // By the address's region, see arm7_stub_counts.
                const auto [high, low] = split(address(arm7_stub_counts.data() + (write ? 16 : 0)));
                emit(rv::srli(R::a0, R::a2, 24));
                emit(rv::andi(R::a0, R::a0, 15));
                emit(rv::slli(R::a0, R::a0, 2));
                emit(rv::lui(R::a1, high));
                emit(rv::add(R::a0, R::a0, R::a1));
                emit(rv::lw(R::a1, R::a0, low));
                emit(rv::addi(R::a1, R::a1, 1));
                emit(rv::sw(R::a1, R::a0, low));
            }
#endif
            const int32_t align = -(1 << width);
            // a0 = a2's low `bits`, aligned to the width.
            const auto offset = [&](unsigned bits) {
                emit(rv::slli(R::a0, R::a2, static_cast<int32_t>(32 - bits)));
                emit(rv::srli(R::a0, R::a0, static_cast<int32_t>(32 - bits)));
                if (width) emit(rv::andi(R::a0, R::a0, align));
            };
            // The access at a0 + `base`, through a1.
            const auto access = [&](uintptr_t base) {
                const auto [high, low] = split(base);
                emit(rv::lui(R::a1, high));
                emit(rv::add(R::a0, R::a0, R::a1));
                if (write)
                    emit(width == 2 ? rv::sw(R::a3, R::a0, low) : width == 1 ? rv::sh(R::a3, R::a0, low) : rv::sb(R::a3, R::a0, low));
                else
                    emit(width == 2 ? rv::lw(R::a0, R::a0, low) : width == 1 ? rv::lhu(R::a0, R::a0, low) : rv::lbu(R::a0, R::a0, low));
            };
            // a1 = the code mark of a2's line in the RAM of `bits` at `marks`.
            const auto code_line = [&](unsigned bits, uintptr_t marks) {
                emit(rv::slli(R::a1, R::a2, static_cast<int32_t>(32 - bits)));
                emit(rv::srli(R::a1, R::a1, static_cast<int32_t>(32 - bits + Memory::kCodeLineBits)));
                const auto [high, low] = split(marks);
                emit(rv::lui(R::a0, high));
                emit(rv::add(R::a1, R::a1, R::a0));
                emit(rv::lbu(R::a1, R::a1, low));
            };
            const auto done = [&] { back(4); };
            std::array<size_t, 5> slow{};
            unsigned slow_count = 0;
            const auto catching = forward();
            const auto resume = at();
            emit(rv::srli(R::a0, R::a2, 24));
            emit(rv::addi(R::a0, R::a0, -3));
            const auto not_iwram = forward();
            if (write) {
                code_line(15, iwram_code);
                slow[slow_count++] = forward();
            }
            offset(15);
            access(iwram);
            emit(rv::addi(kRemaining, kRemaining, -1));
            done();
            land(not_iwram, rv::bne, R::a0, R::zero);
            emit(rv::addi(R::a1, R::a0, 1));
            const auto not_ewram = forward();
            if (write) {
                code_line(18, ewram_code);
                slow[slow_count++] = forward();
            }
            offset(18);
            access(ewram);
            charge_fixed(cycles.ewram[width == 2]);
            done();
            land(not_ewram, rv::bne, R::a1, R::zero);
            size_t not_vram = 0, mirror = 0;
            if (!write || width) {
                // VRAM below its mirror; a write marks its page for the
                // renderer, see Memory::write_video().
                emit(rv::addi(R::a1, R::a0, -3));
                not_vram = forward();
                emit(rv::slli(R::a0, R::a2, 15));
                emit(rv::srli(R::a0, R::a0, 15));
                out.li(R::a1, 0x18000);
                mirror = forward();
                if (width) emit(rv::andi(R::a0, R::a0, align));
                access(vram);
                if (write) {
                    emit(rv::sw(kLink, R::sp, kFrameStubLink));
                    emit(rv::slli(R::a0, R::a2, 15));
                    emit(rv::srli(R::a0, R::a0, 15 + Ppu::kPageBits));
                    emit(rv::srli(R::a1, R::a0, 3));
                    emit(rv::andi(R::a0, R::a0, 7));
                    emit(rv::addi(kLink, R::zero, 1));
                    emit(rv::sll(R::a0, kLink, R::a0));
                    const auto [high, low] = split(dirty);
                    emit(rv::lui(kLink, high));
                    emit(rv::add(R::a1, R::a1, kLink));
                    emit(rv::lbu(kLink, R::a1, low));
                    emit(rv::or_(kLink, kLink, R::a0));
                    emit(rv::sb(kLink, R::a1, low));
                    emit(rv::lw(kLink, R::sp, kFrameStubLink));
                }
                emit(rv::addi(kRemaining, kRemaining, width == 2 ? -2 : -1));
                done();
            }
            std::array<size_t, 4> to_cpp{};
            if (!write) {
                // The ROM's regions 8-12; 13 may hold the EEPROM. a0 is the
                // region - 3.
                land(not_vram, rv::bne, R::a1, R::zero);
                emit(rv::addi(R::a1, R::a0, -5));
                emit(rv::sltiu(R::a1, R::a1, 5));
                to_cpp[0] = forward();
                // Memory holds the buffer, stopped: no halfword to wait for.
                emit(rv::lw(R::a1, R::sp, kFramePrefetchFlags));
                to_cpp[1] = forward();
                const auto [head_high, head_low] = split(prefetch_head);
                emit(rv::lui(R::a1, head_high));
                emit(rv::lw(R::a1, R::a1, head_low));
                to_cpp[2] = forward();
                // The address's page, unless it is not resident.
                emit(rv::slli(R::a0, R::a2, 7));
                emit(rv::srli(R::a0, R::a0, 7 + RomCache::kPageBits));
                emit(rv::slli(R::a0, R::a0, 2));
                const auto [table_high, table_low] = split(rom_table);
                emit(rv::lui(R::a1, table_high));
                emit(rv::lw(R::a1, R::a1, table_low));
                emit(rv::add(R::a0, R::a0, R::a1));
                emit(rv::lw(R::a0, R::a0, 0));
                to_cpp[3] = forward();
                emit(rv::slli(R::a1, R::a2, 32 - RomCache::kPageBits));
                emit(rv::srli(R::a1, R::a1, 32 - RomCache::kPageBits));
                if (width) emit(rv::andi(R::a1, R::a1, align));
                emit(rv::add(R::a0, R::a0, R::a1));
                emit(width == 2 ? rv::lw(R::a0, R::a0, 0) : width == 1 ? rv::lhu(R::a0, R::a0, 0) : rv::lbu(R::a0, R::a0, 0));
                // Memory::gamepak_cycles(): S when the request is S at the
                // address after the last access, within 128 KiB.
                emit(rv::sw(kLink, R::sp, kFrameStubLink));
                emit(rv::lw(kLink, kLink, 0));
                emit(rv::srli(kLink, kLink, 28));
                emit(rv::andi(kLink, kLink, 1));
                const auto [next_high, next_low] = split(gamepak_next);
                emit(rv::lui(R::a1, next_high));
                emit(rv::lw(R::a1, R::a1, next_low));
                emit(rv::xor_(R::a1, R::a1, R::a2));
                emit(rv::sltiu(R::a1, R::a1, 1));
                emit(rv::and_(kLink, kLink, R::a1));
                emit(rv::slli(R::a1, R::a2, 15));
                emit(rv::sltu(R::a1, R::zero, R::a1));
                emit(rv::and_(kLink, kLink, R::a1));
                // cycles_n or cycles_s of the region and width.
                emit(rv::sub(kLink, R::zero, kLink));
                emit(rv::andi(kLink, kLink, cycles_s));
                emit(rv::srli(R::a1, R::a2, 24));
                emit(rv::add(kLink, kLink, R::a1));
                emit(rv::slli(R::a1, R::a1, 1));
                emit(rv::add(kLink, kLink, R::a1));
                const auto [cycles_high, cycles_low] = split(cycles_n + width);
                emit(rv::lui(R::a1, cycles_high));
                emit(rv::add(kLink, kLink, R::a1));
                emit(rv::lbu(kLink, kLink, cycles_low));
                emit(rv::sub(kRemaining, kRemaining, kLink));
                if (width) emit(rv::andi(R::a1, R::a2, align));
                else mv(R::a1, R::a2);
                emit(rv::addi(R::a1, R::a1, 1 << width));
                emit(rv::lui(kLink, next_high));
                emit(rv::sw(R::a1, kLink, next_low));
                emit(rv::lw(kLink, R::sp, kFrameStubLink));
                done();
                // I/O below Memory::kIoReads that reads back from its copy
                // (Memory::io_reads), and VCOUNT while the clock and the
                // deadline are both within the line it holds, so that a
                // wait loop the JIT skips (Compiler::spin()) reads the same
                // in each pass. a0 is the region - 3.
                land(to_cpp[0], rv::beq, R::a1, R::zero);
                emit(rv::addi(R::a1, R::a0, -1));
                const auto not_io = forward();
                emit(rv::slli(R::a0, R::a2, 8));
                emit(rv::srli(R::a0, R::a0, 8));
                if (width) emit(rv::andi(R::a0, R::a0, align));
                emit(rv::sltiu(R::a1, R::a0, Memory::kIoReads));
                const auto beyond = forward();
                emit(rv::sw(kLink, R::sp, kFrameStubLink));
                // t6 = the bits that read back, a1 the next halfword's.
                if (width) mv(R::a1, R::a0);
                else emit(rv::andi(R::a1, R::a0, -2));
                const auto [reads_high, reads_low] = split(io_reads);
                emit(rv::lui(kLink, reads_high));
                emit(rv::add(R::a1, R::a1, kLink));
                emit(rv::lhu(kLink, R::a1, reads_low));
                if (width == 2) emit(rv::lhu(R::a1, R::a1, reads_low + 2));
                std::array<size_t, 5> restore{};
                restore[0] = forward();
                if (width == 2) {
                    restore[1] = forward();
                    emit(rv::slli(R::a1, R::a1, 16));
                    emit(rv::or_(kLink, kLink, R::a1));
                } else if (!width) {
                    emit(rv::andi(R::a1, R::a0, 1));
                    emit(rv::slli(R::a1, R::a1, 3));
                    emit(rv::srl(kLink, kLink, R::a1));
                }
                access(io);
                emit(rv::and_(R::a0, R::a0, kLink));
                emit(rv::lw(kLink, R::sp, kFrameStubLink));
                emit(rv::addi(kRemaining, kRemaining, -1));
                done();
                if (width < 2) {
                    land(restore[0], rv::beq, kLink, R::zero);
                    emit(rv::addi(R::a1, R::a0, -6));
                    restore[2] = forward();
                    // The deadline and the clock since the line's start.
                    const auto [deadline_high, deadline_low] = split(deadline);
                    emit(rv::lui(R::a1, deadline_high));
                    emit(rv::lw(R::a1, R::a1, deadline_low));
                    const auto [from_high, from_low] = split(vcount_from);
                    emit(rv::lui(kLink, from_high));
                    emit(rv::lw(kLink, kLink, from_low));
                    emit(rv::sub(R::a1, R::a1, kLink));
                    emit(rv::addi(R::a0, R::zero, static_cast<int32_t>(kLineCycles + 1)));
                    restore[3] = forward();
                    emit(rv::sub(R::a1, R::a1, kRemaining));
                    emit(rv::addi(R::a0, R::a0, -1));
                    restore[4] = forward();
                    const auto [line_high, line_low] = split(io + 6);
                    emit(rv::lui(R::a0, line_high));
                    emit(rv::lbu(R::a0, R::a0, line_low));
                    emit(rv::lw(kLink, R::sp, kFrameStubLink));
                    emit(rv::addi(kRemaining, kRemaining, -1));
                    done();
                    land(restore[2], rv::bne, R::a1, R::zero);
                    land(restore[3], rv::bgeu, R::a1, R::a0);
                    land(restore[4], rv::bgeu, R::a1, R::a0);
                } else {
                    land(restore[0], rv::beq, kLink, R::zero);
                    land(restore[1], rv::beq, R::a1, R::zero);
                }
                emit(rv::lw(kLink, R::sp, kFrameStubLink));
                land(not_io, rv::bne, R::a1, R::zero);
                land(beyond, rv::beq, R::a1, R::zero);
                land(mirror, rv::bgeu, R::a0, R::a1);
                land(to_cpp[1], rv::bne, R::a1, R::zero);
                land(to_cpp[2], rv::bne, R::a1, R::zero);
                land(to_cpp[3], rv::beq, R::a0, R::zero);
            }
            std::array<size_t, 5> display{};
            size_t other_video = 0;
            if (write) {
                // The display registers 08h-57h but the reference points,
                // once the open line job no longer times their writes (see
                // Memory::display_quiet): only their copy in I/O changes,
                // as Memory::io_write() would leave it, see MappedBus.
                if (not_vram) land(not_vram, rv::bne, R::a1, R::zero);
                emit(rv::srli(R::a0, R::a2, 24));
                emit(rv::addi(R::a0, R::a0, -4));
                display[0] = forward();
                emit(rv::slli(R::a0, R::a2, 8));
                emit(rv::srli(R::a0, R::a0, 8));
                emit(rv::addi(R::a0, R::a0, -8));
                emit(rv::sltiu(R::a1, R::a0, 0x50));
                display[1] = forward();
                emit(rv::addi(R::a1, R::a0, -0x20));
                emit(rv::sltiu(R::a1, R::a1, 8));
                display[2] = forward();
                emit(rv::addi(R::a1, R::a0, -0x30));
                emit(rv::sltiu(R::a1, R::a1, 8));
                display[3] = forward();
                // The clock's low word: the deadline's less the cycles left.
                const auto [deadline_high, deadline_low] = split(deadline);
                emit(rv::lui(R::a1, deadline_high));
                emit(rv::lw(R::a1, R::a1, deadline_low));
                emit(rv::sub(R::a1, R::a1, kRemaining));
                emit(rv::sw(kLink, R::sp, kFrameStubLink));
                const auto [quiet_high, quiet_low] = split(display_quiet);
                emit(rv::lui(kLink, quiet_high));
                emit(rv::lw(kLink, kLink, quiet_low));
                emit(rv::sub(R::a1, R::a1, kLink));
                emit(rv::lw(kLink, R::sp, kFrameStubLink));
                display[4] = forward();
                if (width) emit(rv::andi(R::a0, R::a0, align));
                access(io + 8);
                const auto [epoch_high, epoch_low] = split(spin_epoch);
                emit(rv::lui(R::a1, epoch_high));
                emit(rv::lw(R::a0, R::a1, epoch_low));
                emit(rv::addi(R::a0, R::a0, 1));
                emit(rv::sw(R::a0, R::a1, epoch_low));
                emit(rv::addi(kRemaining, kRemaining, -1));
                done();
                if (!io_writes(width, display[1])) return false;
                if (width) {
                    // Halfwords and words of palette RAM and OAM: stored,
                    // and the page marked for the renderer, as Memory::
                    // write_video() does. a0 is the region - 4.
                    land(display[0], rv::bne, R::a0, R::zero);
                    const auto video = [&](uintptr_t base, unsigned first_page, unsigned page_bits, int32_t cycles) {
                        emit(rv::andi(R::a0, R::a2, 0x3ff & align));
                        access(base);
                        emit(rv::sw(kLink, R::sp, kFrameStubLink));
                        emit(rv::andi(R::a0, R::a2, 0x3ff));
                        emit(rv::srli(R::a0, R::a0, static_cast<int32_t>(page_bits)));
                        emit(rv::addi(R::a0, R::a0, static_cast<int32_t>(first_page)));
                        emit(rv::srli(R::a1, R::a0, 3));
                        emit(rv::andi(R::a0, R::a0, 7));
                        emit(rv::addi(kLink, R::zero, 1));
                        emit(rv::sll(R::a0, kLink, R::a0));
                        const auto [high, low] = split(dirty);
                        emit(rv::lui(kLink, high));
                        emit(rv::add(R::a1, R::a1, kLink));
                        emit(rv::lbu(kLink, R::a1, low));
                        emit(rv::or_(kLink, kLink, R::a0));
                        emit(rv::sb(kLink, R::a1, low));
                        emit(rv::lw(kLink, R::sp, kFrameStubLink));
                        emit(rv::addi(kRemaining, kRemaining, -cycles));
                        done();
                    };
                    emit(rv::addi(R::a1, R::a0, -1));
                    const auto not_palette = forward();
                    // 16 bits wide, as VRAM.
                    video(palette, Ppu::kPalettePage, Ppu::kColorPageBits, width == 2 ? 2 : 1);
                    land(not_palette, rv::bne, R::a1, R::zero);
                    emit(rv::addi(R::a1, R::a0, -3));
                    other_video = forward();
                    video(oam, Ppu::kOamPage, Ppu::kPageBits, 1);
                }
            }
            // C++: mapped_read() or mapped_write(), returning past the word.
            for (unsigned i = 0; i < slow_count; ++i) land(slow[i], rv::bne, R::a1, R::zero);
            if (write && not_vram) land(mirror, rv::bgeu, R::a0, R::a1);
            if (write) {
                if (width) land(other_video, rv::bne, R::a1, R::zero);
                else land(display[0], rv::bne, R::a0, R::zero);
                for (unsigned i = 0; i < to_cpp_io.count; ++i)
                    out.patch(to_cpp_io.at[i], rv::jal(R::zero, distance(to_cpp_io.at[i], at())));
                to_cpp_io.count = 0;
                land(display[2], rv::bne, R::a1, R::zero);
                land(display[3], rv::bne, R::a1, R::zero);
                land(display[4], rv::blt, R::a1, R::zero);
            }
            access_word();
            emit(rv::srli(R::a1, R::a1, 24));
            emit(rv::ori(R::a1, R::a1, static_cast<int32_t>((write ? kWrite : kRead) | width << 2)));
            emit(rv::sw(R::a0, R::sp, kFramePc));
            emit(rv::addi(kLink, kLink, 4));
            emit(rv::jal(R::zero, distance(at(), write ? s.m_write : s.m_read)));
            // No cycles left: mapped_catch() first.
            land(catching, rv::bge, R::zero, kRemaining);
            emit(rv::sw(kLink, R::sp, kFrameStubLink));
            access_word();
            emit(rv::sw(R::a0, R::sp, kFramePc));
            emit(rv::jal(kLink, distance(at(), s.m_catch)));
            emit(rv::lw(kLink, R::sp, kFrameStubLink));
            emit(rv::jal(R::zero, distance(at(), resume)));
        }
    }

    s.access = at();
    helper(reinterpret_cast<uintptr_t>(&flat_access), R::a0);
    s.fetch = at();
    helper(reinterpret_cast<uintptr_t>(&flat_fetch), R::a0);
    s.write_cpsr = at();
    helper(reinterpret_cast<uintptr_t>(&write_cpsr), R::a0);
    s.restore_cpsr = at();
    helper(reinterpret_cast<uintptr_t>(&restore_cpsr), R::a0);
    s.exception = at();
    helper(reinterpret_cast<uintptr_t>(&exception), R::a0);

    // carry_mul: C of MUL from the first two multiply slots into kC; carry:
    // from all five. Both keep every other register.
    for (const bool all : {false, true}) {
        (all ? s.carry : s.carry_mul) = at();
        for (const R r : kCallerSaved)
            if (r != kC) save(r);
        emit(rv::lw(R::a1, R::sp, kFrameMultiply));
        emit(rv::lw(R::a2, R::sp, kFrameMultiply + 4));
        if (all) {
            emit(rv::lw(R::a3, R::sp, kFrameMultiply + 8));
            emit(rv::lw(R::a4, R::sp, kFrameMultiply + 12));
            emit(rv::lw(R::a5, R::sp, kFrameMultiply + 16));
        } else {
            emit(rv::addi(R::a3, R::zero, static_cast<int32_t>(kMultiplySigned)));
        }
        call_c(reinterpret_cast<uintptr_t>(&multiply_carry));
        mv(kC, R::a0);
        for (const R r : kCallerSaved)
            if (r != kC) restore(r);
        back();
    }

    // sync_out: the guest registers and NZCV to Arm7State, keeping every
    // register.
    s.sync_out = at();
    for (const R r : {R::a0, R::a1, R::a2}) save(r);
    emit(rv::lw(R::a0, R::sp, kFrameCpu));
    store_state(R::a0, R::a1, R::a2);
    for (const R r : {R::a0, R::a1, R::a2}) restore(r);
    back();

    // read_cpsr: a0 = CPSR with NZCV, keeping every other register.
    s.read_cpsr = at();
    save(R::a1);
    emit(rv::lw(R::a0, R::sp, kFrameCpu));
    emit(rv::lw(R::a0, R::a0, kStateCpsr));
    pack(R::a0, R::a1);
    restore(R::a1);
    back();

    out.align();
    if (!out.valid()) return false;
    bytes = out.offset();
    return true;
}

bool Arm7Jit::initialize_gateway(size_t pool_bytes, size_t pool_blocks, const Memory* memory,
                                 const platform::ExecutableMemory* reserved) {
    if (memory) stub_cycles_ = stub_cycles(*memory);
    Stubs s;
    size_t bytes = 0;
    if (!generate_gateway(memory, stub_cycles_, s, bytes)) return false;
    if (!cache_.initialize(pool_bytes, pool_blocks, bytes + kSingleBytes, reserved)) return false;
    const auto base = reinterpret_cast<uintptr_t>(cache_.shared().data());
#ifdef GBX_PROFILER
    // Byte offsets of the stub families, in generation order, so that
    // program counter samples in the gateway can be attributed.
    platform::log(platform::LogLevel::warn, "arm7-jit",
                  "gateway %08lx %u bytes: exit %u side_exit %u boundary %u link %u dispatch %u catch %u "
                  "helpers %u rom_fetch %u rom_refill %u ram_refill %u access %u flat %u",
                  static_cast<unsigned long>(base), static_cast<unsigned>(bytes), static_cast<unsigned>(s.exit),
                  static_cast<unsigned>(s.side_exit), static_cast<unsigned>(s.m_boundary),
                  static_cast<unsigned>(s.link), static_cast<unsigned>(s.dispatch),
                  static_cast<unsigned>(s.m_catch_fetch_at), static_cast<unsigned>(s.m_read),
                  static_cast<unsigned>(s.m_rom_fetch[0]), static_cast<unsigned>(s.m_rom_refill[0]),
                  static_cast<unsigned>(s.m_ram_refill[0]), static_cast<unsigned>(s.m_load[0]),
                  static_cast<unsigned>(s.access));
#endif
    auto addresses = std::bit_cast<std::array<uintptr_t, sizeof(Stubs) / sizeof(uintptr_t)>>(s);
    for (auto& address : addresses) address += base;
    stubs_ = std::bit_cast<Stubs>(addresses);
    volatile uint32_t* destination = cache_.shared().data();
    for (size_t i = 0; i < bytes / 4; ++i) destination[i] = output_[i];
    if (!cache_.publish_shared(bytes)) return false;
    gateway_bytes_ = bytes;
    return true;
}

// New wait states: the gateway with their cycles, generated with the same
// layout, in its place and in the copies that start the second level's
// segments. Blocks embed the wait states of their key, so the blocks that
// run under these find their stubs.
bool Arm7Jit::refresh_gateway() {
    Stubs s;
    size_t bytes = 0;
    if (!generate_gateway(memory_, stub_cycles_, s, bytes) || bytes != gateway_bytes_) return false;
    volatile uint32_t* destination = cache_.shared().data();
    for (size_t i = 0; i < bytes / 4; ++i) destination[i] = output_[i];
    return cache_.publish_shared(bytes) && store_.refresh_shared(std::span<const uint32_t>(output_.data(), bytes / 4));
}

}  // namespace gbx::gba
