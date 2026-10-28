#include "sm83_jit_runtime.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

#include "gbx/jit/assembler.h"

namespace gbx::gb {
namespace {
using namespace sm83_runtime;
namespace rv = jit::rv32;
using R = rv::Reg;
using Branch = rv::Instruction (*)(R, R, int32_t);

constexpr int A = offsetof(Sm83State, a), F = offsetof(Sm83State, f);
constexpr int PC = offsetof(Sm83State, pc), SP = offsetof(Sm83State, sp);
constexpr int IME = offsetof(Sm83State, ime), EI = offsetof(Sm83State, ei_pending);
constexpr int MODE = offsetof(Sm83State, mode), HALT_BUG = offsetof(Sm83State, halt_bug);
constexpr int registers[]{offsetof(Sm83State, b), offsetof(Sm83State, c),
    offsetof(Sm83State, d), offsetof(Sm83State, e), offsetof(Sm83State, h),
    offsetof(Sm83State, l), 0, A};
constexpr int resident_offsets[]{A,F,registers[0],registers[1],registers[2],registers[3],
    registers[4],registers[5],SP,PC};
constexpr R saved(unsigned i) {
    return i < 2 ? static_cast<R>(8 + i) : static_cast<R>(16 + i);
}
// Pinned while mapped code runs, see sm83_jit_runtime.h.
constexpr R kBudget = R::a3, kCount = R::a4, kLazy = R::a5, kFast = R::a6, kPages = R::a7;
constexpr int kPageBias = 2048;
// Registers out-of-line stubs keep for generated code, in Context::save[1..].
// Light stubs keep only the pinned ones, for an instruction's single access.
constexpr R kPreserved[]{R::a0, R::a1, R::a2, R::a4, R::a5, R::t0, R::t1, R::t2, R::t3, R::t4, R::t5};
constexpr R kPinned[]{R::a4, R::a5};

// Resident register of a CPU state field.
constexpr R guest(int offset) {
    for (unsigned i = 0; i < 10; ++i) if (resident_offsets[i] == offset) return saved(i + 2);
    return R::zero;
}

// Follows Pan Docs' opcode families and flag definitions, gbctr.pdf's M-cycle
// sequences, and the corrected ordinary STOP contract used by the interpreter.
// The compiler emits integer operations; it never calls the interpreter's ALU.
//
// Mapped blocks keep a hot stream and an out-of-line cold stream. Before each
// instruction, one check proves that its fetch and leading idle M-cycles end
// before the next event; ROM bytes are then compile-time constants. So are
// the bytes of a verified RAM block, which compares all of them with its
// pages on entry; its writes that may reach its own code go through C++.
// Other RAM blocks verify opcodes from their page at each fetch. A failed
// instruction check executes the whole fetch group in C++. Data accesses,
// exits and most flag arithmetic call shared code in the gateway (Sm83Jit::
// Stubs), which keeps blocks small, so that the code pool holds more of a
// game's working set: an access checks its M-cycle and page there and takes
// the exact path in C++ when either fails. Exits use a link word that C++
// patches once the destination exists. Flat test blocks share the semantics
// code and send every access through the flat-bus stubs.
//
// Instructions use their 16-bit forms (RVC) where one exists. Words that are
// read as data or changed after publication stay aligned: the header, the
// words after a call (descriptors, link words, code ranges), the code copy
// and the link table. A call followed by data starts on a word, after a
// c.nop where needed. Calls are relocated at installation as 32-bit `jal`
// on any even offset.
class Compiler {
public:
    // short_jumps: jumps and near branches take their 16-bit forms, and
    // compile() fails, with short_range_failed(), when one does not reach.
    Compiler(std::span<uint32_t> hot, std::span<uint32_t> cold, const Sm83Jit::Stubs& stubs,
             const uint8_t* image, Memory* memory, uint16_t pc, bool marker,
             std::span<const uint8_t> prefetched, bool halt_bug, bool pending_ei, bool verify, bool short_jumps)
        : hot_(hot, true), cold_(cold, true), short_jumps_(short_jumps), stubs_(stubs), image_(image), memory_(memory),
          pc_(pc), entry_pc_(pc),
          marker_(marker), prefetched_(prefetched), halt_bug_(halt_bug),
          pending_ei_(memory ? static_cast<int>(pending_ei) : -1),
          verified_(verify && memory && !halt_bug && prefetched.empty() && verifiable(pc)) {}

    // Returns the block words; relocations give byte offsets into them.
    // Empty on failure.
    std::span<const uint32_t> compile(unsigned limit, std::span<uint32_t> output,
                                      std::span<jit::Relocation> relocations, size_t& relocation_count) {
        for (unsigned i = 0; i < limit && !end_; ++i) {
            if (verified_ && i && !verifiable(pc_)) break;
            instruction();
            ++instructions_;
        }
        if (!end_) {
            done_ = completed_;
            if (memory_) exit_link(pc_, JitExit::limit);
            else flat_exit(JitExit::limit);
        }
        if (!memory_) emit_flat_tail();
        return finish(output, relocations, relocation_count);
    }

    // Instruction starts after the first, where execution may enter the
    // block with the instructions before it counted as completed.
    // offset: bytes from the block's start.
    struct Boundary { uint32_t offset; uint16_t pc; uint8_t completed; bool lazy_zero; };
    std::span<const Boundary> boundaries() const { return std::span(boundaries_).first(boundary_count_); }
    // Word index of a verified block's code copy: its range word, then the
    // bytes. C++ compares them before a mid-block entry. Zero otherwise.
    size_t code_copy() const { return code_copy_; }
    unsigned instructions() const { return instructions_; }
    bool short_range_failed() const { return short_range_failed_; }
    // Whether the block's code lies in ROM, whose bytes change only with the
    // banks in its key, so that a copy of the block stays valid.
    bool fixed_code() const {
        return memory_ && prefetched_.empty() && entry_pc_ + ((pc_ - entry_pc_) & 0xffff) <= 0x8000;
    }

private:
    struct Label { uint32_t at = 0; bool cold = false; };  // Bytes into the stream.
    enum class Kind : uint8_t { branch, jump };
    struct Reference { Label from, to; Kind kind; Branch op; R a, b; bool bound, compressed; };
    struct Call { Label at; uintptr_t target; };

    // --- Streams, labels and relocations ---------------------------------
    jit::Assembler& out() { return cold_mode_ ? cold_ : hot_; }
    Label here() { return {static_cast<uint32_t>(out().offset()), cold_mode_}; }
    void emit(rv::Instruction word) { out().emit(word); }
    void data(uint32_t word) { out().data(word); }
    // A near branch skips a few instructions in its own stream; c.beqz and
    // c.bnez reach 256 bytes. c.j reaches 2 KiB, which covers the jumps
    // between the streams of most blocks.
    unsigned reference(Kind kind, Branch op = nullptr, R a = R::zero, R b = R::zero, bool near = false) {
        if (reference_count_ == references_.size()) { overflow_ = true; return 0; }
        const bool compressed = short_jumps_ && (kind == Kind::jump || near) &&
            rv::compress(kind == Kind::jump ? rv::jal(R::zero, 0) : op(a, b, 0)).compressed;
        references_[reference_count_] = {here(), {}, kind, op, a, b, false, compressed};
        out().emit_exact(compressed ? rv::c_nop() : rv::nop());
        return reference_count_++;
    }
    void bind(unsigned index, Label to) { references_[index].to = to; references_[index].bound = true; }
    unsigned branch(Branch op, R a, R b, bool near = false) { return reference(Kind::branch, op, a, b, near); }
    unsigned jump() { return reference(Kind::jump); }
    void jump(Label to) { bind(jump(), to); }
    // with_data: data words follow, which the stub reads through t6, the
    // call's link, see sm83_jit_runtime.h.
    void call(uintptr_t target, bool with_data = false) {
        if (with_data) out().align();
        if (call_count_ == calls_.size()) { overflow_ = true; return; }
        calls_[call_count_++] = {here(), target};
        out().emit_exact(rv::jal(with_data ? R::t6 : R::ra, 0));
    }
    std::span<const uint32_t> finish(std::span<uint32_t> output, std::span<jit::Relocation> relocations,
                                     size_t& relocation_count) {
        // Each part starts on a word, so that its aligned words stay aligned.
        hot_.align(); cold_.align();
        if (!hot_.valid() || !cold_.valid() || overflow_) return {};
        // Mapped blocks start with their header (Sm83Jit::kHeaderWords), a
        // verified block's code check follows, and they end with their link
        // table; flat blocks start with the psABI prologue.
        std::array<uint32_t, 128> prologue_words{};
        jit::Assembler prologue(prologue_words, true);
        size_t stale_call = 0;
        if (memory_) {
            for (unsigned i = 0; i < Sm83Jit::kHeaderWords; ++i) prologue.data(0);
            // Mark the block used for CLOCK replacement. The gateway, links
            // and the dispatch table enter a block start with it in t6.
            prologue.emit(rv::lw(R::ra, R::t6, -4)); prologue.emit(rv::sw(R::t6, R::ra, 0));
        } else emit_flat_prologue(prologue);
        if (verified_) stale_call = emit_verification(prologue);
        prologue.align();
        if (!prologue.valid()) return {};
        const size_t head = prologue.offset(), hot = hot_.offset(), cold = cold_.offset();
        const unsigned length = (pc_ - entry_pc_) & 0xffff;
        // In words from here on.
        const size_t code = (head + hot + cold) / 4, copy = verified_ ? 1 + (length + 3) / 4 : 0;
        const size_t total = code + copy + (memory_ ? link_count_ + 1 : 0);
        if (total > output.size() || call_count_ + verified_ > relocations.size()) return {};
        std::copy_n(prologue.code().begin(), head / 4, output.begin());
        std::copy_n(hot_.code().begin(), hot / 4, output.begin() + head / 4);
        std::copy_n(cold_.code().begin(), cold / 4, output.begin() + (head + hot) / 4);
        const auto offset = [&](Label label) { return head + (label.cold ? hot : 0) + label.at; };
        for (unsigned i = 0; i < reference_count_; ++i) {
            const auto& r = references_[i];
            if (!r.bound) return {};
            const auto at = offset(r.from);
            const auto distance = static_cast<int32_t>(static_cast<int64_t>(offset(r.to)) - static_cast<int64_t>(at));
            auto word = r.kind == Kind::branch ? r.op(r.a, r.b, distance) : rv::jal(R::zero, distance);
            if (r.compressed) {
                word = rv::compress(word);
                if (!word.compressed) { short_range_failed_ = true; return {}; }
                auto& target = output[at / 4];
                const unsigned shift = (at & 2) * 8;
                target = (target & ~(0xffffu << shift)) | word.bits << shift;
            } else {
                if (!word.valid) return {};
                jit::write_at(output.data(), at, word.bits);
            }
        }
        for (unsigned i = 0; i < fixup_count_; ++i) {
            const auto at = offset(fixups_[i].at), back = offset(fixups_[i].back);
            if (at - back >= 2048) return {};
            output[at / 4] |= static_cast<uint32_t>((at - back) / 2) << 22;
        }
        for (unsigned i = 0; i < call_count_; ++i)
            relocations[i] = {static_cast<uint32_t>(offset(calls_[i].at)), calls_[i].target};
        relocation_count = call_count_;
        if (verified_) relocations[relocation_count++] = {static_cast<uint32_t>(stale_call), stubs_.exit};
        for (unsigned i = 0; i < range_count_; ++i) output[offset(ranges_[i]) / 4] = code_range();
        for (unsigned i = 0; i < boundary_count_; ++i)
            boundaries_[i].offset = static_cast<uint32_t>(offset(boundary_labels_[i]));
        if (verified_) {
            code_copy_ = code;
            output[code_copy_] = code_range();
            for (unsigned i = 1; i < copy; ++i) output[code_copy_ + i] = 0;
            for (unsigned k = 0; k < length; ++k)
                output[code_copy_ + 1 + k / 4] |= uint32_t{memory_->peek(static_cast<uint16_t>(entry_pc_ + k))} << (k % 4 * 8);
        }
        if (memory_) {
            // Word indices of the linked exits' calls.
            for (unsigned i = 0; i < link_count_; ++i) output[code + copy + i] = static_cast<uint32_t>(offset(links_[i]) / 4);
            output[total - 1] = link_count_;
        }
        return output.first(total);
    }

    // --- Verified RAM blocks ----------------------------------------------
    // Cartridge RAM, WRAM and HRAM pages change permissions only with the
    // mapping, which stops native code. VRAM follows PPU modes; OAM and IO
    // pages are never direct.
    bool static_ram(uint16_t address) const {
        return ((address >= 0xa000 && address < 0xfe00) || address >= 0xff80) &&
            memory_->pages[address >> 7].read;
    }
    // A verified block spans at most its entry page and the next one.
    bool verifiable(uint16_t pc) const {
        const unsigned length = instruction_length(memory_->peek(pc));
        for (unsigned k = 0; k < length; ++k) {
            const auto address = static_cast<uint16_t>(pc + k);
            if ((address >> 7) - (entry_pc_ >> 7) > 1u || !static_ram(address)) return false;
        }
        return true;
    }
    uint32_t code_range() const { return entry_pc_ | ((pc_ - entry_pc_) & 0xffff) << 16; }
    // Pages a verified block's code may occupy: at most 16 instructions of
    // three bytes from its entry.
    unsigned code_pages(std::array<unsigned, 2>& pages) const {
        unsigned count = 0;
        pages[count++] = entry_pc_ >> 7;
        const auto next = static_cast<uint16_t>((entry_pc_ | 127) + 1);
        if ((entry_pc_ & 127) + 48 > 128 && next && static_ram(next)) pages[count++] = next >> 7;
        return count;
    }
    // The C++ path compares the written byte with the final code range.
    void range_word() {
        if (range_count_ == ranges_.size()) overflow_ = true;
        else ranges_[range_count_++] = here();
        data(0);
    }
    // Compares every code byte with its page, aligned words at a time, and
    // leaves through a refetch exit marked stale on any difference.
    size_t emit_verification(jit::Assembler& out) {
        // Branch positions; page checks test t6, word checks t5 against t4.
        struct Fail { size_t at; bool page; };
        std::array<Fail, 40> fails{};
        unsigned fail_count = 0;
        const unsigned length = (pc_ - entry_pc_) & 0xffff;
        for (unsigned k = 0; k < length;) {
            const unsigned page = static_cast<uint16_t>(entry_pc_ + k) >> 7;
            out.emit(rv::lw(R::t6, kPages, static_cast<int32_t>(page * 8) - kPageBias));
            if (fail_count == fails.size()) { overflow_ = true; return 0; }
            fails[fail_count++] = {out.offset(), true}; out.emit_exact(rv::nop());
            while (k < length && (static_cast<unsigned>(static_cast<uint16_t>(entry_pc_ + k)) >> 7) == page) {
                const auto address = static_cast<uint16_t>(entry_pc_ + k);
                const unsigned first = address & 3, last = std::min(4u, first + length - k);
                uint32_t expected = 0;
                for (unsigned b = first; b < last; ++b)
                    expected |= uint32_t{memory_->peek(static_cast<uint16_t>(address - first + b))} << (b * 8);
                out.emit(rv::lw(R::t5, R::t6, address & 124));
                // Keep bytes [first, last) of the word.
                const unsigned left = (4 - last) * 8, right = (first + 4 - last) * 8;
                if (left) out.emit(rv::slli(R::t5, R::t5, left));
                if (right) out.emit(rv::srli(R::t5, R::t5, right));
                out.li(R::t4, static_cast<uint32_t>(expected << left) >> right);
                if (fail_count == fails.size()) { overflow_ = true; return 0; }
                fails[fail_count++] = {out.offset(), false}; out.emit_exact(rv::nop());
                k += last - first;
            }
        }
        // The last comparison skips the exit when it matches. The exit's
        // descriptor follows its call, which therefore starts on a word.
        out.align();
        const size_t fail = out.offset();
        for (unsigned i = 0; i < fail_count; ++i) {
            const auto offset = static_cast<int32_t>(fail - fails[i].at);
            if (fails[i].page) out.patch(fails[i].at, rv::beq(R::t6, R::zero, offset));
            else if (i + 1 == fail_count) out.patch(fails[i].at, rv::beq(R::t5, R::t4, offset + 8));
            else out.patch(fails[i].at, rv::bne(R::t5, R::t4, offset));
        }
        out.emit_exact(rv::jal(R::t6, 0));  // To exit, relocated.
        out.data(descriptor(entry_pc_, JitExit::refetch, 0, false) | exit_bits::stale);
        return fail;
    }

