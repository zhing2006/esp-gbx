#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx::gb {
class Offload;
}

namespace gbx::gba {

class Memory;
class Renderer;
#ifdef GBX_PROFILER
// In profiling builds, the renderer's time on the worker: the jobs that
// ended a frame, and the microseconds spent drawing every job. The reader
// takes differences.
// Frames drawn and the time drawing them; since worst_draw_us and
// worst_span_us were last cleared, the longest time drawing one frame's
// lines and from its first line's start to its last line's end; the CPU
// core's waits for the renderer to finish with the payload ring's oldest
// part. draw_cycles and draw_retired are the worker core's mcycle and
// minstret over the same jobs, and part_cycles and part_retired those of
// the renderer's parts (kRenderParts), in units of 1 << kCountShift; all
// include what interrupts and other tasks ran in between.
enum RenderPart : unsigned {
    kPartPages, kPartObjects, kPartSetup, kPartPaint, kPartLayers, kPartObjLayer, kPartConvert, kPartOutput, kRenderParts
};
inline constexpr unsigned kCountShift = 4, kRecentFrames = 16;
// A running total that the worker adds to and the CPU core reads: the low
// 32 bits of total >> shift, which readers subtract modulo 2^32, as 64-bit
// atomics take a lock on RV32.
struct ProfileTotal {
    uint64_t total = 0;  // The worker's.
    std::atomic<uint32_t> units{0};
    void add(uint32_t value, unsigned shift = 0) {
        total += value;
        units.store(static_cast<uint32_t>(total >> shift), std::memory_order_relaxed);
    }
    uint32_t read() const { return units.load(std::memory_order_relaxed); }
};
struct RenderCounters {
    std::atomic<uint32_t> frames;
    ProfileTotal draw_us, draw_cycles, draw_retired;
    std::array<ProfileTotal, kRenderParts> part_cycles, part_retired;
    std::atomic<uint32_t> worst_draw_us, worst_span_us, payload_waits;
    std::atomic<uint32_t> worst_draw_cycles, worst_draw_retired;  // The longest frame's, see draw_cycles.
    // The parts' instructions in the frame being drawn and in the longest.
    std::array<uint32_t, kRenderParts> frame_part_retired;
    std::array<std::atomic<uint32_t>, kRenderParts> worst_part_retired;
    std::atomic<uint64_t> worst_draw_frame;  // Its frame number (Memory::frames).
    std::atomic<uint64_t> payload_wait_us;
    // The latest frames drawn, for the console's stall watch: the frame
    // number, the microseconds drawing it and its parts' instructions.
    // recent_count counts them; the latest is at (recent_count - 1) %
    // kRecentFrames.
    struct RecentFrame {
        uint64_t frame;
        uint32_t draw_us;
        std::array<uint32_t, kRenderParts> part_retired;
    };
    std::array<RecentFrame, kRecentFrames> recent;
    std::atomic<uint32_t> recent_count;
};
extern RenderCounters render_counters;
inline uint32_t profile_cycles() {
    uint32_t value = 0;
#if defined(__riscv)
    asm volatile("csrr %0, mcycle" : "=r"(value));
#endif
    return value;
}
inline uint32_t profile_retired() {
    uint32_t value = 0;
#if defined(__riscv)
    asm volatile("csrr %0, minstret" : "=r"(value));
#endif
    return value;
}
// The CPU core's catch-ups, when its cycles to the next event ran out,
// and the events of the PPU, the timers and DMA.
extern uint32_t catch_ups;
extern std::array<uint32_t, 3> event_counts;
// The ARM7 JIT's block entries through links and the dispatch table, the
// calls of its access stubs, its dispatch lookups and its stops at
// boundaries with no cycles left.
extern std::array<uint32_t, 16> arm7_gateway_counts;
// The ARM7 JIT's access stub calls by the address's region: reads, then
// writes from 16 on. Accesses inline in blocks do not call them.
extern std::array<uint32_t, 32> arm7_stub_counts;
// Where native code left for C++ without a stop, a link or a count, and
// find_block()'s outcomes, see arm7_jit_runtime.cpp.
struct ExitSite { uint32_t r15, count, cpsr; };
extern std::array<ExitSite, 64> arm7_exit_sites;
extern std::array<uint32_t, 16> arm7_find_counts;
// The ARM7 JIT's writes [0] and reads [1] through C++: by region, then
// those of I/O below 210h by halfword from 16 on.
extern std::array<std::array<uint32_t, 16 + 0x108>, 2> arm7_access_counts;
// Host cycles of the ARM7 JIT's run by part, see CycleSpan.
extern std::array<uint64_t, 28> arm7_cycle_counts;
#endif
}  // namespace gbx::gba
#ifdef GBX_PROFILER
namespace gbx::gba {
// Adds the host cycles of its scope to arm7_cycle_counts[part]: 0 the run,
// 1 native code, 2 writes and 3 reads through C++, 4 boundaries, 5 HALT,
// 6 interrupt entries, 7 lookups and the interpreter, 8 refills and
// fetches through C++, 9 catch-ups of accesses, 10 internal cycles
// through C++, 11 PPU events, 12 their job openings, 13 their job
// submissions, 14 their scheduling, 15 their interrupt requests; and
// counts: 16 writes, 17 reads, 18 catch-ups and 19 boundaries through C++;
// 20 compiling, 21 installing and 22 storing blocks; 23 catch-ups and 24
// boundaries while the interrupt stage is watched; 25 IME and 26 IE or IF
// changes that start a watch; 27 instructions the JIT's runs completed.
struct CycleSpan {
    static uint32_t now() {
        uint32_t cycles = 0;
#if defined(__riscv)
        asm volatile("csrr %0, mcycle" : "=r"(cycles));
#endif
        return cycles;
    }
    unsigned part;
    uint32_t start = now();
    ~CycleSpan() { arm7_cycle_counts[part] += now() - start; }
};
}  // namespace gbx::gba
#define GBX_CYCLE_SPAN(part) const ::gbx::gba::CycleSpan cycle_span{part}
#else
#define GBX_CYCLE_SPAN(part)
#endif
namespace gbx::gba {

inline constexpr unsigned kScreenWidth = 240, kScreenHeight = 160;
inline constexpr unsigned kFramePixels = kScreenWidth * kScreenHeight;
// GBATEK "LCD Dimensions and Timings": 308 dots of 4 cycles a line, 228
// lines a frame, 160 of them visible.
inline constexpr unsigned kLineCycles = 1232, kVisibleCycles = 960, kVisibleLines = 160, kLines = 228;
inline constexpr Tick kFrameCycles = Tick{kLineCycles} * kLines;

// Receives each frame the renderer completes, on the worker of the other
// core, as gb::FrameSink does for the GB.
class FrameSink {
public:
    // Where the frame goes: kFramePixels RGB565 pixels, row by row, as a
    // panel takes them, or nullptr to skip it.
    virtual uint16_t* frame_buffer() = 0;
    virtual void frame_ready(uint64_t id) = 0;
protected:
    ~FrameSink() = default;
};

// A register write in the visible part of a line, which the renderer
// applies from the dot it happened at: the halfword's offset in the I/O
// area and its new value.
struct TimedWrite {
    uint16_t cycle;
    uint8_t offset;
    uint8_t unused;
    uint16_t value;
};

// What the renderer gets for each visible line, handed over at its HBlank,
// and at the start of VBlank to finish the frame: the registers at the
// line's start, the VRAM, palette RAM and OAM pages written before that,
// and the register writes in the line's visible part. Pages and writes are
// in the payload ring.
struct LineJob {
    static constexpr unsigned kRegisterBytes = 0x58;
    uint8_t line = 0;            // 0..159, or 160 for the start of VBlank.
    uint8_t affine_written = 0;  // BG2X, BG2Y, BG3X, BG3Y written since the last job, bits 0-3.
    uint8_t pages = 0;           // Page numbers, then their contents, in the payload.
    uint8_t drawing = 1;         // 0: apply pages and registers, draw nothing.
    uint16_t writes = 0;
    uint32_t payload = 0;        // Offset in the payload ring.
    uint64_t frame = 0;
    alignas(4) std::array<uint8_t, kRegisterBytes> registers{};
};

// The PPU's timing on the CPU's core, and the jobs that hand its drawing to
// the other core (architecture, "双核分工"). The renderer reads only its
// jobs and its own copies of VRAM, palette RAM and OAM.
//
// Timing. A line starts when VCOUNT changes; nba-hw-test ppu/status-irq-dma
// gives the other edges in cycles from there, see gba_ppu.cpp. DISPSTAT's
// flags and VCOUNT are computed from the time when read, so events come
// only where something happens: the jobs at visible lines' starts and
// HBlanks, and the interrupts and DMA that are enabled.
//
// Changes within a line. Register writes in the visible part of a line
// take effect from the dot they happen at. Writes to VRAM, palette RAM and
// OAM take effect from the next line: a line's job carries the pages
// written before it starts, but in bitmap modes 4 and 5 those of the frame
// not shown wait for the first line that shows it, so that a frame select
// written within a line shows that frame's latest pages from the next line
// only. Which pixel a mid-line change reaches first is not guaranteed to
// match the hardware.
class Ppu {
public:
    // VRAM in 512-byte pages, palette RAM in 32-byte pages of 16 colors, as
    // games change a few colors per line, then OAM in 512-byte pages: 226.
    static constexpr unsigned kPageBits = 9, kPageBytes = 1u << kPageBits;
    static constexpr unsigned kColorPageBits = 5, kColorPageBytes = 1u << kColorPageBits;
    static constexpr unsigned kVramPages = 96 * 1024 >> kPageBits, kPalettePage = kVramPages,
                              kOamPage = kPalettePage + (1024 >> kColorPageBits), kPages = kOamPage + 2;
    static constexpr unsigned kDirtyWords = (kPages + 31) / 32;
    static_assert(kPalettePage % 32 == 0 && kOamPage == kPalettePage + 32, "palette RAM's pages are one dirty word");
    static constexpr unsigned page_bytes(unsigned page) {
        return page - kPalettePage < kOamPage - kPalettePage ? kColorPageBytes : kPageBytes;
    }

