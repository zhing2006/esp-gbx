// The GBA system: loading, power-on and runs on either backend.

#include "gbx/gba/system.h"

#include "gbx/gba/jit/arm7_jit.h"

namespace gbx::gba {

System::System() { memory.cpu = &cpu; }
System::~System() = default;

bool System::initialize() { return memory.initialize(); }

bool System::load(std::span<const uint8_t, kBiosBytes> bios, RomCache& rom) {
    if (!memory.load(bios, rom)) return false;
    memory.backup.reset();
    // Blocks of another BIOS or ROM.
    if (jit_) jit_->clear();
    reset();
    return true;
}

// The state the BIOS leaves when it starts the cartridge at 8000000h in ARM
// state, System mode, from its disassembly (start-up at 0000h-00B0h and the
// branch at 0000h-02xxh): R0-R12 zero, the stacks of System, IRQ and SVC
// mode at 3007F00h, 3007FA0h and 3007FE0h, LR and SPSR of IRQ and SVC zero,
// and LR 8000000h, the address the BIOS branched to.
void System::reset() {
    if (jit_) jit_->forget_ram_code();
    memory.reset();
    cpu = {};
    cpu.cpsr = kModeSystem;
    cpu.r[13] = 0x03007f00;
    cpu.r[14] = 0x08000000;
    cpu.r13[2] = {0x03007fe0, 0};  // SVC
    cpu.r13[4] = {0x03007fa0, 0};  // IRQ
    cpu.r[15] = 0x08000008;
    cpu.pipeline = {memory.peek(0x08000000, 2), memory.peek(0x08000004, 2)};
    cpu.sequential = true;
}

void System::reset_to_bios() {
    if (jit_) jit_->forget_ram_code();
    memory.reset(true);
    cpu = {};
    cpu.cpsr = kModeSupervisor | kFlagI | kFlagF;
    cpu.r[15] = 8;
    cpu.pipeline = {memory.peek(0, 2), memory.peek(4, 2)};
    cpu.sequential = true;
}

bool System::select_backend(Backend backend, const platform::ExecutableMemory* reserved) {
    if (backend == Backend::jit && !jit_) {
        jit_.reset(new (std::nothrow) Arm7Jit);
        if (!jit_ || !jit_->initialize(memory, jit_capacity_, reserved)) {
            jit_.reset();
            return false;
        }
    }
    backend_ = backend;
    return true;
}

Tick System::run_until(Tick target) {
    memory.begin_run(target);
    if (backend_ == Backend::jit) jit_->run(cpu, memory);
    else arm7_run_mapped(cpu, memory);
    // The sound of the run goes to the other core at its end.
    memory.flush_sound();
    return now();
}

bool System::idle_loop() const {
    const uint32_t op = cpu.pipeline[0];
    // B to itself: ARM EAFFFFFEh, Thumb E7FEh.
    return cpu.cpsr & kFlagT ? (op & 0xffff) == 0xe7fe : op == 0xeafffffe;
}

}  // namespace gbx::gba