    // --- Guest registers --------------------------------------------------
    void move(R d, R s) { if (d != s) emit(rv::addi(d, s, 0)); }
    void mask16(R r) { out().zero_extend16(r, r); }
    static constexpr auto resident_indices = [] {
        std::array<unsigned, sizeof(Sm83State)> result{};
        for (unsigned i = 0; i < 10; ++i) result[resident_offsets[i]] = i + 1;
        return result;
    }();
    static unsigned mask_for(int offset) {
        const auto index = resident_indices[offset];
        return index ? 1u << (index - 1) : 0;
    }
    R resident(int offset) {
        const auto index = resident_indices[offset];
        if (!index) return R::zero;
        used_ |= 1u << (index - 1);
        return saved(index + 1);
    }
    void materialize_flags() {
        if (!pending_zero_) return;
        resident(F);
        call(stubs_.zero_flag);
        pending_zero_ = false;
    }
    // After a branch on a pending Z, its value is known on each outcome. A
    // pending Z is clear in F, so only a set Z costs an instruction.
    void known_zero(bool set) {
        if (!pending_zero_) return;
        if (set) emit(rv::ori(resident(F), resident(F), 0x80));
        pending_zero_ = false;
    }
    // A resident register the following code writes in place.
    R def(int offset) {
        if (offset == F) pending_zero_ = false;
        dirty_ |= mask_for(offset);
        return resident(offset);
    }
    void load(R d, int offset, bool wide = false) {
        if (offset == F) materialize_flags();
        if (resident(offset) != R::zero) move(d, resident(offset));
        else {
            emit(rv::lw(R::t6, R::s0, offsetof(Context, cpu)));
            emit(wide ? rv::lhu(d, R::t6, offset) : rv::lbu(d, R::t6, offset));
        }
    }
    void store(R s, int offset, bool wide = false) {
        if (offset == F) pending_zero_ = false;
        const auto dest = resident(offset);
        dirty_ |= mask_for(offset);
        if (dest == R::zero) {
            emit(rv::lw(R::t6, R::s0, offsetof(Context, cpu)));
            emit(wide ? rv::sh(s, R::t6, offset) : rv::sb(s, R::t6, offset));
        }
        else if (wide) out().zero_extend16(dest, s);
        else emit(rv::andi(dest, s, 255));
    }
    void constant(int offset, uint32_t value) {
        const auto dest = resident(offset);
        if (dest == R::zero) {
            if (value) { out().li(R::t0, value); store(R::t0, offset); }
            else store(R::zero, offset);
        } else {
            dirty_ |= mask_for(offset);
            if (offset == F) pending_zero_ = false;
            out().li(dest, value & ((offset == PC || offset == SP) ? 0xffff : 0xff));
        }
    }
    void pair(unsigned index, R d) {
        if (index == 3) { load(d, SP, true); return; }
        emit(rv::slli(d, resident(registers[index * 2]), 8));
        emit(rv::or_(d, d, resident(registers[index * 2 + 1])));
    }
    void set_pair(unsigned index, R s) {
        if (index == 3) { store(s, SP, true); return; }
        store(s, registers[index * 2 + 1]); emit(rv::srli(R::t5, s, 8));
        store(R::t5, registers[index * 2]);
    }
    // Static PC after the current instruction, or the flat runtime PC.
    void load_pc(R d) {
        if (memory_) out().li(d, pc_);
        else load(d, PC, true);
    }

    // --- Instruction fetch ------------------------------------------------
    uint8_t byte_at(unsigned index, uint16_t address) const {
        if (index < prefetched_.size()) return prefetched_[index];
        return memory_ ? memory_->peek(address) : image_[address];
    }
    void instruction() {
        const uint16_t start = pc_;
        // Mid-block entry needs the entry state of this boundary: no delayed
        // EI and no HALT bug, as in the key of a block starting here.
        if (memory_ && completed_ && !pending_ei_ && boundary_count_ < boundaries_.size()) {
            boundary_labels_[boundary_count_] = here();
            boundaries_[boundary_count_++] = {0, start, static_cast<uint8_t>(completed_), pending_zero_};
        }
        const bool bugged = halt_bug_ && completed_ == 0;
        // A HALT bug repeats the opcode address for the following byte.
        const auto address = [&](unsigned k) { return static_cast<uint16_t>(start + k - (bugged && k)); };
        bytes_[0] = byte_at(0, address(0));
        length_ = instruction_length(bytes_[0]);
        bytes_[1] = length_ > 1 ? byte_at(1, address(1)) : 0;
        bytes_[2] = length_ > 2 ? byte_at(2, address(2)) : 0;
        const unsigned first = completed_ ? 0 : static_cast<unsigned>(std::min<size_t>(prefetched_.size(), length_));
        rom_ = memory_ && start < 0x8000 && address(length_ - 1) < 0x8000;
        constant_ = verified_ || rom_;
        group_idles_ = leading_idles(bytes_[0]);
        if (memory_) mapped_group(start, first, bugged);
        else flat_group();
        pc_ = static_cast<uint16_t>(start + length_ - bugged);
        if (pending_ei_ < 0) {
            load(R::t0, EI);
            emit(rv::sw(R::t0, R::s0, offsetof(Context, value)));
        }
        if (pending_ei_ != 0) store(R::zero, EI);
        if (bugged) store(R::zero, HALT_BUG);
        done_ = completed_ + 1;
        // EI takes effect after the following instruction; DI cancels it.
        ime_ = bytes_[0] == 0xf3 || !pending_ei_ ? Ime::none : pending_ei_ > 0 ? Ime::set : Ime::pending;
        decode(bytes_[0]);
        if (group_idles_) overflow_ = true;  // Shape table and semantics disagree.
        if (light_) overflow_ = true;  // Marked for an access it did not make.
        if (!end_) {
            if (marker_ && bytes_[0] == 0x40) exit_plain(pc_, JitExit::breakpoint);
            else if (ime_ == Ime::set && memory_) exit_plain(pc_, JitExit::event);
            else if (ime_ != Ime::none) apply_ime();
        }
        ime_ = Ime::none;
        pending_ei_ = bytes_[0] == 0xfb;
        ++completed_;
    }
    void mapped_group(uint16_t start, unsigned first, bool bugged) {
        const unsigned cycles = length_ - first + group_idles_;
        // Writable regions verify opcode bytes from their page and read
        // operands at runtime. A page-crossing instruction there, a HALT bug
        // and a prefetched continuation always use the careful path.
        const bool careful = bugged || first || (!constant_ && ((start & 127) + length_ > 128));
        if (cycles) emit(rv::addi(R::s1, R::s1, static_cast<int32_t>(cycles)));
        unsigned slow[4]; unsigned count = 0;
        if (careful) slow[count++] = jump();
        else {
            slow[count++] = branch(rv::bltu, kFast, R::s1);
            if (!constant_) {
                emit(rv::lw(R::t6, kPages, (start >> 7) * 8 - kPageBias));
                slow[count++] = branch(rv::beq, R::t6, R::zero);
                const unsigned opcodes = bytes_[0] == 0xcb ? 2 : 1;
                for (unsigned k = 0; k < opcodes; ++k) {
                    emit(rv::lbu(R::a0, R::t6, (start + k) & 127));
                    emit(rv::addi(R::a0, R::a0, -static_cast<int32_t>(bytes_[k])));
                    slow[count++] = branch(rv::bne, R::a0, R::zero);
                }
                if (opcodes < length_) emit(rv::lbu(R::a0, R::t6, (start + 1) & 127));
                if (length_ == 3) {
                    emit(rv::lbu(R::ra, R::t6, (start + 2) & 127));
                    emit(rv::slli(R::ra, R::ra, 8)); emit(rv::or_(R::a0, R::a0, R::ra));
                }
            }
        }
        const auto back = here();
        cold_mode_ = true;
        const auto stub = here();
        for (unsigned i = 0; i < count; ++i) bind(slow[i], stub);
        if (rom_ && !careful && cycles <= 4) {
            // The runtime reads ROM bytes again from the bank in the key.
            // The word keeps the cycles counted above: a bank switch by the
            // previous instruction leaves through here, when another bank
            // may hold another instruction at start.
            call(stubs_.group_rom, true);
            if (fixup_count_ == fixups_.size()) overflow_ = true;
            else fixups_[fixup_count_++] = {here(), back};
            data(start | (cycles - 1) << 15 | completed_ << 17 | (pending_zero_ ? 1u << 21 : 0));
        } else {
            call(stubs_.group, true);
            const unsigned verify = constant_ ? (1u << length_) - 1 : bytes_[0] == 0xcb ? 3 : 1;
            data(start | length_ << 16 | group_idles_ << 18 | completed_ << 20 | (pending_zero_ ? 1u << 25 : 0));
            data(bytes_[0] | bytes_[1] << 8 | bytes_[2] << 16 | first << 24 | verify << 26 | (bugged ? 1u << 29 : 0));
            jump(back);
        }
        cold_mode_ = false;
    }
    void flat_group() {
        for (unsigned k = 0; k < length_; ++k) {
            load(R::a1, PC, true); call(stubs_.flat_read);
            load(R::t0, PC, true); emit(rv::addi(R::t0, R::t0, 1)); store(R::t0, PC, true);
            if (k == 1) move(R::t1, R::a0);
            if (k == 2) { emit(rv::slli(R::a0, R::a0, 8)); emit(rv::or_(R::a0, R::a0, R::t1)); }
        }
        for (unsigned i = 0; i < group_idles_; ++i) call(stubs_.flat_idle);
    }
    // Immediate operand: a compile-time constant in ROM, otherwise a0 after
    // the group. Every caller consumes it before a0 is reused.
    void immediate(R d) {
        if (constant_) out().li(d, immediate_value());
        else move(d, R::a0);
    }
    uint16_t immediate_value() const { return static_cast<uint16_t>(bytes_[1] | (length_ == 3 ? bytes_[2] << 8 : 0)); }

    // --- Data accesses ----------------------------------------------------
    // The address is in a1 or a register pair, see Sm83Jit::Stubs. Reads go
    // into a0 or a resident register; writes take a2 or a resident register.
    // Mapped accesses clobber a1.
    enum : unsigned { in_a1, at_bc, at_de, at_hl };
    void read(R into = R::a0, unsigned address = in_a1) { access(false, address, into); }
    void write(R value = R::a2, unsigned address = in_a1) { access(true, address, value); }
    // A single access of an instruction, with no temporary live across it,
    // may use the light paths, which keep fewer registers around C++.
    bool take_light() {
        const bool light = light_;
        light_ = false;
        return light;
    }
    uintptr_t careful_stub(bool write, bool light) const {
        if (write) return light ? stubs_.write_light : stubs_.write;
        return light ? stubs_.read_light : stubs_.read;
    }
    // A register's index in the shared access code, 8 when it has none.
    unsigned shared_index(R r, bool write) const {
        if (r == (write ? R::a2 : R::a0)) return 6;
        for (unsigned i = 0; i < 8; ++i) if (i != 6 && guest(registers[i]) == r) return i;
        return 8;
    }
    void shared_access(bool write, unsigned address, R r, bool light) {
        auto index = shared_index(r, write);
        if (index == 8 && write) { move(R::a2, r); index = 6; }
        call((write ? stubs_.store : stubs_.load)[light][address][index == 8 ? 6 : index]);
        if (index == 8) move(r, R::a0);
    }
    void access(bool write, unsigned address, R r) {
        if (group_idles_) overflow_ = true;
        const bool light = take_light();
        if (memory_ && !(write && verified_)) { shared_access(write, address, r, light); return; }
        if (address != in_a1) pair(address - 1, R::a1);
        if (write) move(R::a2, r);
        if (!memory_) {
            call(write ? stubs_.flat_write : stubs_.flat_read);
            if (!write) move(r, R::a0);
            return;
        }
        // A verified block's write to a page of its own code takes the C++
        // path, which compares the byte with the code range.
        unsigned slow[5]; unsigned count = 0;
        slow[count++] = branch(rv::bgeu, R::s1, kFast);
        // IE writes always change interrupt state through Memory.
        emit(rv::addi(R::t6, R::a1, 1)); emit(rv::srli(R::t6, R::t6, 16));
        slow[count++] = branch(rv::bne, R::t6, R::zero);
        emit(rv::srli(R::t6, R::a1, 7));
        if (rv::kZb) emit(rv::sh3add(R::t6, R::t6, kPages));
        else { emit(rv::slli(R::t6, R::t6, 3)); emit(rv::add(R::t6, R::t6, kPages)); }
        emit(rv::lw(R::t6, R::t6, 4 - kPageBias));
        slow[count++] = branch(rv::beq, R::t6, R::zero);
        std::array<unsigned, 2> pages{};
        for (unsigned i = 0, n = code_pages(pages); i < n; ++i) {
            emit(rv::lw(R::ra, kPages, static_cast<int32_t>(pages[i] * 8) - kPageBias));
            slow[count++] = branch(rv::beq, R::t6, R::ra);
        }
        emit(rv::andi(R::ra, R::a1, 127)); emit(rv::add(R::t6, R::t6, R::ra));
        emit(rv::sb(R::a2, R::t6, 0));
        emit(rv::addi(R::s1, R::s1, 1));
        const auto back = here();
        cold_mode_ = true;
        const auto stub = here();
        for (unsigned i = 0; i < count; ++i) bind(slow[i], stub);
        call(stubs_.code_write, true); range_word();
        jump(back);
        cold_mode_ = false;
    }
    // Whether a static write may reach a page of this verified block's code,
    // including through echo RAM or mirrored cartridge RAM.
    bool code_page(uint16_t address) const {
        const auto* data = memory_->backing(address);
        if (!data) return false;
        std::array<unsigned, 2> pages{};
        for (unsigned i = 0, n = code_pages(pages); i < n; ++i)
            if (memory_->backing(static_cast<uint16_t>(pages[i] * 128)) == data - (address & 127)) return true;
        return false;
    }
    // r: the destination of a read or the value of a write.
    void access_at(bool write, uint16_t address, R r) {
        if (group_idles_) overflow_ = true;
        if (write && verified_ && code_page(address)) { out().li(R::a1, address); access(true, in_a1, r); return; }
        if (!write && memory_ && address >= 0xff00 && address < 0xff80 &&
            memory_->io_read_mask(static_cast<uint8_t>(address)) >= 0) { io_read(address, r); return; }
        const bool light = take_light();
        out().li(R::a1, address);
        if (memory_ && (address < 0xfe00 || address >= 0xff80) && !(write && address == 0xffff)) {
            shared_access(write, in_a1, r, light);
            return;
        }
        // OAM, unusable and IO pages are never direct.
        if (write) move(R::a2, r);
        call(!memory_ ? (write ? stubs_.flat_write : stubs_.flat_read) : careful_stub(write, light));
        if (!write) move(r, R::a0);
    }
    // A register that reads as io[address] | mask, such as STAT or LY, is
    // read inline before the next event, even one a batch defers. A CGB's
    // LY reads otherwise in the last 2 ticks before it changes, see
    // Ppu::ly_cgb(), which at double speed fall in the last M-cycle before
    // the event; LY reads inline only before that one.
    void io_read(uint16_t address, R into) {
        const auto mask = memory_->io_read_mask(static_cast<uint8_t>(address));
        const auto careful = careful_stub(false, take_light());
        const auto location = reinterpret_cast<uintptr_t>(&memory_->io[address & 127]);
        const int32_t low = static_cast<int32_t>(location & 0xfff) - ((location & 0x800) ? 4096 : 0);
        emit(rv::lw(R::t6, R::s0, offsetof(Context, exact_until)));
        // `into`, A's resident register, is written on either path, so it
        // can hold the next M-cycle.
        const bool ly = (address & 127) == 0x44 && memory_->color();
        if (ly) emit(rv::addi(into, R::s1, 1));
        const unsigned slow = branch(rv::bgeu, ly ? into : R::s1, R::t6);
        emit(rv::lui(R::t6, static_cast<uint32_t>((location + 0x800) >> 12)));
        emit(rv::lbu(into, R::t6, low));
        if (mask) emit(rv::ori(into, into, mask));
        emit(rv::addi(R::s1, R::s1, 1));
        const auto back = here();
        cold_mode_ = true;
        bind(slow, here());
        out().li(R::a1, address);
        call(careful);
        move(into, R::a0);
        jump(back);
        cold_mode_ = false;
    }
    void idle() {
        if (group_idles_) { --group_idles_; return; }
        call(memory_ ? stubs_.idle_fast : stubs_.flat_idle);
    }