    Ppu();
    ~Ppu();
    // Lines go to the worker of `offload` (gb/offload.h, which both systems
    // use) until detach().
    bool initialize(gb::Offload& offload);
    // Power-on, with `time` at cycle `cycle` of `line`.
    void reset(Memory& memory, Tick time, unsigned line, unsigned cycle);
    uint16_t dispstat(Tick time) const;
    uint16_t vcount(Tick time) const { return static_cast<uint16_t>(position(time) / kLineCycles); }
    // VCOUNT's copy in I/O, and Memory::vcount_from, at `time`.
    void mirror_vcount(Tick time);
    // Before a register below 58h changes: a write in a visible line's
    // drawing time is timed.
    void write_register(Tick time, unsigned offset, uint16_t value);
    // DISPSTAT's interrupt enables or LYC changed, or a blanking or capture
    // DMA was armed: the next event may move.
    void reschedule();
    // The next start of VBlank after `time`.
    Tick next_vblank(Tick time) const;

    // Waits for the renderer to draw every line handed to it.
    void finish();
    // finish(), then lets go of the worker, which may stop before this PPU
    // goes.
    void detach();
    std::span<const uint16_t> pixels();
    uint64_t frame_id();
    void set_sink(FrameSink* sink);
    // Frames skipped without drawing still carry their pages and registers.
    void set_drawing(bool drawing) { drawing_ = drawing; }
    // Register writes timed in the line under way, for the tests' save
    // points.
    unsigned timed_writes() const { return open_ ? job_.writes : 0; }
#ifdef GBX_PROFILER
    uint64_t frames_begun() const { return frame_; }  // LineJob::frame of the next frame
#endif

private:
    friend struct SnapshotAccess;
    // Cycles since line 0 of the frame `time` is in. frame_start_ moves on
    // at every frame's line 0, so the 64-bit remainder, a libgcc call, is
    // rarely needed.
    unsigned position(Tick time) const {
        const Tick since = time - frame_start_;
        return static_cast<unsigned>(since < kFrameCycles ? since : since % kFrameCycles);
    }
    static void event(void* context);
    void on_event();
    void schedule();
    void begin_line(unsigned line);
    void submit();
    // Sets Memory::display_quiet from the open job.
    void quiet();
    Memory* memory_ = nullptr;
    gb::Offload* offload_ = nullptr;
    std::unique_ptr<Renderer> renderer_;
    // A time at line 0, cycle 0, of some frame.
    Tick frame_start_ = 0;
    Tick next_event_ = kNever;
    bool drawing_ = true;
    uint64_t frame_ = 0;
    // The job being filled: the line whose visible part runs now.
    bool open_ = false;
    LineJob job_{};
    uint8_t affine_written_ = 0;
    // Writes timed in the open job, up to kMaxWrites; later ones in the
    // same line take effect from its end.
    static constexpr unsigned kMaxWrites = 256;
    std::array<TimedWrite, kMaxWrites> writes_{};
    // Jobs and their payload ring, in external memory. The queue holds
    // three frames' lines: the renderer catches up in VBlank and over the
    // next frames, so the CPU waits only when drawing takes longer than
    // running for a while, not when the renderer's core is busy for a
    // moment.
    static constexpr unsigned kJobs = 512;
    static constexpr size_t kPayloadBytes = 1024 * 1024;
    LineJob* jobs_ = nullptr;
    uint8_t* payload_ = nullptr;
    // Positions in the payload ring counted from the reset, without
    // wrapping: where the next payload goes, and where the payload of each
    // job in the queue starts, by its index % kJobs (kJobs of them, in
    // external memory).
    uint64_t payload_head_ = 0, job_start_ = 0;
    uint64_t* payload_starts_ = nullptr;
    uint32_t submitted_ = 0;  // Index of the next job handed over.
    // Pages written in the bitmap frame not shown, see begin_line().
    std::array<uint32_t, kDirtyWords> held_{};
    // The jobs before this one hold no payload a new one may overwrite.
    uint32_t reused_before_ = 0;
    static void consume(void* context, uint32_t index);
    // The worker's last rest, see consume(); the worker's own.
    uint64_t rested_us_ = 0;
};

}  // namespace gbx::gba
