// The block loop, code streams, bus cycles and exits of the compiler.

#include "arm7_compiler.h"

#include <algorithm>
#include <bit>

#include "../arm7_spin.h"
#include "../executor.h"

namespace gbx::gba::arm7_compiler {

Compiler::Compiler(std::span<uint32_t> hot, std::span<uint32_t> cold, const Arm7Jit::Stubs& stubs,
                   Arm7CodeSource code, uint32_t r15, uint32_t cpsr, const MappedTarget* mapped)
    : hot_(hot, true), cold_(cold, true), stubs_(stubs), code_(code), mapped_(mapped), entry_(r15), r15_(r15),
      thumb_((cpsr & kFlagT) != 0), mode_(cpsr & kModeMask), bank_(bank_of(cpsr)), length_(thumb_ ? 2 : 4) {
    if (mapped_ && mapped_->registers) {
        for (unsigned i = 0; i < regions_.size(); ++i) regions_[i] = static_cast<uint8_t>(mapped_->registers[i] >> 24);
    }
}

std::span<const uint32_t> Compiler::compile(unsigned limit, std::span<uint32_t> output,
                                            std::span<jit::Relocation> relocations, size_t& count) {
    const size_t hot_limit = mapped_ ? kMappedHotLimit : kHotLimit, cold_limit = mapped_ ? kMappedColdLimit : kColdLimit;
#ifdef GBX_GBA_RELAXED_TIMING
    if (mapped_) {
        if (const unsigned instructions = spin_loop(); instructions && instructions <= limit) spin(instructions);
    }
#endif
    for (unsigned i = 0; i < limit && !end_ && !failed_; ++i) {
        if (i && (hot_.offset() > hot_limit || cold_.offset() > cold_limit)) break;
        // A mapped block keeps to its code's region: the instruction and
        // the prefetch it starts with.
        if (i && mapped_ && (!in_region(r15_ - 2 * length_) || !in_region(r15_))) break;
        if (i && mapped_ && link_count_ == links_.size()) break;
        instruction();
    }
    if (!end_ && !failed_) {
        completed_ = index_;
        leave(r15_, false, sequential_, Arm7JitExit::limit);
    }
    return mapped_ ? m_finish(output, relocations, count) : finish(output, relocations, count);
}

// Every instruction: the side exit before it, its prefetch, which it always
// starts with, and its body. The opcode comes from the code source rather
// than from the pipeline, so that a block runs again for the same R15.
void Compiler::instruction() {
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed, as a block's cycles are counted elsewhere: events come due
    // at the next access or at the block's end, which stops the run for
    // an interrupt there; only a write must stop it at once. MSR and
    // exception returns end the block.
    if (index_ && (wrote_ || !mapped_)) boundary();
    wrote_ = false;
#else
    if (index_) boundary();
#endif
    completed_ = index_ + 1;
    busy_ = 0;
    last_ = r15_ - 2 * length_;
    const uint32_t op = code_.read(code_.context, last_, thumb_);
    // A BL's second half right after its first: its target is constant.
    const bool bl_second = bl_pending_;
    bl_pending_ = false;
    prefetch();
    if (thumb_ && bl_second && (op & 0xf800) == 0xf800) {
        const uint32_t target = bl_value_ + (op & 0x7ff) * 2;
        set_guest(14, fixed((r15_ - 2) | 1));
        refill_static(target, true);
        return;
    }
    if (thumb_) thumb(op & 0xffff);
    else arm(op);
    if (mapped_) track(thumb_ ? op & 0xffff : op);
    if (thumb_ && mapped_ && (op & 0xf800) == 0xf000) {
        bl_pending_ = true;
        bl_value_ = r15_ + static_cast<uint32_t>(static_cast<int32_t>(op << 21) >> 9);
    }
    if (end_) return;
    flush_charge();
    ++index_;
    r15_ += length_;
}

// --- Wait loops ------------------------------------------------------------

// The instructions of the wait loop that the block's code is, or 0, see
// arm7_spin.h; within the block's region.
unsigned Compiler::spin_loop() const {
    return arm7_spin_loop(entry_ - 2 * length_, thumb_, [this](uint32_t address) {
        return in_region(address) ? code_.read(code_.context, address, thumb_) : ~0u;
    });
}

// A wait loop's start. Once two passes in a row took the same cycles with
// no C++ between them, every pass takes them, and those that would end
// with cycles left are skipped at once, their instructions counted. The
// last one runs, so that an event comes due at the same boundary as
// without skipping. The first pass may take other cycles than the rest,
// after the code before the loop. C++ clears Frame::spin_site (the
// gateway's helpers), and so does the loop's exit.
void Compiler::spin(unsigned instructions) {
    spin_ = true;
    const uint32_t site = entry_ | (thumb_ ? 1u : 0u);
    emit(rv::lw(R::a0, R::sp, kFrameSpinSite));
    li(R::a1, site);
    const unsigned fresh = branch(rv::bne, R::a0, R::a1);
    // a0 = the cycles of the pass that ended here.
    emit(rv::lw(R::a0, R::sp, kFrameSpinRemaining));
    emit(rv::sub(R::a0, R::a0, kRemaining));
    emit(rv::lw(R::t6, R::sp, kFrameSpinCycles));
    const unsigned changed = branch(rv::bne, R::a0, R::t6);
    emit(rv::addi(R::t6, kRemaining, -1));
    const unsigned none = branch(rv::bge, R::zero, R::a0);
    const unsigned short_of = branch(rv::blt, R::t6, R::a0);
    cold_mode_ = true;
    const Label skip = here();
    // t6 = the passes to skip, those before the last.
    emit(rv::divu(R::t6, R::t6, R::a0));
    emit(rv::mul(R::a0, R::t6, R::a0));
    emit(rv::sub(kRemaining, kRemaining, R::a0));
    li(R::a0, instructions);
    emit(rv::mul(R::t6, R::t6, R::a0));
    emit(rv::lw(R::a0, R::sp, kFrameContext));
    emit(rv::lw(R::a1, R::a0, kContextInstructions));
    emit(rv::add(R::a1, R::a1, R::t6));
    emit(rv::sw(R::a1, R::a0, kContextInstructions));
    const unsigned back = jump();
    cold_mode_ = false;
    bind(jump(), skip);
    bind(fresh);
    emit(rv::addi(R::a0, R::zero, 0));
    bind(changed);
    emit(rv::sw(R::a0, R::sp, kFrameSpinCycles));
    bind(none);
    bind(short_of);
    bind(back);
    emit(rv::sw(kRemaining, R::sp, kFrameSpinRemaining));
    li(R::a1, site);
    emit(rv::sw(R::a1, R::sp, kFrameSpinSite));
}

// --- Streams, labels and relocations ----------------------------------------

unsigned Compiler::reference(Kind kind, Branch op, R a, R b) {
    if (reference_count_ == references_.size()) {
        failed_ = true;
        return 0;
    }
    references_[reference_count_] = {here(), 0, op, a, b, kind, false};
    out().emit_exact(rv::nop());
    return reference_count_++;
}

void Compiler::call(uintptr_t target) {
    if (call_count_ == calls_.size()) {
        failed_ = true;
        return;
    }
    calls_[call_count_++] = {here(), target};
    out().emit_exact(rv::jal(kLink, 0));
}

std::span<const uint32_t> Compiler::finish(std::span<uint32_t> output, std::span<jit::Relocation> relocations,
                                           size_t& count) {
    hot_.align();
    cold_.align();
    if (failed_ || !hot_.valid() || !cold_.valid()) return {};
    const size_t hot = hot_.offset(), cold = cold_.offset(), total = (hot + cold) / 4;
    if (total > output.size() || call_count_ > relocations.size()) return {};
    std::copy_n(hot_.code().begin(), hot / 4, output.begin());
    std::copy_n(cold_.code().begin(), cold / 4, output.begin() + hot / 4);
    const auto offset = [&](Label label) { return (label & kCold ? hot : 0) + (label & ~kCold); };
    for (unsigned i = 0; i < reference_count_; ++i) {
        const auto& r = references_[i];
        if (!r.bound) return {};
        const auto at = offset(r.from);
        const auto distance = static_cast<int32_t>(static_cast<int64_t>(offset(r.to)) - static_cast<int64_t>(at));
        const auto word = r.kind == Kind::branch ? r.op(r.a, r.b, distance) : rv::jal(R::zero, distance);
        if (!word.valid) return {};
        jit::write_at(output.data(), at, word.bits);
    }
    for (unsigned i = 0; i < call_count_; ++i)
        relocations[i] = {static_cast<uint32_t>(offset(calls_[i].at)), calls_[i].target};
    count = call_count_;
    return output.first(total);
}

// --- Time and the bus --------------------------------------------------------

void Compiler::flush_charge() {
    if (charge_) emit(rv::addi(kRemaining, kRemaining, -static_cast<int32_t>(charge_)));
    charge_ = 0;
}

// One bus cycle through the gateway: a1 = request, a2 = address, a3 = data.
// The stubs keep every register but a0, which a read returns, and t6.
void Compiler::bus(uint32_t request) {
    if (mapped_) {
        m_access(request);
        return;
    }
    li(R::a1, request);
    call(stubs_.access);
    charge();
}

// A code fetch from a2, which also moves Arm7State::pipeline.
void Compiler::fetch(uint32_t request) {
    li(R::a1, kFetch | request);
    call(stubs_.fetch);
    charge();
}

// Every instruction's first cycle: the prefetch from R15. Its S is the
// previous instruction's last SEQ, which at a block's start Arm7State keeps.
void Compiler::prefetch() {
    if (mapped_) {
        m_prefetch();
        return;
    }
    li(R::a2, r15_);
    if (sequential_ == kRequested) {
        call(stubs_.fetch);
        charge();
        return;
    }
    uint32_t request = thumb_ ? kHalf : kWord;
    if (sequential_ < 0) request |= kStateSequential;
    else if (sequential_) request |= kSequential;
    fetch(request);
}

// a3 = the internal cycles of a multiply (multiply_cycles() in executor.h)
// plus `extra`, from the multiplier's leading bytes. Clobbers a2.
void Compiler::multiply_cycles(Value multiplier, bool is_signed, unsigned extra) {
    if (multiplier.fixed) {
        li(R::a3, gba::multiply_cycles(multiplier.constant, is_signed) + extra);
        return;
    }
    const R v = multiplier.reg;
    if (is_signed) {
        emit(rv::srai(R::t6, v, 31));
        emit(rv::xor_(R::a3, v, R::t6));
    } else {
        mv(R::a3, v);
    }
    if (rv::kZb) {
        // max(1, (39 - leading zeros) / 8): the bytes above the lowest in use.
        emit(rv::clz(R::t6, R::a3));
        emit(rv::sub(R::a3, R::zero, R::t6));
        emit(rv::addi(R::a3, R::a3, 39));
        emit(rv::srli(R::a3, R::a3, 3));
        emit(rv::addi(R::t6, R::zero, 1));
        emit(rv::maxu(R::a3, R::a3, R::t6));
        if (extra) emit(rv::addi(R::a3, R::a3, static_cast<int32_t>(extra)));
        return;
    }
    emit(rv::srli(R::t6, R::a3, 8));
    emit(rv::sltu(R::t6, R::zero, R::t6));
    for (const unsigned shift : {16u, 24u}) {
        emit(rv::srli(R::a2, R::a3, shift));
        emit(rv::sltu(R::a2, R::zero, R::a2));
        emit(rv::add(R::t6, R::t6, R::a2));
    }
    emit(rv::addi(R::a3, R::t6, static_cast<int32_t>(1 + extra)));
}

// a3 internal cycles, at least one.
void Compiler::idle_loop() {
    if (mapped_) {
        m_idle_loop();
        return;
    }
    flush_charge();
    emit(rv::sub(kRemaining, kRemaining, R::a3));
    const Label loop = here();
    li(R::a1, kIdle);
    call(stubs_.access);
    emit(rv::addi(R::a3, R::a3, -1));
    bind(branch(rv::bne, R::a3, R::zero), loop);
}

// --- Guest registers ---------------------------------------------------------

// A value in a register; constants go to `scratch`.
R Compiler::reg(Value value, R scratch) {
    if (!value.fixed) return value.reg;
    if (!value.constant) return R::zero;
    li(scratch, value.constant);
    return scratch;
}

void Compiler::load_value(R d, Value value) {
    if (value.fixed) li(d, value.constant);
    else mv(d, value.reg);
}

// Guest register n < 15 = value.
void Compiler::set_guest(unsigned n, Value value) {
    clobber(host(n));
    load_value(host(n), value);
}

// d = a op b. t6 holds constants that no immediate takes.
void Compiler::binary(Op op, R d, Value a, Value b) {
    if (op == Op::mov) {
        load_value(d, b);
        return;
    }
    if (op == Op::mvn) {
        if (b.fixed) li(d, ~b.constant);
        else emit(rv::xori(d, b.reg, -1));
        return;
    }
    if (a.fixed && b.fixed) {
        const uint32_t x = a.constant, y = b.constant;
        switch (op) {
        case Op::add: li(d, x + y); break;
        case Op::sub: li(d, x - y); break;
        case Op::and_: li(d, x & y); break;
        case Op::or_: li(d, x | y); break;
        case Op::xor_: li(d, x ^ y); break;
        default: li(d, x & ~y); break;
        }
        return;
    }
    // Zbs sets, inverts and clears a single bit that no immediate takes,
    // such as the bits of I/O registers.
    if (rv::kZb && a.fixed != b.fixed && op != Op::add && op != Op::sub) {
        if (a.fixed && op != Op::andn) std::swap(a, b);
        const uint32_t bit = op == Op::and_ ? ~b.constant : b.constant;
        if (b.fixed && std::has_single_bit(bit) && !fits(op == Op::andn ? ~b.constant : b.constant)) {
            const auto n = static_cast<unsigned>(std::countr_zero(bit));
            emit(op == Op::or_ ? rv::bseti(d, a.reg, n) : op == Op::xor_ ? rv::binvi(d, a.reg, n) : rv::bclri(d, a.reg, n));
            return;
        }
    }
    using Immediate = rv::Instruction (*)(R, R, int32_t);
    using Registers = rv::Instruction (*)(R, R, R);
    Immediate with_immediate = nullptr;
    Registers with_register = nullptr;
    switch (op) {
    case Op::add: with_immediate = rv::addi; with_register = rv::add; break;
    case Op::and_: with_immediate = rv::andi; with_register = rv::and_; break;
    case Op::or_: with_immediate = rv::ori; with_register = rv::or_; break;
    case Op::xor_: with_immediate = rv::xori; with_register = rv::xor_; break;
    case Op::sub:
        if (!b.fixed) emit(rv::sub(d, reg(a, R::t6), b.reg));
        else if (fits(0u - b.constant)) emit(rv::addi(d, a.reg, immediate(0u - b.constant)));
        else {
            li(R::t6, b.constant);
            emit(rv::sub(d, a.reg, R::t6));
        }
        return;
    default:  // a & ~b
        if (b.fixed) {
            if (fits(~b.constant)) emit(rv::andi(d, a.reg, immediate(~b.constant)));
            else {
                li(R::t6, ~b.constant);
                emit(rv::and_(d, a.reg, R::t6));
            }
        } else if (rv::kZb) {
            emit(rv::andn(d, reg(a, R::t6), b.reg));
        } else {
            emit(rv::xori(R::t6, b.reg, -1));
            emit(rv::and_(d, reg(a, d), R::t6));
        }
        return;
    }
    // Commutative from here on.
    if (a.fixed) std::swap(a, b);
    if (!b.fixed) emit(with_register(d, a.reg, b.reg));
    else if (fits(b.constant)) emit(with_immediate(d, a.reg, immediate(b.constant)));
    else {
        li(R::t6, b.constant);
        emit(with_register(d, a.reg, R::t6));
    }
}

// --- Exits and control flow --------------------------------------------------

// Leaves the block after the current instruction, through the gateway's
// exit: R15 is r15 or, when dynamic, a0. A synced exit follows a helper that
// wrote the guest registers and CPSR to Arm7State itself.
void Compiler::leave(uint32_t r15, bool dynamic, int sequential, Arm7JitExit reason, bool synced) {
    if (mapped_) {
        m_leave(r15, dynamic, sequential, reason, synced);
        return;
    }
    flush_charge();
    if (!synced) materialize_all();
    out().align();
    call(synced ? stubs_.exit_synced : stubs_.exit);
    data(dynamic ? 0 : r15);
    data(completed_ << exit_bits::completed | (sequential == 1 ? exit_bits::sequential : 0) |
         (sequential == kRequested ? 1u << exit_bits::sequential_request : 0) |
         static_cast<uint32_t>(reason) << exit_bits::reason | (dynamic ? exit_bits::dynamic : 0));
    end_ = true;
}

// Before each instruction but the first: stop when no bus cycles remain,
// describing the pending flags (side_bits).
void Compiler::boundary() {
    // A mapped run may enter here when no flag is pending: a pending one
    // follows from registers as this block's path left them, which another
    // path to the same address need not (nba-hw-test status-irq-dma).
    const auto& pending = flags_;
    if (mapped_ && pending.nz == kNzDone && pending.c == kCDone && !pending.v && free_held() == kHeld.size() &&
        sequential_ != kRequested && !bl_pending_ && boundary_count_ < boundaries_.size())
        boundaries_[boundary_count_++] = {r15_, static_cast<uint16_t>(here()), static_cast<uint8_t>(index_),
                                          sequential_ == 1};
    const unsigned stop = branch(rv::bge, R::zero, kRemaining);
    const Label resume = here();
    cold_mode_ = true;
    out().align();
    bind(stop);
    if (mapped_) {
        // Events come due here: the run goes on unless it must stop, see
        // the gateway's m_stop.
        call(stubs_.m_stop);
        bind(jump(), resume);
    } else {
        call(stubs_.side_exit);
    }
    data(r15_);
    const auto& f = flags_;
    uint32_t bits = index_ << side_bits::completed | (sequential_ == 1 ? side_bits::sequential : 0) |
                    (sequential_ == kRequested ? side_bits::sequential_request : 0) |
                    uint32_t{f.nz} << side_bits::nz | uint32_t{f.c} << side_bits::c | (f.v ? side_bits::v : 0) |
                    uint32_t{f.arith} << side_bits::arith;
    if (f.nz == kNzRegister) bits |= number(f.nz_reg) << side_bits::nz_register;
    if (f.c == kCBit || f.c == kCMultiply)
        bits |= number(f.c_first) << side_bits::c_first | uint32_t{f.c_second} << side_bits::c_second;
    data(bits);
    if (f.arith != kArithNone) {
        const auto code = [](const Operand& o) {
            return o.kind == kUnknown ? kOperandUnknown : o.kind == kImmediate ? kOperandImmediate : number(o.reg);
        };
        data(code(f.result) | code(f.first) << 6 | code(f.second) << 12);
        if (f.first.kind == kImmediate) data(f.first.imm);
        if (f.second.kind == kImmediate) data(f.second.imm);
    }
    cold_mode_ = false;
}

// Refills the pipeline from a constant target in the block's state, or in
// the given one after BX, and leaves (TRM 6.3).
void Compiler::refill_static(uint32_t target, bool thumb, bool synced) {
    if (mapped_) {
        m_refill_static(target, thumb, synced);
        return;
    }
    const uint32_t length = thumb ? 2 : 4, width = thumb ? kHalf : kWord;
    target &= ~(length - 1);
    li(R::a2, target);
    fetch(width);
    li(R::a2, target + length);
    fetch(width | kSequential);
    leave(target + 2 * length, false, true, Arm7JitExit::control_flow, synced);
}

// The same from a register that the fetches keep.
void Compiler::refill_dynamic(R target, bool thumb, bool synced) {
    if (mapped_) {
        m_refill_dynamic(target, thumb, synced);
        return;
    }
    const uint32_t length = thumb ? 2 : 4, width = thumb ? kHalf : kWord;
    emit(rv::andi(R::a3, target, -static_cast<int32_t>(length)));
    mv(R::a2, R::a3);
    fetch(width);
    emit(rv::addi(R::a2, R::a3, static_cast<int32_t>(length)));
    fetch(width | kSequential);
    emit(rv::addi(R::a0, R::a3, static_cast<int32_t>(2 * length)));
    leave(0, true, true, Arm7JitExit::control_flow, synced);
}

// A write to R15 in the block's state.
void Compiler::branch_to(Value target) {
    if (target.fixed) refill_static(target.constant, thumb_);
    else refill_dynamic(target.reg, thumb_);
}

// After a helper restored CPSR: the refill in the state it set.
void Compiler::refill_by_state(R target) {
    flush_charge();
    state_pointer(R::a0);
    emit(rv::lw(R::a0, R::a0, kStateCpsr));
    emit(rv::andi(R::a0, R::a0, kFlagT));
    const unsigned thumb = branch(rv::bne, R::a0, R::zero);
    refill_dynamic(target, false, true);
    bind(thumb);
    end_ = false;
    refill_dynamic(target, true, true);
}

// Writes the guest registers and NZCV to Arm7State for a helper that changes
// the mode; the block leaves synced after it.
void Compiler::sync() {
    materialize_all();
    call(stubs_.sync_out);
}

// Data processing with S into R15, and LDM with S loading R15, in a mode with
// an SPSR: CPSR = SPSR, then the refill in the restored state.
void Compiler::return_from_exception(R target) {
    sync();
    li(R::a1, 0);
    call(stubs_.restore_cpsr);
    refill_by_state(target);
}

// GBATEK "ARM CPU Exceptions": SWI and undefined instructions, the latter
// after an internal cycle (TRM 6.18). The return address is the next
// instruction's.
void Compiler::exception(uint32_t mode, uint32_t vector, bool undefined) {
    if (undefined) idle();
    sync();
    li(R::a1, mode);
    li(R::a2, r15_ - length_);
    call(stubs_.exception);
    refill_static(vector, false, true);
}

// CPSR's T in Arm7State, for BX: exits keep the bits other than NZCV.
void Compiler::set_thumb(bool on) {
    state_pointer(R::a0);
    emit(rv::lw(R::a1, R::a0, kStateCpsr));
    emit(on ? rv::ori(R::a1, R::a1, kFlagT) : rv::andi(R::a1, R::a1, ~static_cast<int32_t>(kFlagT)));
    emit(rv::sw(R::a1, R::a0, kStateCpsr));
}

// BX: Thumb state when bit 0 of the target is set.
void Compiler::exchange(Value target) {
    if (target.fixed) {
        const bool thumb = target.constant & 1;
        if (thumb != thumb_) set_thumb(thumb);
        refill_static(target.constant, thumb);
        return;
    }
    flush_charge();
    materialize_all();
    emit(rv::andi(R::t6, target.reg, 1));
    const unsigned to_thumb = branch(rv::bne, R::t6, R::zero);
    if (thumb_) set_thumb(false);
    refill_dynamic(target.reg, false);
    bind(to_thumb);
    end_ = false;
    if (!thumb_) set_thumb(true);
    refill_dynamic(target.reg, true);
}

}  // namespace gbx::gba::arm7_compiler