    // --- Exits ------------------------------------------------------------
    enum class Ime : uint8_t { none, set, pending };
    // The instruction after EI sets IME before leaving. Flat tests keep the
    // EI state of their initial CPU, sampled into Context::value.
    void apply_ime() {
        if (ime_ == Ime::set) constant(IME, 1);
        else if (ime_ == Ime::pending) {
            emit(rv::lw(R::t0, R::s0, offsetof(Context, value))); load(R::t1, IME);
            emit(rv::or_(R::t1, R::t1, R::t0)); store(R::t1, IME);
        }
    }
    uint32_t descriptor(uint32_t pc, JitExit reason, unsigned completed, bool lazy,
                        uint32_t link = kLinkNone, uint32_t action = kActionNone) const {
        return pc | completed << exit_bits::completed | static_cast<uint32_t>(reason) << exit_bits::reason |
            (lazy ? exit_bits::lazy_zero : 0) | link << exit_bits::link | action << exit_bits::action;
    }
    // Leave through a descriptor without a native link. A negative pc keeps
    // the dynamic PC in s11.
    void exit_plain(int pc, JitExit reason, uint32_t action = kActionNone) {
        if (!memory_) { flat_exit(reason); return; }
        // HALT samples IME before the delayed EI takes effect.
        if (action == kActionHalt && ime_ == Ime::set) action = kActionHaltIme;
        else apply_ime();
        call(stubs_.exit, true);
        data(descriptor(pc < 0 ? exit_bits::dynamic_pc : static_cast<uint32_t>(pc), reason, done_, pending_zero_,
                        kLinkNone, action));
        end_ = true;
    }
    // A linkable exit to a static (target >= 0) or dynamic (s11) PC.
    void exit_link(int target, JitExit reason) {
        if (!memory_) { flat_exit(reason); return; }
        // An IME change must reach C++ to check for a pending interrupt.
        // Setting IME also ends a batch: its deadline window does not cover
        // the events that may request an interrupt the CPU now takes.
        if (ime_ != Ime::none) { exit_plain(target, ime_ == Ime::set ? JitExit::event : reason); return; }
        const bool pending = pending_zero_;
        materialize_flags();
        // A native link stays valid while this block is valid only if the
        // destination depends on no other bank window, see regions(). Other
        // static targets, and dynamic ones, use the dispatch table, whose
        // tags carry the banks. Guest temporaries are dead at a block exit.
        const bool cgb = memory_->mode() == Mode::cgb;
        const bool linked = target >= 0 && !(regions(static_cast<uint16_t>(target), cgb) & ~regions(entry_pc_, cgb));
        if (done_ >= stubs_.link.size()) overflow_ = true;
        else if (linked) {
            out().align();
            if (link_count_ == links_.size()) overflow_ = true;
            else links_[link_count_++] = here();
            call(stubs_.link[done_], true);
        } else {
            if (target >= 0) out().li(saved(11), static_cast<uint32_t>(target));
            call(stubs_.dispatch[done_], true);
        }
        data(descriptor(target < 0 ? exit_bits::dynamic_pc : static_cast<uint32_t>(target), reason, 0, false,
                        linked ? kLinkStatic : kLinkNone));
        if (linked) data(0);  // The link word, set at installation.
        pending_zero_ = pending;
        end_ = true;
    }
    void flat_exit(JitExit reason) {
        const bool pending = pending_zero_;
        apply_ime();
        materialize_flags();
        out().li(R::t6, done_);
        emit(rv::sw(R::t6, R::s0, offsetof(Context, instructions)));
        out().li(R::a0, static_cast<uint32_t>(reason));
        if (flat_exit_count_ == flat_exits_.size()) overflow_ = true;
        else flat_exits_[flat_exit_count_++] = jump();
        pending_zero_ = pending;
        end_ = true;
    }
    void emit_flat_tail() {
        const auto tail = here();
        for (unsigned i = 0; i < flat_exit_count_; ++i) bind(flat_exits_[i], tail);
        emit(rv::sw(R::s1, R::s0, offsetof(Context, mcycles)));
        emit(rv::lw(R::t6, R::s0, offsetof(Context, cpu)));
        for (int offset : resident_offsets) if (dirty_ & mask_for(offset))
            emit((offset == SP || offset == PC) ? rv::sh(resident(offset), R::t6, offset) :
                                                 rv::sb(resident(offset), R::t6, offset));
        for (unsigned r = 0; r < 12; ++r) if (r < 2 || (used_ & (1u << (r - 2))))
            emit(rv::lw(saved(r), R::sp, r * 4));
        emit(rv::lw(R::ra, R::sp, 48));
        emit(rv::addi(R::sp, R::sp, 64)); emit(rv::ret());
    }
    void emit_flat_prologue(jit::Assembler& prefix) {
        prefix.emit(rv::addi(R::sp, R::sp, -64));
        prefix.emit(rv::sw(R::ra, R::sp, 48));
        for (unsigned r = 0; r < 12; ++r) if (r < 2 || (used_ & (1u << (r - 2))))
            prefix.emit(rv::sw(saved(r), R::sp, r * 4));
        prefix.emit(rv::addi(R::s0, R::a0, 0));
        prefix.emit(rv::addi(R::s1, R::zero, 0));
        prefix.emit(rv::lw(R::t6, R::s0, offsetof(Context, cpu)));
        for (int offset : resident_offsets) if (used_ & mask_for(offset))
            prefix.emit((offset == SP || offset == PC) ? rv::lhu(resident(offset), R::t6, offset) :
                                                        rv::lbu(resident(offset), R::t6, offset));
    }

    // --- Instruction semantics ---------------------------------------------
    void get8(unsigned index) {
        if (index == 6) read(R::a0, at_hl);
        else load(R::a0, registers[index]);
    }
    void set8(unsigned index, R value = R::a0) {
        if (index == 6) write(value, at_hl);
        else store(value, registers[index]);
    }
    void increment(int offset, int amount) {
        load(R::t0, offset, true); emit(rv::addi(R::t0, R::t0, amount)); store(R::t0, offset, true);
    }
    // Skip branch taken when the condition is false (Pan Docs, cc encodings).
    unsigned condition(unsigned index) {
        // A zero result means Z is set.
        // a1 is free here, and in x8-x15, which c.beqz and c.bnez need.
        if (index < 2 && pending_zero_) return branch(index & 1 ? rv::bne : rv::beq, kLazy, R::zero, true);
        emit(rv::andi(R::a1, resident(F), index < 2 ? 0x80 : 0x10));
        return branch(index & 1 ? rv::beq : rv::bne, R::a1, R::zero, true);
    }
    // PUSH of BC, DE, HL or AF (index 0-3), or of t2 (index 4).
    void push(unsigned index) {
        idle();
        if (index == 3) materialize_flags();
        if (index < 4) { resident(index == 3 ? A : registers[index * 2]); resident(index == 3 ? F : registers[index * 2 + 1]); }
        // A verified block's writes go through write(), which catches a
        // write to its own code.
        if (memory_ && !verified_) { def(SP); call(stubs_.push[index]); return; }
        if (index == 3) {
            load(R::t2, A); emit(rv::slli(R::t2, R::t2, 8)); load(R::t1, F); emit(rv::or_(R::t2, R::t2, R::t1));
        } else if (index < 3) pair(index, R::t2);
        // The high byte is written first.
        load(R::a1, SP, true); emit(rv::addi(R::a1, R::a1, -1)); mask16(R::a1);
        emit(rv::srli(R::a2, R::t2, 8)); write();
        load(R::a1, SP, true); emit(rv::addi(R::a1, R::a1, -2)); mask16(R::a1);
        emit(rv::andi(R::a2, R::t2, 255)); write();
        increment(SP, -2);
    }
    // POP into BC, DE, HL or AF (index 0-3), or into a0 (index 4).
    void pop(unsigned index) {
        if (index < 3) { def(registers[index * 2]); def(registers[index * 2 + 1]); }
        else if (index == 3) { def(A); def(F); }
        if (memory_) { def(SP); call(stubs_.pop[index]); return; }
        load(R::a1, SP, true); read(); move(R::t1, R::a0);
        load(R::a1, SP, true); emit(rv::addi(R::a1, R::a1, 1)); mask16(R::a1); read();
        emit(rv::slli(R::a0, R::a0, 8)); emit(rv::or_(R::a0, R::a0, R::t1));
        increment(SP, 2);
        if (index == 3) {
            emit(rv::andi(R::t0, R::a0, 240)); store(R::t0, F);
            emit(rv::srli(R::a0, R::a0, 8)); store(R::a0, A);
        } else if (index < 3) set_pair(index, R::a0);
    }
    void inc_dec(unsigned index, bool decrement) {
        if (index == 6) get8(index);
        else def(registers[index]);
        def(F);
        call(stubs_.inc_dec[decrement][index]);
        pending_zero_ = true;
        if (index == 6) write(R::a2, at_hl);
    }
    // The operand is a register holding a clean byte, or an immediate n with
    // x = zero. A and F are updated in place.
    void alu(unsigned operation, R x, int n = -1) {
        const bool constant = n >= 0;
        const R a = def(A), f = def(F);
        if (operation >= 4 && operation != 7) {
            if (operation == 4) emit(constant ? rv::andi(a, a, n) : rv::and_(a, a, x));
            if (operation == 5) emit(constant ? rv::xori(a, a, n) : rv::xor_(a, a, x));
            if (operation == 6) emit(constant ? rv::ori(a, a, n) : rv::or_(a, a, x));
            out().li(f, operation == 4 ? 32 : 0);
            move(kLazy, a);
            pending_zero_ = true;
            return;
        }
        if (constant) out().li(R::a0, static_cast<uint32_t>(n));
        else move(R::a0, x);
        call(stubs_.alu[operation]);
        pending_zero_ = true;
    }
    // a0 to a0; F gets every flag.
    void rotate(unsigned operation, bool zero_flag) {
        def(F);
        call(stubs_.rotate[zero_flag][operation]);
    }
    void daa() { def(A); def(F); call(stubs_.daa); }
    void add_hl(unsigned source) {
        idle();
        materialize_flags();
        if (source == 3) resident(SP);
        else { resident(registers[source * 2]); resident(registers[source * 2 + 1]); }
        def(registers[4]); def(registers[5]); def(F);
        call(stubs_.add_hl[source]);
    }
    void add_sp(bool to_hl) {
        immediate(R::t0); idle(); if (!to_hl) idle();
        load(R::t1, SP, true);
        out().sign_extend8(R::t2, R::t0); emit(rv::add(R::t3, R::t1, R::t2));
        emit(rv::xor_(R::t4, R::t1, R::t0)); emit(rv::xor_(R::t4, R::t4, R::t3));
        emit(rv::andi(R::t4, R::t4, 16)); emit(rv::slli(R::t4, R::t4, 1));
        emit(rv::andi(R::t5, R::t1, 255)); emit(rv::add(R::t5, R::t5, R::t0));
        emit(rv::srli(R::t5, R::t5, 8)); emit(rv::slli(R::t5, R::t5, 4)); emit(rv::or_(R::t4, R::t4, R::t5));
        store(R::t4, F); set_pair(to_hl ? 2 : 3, R::t3);
    }
    // Both outcomes of a condition continue from the state before it.
    void other_outcome(bool pending, unsigned skip, unsigned test) {
        pending_zero_ = pending; end_ = false;
        bind(skip, here());
        if (test < 2) known_zero(test == 0);
        if (memory_) exit_link(pc_, JitExit::control_flow);
        else flat_exit(JitExit::control_flow);
    }
    // kind: 0=JR, 1=JP, 2=CALL; test 4 means unconditional. ROM targets are
    // static; otherwise t4 holds the target across the condition and push.
    void transfer(unsigned kind, unsigned test = 4) {
        int target = -1;
        if (constant_) target = kind == 0 ? static_cast<uint16_t>(pc_ + static_cast<int8_t>(bytes_[1])) : immediate_value();
        else {
            immediate(R::t4);
            if (kind == 0) {
                out().sign_extend8(R::t4, R::t4);
                load_pc(R::t1); emit(rv::add(R::t4, R::t4, R::t1)); mask16(R::t4);
            }
        }
        const bool pending = pending_zero_;
        const auto skip = test < 4 ? condition(test) : 0;
        if (test < 2) known_zero(test == 1);
        if (kind == 2) { load_pc(R::t2); push(4); }
        else idle();
        if (target < 0) store(R::t4, PC, true);
        exit_link(target, JitExit::control_flow);
        if (test < 4) other_outcome(pending, skip, test);
    }
    void return_pc(bool reti) {
        pop(4); idle(); move(def(PC), R::a0);
        // RETI returns to System, which checks for a pending interrupt; the
        // interrupted instruction then resumes as a first entry.
        if (reti) { constant(IME, 1); store(R::zero, EI); exit_plain(-1, memory_ ? JitExit::event : JitExit::control_flow); }
        else exit_link(-1, JitExit::control_flow);
    }
    void cb() {
        const auto opcode = bytes_[1];
        const unsigned group = opcode >> 6, bit = (opcode >> 3) & 7, reg = opcode & 7;
        get8(reg);
        if (group == 0) { rotate(bit, true); set8(reg); }
        else if (group == 1) { def(F); call(stubs_.bit[bit]); }
        else {
            emit(group == 2 ? rv::andi(R::a0, R::a0, 255 ^ (1u << bit)) : rv::ori(R::a0, R::a0, 1u << bit));
            set8(reg);
        }
    }
    void suspend(CpuMode mode) {
        constant(MODE, static_cast<uint32_t>(mode));
        exit_plain(pc_, JitExit::suspended);
    }
    void high_page(bool store_a, R offset) {
        // LDH and LD (C): FF00 plus an 8-bit offset.
        out().li(R::a1, 0xff00); emit(rv::or_(R::a1, R::a1, offset));
        light_ = true;
        if (store_a) write(resident(A));
        else read(def(A));
    }
    void block0(unsigned y, unsigned z) {
        const unsigned p = y >> 1;
        const bool odd = (y & 1) != 0;
        switch (z) {
        case 0:
            switch (y) {
            case 0: break;
            case 1:
                if (constant_) {
                    const auto address = immediate_value();
                    load(R::a2, SP, true); access_at(true, address, R::a2);
                    load(R::a2, SP, true); emit(rv::srli(R::a2, R::a2, 8));
                    access_at(true, static_cast<uint16_t>(address + 1), R::a2);
                } else {
                    immediate(R::t1); move(R::a1, R::t1); load(R::a2, SP, true); write();
                    emit(rv::addi(R::a1, R::t1, 1)); mask16(R::a1);
                    load(R::a2, SP, true); emit(rv::srli(R::a2, R::a2, 8)); write();
                }
                break;
            case 2:
                if (memory_) exit_plain(pc_, JitExit::suspended, kActionStop);
                else {
                    increment(PC, 1);
                    constant(MODE, static_cast<uint32_t>(CpuMode::stopped)); flat_exit(JitExit::suspended);
                }
                break;
            case 3: transfer(0); break;
            default: transfer(0, y - 4); break;
            }
            break;
        case 1:
            if (odd) add_hl(p); else { immediate(R::a0); set_pair(p, R::a0); }
            break;
        case 2:
            light_ = true;
            if (p >= 2 && memory_ && (odd || !verified_)) {
                // (HL+) and (HL-) step HL in the shared code.
                def(registers[4]); def(registers[5]);
                if (odd) def(A); else resident(A);
                call((odd ? stubs_.load_step : stubs_.store_step)[take_light()][p == 3]);
                break;
            }
            if (odd) read(def(A), p < 2 ? p + 1 : at_hl);
            else write(resident(A), p < 2 ? p + 1 : at_hl);
            if (p >= 2) { pair(2, R::t0); emit(rv::addi(R::t0, R::t0, p == 2 ? 1 : -1)); set_pair(2, R::t0); }
            break;
        case 3:
            idle();
            if (p < 3) {
                def(registers[p * 2]); def(registers[p * 2 + 1]);
                call(stubs_.step_pair[odd][p]);
            } else { pair(3, R::t0); emit(rv::addi(R::t0, R::t0, odd ? -1 : 1)); set_pair(3, R::t0); }
            break;
        case 4: inc_dec(y, false); break;
        case 5: inc_dec(y, true); break;
        case 6:
            if (y == 6) { immediate(R::a2); light_ = true; write(R::a2, at_hl); }
            else if (constant_) constant(registers[y], bytes_[1]);
            else { immediate(R::a0); set8(y); }
            break;
        case 7:
            if (y < 4) { load(R::a0, A); rotate(y, false); store(R::a0, A); }
            else if (y == 4) daa();
            else if (y == 5) {
                load(R::t0, A); emit(rv::xori(R::t0, R::t0, 255)); store(R::t0, A);
                load(R::t0, F); emit(rv::ori(R::t0, R::t0, 96)); store(R::t0, F);
            } else {
                load(R::t0, F);
                if (y == 6) { emit(rv::andi(R::t0, R::t0, 128)); emit(rv::ori(R::t0, R::t0, 16)); }
                else { emit(rv::andi(R::t0, R::t0, 144)); emit(rv::xori(R::t0, R::t0, 16)); }
                store(R::t0, F);
            }
            break;
        }
    }
    void block3(unsigned y, unsigned z) {
        const unsigned p = y >> 1;
        const bool odd = (y & 1) != 0;
        switch (z) {
        case 0:
            if (y < 4) {
                idle();
                const bool pending = pending_zero_;
                const auto skip = condition(y);
                if (y < 2) known_zero(y == 1);
                return_pc(false);
                other_outcome(pending, skip, y);
            } else if (y == 5 || y == 7) add_sp(y == 7);
            else if (constant_) {
                const auto address = static_cast<uint16_t>(0xff00 | bytes_[1]);
                light_ = true;
                if (y == 4) access_at(true, address, resident(A));
                else access_at(false, address, def(A));
            } else { immediate(R::t1); high_page(y == 4, R::t1); }
            break;
        case 1:
            if (!odd) pop(p);
            else {
                switch (p) {
                case 0: return_pc(false); break;
                case 1: return_pc(true); break;
                case 2: pair(2, def(PC)); exit_link(-1, JitExit::control_flow); break;
                case 3: idle(); pair(2, def(SP)); break;
                }
            }
            break;
        case 2:
            if (y < 4) transfer(1, y);
            else if (!odd) high_page(y == 4, resident(registers[1]));
            else if (constant_) {
                light_ = true;
                if (y == 5) access_at(true, immediate_value(), resident(A));
                else access_at(false, immediate_value(), def(A));
            } else {
                immediate(R::a1);
                light_ = true;
                if (y == 5) write(resident(A));
                else read(def(A));
            }
            break;
        case 3:
            switch (y) {
            case 0: transfer(1); break;
            case 1: cb(); break;
            case 6: store(R::zero, IME); store(R::zero, EI); break;
            case 7: constant(EI, 1); break;
            default: suspend(CpuMode::locked); break;
            }
            break;
        case 4: if (y < 4) transfer(2, y); else suspend(CpuMode::locked); break;
        case 5:
            if (!odd) push(p);
            else if (p == 0) transfer(2); else suspend(CpuMode::locked);
            break;
        case 6:
            if (constant_) alu(y, R::zero, bytes_[1]);
            else { immediate(R::a0); alu(y, R::a0); }
            break;
        case 7:
            load_pc(R::t2); push(4);
            if (memory_) exit_link(y * 8, JitExit::control_flow);
            else { constant(PC, y * 8); flat_exit(JitExit::control_flow); }
            break;
        }
    }
    void decode(uint8_t opcode) {
        const unsigned y = (opcode >> 3) & 7, z = opcode & 7;
        switch (opcode >> 6) {
        case 0: block0(y, z); break;
        case 1:
            if (opcode == 0x76) {
                if (memory_) exit_plain(pc_, JitExit::suspended, kActionHalt);
                else { constant(MODE, static_cast<uint32_t>(CpuMode::halted)); flat_exit(JitExit::suspended); }
            } else if (z == 6) { light_ = true; read(def(registers[y]), at_hl); }
            else if (y == 6) { light_ = true; write(resident(registers[z]), at_hl); }
            else move(def(registers[y]), resident(registers[z]));
            break;
        case 2:
            if (z != 6) alu(y, resident(registers[z]));
            else { light_ = true; get8(z); alu(y, R::a0); }
            break;
        case 3: block3(y, z); break;
        }
    }

    jit::Assembler hot_, cold_;
    bool cold_mode_ = false;
    bool short_jumps_, short_range_failed_ = false;
    const Sm83Jit::Stubs& stubs_;
    const uint8_t* image_;
    Memory* memory_;
    uint16_t pc_, entry_pc_;
    bool marker_;
    std::span<const uint8_t> prefetched_;
    bool halt_bug_ = false;
    int pending_ei_;  // Mapped entry state is keyed; flat tests keep dynamic EI.
    Ime ime_ = Ime::none;
    std::array<uint8_t, 3> bytes_{};
    unsigned length_ = 1, group_idles_ = 0;
    bool constant_ = false;
    bool rom_ = false;  // The instruction's bytes lie in ROM.
    bool pending_zero_ = false;
    bool light_ = false;  // The next careful access may use a light stub.
    unsigned used_ = 0, dirty_ = 0;
    unsigned completed_ = 0, done_ = 0;
    bool end_ = false, overflow_ = false;
    std::array<Reference, 192> references_{};
    unsigned reference_count_ = 0;
    std::array<Call, 96> calls_{};
    unsigned call_count_ = 0;
    std::array<Label, 24> links_{};
    unsigned link_count_ = 0;
    std::array<unsigned, 24> flat_exits_{};
    unsigned flat_exit_count_ = 0;
    std::array<Label, 16> boundary_labels_{};
    std::array<Boundary, 16> boundaries_{};
    unsigned boundary_count_ = 0, instructions_ = 0;
    bool verified_;
    size_t code_copy_ = 0;
    std::array<Label, 40> ranges_{};
    unsigned range_count_ = 0;
    // ROM fetch groups, whose word gives the distance back to the hot stream
    // in halfwords.
    struct Fixup { Label at, back; };
    std::array<Fixup, 16> fixups_{};
    unsigned fixup_count_ = 0;
};

}  // namespace

Sm83Jit::Sm83Jit() = default;
Sm83Jit::~Sm83Jit() = default;

bool Sm83Jit::initialize_gateway(size_t pool_bytes, size_t pool_blocks, const platform::ExecutableMemory* reserved) {
    // Compressed where possible, as generated blocks.
    jit::Assembler out(scratch_, true);
    const auto emit = [&](rv::Instruction word) { out.emit(word); };
    const auto at = [&]() { return out.offset(); };
    const auto save = [&](int index) { return static_cast<int>(offsetof(Context, save) + index * 4); };
    const auto distance = [](size_t from, size_t to) {
        return static_cast<int32_t>(static_cast<int64_t>(to) - static_cast<int64_t>(from));
    };
    // A forward branch: a 32-bit placeholder that patch() replaces.
    const auto forward = [&]() { const auto position = at(); out.emit_exact(rv::nop()); return position; };
    const auto patch = [&](size_t from, Branch op, R a, R b) { out.patch(from, op(a, b, distance(from, at()))); };
    const auto jump_to = [&](R link, size_t to) { emit(rv::jal(link, distance(at(), to))); };
    // Save slots of the C++ call cores and of POP and PUSH, see Context::save.
    constexpr int kSaveCore = 12, kSaveHelper = 13;
    // Calls function(context, a1, a2) with every register generated code may
    // keep live preserved; t6 receives the result. Mapped stubs reload the
    // pinned registers that C++ may have changed.
    const auto call_c = [&](uintptr_t function, bool mapped, std::span<const R> preserved = kPreserved) {
        for (unsigned i = 0; i < preserved.size(); ++i) emit(rv::sw(preserved[i], R::s0, save(i + 1)));
        emit(rv::sw(R::s1, R::s0, offsetof(Context, mcycles)));
        emit(rv::addi(R::a0, R::s0, 0));
        out.li(R::t6, static_cast<uint32_t>(function));
        emit(rv::jalr(R::ra, R::t6, 0));
        emit(rv::addi(R::t6, R::a0, 0));
        emit(rv::lw(R::s1, R::s0, offsetof(Context, mcycles)));
        for (unsigned i = 0; i < preserved.size(); ++i) emit(rv::lw(preserved[i], R::s0, save(i + 1)));
        if (mapped) {
            emit(rv::lw(kBudget, R::s0, offsetof(Context, budget)));
            emit(rv::lw(kFast, R::s0, offsetof(Context, fast_until)));
            emit(rv::lw(kPages, R::s0, offsetof(Context, pages)));
            emit(rv::addi(kPages, kPages, kPageBias / 2)); emit(rv::addi(kPages, kPages, kPageBias / 2));
        }
    };
    const auto stub = [&](uintptr_t function, bool mapped, bool result, std::span<const R> preserved = kPreserved) {
        const auto start = at();
        emit(rv::sw(R::ra, R::s0, save(0)));
        call_c(function, mapped, preserved);
        if (result) emit(rv::addi(R::a0, R::t6, 0));
        emit(rv::lw(R::ra, R::s0, save(0)));
        emit(rv::ret());
        return start;
    };
    // Byte offsets until the gateway is placed.
    Stubs s{};

    // psABI entry (context, block). The frame belongs to the whole native run.
    s.enter = at();
    emit(rv::addi(R::sp, R::sp, -64));
    emit(rv::sw(R::ra, R::sp, 48));
    for (unsigned r = 0; r < 12; ++r) emit(rv::sw(saved(r), R::sp, r * 4));
    emit(rv::addi(R::s0, R::a0, 0));
    emit(rv::lw(R::s1, R::s0, offsetof(Context, mcycles)));
    emit(rv::lw(kCount, R::s0, offsetof(Context, instructions)));
    emit(rv::lw(kLazy, R::s0, offsetof(Context, lazy)));
    emit(rv::lw(R::t6, R::s0, offsetof(Context, cpu)));
    for (unsigned r = 0; r < 10; ++r) {
        const int offset = resident_offsets[r];
        emit((offset == SP || offset == PC) ? rv::lhu(saved(r + 2), R::t6, offset) :
                                             rv::lbu(saved(r + 2), R::t6, offset));
    }
    emit(rv::lw(kBudget, R::s0, offsetof(Context, budget)));
    emit(rv::lw(kFast, R::s0, offsetof(Context, fast_until)));
    emit(rv::lw(kPages, R::s0, offsetof(Context, pages)));
    emit(rv::addi(kPages, kPages, kPageBias / 2)); emit(rv::addi(kPages, kPages, kPageBias / 2));
    emit(rv::addi(R::t6, R::a1, 0));  // A block start marks itself used through t6.
    emit(rv::jalr(R::zero, R::a1, 0));

    // Exit: `jal t6, exit` followed by a descriptor; link data follows it.
    // unlinked, the target of a link word while its exit is not linked,
    // takes the descriptor's address in ra. The core takes t6 = the
    // descriptor and ra = its address.
    s.exit = at();
    emit(rv::addi(R::ra, R::t6, 0));
    s.unlinked = at();
    emit(rv::lw(R::t6, R::ra, 0));
    const auto exit_core = at();
    emit(rv::sw(R::t6, R::s0, offsetof(Context, exit)));
    emit(rv::addi(R::t0, R::ra, 4));
    emit(rv::sw(R::t0, R::s0, offsetof(Context, link)));
    out.extract_bit(R::t0, R::t6, 25);
    const auto keep_pc = forward();
    out.zero_extend16(saved(11), R::t6);
    patch(keep_pc, rv::bne, R::t0, R::zero);
    emit(rv::srli(R::t0, R::t6, exit_bits::completed)); emit(rv::andi(R::t0, R::t0, 31));
    emit(rv::add(kCount, kCount, R::t0));
    out.extract_bit(R::t0, R::t6, 24);
    const auto complete = forward();
    emit(rv::andi(R::t0, kLazy, 255)); emit(rv::sltiu(R::t0, R::t0, 1)); emit(rv::slli(R::t0, R::t0, 7));
    emit(rv::or_(saved(3), saved(3), R::t0));
    patch(complete, rv::beq, R::t0, R::zero);
    emit(rv::sw(R::s1, R::s0, offsetof(Context, mcycles)));
    emit(rv::sw(kCount, R::s0, offsetof(Context, instructions)));
    emit(rv::lw(R::t0, R::s0, offsetof(Context, cpu)));
    for (unsigned r = 0; r < 10; ++r) {
        const int offset = resident_offsets[r];
        emit((offset == SP || offset == PC) ? rv::sh(saved(r + 2), R::t0, offset) :
                                             rv::sb(saved(r + 2), R::t0, offset));
    }
    emit(rv::srli(R::a0, R::t6, exit_bits::reason)); emit(rv::andi(R::a0, R::a0, 7));
    for (unsigned r = 0; r < 12; ++r) emit(rv::lw(saved(r), R::sp, r * 4));
    emit(rv::lw(R::ra, R::sp, 48));
    emit(rv::addi(R::sp, R::sp, 64)); emit(rv::ret());

    // Linked exits: `jal t6, link[n]`, the descriptor, then the link word,
    // which holds the destination, or unlinked. n: instructions completed
    // since the block last counted them. Within branch range of exit.
    for (unsigned n = 0; n < s.link.size(); ++n) {
        s.link[n] = at();
        if (n) emit(rv::addi(kCount, kCount, static_cast<int32_t>(n)));
        emit(rv::bgeu(R::s1, kBudget, distance(at(), s.exit)));
        emit(rv::addi(R::ra, R::t6, 0));
        emit(rv::lw(R::t6, R::ra, 4));
        emit(rv::jalr(R::zero, R::t6, 0));
    }
    // Other exits: s11 = destination PC, `jal t6, dispatch[n]`, the
    // descriptor. An entry's tag is its PC | the bank of its window when it
    // was made; it serves while that bank is mapped, see Memory::code_tags.
    for (unsigned n = 0; n < s.dispatch.size(); ++n) {
        s.dispatch[n] = at();
        if (n) emit(rv::addi(kCount, kCount, static_cast<int32_t>(n)));
        emit(rv::bgeu(R::s1, kBudget, distance(at(), s.exit)));
        emit(rv::lw(R::t0, R::s0, offsetof(Context, dispatch)));
        if (rv::kZb) {
            emit(rv::andi(R::t1, saved(11), (1 << kDispatchBits) - 1)); emit(rv::sh3add(R::t0, R::t1, R::t0));
        } else {
            emit(rv::slli(R::t1, saved(11), 32 - kDispatchBits)); emit(rv::srli(R::t1, R::t1, 29 - kDispatchBits));
            emit(rv::add(R::t0, R::t0, R::t1));
        }
        emit(rv::lw(R::t1, R::t0, 0));
        // Guest temporaries, t2 among them, are dead at a block exit.
        emit(rv::lw(R::ra, R::s0, offsetof(Context, banks)));
        emit(rv::srli(R::t2, saved(11), 12)); emit(rv::slli(R::t2, R::t2, 2)); emit(rv::add(R::ra, R::ra, R::t2));
        emit(rv::lw(R::ra, R::ra, 0));
        emit(rv::or_(R::ra, R::ra, saved(11)));
        emit(rv::bne(R::t1, R::ra, distance(at(), s.exit)));
        emit(rv::lw(R::t6, R::t0, 4));
        emit(rv::jalr(R::zero, R::t6, 0));
    }

    // A block start in the second level: `jal ra, count_entry` takes the
    // place of the first word of the CLOCK mark, so ra points at the
    // second, an instruction, which the stub skips. t6 holds the block
    // start, as for every block entry. The entry is counted down in the
    // block's second header word; when the count runs out, the block leaves
    // before it runs, so that C++ copies it into the pool.
    s.count_entry = at();
    emit(rv::lw(R::t0, R::t6, -4));
    emit(rv::addi(R::t0, R::t0, -1));
    emit(rv::sw(R::t0, R::t6, -4));
    const auto spent = forward();
    emit(rv::jalr(R::zero, R::ra, 4));
    patch(spent, rv::bge, R::zero, R::t0);
    emit(rv::lhu(R::t6, R::t6, -static_cast<int32_t>(kHeaderWords * 4)));
    out.li(R::t0, static_cast<uint32_t>(JitExit::refetch) << exit_bits::reason | exit_bits::spent);
    emit(rv::or_(R::t6, R::t6, R::t0));
    jump_to(R::zero, exit_core);

    // Careful fetch group: `jal t6, group` followed by two descriptor words.
    const auto group_failed = [&](size_t failed) {
        patch(failed, rv::bne, R::t6, R::zero);
        emit(rv::lw(R::t6, R::s0, offsetof(Context, exit)));
        emit(rv::addi(R::ra, R::zero, 0));
        jump_to(R::zero, exit_core);
    };
    s.group = at();
    emit(rv::sw(R::t6, R::s0, save(0)));
    emit(rv::addi(R::a1, R::t6, 0));
    call_c(reinterpret_cast<uintptr_t>(&careful_group), true);
    const auto failed = forward();
    emit(rv::lw(R::a0, R::s0, offsetof(Context, value)));
    emit(rv::lw(R::t6, R::s0, save(0)));
    emit(rv::jalr(R::zero, R::t6, 8));
    group_failed(failed);
    // ROM fetch group: `jal t6, group_rom` and one word, which also gives the
    // way back in halfwords. Its operands are constants, so a0 stays unset.
    s.group_rom = at();
    emit(rv::sw(R::t6, R::s0, save(0)));
    emit(rv::addi(R::a1, R::t6, 0));
    call_c(reinterpret_cast<uintptr_t>(&careful_group_rom), true);
    const auto failed_rom = forward();
    emit(rv::lw(R::ra, R::s0, save(0)));
    emit(rv::lw(R::t6, R::ra, 0));
    emit(rv::srli(R::t6, R::t6, 22)); emit(rv::slli(R::t6, R::t6, 1));
    emit(rv::sub(R::t6, R::ra, R::t6));
    emit(rv::jalr(R::zero, R::t6, 0));
    group_failed(failed_rom);

    // Verified block write: `jal t6, code_write` followed by the code range.
    s.code_write = at();
    emit(rv::sw(R::t6, R::s0, save(0)));
    emit(rv::addi(kBudget, R::t6, 0));  // Reloaded after the call.
    call_c(reinterpret_cast<uintptr_t>(&careful_code_write), true);
    emit(rv::lw(R::t6, R::s0, save(0)));
    emit(rv::jalr(R::zero, R::t6, 4));

    s.read = stub(reinterpret_cast<uintptr_t>(&careful_read), true, true);
    s.write = stub(reinterpret_cast<uintptr_t>(&careful_write), true, false);
    s.read_light = stub(reinterpret_cast<uintptr_t>(&careful_read), true, true, kPinned);
    s.write_light = stub(reinterpret_cast<uintptr_t>(&careful_write), true, false, kPinned);
    s.flat_read = stub(reinterpret_cast<uintptr_t>(&read_bus), false, true);
    s.flat_write = stub(reinterpret_cast<uintptr_t>(&write_bus), false, false);
    s.flat_idle = stub(reinterpret_cast<uintptr_t>(&idle_bus), false, false);

    // Mapped accesses, reached by `jal ra` with the address in a1 or a
    // register pair. A direct page pointer serves an access that precedes
    // the deadline; anything else takes the exact path in C++. They clobber
    // a1, t6 and ra only, unless C++ runs.
    const auto core = [&](uintptr_t function, std::span<const R> preserved) {
        const auto start = at();
        emit(rv::sw(R::ra, R::s0, save(kSaveCore)));
        call_c(function, true, preserved);
        emit(rv::lw(R::ra, R::s0, save(kSaveCore)));
        emit(rv::ret());
        return start;
    };
    const size_t read_core[]{core(reinterpret_cast<uintptr_t>(&careful_read), kPreserved),
                             core(reinterpret_cast<uintptr_t>(&careful_read), kPinned)};
    const size_t write_core[]{core(reinterpret_cast<uintptr_t>(&careful_write), kPreserved),
                              core(reinterpret_cast<uintptr_t>(&careful_write), kPinned)};
    const size_t idle_core = core(reinterpret_cast<uintptr_t>(&careful_idle), kPreserved);
    const auto guest_register = [](unsigned index, R temporary) {
        return index == 6 ? temporary : guest(registers[index]);
    };
    const auto pair_address = [&](unsigned pair) {
        emit(rv::slli(R::a1, guest(registers[pair * 2]), 8));
        emit(rv::or_(R::a1, R::a1, guest(registers[pair * 2 + 1])));
    };
    // The page's direct pointer for a1, or a branch to `slow` when there is none.
    const auto page_pointer = [&](bool write, std::array<size_t, 3>& slow, unsigned& count) {
        emit(rv::srli(R::t6, R::a1, 7));
        if (rv::kZb) emit(rv::sh3add(R::t6, R::t6, kPages));
        else { emit(rv::slli(R::t6, R::t6, 3)); emit(rv::add(R::t6, R::t6, kPages)); }
        emit(rv::lw(R::t6, R::t6, (write ? 4 : 0) - kPageBias));
        slow[count++] = forward();
        emit(rv::andi(R::a1, R::a1, 127)); emit(rv::add(R::t6, R::t6, R::a1));
    };
    std::array<std::array<size_t, 8>, 2> load_at{}, store_at{};
    for (unsigned light = 0; light < 2; ++light) {
        for (unsigned r = 0; r < 8; ++r) {
            const R into = guest_register(r, R::a0);
            load_at[light][r] = at();
            std::array<size_t, 3> slow{};
            unsigned count = 0;
            slow[count++] = forward();
            page_pointer(false, slow, count);
            emit(rv::lbu(into, R::t6, 0));
            emit(rv::addi(R::s1, R::s1, 1));
            emit(rv::ret());
            patch(slow[0], rv::bgeu, R::s1, kFast);
            patch(slow[1], rv::beq, R::t6, R::zero);
            emit(rv::sw(R::ra, R::s0, save(0)));
            jump_to(R::ra, read_core[light]);
            emit(rv::addi(into, R::t6, 0));
            emit(rv::lw(R::ra, R::s0, save(0)));
            emit(rv::ret());
        }
        for (unsigned r = 0; r < 8; ++r) {
            const R value = guest_register(r, R::a2);
            store_at[light][r] = at();
            std::array<size_t, 3> slow{};
            unsigned count = 0;
            slow[count++] = forward();
            // IE writes always change interrupt state through Memory.
            emit(rv::addi(R::t6, R::a1, 1)); emit(rv::srli(R::t6, R::t6, 16));
            slow[count++] = forward();
            page_pointer(true, slow, count);
            emit(rv::sb(value, R::t6, 0));
            emit(rv::addi(R::s1, R::s1, 1));
            emit(rv::ret());
            patch(slow[0], rv::bgeu, R::s1, kFast);
            patch(slow[1], rv::bne, R::t6, R::zero);
            patch(slow[2], rv::beq, R::t6, R::zero);
            emit(rv::sw(R::ra, R::s0, save(0)));
            if (value != R::a2) emit(rv::addi(R::a2, value, 0));
            jump_to(R::ra, write_core[light]);
            emit(rv::lw(R::ra, R::s0, save(0)));
            emit(rv::ret());
        }
        for (unsigned r = 0; r < 8; ++r) {
            s.load[light][0][r] = load_at[light][r];
            s.store[light][0][r] = store_at[light][r];
            for (unsigned pair = 0; pair < 3; ++pair) {
                s.load[light][pair + 1][r] = at();
                pair_address(pair);
                jump_to(R::zero, load_at[light][r]);
                s.store[light][pair + 1][r] = at();
                pair_address(pair);
                jump_to(R::zero, store_at[light][r]);
            }
        }
        // (HL+) and (HL-): HL steps before the access, which observes only a1.
        for (unsigned decrement = 0; decrement < 2; ++decrement) {
            const auto step = [&] {
                pair_address(2);
                emit(rv::addi(R::t6, R::a1, decrement ? -1 : 1));
                emit(rv::andi(guest(registers[5]), R::t6, 255));
                emit(rv::srli(R::t6, R::t6, 8)); emit(rv::andi(guest(registers[4]), R::t6, 255));
            };
            s.load_step[light][decrement] = at();
            step();
            jump_to(R::zero, load_at[light][7]);
            s.store_step[light][decrement] = at();
            step();
            jump_to(R::zero, store_at[light][7]);
        }
    }
    s.idle_fast = at();
    {
        const auto slow = forward();
        emit(rv::addi(R::s1, R::s1, 1));
        emit(rv::ret());
        patch(slow, rv::bgeu, R::s1, kFast);
        emit(rv::sw(R::ra, R::s0, save(0)));
        jump_to(R::ra, idle_core);
        emit(rv::lw(R::ra, R::s0, save(0)));
        emit(rv::ret());
    }

    // Instruction semantics shared by mapped and flat blocks. They update
    // resident registers in place and clobber a0-a2 and t0-t6 as noted.
    const R a = guest(A), f = guest(F);
    s.zero_flag = at();
    emit(rv::sltiu(R::t6, kLazy, 1)); emit(rv::slli(R::t6, R::t6, 7));
    emit(rv::or_(f, f, R::t6)); emit(rv::ret());
    // INC r and DEC r keep C and defer Z; (HL) takes a0 and leaves a2.
    for (unsigned decrement = 0; decrement < 2; ++decrement)
        for (unsigned r = 0; r < 8; ++r) {
            s.inc_dec[decrement][r] = at();
            const R x = r == 6 ? R::a0 : guest(registers[r]), result = r == 6 ? R::a2 : x;
            emit(rv::addi(R::t0, x, decrement ? -1 : 1)); emit(rv::xor_(R::t1, x, R::t0));
            emit(rv::andi(result, R::t0, 255));
            emit(rv::andi(R::t1, R::t1, 16)); emit(rv::slli(R::t1, R::t1, 1));
            emit(rv::andi(f, f, 16)); emit(rv::or_(f, f, R::t1));
            if (decrement) emit(rv::ori(f, f, 64));
            emit(rv::addi(kLazy, result, 0));
            emit(rv::ret());
        }
    // A op a0, deferring Z (Pan Docs, 8-bit arithmetic and logic).
    for (unsigned operation = 0; operation < 8; ++operation) {
        s.alu[operation] = at();
        if (operation >= 4 && operation != 7) {
            emit(operation == 4 ? rv::and_(a, a, R::a0) : operation == 5 ? rv::xor_(a, a, R::a0) : rv::or_(a, a, R::a0));
            emit(rv::addi(f, R::zero, operation == 4 ? 32 : 0));
            emit(rv::addi(kLazy, a, 0));
            emit(rv::ret());
            continue;
        }
        const bool subtract = operation >= 2, carry = operation == 1 || operation == 3;
        R operand = R::a0;  // Including the carry flag.
        if (carry) {
            out.extract_bit(R::t1, f, 4);
            emit(rv::add(R::t2, R::a0, R::t1));
            operand = R::t2;
        }
        emit(subtract ? rv::sub(R::t3, a, operand) : rv::add(R::t3, a, operand));
        // H: carry or borrow out of bit 3. C: out of bit 7.
        emit(rv::xor_(R::t5, a, R::a0)); emit(rv::xor_(R::t5, R::t5, R::t3));
        emit(rv::andi(R::t5, R::t5, 16)); emit(rv::slli(R::t4, R::t5, 1));
        emit(subtract ? rv::sltu(R::t2, a, operand) : rv::srli(R::t2, R::t3, 8));
        emit(rv::slli(R::t2, R::t2, 4)); emit(rv::or_(f, R::t4, R::t2));
        if (subtract) emit(rv::ori(f, f, 64));
        if (operation == 7) emit(rv::andi(kLazy, R::t3, 255));
        else { emit(rv::andi(a, R::t3, 255)); emit(rv::addi(kLazy, a, 0)); }
        emit(rv::ret());
    }
    // Rotates, shifts and SWAP of a0 into a0, setting every flag.
    for (unsigned zero = 0; zero < 2; ++zero)
        for (unsigned operation = 0; operation < 8; ++operation) {
            s.rotate[zero][operation] = at();
            R carry = R::t1;
            switch (operation) {
            case 0:
                emit(rv::srli(R::t1, R::a0, 7)); emit(rv::slli(R::t2, R::a0, 1)); emit(rv::or_(R::t2, R::t2, R::t1));
                break;
            case 1:
                emit(rv::andi(R::t1, R::a0, 1)); emit(rv::slli(R::t3, R::t1, 7)); emit(rv::srli(R::t2, R::a0, 1));
                emit(rv::or_(R::t2, R::t2, R::t3));
                break;
            case 2:
                emit(rv::srli(R::t1, R::a0, 7)); emit(rv::slli(R::t2, R::a0, 1)); out.extract_bit(R::t3, f, 4);
                emit(rv::or_(R::t2, R::t2, R::t3));
                break;
            case 3:
                emit(rv::andi(R::t1, R::a0, 1)); emit(rv::srli(R::t2, R::a0, 1)); emit(rv::andi(R::t3, f, 16));
                emit(rv::slli(R::t3, R::t3, 3)); emit(rv::or_(R::t2, R::t2, R::t3));
                break;
            case 4: emit(rv::srli(R::t1, R::a0, 7)); emit(rv::slli(R::t2, R::a0, 1)); break;
            case 5:
                emit(rv::andi(R::t1, R::a0, 1)); emit(rv::srli(R::t2, R::a0, 1)); emit(rv::andi(R::t3, R::a0, 128));
                emit(rv::or_(R::t2, R::t2, R::t3));
                break;
            case 6:
                emit(rv::srli(R::t2, R::a0, 4)); emit(rv::slli(R::t3, R::a0, 4)); emit(rv::or_(R::t2, R::t2, R::t3));
                carry = R::zero;
                break;
            case 7: emit(rv::andi(R::t1, R::a0, 1)); emit(rv::srli(R::t2, R::a0, 1)); break;
            }
            emit(rv::andi(R::t2, R::t2, 255)); emit(rv::slli(R::t1, carry, 4));
            if (zero) {
                emit(rv::sltiu(R::t3, R::t2, 1)); emit(rv::slli(R::t3, R::t3, 7)); emit(rv::or_(R::t1, R::t1, R::t3));
            }
            emit(rv::andi(f, R::t1, 255)); emit(rv::addi(R::a0, R::t2, 0));
            emit(rv::ret());
        }
    // BIT b,a0.
    for (unsigned bit = 0; bit < 8; ++bit) {
        s.bit[bit] = at();
        emit(rv::andi(R::t0, R::a0, 1 << bit)); emit(rv::sltiu(R::t0, R::t0, 1)); emit(rv::slli(R::t0, R::t0, 7));
        emit(rv::andi(R::t1, f, 16)); emit(rv::ori(R::t1, R::t1, 32));
        emit(rv::or_(f, R::t0, R::t1));
        emit(rv::ret());
    }
    // INC rr and DEC rr for BC, DE and HL; flags unchanged.
    for (unsigned decrement = 0; decrement < 2; ++decrement)
        for (unsigned pair = 0; pair < 3; ++pair) {
            s.step_pair[decrement][pair] = at();
            const R high = guest(registers[pair * 2]), low = guest(registers[pair * 2 + 1]);
            emit(rv::slli(R::t0, high, 8)); emit(rv::or_(R::t0, R::t0, low));
            emit(rv::addi(R::t0, R::t0, decrement ? -1 : 1));
            emit(rv::andi(low, R::t0, 255)); emit(rv::srli(R::t0, R::t0, 8)); emit(rv::andi(high, R::t0, 255));
            emit(rv::ret());
        }
    // ADD HL,rr; Z must be materialized.
    const R h = guest(registers[4]), l = guest(registers[5]);
    for (unsigned pair = 0; pair < 4; ++pair) {
        s.add_hl[pair] = at();
        if (pair == 3) emit(rv::addi(R::t0, guest(SP), 0));
        else { emit(rv::slli(R::t0, guest(registers[pair * 2]), 8)); emit(rv::or_(R::t0, R::t0, guest(registers[pair * 2 + 1]))); }
        emit(rv::slli(R::t1, h, 8)); emit(rv::or_(R::t1, R::t1, l));
        emit(rv::add(R::t2, R::t0, R::t1));
        emit(rv::xor_(R::t3, R::t0, R::t1)); emit(rv::xor_(R::t3, R::t3, R::t2));
        out.extract_bit(R::t3, R::t3, 12); emit(rv::slli(R::t3, R::t3, 5));
        emit(rv::srli(R::t4, R::t2, 16)); emit(rv::slli(R::t4, R::t4, 4)); emit(rv::or_(R::t3, R::t3, R::t4));
        emit(rv::andi(R::t4, f, 128)); emit(rv::or_(f, R::t3, R::t4));
        emit(rv::andi(l, R::t2, 255)); emit(rv::srli(R::t2, R::t2, 8)); emit(rv::andi(h, R::t2, 255));
        emit(rv::ret());
    }
    // DAA (Pan Docs, DAA). Z in F is not used.
    {
        s.daa = at();
        emit(rv::andi(R::t2, f, 16)); emit(rv::addi(R::t3, R::zero, 0));
        emit(rv::andi(R::t4, f, 64));
        const auto subtract = forward();
        emit(rv::andi(R::t4, f, 32)); emit(rv::andi(R::t5, a, 15));
        emit(rv::sltiu(R::t5, R::t5, 10)); emit(rv::xori(R::t5, R::t5, 1)); emit(rv::or_(R::t4, R::t4, R::t5));
        const auto low = forward();
        emit(rv::ori(R::t3, R::t3, 6));
        patch(low, rv::beq, R::t4, R::zero);
        emit(rv::sltiu(R::t4, a, 0x9a)); emit(rv::xori(R::t4, R::t4, 1)); emit(rv::or_(R::t4, R::t4, R::t2));
        const auto high = forward();
        emit(rv::ori(R::t3, R::t3, 0x60)); emit(rv::addi(R::t2, R::zero, 16));
        patch(high, rv::beq, R::t4, R::zero);
        emit(rv::add(R::t0, a, R::t3));
        const auto end_add = forward();
        patch(subtract, rv::bne, R::t4, R::zero);
        emit(rv::andi(R::t4, f, 32));
        const auto sub_low = forward();
        emit(rv::ori(R::t3, R::t3, 6));
        patch(sub_low, rv::beq, R::t4, R::zero);
        const auto sub_high = forward();
        emit(rv::ori(R::t3, R::t3, 0x60));
        patch(sub_high, rv::beq, R::t2, R::zero);
        emit(rv::sub(R::t0, a, R::t3));
        out.patch(end_add, rv::jal(R::zero, distance(end_add, at())));
        emit(rv::andi(a, R::t0, 255)); emit(rv::andi(R::t1, f, 64)); emit(rv::or_(R::t1, R::t1, R::t2));
        emit(rv::sltiu(R::t4, a, 1)); emit(rv::slli(R::t4, R::t4, 7)); emit(rv::or_(f, R::t1, R::t4));
        emit(rv::ret());
    }
    // POP and PUSH through the full load and store paths, which keep t1 and
    // t2. pop[4] leaves the value in a0; push[4] takes it in t2. F must be
    // materialized for PUSH AF.
    const R sp_register = guest(SP);
    const auto mask16 = [&](R r) { out.zero_extend16(r, r); };
    for (unsigned pair = 0; pair < 5; ++pair) {
        s.pop[pair] = at();
        emit(rv::sw(R::ra, R::s0, save(kSaveHelper)));
        emit(rv::addi(R::a1, sp_register, 0));
        jump_to(R::ra, load_at[0][6]);
        emit(rv::addi(R::t1, R::a0, 0));
        emit(rv::addi(R::a1, sp_register, 1)); mask16(R::a1);
        jump_to(R::ra, load_at[0][6]);
        emit(rv::addi(sp_register, sp_register, 2)); mask16(sp_register);
        if (pair < 3) {
            emit(rv::addi(guest(registers[pair * 2 + 1]), R::t1, 0));
            emit(rv::addi(guest(registers[pair * 2]), R::a0, 0));
        } else if (pair == 3) {
            emit(rv::andi(f, R::t1, 240)); emit(rv::addi(a, R::a0, 0));
        } else {
            emit(rv::slli(R::a0, R::a0, 8)); emit(rv::or_(R::a0, R::a0, R::t1));
        }
        emit(rv::lw(R::ra, R::s0, save(kSaveHelper)));
        emit(rv::ret());
    }
    for (unsigned pair = 0; pair < 5; ++pair) {
        s.push[pair] = at();
        emit(rv::sw(R::ra, R::s0, save(kSaveHelper)));
        if (pair < 4) {
            const R high = pair == 3 ? a : guest(registers[pair * 2]), low = pair == 3 ? f : guest(registers[pair * 2 + 1]);
            emit(rv::slli(R::t2, high, 8)); emit(rv::or_(R::t2, R::t2, low));
        }
        // The high byte is written first.
        emit(rv::addi(R::a1, sp_register, -1)); mask16(R::a1);
        emit(rv::srli(R::a2, R::t2, 8));
        jump_to(R::ra, store_at[0][6]);
        emit(rv::addi(R::a1, sp_register, -2)); mask16(R::a1);
        emit(rv::andi(R::a2, R::t2, 255));
        jump_to(R::ra, store_at[0][6]);
        emit(rv::addi(sp_register, sp_register, -2)); mask16(sp_register);
        emit(rv::lw(R::ra, R::s0, save(kSaveHelper)));
        emit(rv::ret());
    }

    if (!out.valid()) return false;
    if (!cache_.initialize(pool_bytes, pool_blocks, out.code().size_bytes(), reserved)) return false;
    gateway_bytes_ = out.code().size_bytes();
    const auto base = reinterpret_cast<uintptr_t>(cache_.shared().data());
#ifdef GBX_PROFILER
    // Byte offsets of the stub families, in generation order, so that
    // program counter samples in the gateway can be attributed.
    platform::log(platform::LogLevel::warn, "jit",
                  "gateway %08lx %u bytes: exit %u link %u dispatch %u group %u code_write %u access %u idle %u "
                  "flags %u step %u add_hl %u daa %u stack %u",
                  static_cast<unsigned long>(base), static_cast<unsigned>(out.code().size_bytes()),
                  static_cast<unsigned>(s.exit), static_cast<unsigned>(s.link[0]),
                  static_cast<unsigned>(s.dispatch[0]), static_cast<unsigned>(s.group),
                  static_cast<unsigned>(s.code_write), static_cast<unsigned>(s.read),
                  static_cast<unsigned>(s.idle_fast), static_cast<unsigned>(s.zero_flag),
                  static_cast<unsigned>(s.step_pair[0][0]), static_cast<unsigned>(s.add_hl[0]),
                  static_cast<unsigned>(s.daa), static_cast<unsigned>(s.pop[0]));
#endif
    auto addresses = std::bit_cast<std::array<uintptr_t, sizeof(Stubs) / sizeof(uintptr_t)>>(s);
    for (auto& address : addresses) address = base + address;
    stubs_ = std::bit_cast<Stubs>(addresses);
    volatile uint32_t* destination = cache_.shared().data();
    for (size_t i = 0; i < out.code().size(); ++i) destination[i] = out.code()[i];
    return cache_.publish_shared(gateway_bytes_);
}

bool Sm83Jit::initialize(std::span<const uint8_t, 65536> code) {
    // Flat single-step blocks use a smaller pool.
    if (!initialize_gateway(64 * 1024, 512)) return false;
    code_ = code.data();
    return true;
}

bool Sm83Jit::initialize(Memory& memory, bool stop_on_marker, const Capacity& capacity,
                         const platform::ExecutableMemory* reserved) {
    // Mapped PPU workloads execute many event-split paths. Keep their working
    // set in 192 KiB of internal executable memory by default: the pool
    // shares the high SRAM with the GBA's execution core, see
    // docs/architecture.md. With accesses, exits and flag arithmetic in
    // shared code and compressed instructions, blocks measured in games
    // average 150 to 220 bytes, so filling the pool takes about 900 blocks.
    // Each slot costs about 40 bytes of internal RAM.
    if (!initialize_gateway(capacity.pool_bytes, capacity.pool_blocks, reserved)) {
        platform::log(platform::LogLevel::error, "jit", "No code pool of %u bytes; largest block %u",
                      static_cast<unsigned>(capacity.pool_bytes),
                      static_cast<unsigned>(platform::largest_executable()));
        return false;
    }
    if (capacity.store_bytes) initialize_store(capacity.store_bytes, capacity.store_records);
    dispatch_.reset(new (std::nothrow) Dispatch);
    mid_.reset(new (std::nothrow) MidEntries{});
    link_records_.reset(new (std::nothrow) LinkRecord[kLinkRecords]);
    unstable_.reset(static_cast<uint8_t*>(platform::allocate_external(1u << kUnstableBits)));
    reinstall_.reset(static_cast<Reinstall*>(platform::allocate_external(sizeof(Reinstall) << kReinstallBits)));
    if (!dispatch_ || !mid_ || !link_records_ || !unstable_ || !reinstall_) return false;
    std::fill_n(unstable_.get(), 1u << kUnstableBits, uint8_t{0});
    std::fill_n(reinstall_.get(), 1u << kReinstallBits, Reinstall{});
    reset_links();
    for (auto& slot : *dispatch_) slot[0] = UINT32_MAX;
    memory_ = &memory;
    stop_on_marker_ = stop_on_marker;
    return true;
}

bool Sm83Jit::initialize_store(size_t bytes, size_t records) {
    // Each segment of the second level starts with a copy of the gateway.
    if (!store_.initialize(bytes, records, cache_.shared().first(gateway_bytes_ / 4),
                           reinterpret_cast<uintptr_t>(cache_.shared().data())))
        return false;
#ifdef GBX_PROFILER
    // For attributing profiler samples in the copies, see the gateway's line.
    platform::log(platform::LogLevel::warn, "jit", "store %08lx %u bytes, segments of %u bytes",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(store_.storage().data())),
                  static_cast<unsigned>(store_.storage().size_bytes()),
                  static_cast<unsigned>(jit::CodeStore::kSegmentBytes));
#endif
    return true;
}

void Sm83Jit::reset_links() {
    link_free_ = 0;
    for (uint32_t i = kLinkRecords; i; --i) { link_records_[i - 1].next = link_free_; link_free_ = i; }
}
void Sm83Jit::free_link(uint32_t index) {
    link_records_[index - 1].next = link_free_;
    link_free_ = index;
}
// Patches a static exit's link word and records it with the destination.
// Without a free record the exit stays unlinked.
void Sm83Jit::link(uint32_t* word, uintptr_t target) {
    if (!link_free_) return;
    auto* header = reinterpret_cast<uint32_t*>(target) - kHeaderWords;
    const auto index = link_free_;
    auto& record = link_records_[index - 1];
    link_free_ = record.next;
    record = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(word)), header[0] >> 16};
    header[0] = (header[0] & 0xffff) | index << 16;
    *word = static_cast<uint32_t>(target);
}
void Sm83Jit::retired(void* self, uintptr_t begin, uintptr_t end) {
    auto& jit = *static_cast<Sm83Jit*>(self);
    auto* block = reinterpret_cast<uint32_t*>(begin);
    // Links into this block return to the exit stub.
    const auto unlinked = static_cast<uint32_t>(jit.stubs_.unlinked);
    for (auto index = block[0] >> 16; index;) {
        const auto& record = jit.link_records_[index - 1];
        *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(record.word)) = unlinked;
        const auto next = record.next;
        jit.free_link(index);
        index = next;
    }
    block[0] &= 0xffff;
    // Links out of this block leave their destinations' lists.
    const size_t size = (end - begin) / 4, count = block[size - 1];
    for (size_t i = 0; i < count; ++i) {
        uint32_t* stub = block + block[size - 1 - count + i];
        if (stub[2] == unlinked) continue;
        auto* header = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(stub[2])) - kHeaderWords;
        const auto word = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stub + 2));
        const auto head = header[0] >> 16;
        if (jit.link_records_[head - 1].word == word) {
            header[0] = (header[0] & 0xffff) | jit.link_records_[head - 1].next << 16;
            jit.free_link(head);
            continue;
        }
        for (auto previous = head; previous;) {
            const auto next = jit.link_records_[previous - 1].next;
            if (next && jit.link_records_[next - 1].word == word) {
                jit.link_records_[previous - 1].next = jit.link_records_[next - 1].next;
                jit.free_link(next);
                break;
            }
            previous = next;
        }
    }
    auto& slot = (*jit.dispatch_)[block[0] & ((1u << kDispatchBits) - 1)];
    if (slot[1] == begin + kHeaderWords * 4) slot[0] = UINT32_MAX;
}
void Sm83Jit::clear() {
    cache_.clear();
    // Keys name banks, not their contents: another ROM or tracing makes
    // every copy invalid.
    store_.clear();
    if (dispatch_) for (auto& slot : *dispatch_) slot[0] = UINT32_MAX;
    if (mid_) for (auto& mid : *mid_) mid.block = 0;
    if (link_records_) reset_links();
    if (unstable_) std::fill_n(unstable_.get(), 1u << kUnstableBits, uint8_t{0});
    if (reinstall_) std::fill_n(reinstall_.get(), 1u << kReinstallBits, Reinstall{});
}
// RAM blocks verify their code on entry, but a restored state is a new
// program in RAM, not code a game rewrote: its blocks start afresh, and the
// counts of code changes by entry PC stay. The block addresses are gathered
// in the compiler's scratch space, which is idle outside compilation.
void Sm83Jit::forget_ram_code() {
    prefetched_count_ = 0;
    if (!memory_) { clear(); return; }
    size_t count = 0;
    cache_.visit([&](const uint32_t* block, size_t) {
        if ((block[0] & 0xffff) >= 0x8000 && count < scratch_.size())
            scratch_[count++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
    });
    for (size_t i = 0; i < count; ++i) {
        uintptr_t begin, end;
        if (cache_.erase_containing(scratch_[i], begin, end)) retired(this, begin, end);
    }
}
// A mid-block entry skips the verification at the start of a verified block.
bool Sm83Jit::code_matches(const uint32_t* copy) const {
    const auto pc = static_cast<uint16_t>(copy[0]);
    const auto* bytes = reinterpret_cast<const uint8_t*>(copy + 1);
    for (unsigned k = 0; k < (copy[0] >> 16); ++k) {
        const auto address = static_cast<uint16_t>(pc + k);
        const auto* page = memory_->pages[address >> 7].read;
        if (!page || page[address & 127] != bytes[k]) return false;
    }
    return true;
}
void Sm83Jit::unstable(uint16_t pc, uint8_t count) {
    auto& entry = unstable_[pc & ((1u << kUnstableBits) - 1)];
    entry = static_cast<uint8_t>(std::min<unsigned>(kUnstable, entry + count));
}
JitResult Sm83Jit::run_block(Sm83State& state, const Bus& bus, unsigned limit) {
    return execute(state, &bus, kNever, limit);
}
JitResult Sm83Jit::run_block(Sm83State& state, Tick until, unsigned limit) {
    return execute(state, nullptr, until, limit);
}
JitResult Sm83Jit::run(Sm83State& state, Tick until, bool frame) {
    return execute(state, nullptr, until, 16, true, frame);
}
// A batch leaves native code at the first exit after each kLeg M-cycles:
// C++ then dispatches the device events the batch deferred, which hands
// the scanlines drawn so far to the other core, so that rendering keeps
// pace with the CPU instead of waiting for a burst. It returns to its
// caller, which takes interrupts and HALT, after kBatch M-cycles, or
// earlier at the target time, an interrupt or another stop. A return costs
// a new context and deadline window, more than a leg does: a CGB frame is
// 35112 M-cycles at double speed.
constexpr uint32_t kLeg = 1024, kBatch = 1u << 16;
// A block that execute() finds in neither level: compiled now, or its copy
// in the second level moved into the pool.
Sm83Jit::Installed Sm83Jit::install(const Sm83State& state, jit::BlockKey key, unsigned block_limit,
                                    std::span<const uint8_t> prefix, bool halt_bug, bool promote,
                                    const jit::CodeStore::Record& copy, Reinstall* reinstall, uint16_t reinstall_tag) {
    jit::BlockEntry entry = nullptr;
    uintptr_t target = 0;
    bool start = false;
    JitFallback failure = JitFallback::unavailable;
    const auto hot = std::span(scratch_).first(kStreamWords);
    const auto cold = std::span(scratch_).subspan(kStreamWords, kStreamWords);
    const auto output = std::span(scratch_).subspan(2 * kStreamWords, kOutputWords);
    // One relocation more than a block makes, for count_entry's
    // call in the second level.
    constexpr size_t kRelocations = 97;
    static_assert(sizeof(jit::Relocation) * kRelocations + sizeof(Compiler) <= kStateWords * 4 &&
                  alignof(Compiler) <= 8 && alignof(jit::Relocation) <= 8);
    static_assert(std::is_trivially_destructible_v<Compiler>);
    auto* storage = reinterpret_cast<std::byte*>(scratch_.data() + 2 * kStreamWords + kOutputWords);
    const std::span relocations(reinterpret_cast<jit::Relocation*>(storage), kRelocations);
    void* compiler_storage = storage + sizeof(jit::Relocation) * kRelocations;
    std::span<const uint32_t> words;
    size_t relocation_count = 0;
    Compiler* compiler = nullptr;
    // A copy keeps the boundaries and then the block's instruction
    // count, which they do not give: they leave out the
    // instruction after EI.
    std::array<Compiler::Boundary, 17> stored{};
    std::span<const Compiler::Boundary> boundaries;
    unsigned instructions = 0;
    if (promote) {
        // No compilation, only relocation, and the CLOCK mark
        // back in place of the call to count_entry.
        std::copy(copy.code.begin(), copy.code.end(), output.begin());
        output[kHeaderWords] = rv::lw(R::ra, R::t6, -4).bits;
        words = output.first(copy.code.size());
        for (size_t i = 0; i < copy.relocations.size(); ++i) {
            const auto call = store_.target(copy, i);
            if (call != stubs_.count_entry) relocations[relocation_count++] = {copy.relocations[i], call};
        }
        std::memcpy(stored.data(), copy.data.data(), copy.data.size());
        const size_t count = copy.data.size() / sizeof(Compiler::Boundary) - 1;
        boundaries = std::span(stored).first(count);
        instructions = stored[count].completed;
        // The next eviction gets a count twice as long, so that a
        // block the pool keeps evicting moves less often.
        const auto level = std::min<uint32_t>(*copy.tag + 1, kMaxStoredLevel);
        *copy.tag = level;
        copy.code[1] = kStoredEntries << level;
    }
    // A block whose 16-bit jumps do not reach is compiled again
    // with 32-bit ones; one too large for those ends earlier.
    for (unsigned length = block_limit; length && words.empty(); length /= 2) {
        const bool verify = memory_ &&
            unstable_[state.pc & ((1u << kUnstableBits) - 1)] < kUnstable;
        for (bool short_jumps : {true, false}) {
            compiler = new (compiler_storage) Compiler(hot, cold, stubs_, code_, memory_, state.pc,
                stop_on_marker_, prefix, halt_bug, state.ei_pending, verify,
                short_jumps);
            words = compiler->compile(length, output, relocations.first(kRelocations - 1), relocation_count);
            if (!words.empty() || !compiler->short_range_failed()) break;
        }
        boundaries = compiler->boundaries();
        instructions = compiler->instructions();
    }
    if (words.empty()) failure = JitFallback::encoding;
    else {
        auto result = cache_.install_replacing(key, words, relocations.first(relocation_count),
                                               memory_ ? &retired : nullptr, this);
        evictions_ += result.evicted;
        entry = result.entry;
        if (entry) target = reinterpret_cast<uintptr_t>(entry) + (memory_ ? kHeaderWords * 4 : 0);
        start = entry != nullptr;
        if (entry) {
            if (promote) ++restored_blocks_;
            else {
                ++compiled_blocks_; GBX_JIT_COUNT(compiles);
#ifdef GBX_PROFILER
                compiled_bytes_ += static_cast<uint32_t>(words.size_bytes());
#endif
            }
            if (memory_) {
                // Inline data, never executed: no republication.
                auto* block = reinterpret_cast<uint32_t*>(entry);
                block[0] = state.pc;
                block[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(result.referenced));
                // A repeated installation number could match an
                // old mid-block entry.
                if (result.installation < installation_) for (auto& mid : *mid_) mid.block = 0;
                installation_ = result.installation;
                const size_t size = words.size(), count = block[size - 1];
                for (size_t i = 0; i < count; ++i)
                    block[block[size - 1 - count + i] + 2] = static_cast<uint32_t>(stubs_.unlinked);
                // Instruction starts inside the block, under the
                // mapping it was compiled for.
                for (const auto& boundary : boundaries) {
                    const auto pc = boundary.pc;
                    // Same limit; no HALT bug, delayed EI or prefix.
                    const jit::BlockKey mid_key = (key & UINT64_C(0xf0000)) | pc |
                        (uint64_t{memory_->code_bank(pc)} << 22) |
                        (uint64_t{memory_->code_bank(static_cast<uint16_t>((pc | 127) + 1))} << 32);
                    (*mid_)[mid_slot(mid_key)] = {mid_key, key, reinterpret_cast<uintptr_t>(entry),
                        result.live, result.installation, static_cast<uint16_t>(boundary.offset),
                        static_cast<uint16_t>(compiler ? compiler->code_copy() : 0),
                        boundary.completed, static_cast<uint8_t>(instructions - boundary.completed),
                        boundary.lazy_zero};
                }
                if (promote) {
                    // Links into the copy in the second level now
                    // enter this one; their records move along.
                    auto* second = copy.code.data();
                    for (auto index = second[0] >> 16; index; index = link_records_[index - 1].next)
                        *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(link_records_[index - 1].word)) =
                            static_cast<uint32_t>(target);
                    block[0] |= second[0] & 0xffff0000u;
                    second[0] &= 0xffff;
                }
            }
            // Only blocks that the pool accepted are stored. The
            // copy enters through count_entry instead of the
            // CLOCK mark, see Stubs.
            if (!promote && compiler->fixed_code()) {
                const auto count = boundaries.size();
                std::copy(boundaries.begin(), boundaries.end(), stored.begin());
                stored[count] = {0, 0, static_cast<uint8_t>(instructions), false};
                relocations[relocation_count] = {kHeaderWords * 4, stubs_.count_entry};
                output[kHeaderWords] = rv::jal(R::ra, 0).bits;
                const auto second = store_.store(key, words, relocations.first(relocation_count + 1),
                    std::as_bytes(std::span(stored).first(count + 1)), &retired, this);
                if (!second.empty()) {
                    second[0] = state.pc;
                    second[1] = kStoredEntries;
                    const size_t size = second.size(), links = second[size - 1];
                    for (size_t i = 0; i < links; ++i)
                        second[second[size - 1 - links + i] + 2] = static_cast<uint32_t>(stubs_.unlinked);
                }
            }
            if (reinstall && compiler->fixed_code()) {
                auto& r = *reinstall;
                if (!r.count || r.tag != reinstall_tag) r = {reinstall_tag, 0, 0, 0};
                r.count = static_cast<uint8_t>(instructions);
            }
        } else if (promote) {
            // The pool did not take it: run the copy where it is.
            target = reinterpret_cast<uintptr_t>(copy.code.data()) + kHeaderWords * 4;
            start = true;
            ++stored_entries_;
        }
        failure = result.status == jit::InstallStatus::full ? JitFallback::capacity :
            result.status == jit::InstallStatus::invalid ? JitFallback::encoding : JitFallback::publication;
    }
    return {entry, target, start, failure};
}
// Retires the block that contains the address, if one does.
void Sm83Jit::retire_containing(uintptr_t address) {
    uintptr_t begin, end;
    if (cache_.erase_containing(address, begin, end)) {
        ++invalidations_;
        retired(this, begin, end);
    }
}
JitResult Sm83Jit::execute(Sm83State& state, const Bus* bus, Tick until, unsigned limit, bool batch, bool frame) {
    if (state.mode != CpuMode::running) return {0, 0, JitExit::suspended};
    if (memory_ && !prefetched_count_ && until <= memory_->clock.now) return {};
    GBX_JIT_COUNT(execute);
    if (!limit) return {};
    // Fields are written before use. Keep one context and one deadline window
    // while mapping/events remain unchanged.
    Context context;
    context.instructions = context.mcycles = context.synced_cycles = context.stop = 0;
    context.cpu = &state; context.bus = bus;
    context.mcycle_shift = memory_ && memory_->double_speed() ? 2 : 3;
    if (memory_) {
        context.memory = memory_; context.pages = memory_->pages.data();
        context.dispatch = dispatch_->data()->data();
        // Blocks are looked up under the banks mapped now.
        memory_->remapped = 0;
        context.remap = 0;
        context.banks = memory_->code_tags.data();
        context.clock = &memory_->clock; context.until = until;
        context.instruction_pc = &memory_->instruction_pc;
        context.mapping_generation = memory_->mapping_generation;
        context.event_generation = memory_->clock.generation;
        context.interrupt_generation = memory_->interrupt_generation;
        context.tracing = tracing_;
        context.batch = batch;
        context.frame = frame;
        context.entry_generation = memory_->clock.generation;
        context.entry_frames = memory_->frames;
        context.budget = 0;
        context.lazy = 0;
        context.smc = 0;
        context.horizon = 0;
        context.open_fast = 0;
        update_deadline(&context);
    }
    uintptr_t link = 0;
    // A control-flow target reached within a batch gets its own block, so
    // that its exit can link to it. A run's first entry, which follows an
    // event, an interrupt or RETI, may resume at an inner block boundary.
    bool first_entry = true;
    for (;;) {
        const bool halt_bug = memory_ && state.halt_bug;
        const unsigned block_limit = std::min(limit, halt_bug ? 1u : 16u);
        const auto prefix_count = prefetched_count_;
        const auto prefix = prefetched_;
        const bool continuing = prefix_count != 0;
        JitFallback failure = JitFallback::unavailable;
        if (available()) {
            prefetched_count_ = 0;
            // At most 16 three-byte instructions span at most two 128-byte pages.
            // PC:16, limit-1:4, halt bug:1, EI:1, two page banks:10 each,
            // prefetched bytes:16, prefix length:2. A complete three-byte prefix
            // fetches nothing more and keeps its last byte in the second bank.
            // Bit 63 reserves flat opcodes.
            jit::BlockKey key = state.pc | (static_cast<uint32_t>(block_limit - 1) << 16) | (uint32_t{halt_bug} << 20);
            if (memory_) key |= (uint64_t{state.ei_pending} << 21) |
                (uint64_t{memory_->code_bank(state.pc)} << 22) |
                (static_cast<uint64_t>(prefix_count == 3 ? prefix[2] :
                    memory_->code_bank(static_cast<uint16_t>((state.pc | 127) + 1))) << 32);
            if (prefix_count) key |= (uint64_t{prefix_count} << 58) | (uint64_t{prefix[0]} << 42) |
                (uint64_t{prefix[1]} << 50);
            if (block_limit == 1 && !memory_) {
                // The compiler embeds only the opcode's semantics. PC, registers,
                // flags and immediate operands are all read by the generated code.
                // Keep a separate key space from PC-based multi-instruction blocks.
                const auto opcode = code_[state.pc];
                const unsigned identity = opcode == 0xcb ?
                    0x100u + code_[static_cast<uint16_t>(state.pc + 1)] : opcode;
                key = (UINT64_C(1) << 63) | identity;
            }
            const auto retired_before = evictions_ + invalidations_ + store_.drops();
            auto entry = cache_.find(key);
            uintptr_t target = entry ? reinterpret_cast<uintptr_t>(entry) + (memory_ ? kHeaderWords * 4 : 0) : 0;
            if (entry) GBX_JIT_COUNT(hit);
            // A block start, in the pool or the second level: exits may link
            // to it and the dispatch table may hold it.
            bool start = entry != nullptr;
            // Instructions to run on the interpreter instead of a block here.
            const bool plain = batch && !tracing_ && block_limit == 16 && !halt_bug && !prefix_count;
            unsigned interpret = 0;
            // The block's copy in the second level, when it moves into the
            // pool now.
            jit::CodeStore::Record copy{};
            bool promote = false;
            if (!entry && memory_ && !prefix_count) {
                // Resume inside a block that contains this instruction start.
                const MidEntry* mid = nullptr;
                if (first_entry) {
                    const auto& candidate = (*mid_)[mid_slot(key)];
                    if (candidate.key == key && candidate.block) mid = &candidate;
                }
                const auto resume = [&](uintptr_t block) {
                    GBX_JIT_COUNT(mid);
                    target = block + mid->offset;
                    context.instructions -= mid->completed;
                    if (mid->lazy_zero) {
                        context.lazy = state.f & 0x80 ? 0 : 1;
                        state.f &= 0x7f;
                    }
                    link = 0;
                };
                jit::CodeStore::Record owner;
                if (mid && *mid->live == mid->installation &&
                    (!mid->code || code_matches(reinterpret_cast<const uint32_t*>(mid->block) + mid->code))) {
                    resume(mid->block);
                } else if (store_.find(key, copy)) {
                    // The pool evicted the block. It runs in the second level
                    // until its entries there run out, see Stubs::count_entry.
                    if (static_cast<int32_t>(copy.code[1]) > 0) {
                        target = reinterpret_cast<uintptr_t>(copy.code.data()) + kHeaderWords * 4;
                        start = true;
                        ++stored_entries_;
                    } else promote = true;
                } else if (mid && !mid->code && store_.find(mid->owner, owner)) {
                    // The same instruction start in the evicted ROM block's copy.
                    resume(reinterpret_cast<uintptr_t>(owner.code.data()));
                } else if (mid && !mid->code && plain) {
                    // The ROM block left both levels. A block starting here
                    // would cover the code after it a second time, offset by
                    // these instructions; finish the old one instead.
                    interpret = mid->remaining;
                }
            }
            Reinstall* reinstall = nullptr;
            uint16_t reinstall_tag = 0;
            if (!target && !interpret && !promote && plain) {
                const auto hash = reinstall_hash(key);
                reinstall = &reinstall_[hash >> (32 - kReinstallBits)];
                reinstall_tag = static_cast<uint16_t>(hash);
                auto& r = *reinstall;
                if (r.count && r.tag == reinstall_tag) {
                    if (r.misses < (1u << r.level) - 1) {
                        ++r.misses;
                        interpret = r.count;
                    } else {
                        r.misses = 0;
                        if (r.level < kMaxLevel) ++r.level;
                    }
                }
            }
            if (interpret) {
                // Only a block's last instruction transfers control, so the
                // interpreter leaves where the block would: after them, or
                // earlier where the CPU must leave the batch. Device events
                // alone, which end an interpreter block, do not.
                const auto interrupts = memory_->interrupt_generation;
                const auto mapping = memory_->mapping_generation;
                unsigned done = 0;
                bool marker = false;
                sync_clock(&context);
                while (done < interpret) {
                    const auto step = memory_->double_speed()
                        ? sm83_run_double_speed(state, *memory_, until, interpret - done, stop_on_marker_)
                        : sm83_run_block(state, *memory_, until, interpret - done, stop_on_marker_);
                    if (!step.instructions) break;
                    done += step.instructions;
                    context.mcycles += step.mcycles;
                    marker = stop_on_marker_ && step.opcode == 0x40;
                    if (state.mode != CpuMode::running || marker || memory_->interrupt_generation != interrupts ||
                        memory_->mapping_generation != mapping)
                        break;
                }
                if (done) {
                    GBX_JIT_COUNT(interpreted);
                    GBX_JIT_ADD(interpreted_instructions, done);
                    context.instructions += done;
                    context.synced_cycles = context.mcycles;
                    // An interpreted STOP may have switched the speed.
                    context.mcycle_shift = memory_->double_speed() ? 2 : 3;
                    // The interpreter's writes and IME changes may move the
                    // deferral horizon. The next block is looked up under
                    // the banks it left.
                    context.horizon = 0;
                    update_deadline(&context);
                    if (context.remap) { remapped(&context); update_deadline(&context); }
                    link = 0;
                    if (marker) return {context.instructions, context.mcycles, JitExit::breakpoint, JitFallback::none};
                    if (!context.stop && context.mcycles < kBatch && state.mode == CpuMode::running &&
                        !(state.ime && memory_->pending_interrupts())) {
                        // Stopped inside: resume as after an event.
                        first_entry = done < interpret;
                        continue;
                    }
                    return {context.instructions, context.mcycles, JitExit::limit, JitFallback::none};
                }
            }
            if (!target) {
                const auto installed = install(state, key, block_limit, std::span(prefix).first(prefix_count), halt_bug,
                                               promote, copy, reinstall, reinstall_tag);
                entry = installed.entry;
                target = installed.target;
                start = installed.start;
                failure = installed.failure;
            }
            if (target) {
                // The exit that requested this block is still alive only if
                // no block was retired meanwhile.
                if (link && start && evictions_ + invalidations_ + store_.drops() == retired_before)
                    this->link(reinterpret_cast<uint32_t*>(link), target);
                link = 0;
                // Generated dynamic exits reach batch blocks through this table.
                if (start && memory_ && block_limit == 16 && !halt_bug && !prefix_count && !state.ei_pending &&
                    dispatchable(state.pc))
                    (*dispatch_)[state.pc & ((1u << kDispatchBits) - 1)] =
                        {state.pc | memory_->code_tags[state.pc >> 12], static_cast<uint32_t>(target)};
                if (!memory_) {
                    const auto reason = static_cast<JitExit>(entry(&context));
                    return {context.instructions, context.mcycles, reason, JitFallback::none};
                }
                context.chain = batch && block_limit == 16 && !halt_bug && !prefix_count ? context.mcycles + kLeg : 0;
                context.budget = context.stop ? 0 : context.chain;
                using Gateway = uint32_t (*)(Context*, uintptr_t);
                GBX_JIT_COUNT(enter);
                const auto exit = static_cast<JitExit>(reinterpret_cast<Gateway>(stubs_.enter)(&context, target));
                context.lazy = 0;
#ifdef GBX_PROFILER
                GBX_JIT_COUNT(exits[static_cast<unsigned>(exit) & 7]);
                if (exit == JitExit::control_flow || exit == JitExit::limit) {
                    if (context.chain && context.mcycles >= context.chain) GBX_JIT_COUNT(budget);
                    else if (context.exit & exit_bits::dynamic_pc) GBX_JIT_COUNT(cf_dynamic);
                    else if ((context.exit >> exit_bits::link & 3) == kLinkStatic) GBX_JIT_COUNT(cf_linked);
                    else GBX_JIT_COUNT(cf_none);
                }
#endif
                sync_clock(&context);
                // Dispatch device events that a deferred run left due.
                memory_->clock.advance(memory_->clock.now);
                if (context.smc) {
                    // A verified block changed its own code: compile it again,
                    // with per-fetch opcode checks, before it runs on.
                    unstable(static_cast<uint16_t>(*reinterpret_cast<const uint32_t*>(context.smc)), kUnstable);
                    retire_containing(context.smc);
                    context.smc = 0;
                    link = 0;
                }
                if (exit == JitExit::refetch && (context.exit & exit_bits::stale)) {
                    // A verified block found its code changed before running
                    // any of it. Compile the current code and enter again.
                    retire_containing(context.link);
                    unstable(state.pc, 1);
                    GBX_JIT_COUNT(stale);
                    link = 0;
                    continue;
                }
                if (exit == JitExit::refetch && (context.exit & exit_bits::spent)) {
                    // A block in the second level ran out of entries before
                    // running: the next lookup copies it into the pool.
                    link = 0;
                    first_entry = false;
                    continue;
                }
                const auto action = context.exit >> exit_bits::action & 3;
                if (action == kActionHalt || action == kActionHaltIme) sm83_halt(state, *memory_);
                else if (action == kActionStop) sm83_stop(state, *memory_);
                if (action == kActionHaltIme) { state.ime = true; ++memory_->interrupt_generation; }
                if (exit == JitExit::refetch) {
                    // RAM remains directly writable. An opcode mismatch retires
                    // this translation only after its native frame has returned.
                    // Other cached entries validate their own fetches on use.
                    retire_containing(context.active);
                    // The fetch already happened at its original bus timestamp.
                    // Continue using those latched bytes; do not read them twice,
                    // undo device events, or silently run the interpreter. The
                    // exit restored PC to the start of this instruction.
                    prefetched_count_ = context.consumed;
                    for (unsigned i = 0; i < prefetched_count_; ++i) prefetched_[i] = static_cast<uint8_t>(context.observed[i]);
                    const auto continuation = execute(state, bus, until, 1);
                    return {context.instructions + continuation.instructions,
                        context.mcycles + continuation.mcycles, continuation.exit, continuation.fallback};
                }
                // Bound accumulated 32-bit counters even with the LCD and
                // all other scheduled devices disabled during a long run.
                // STOP may switch the speed, which the context's windows assume.
                if (batch && !context.stop && context.mcycles < kBatch && action != kActionStop &&
                    (exit == JitExit::limit || exit == JitExit::control_flow) &&
                    state.mode == CpuMode::running && !(state.ime && memory_->pending_interrupts())) {
                    if (context.remap) {
                        // The banks changed after the last fetch, maybe under
                        // the exit's own block: no link from it. The next
                        // block is looked up under the new banks.
                        remapped(&context);
                        update_deadline(&context);
                        if (context.stop) return {context.instructions, context.mcycles, exit, JitFallback::none};
                    } else if ((context.exit >> exit_bits::link & 3) == kLinkStatic &&
                               *reinterpret_cast<const uint32_t*>(context.link) == static_cast<uint32_t>(stubs_.unlinked)) {
                        // A linked exit also leaves at the end of a leg; its
                        // link stays, with its one record.
                        link = context.link;
                    }
                    first_entry = false;
                    continue;
                }
                return {context.instructions, context.mcycles, exit, JitFallback::none};
            }
        }
        if (memory_) sync_clock(&context);
        ++fallbacks_[static_cast<unsigned>(failure)];
        if (continuing) {
            prefetched_count_ = prefix_count; prefetched_ = prefix;
            return {context.instructions, context.mcycles, JitExit::fallback, failure};
        }
        TraceBuffer* sink = memory_ && tracing_ ? memory_->trace_sink() : nullptr;
        if (sink) memory_->trace(sink, false);
        const auto step = memory_ ? sm83_step(state, *memory_) : sm83_step(state, *bus);
        if (sink) memory_->trace(sink, true);
        return {context.instructions + 1, context.mcycles + step.mcycles, JitExit::fallback, failure};
    }
}

}  // namespace gbx::gb
