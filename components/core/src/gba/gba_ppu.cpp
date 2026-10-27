// The PPU's timing on the CPU's core and the jobs it hands to the renderer
// (gba_renderer.cpp) on the other core, see Ppu's comment.

#include <algorithm>
#include <cstring>
#include <new>

#include "gbx/gba/memory.h"
#include "gbx/platform.h"
#include "../gb/offload.h"
#include "bits.h"
#include "renderer.h"

namespace gbx::gba {
namespace {

// Cycles from a line's start, where VCOUNT changes and the HBlank flag
// clears, to its other edges (nba-hw-test ppu/status-irq-dma): the
// V-counter flag follows a cycle later, the HBlank flag rises 1007 cycles
// in, and each interrupt request follows its flag a cycle later, which IF
// reads see a cycle after that (gba_irq.cpp).
constexpr unsigned kVCountFlag = 1, kHBlankFlag = 1007, kIrqLag = 1;
constexpr unsigned kVBlankIrq = kIrqLag, kVCountIrq = kVCountFlag + kIrqLag, kHBlankIrq = kHBlankFlag + kIrqLag;
// Video capture DMA starts this many cycles into each of lines 2 to 161,
// not at HBlank as GBATEK's "similar like HBlank DMA" suggests
// (nba-hw-test ppu/status-irq-dma, VIDEO DMA).
constexpr unsigned kCaptureCycle = 4;

}  // namespace

#ifdef GBX_PROFILER
RenderCounters render_counters;
uint32_t catch_ups = 0;
std::array<uint32_t, 3> event_counts{};
std::array<uint32_t, 16> arm7_gateway_counts{};
std::array<uint32_t, 32> arm7_stub_counts{};
std::array<ExitSite, 64> arm7_exit_sites{};
std::array<uint32_t, 16> arm7_find_counts{};
std::array<std::array<uint32_t, 16 + 0x108>, 2> arm7_access_counts{};
std::array<uint64_t, 28> arm7_cycle_counts{};
#endif

Ppu::Ppu() = default;

Ppu::~Ppu() {
    finish();
    platform::free_external(jobs_);
    platform::free_external(payload_);
    platform::free_external(payload_starts_);
}

bool Ppu::initialize(gb::Offload& offload) {
    offload_ = &offload;
    if (!jobs_) jobs_ = static_cast<LineJob*>(platform::allocate_external(kJobs * sizeof(LineJob)));
    if (!payload_) payload_ = static_cast<uint8_t*>(platform::allocate_external(kPayloadBytes));
    if (!payload_starts_) payload_starts_ = static_cast<uint64_t*>(platform::allocate_external(kJobs * sizeof(uint64_t)));
    if (!renderer_) renderer_.reset(new (std::nothrow) Renderer);
    if (!jobs_ || !payload_ || !payload_starts_ || !renderer_ || !renderer_->initialize()) return false;
    offload.attach(gb::Offload::rows, kJobs, consume, this);
    return true;
}

void Ppu::reset(Memory& memory, Tick time, unsigned line, unsigned cycle) {
    finish();
    memory_ = &memory;
    frame_start_ = time - (Tick{line} * kLineCycles + cycle);
    frame_ = 0;
    open_ = false;
    affine_written_ = 0xf;
    // Everything is new to the renderer.
    memory.dirty.fill(0xffffffff);
    memory.dirty.back() = (1u << (kPages % 32)) - 1;
    held_ = {};
    renderer_->reset();
    quiet();
    mirror_vcount(time);
    schedule();
}

void Ppu::mirror_vcount(Tick time) {
    const unsigned at = position(time);
    memory_->io_[6] = static_cast<uint8_t>(at / kLineCycles);
    memory_->vcount_from = static_cast<uint32_t>(time - at % kLineCycles);
}

void Ppu::quiet() {
    const Tick from = open_ ? frame_start_ + Tick{job_.line} * kLineCycles + kVisibleCycles : memory_->clock.now;
    memory_->display_quiet = static_cast<uint32_t>(from);
}

uint16_t Ppu::dispstat(Tick time) const {
    const unsigned at = position(time), line = at / kLineCycles, cycle = at % kLineCycles;
    const uint16_t stored = memory_->io16(4);
    uint16_t flags = 0;
    if (line >= kVisibleLines && line < kLines - 1) flags |= 1;
    if (cycle >= kHBlankFlag) flags |= 2;
    // The V-counter flag compares the line a cycle ago.
    const unsigned compared = cycle >= kVCountFlag ? line : (line ? line - 1 : kLines - 1);
    if (compared == (stored >> 8)) flags |= 4;
    return static_cast<uint16_t>((stored & 0xff38) | flags);
}

Tick Ppu::next_vblank(Tick time) const {
    Tick vblank = time - position(time) + Tick{kVisibleLines} * kLineCycles;
    if (vblank <= time) vblank += kFrameCycles;
    return vblank;
}

void Ppu::write_register(Tick time, unsigned offset, uint16_t value) {
    if ((offset >= 0x28 && offset < 0x30) || (offset >= 0x38 && offset < 0x40))
        affine_written_ |= static_cast<uint8_t>(1u << ((((offset - 0x28) >> 2) & 1) | (offset >= 0x38 ? 2 : 0)));
    if (!open_) return;
    const unsigned at = position(time), cycle = at % kLineCycles;
    if (at / kLineCycles != job_.line || cycle >= kVisibleCycles) return;
    if (job_.writes < kMaxWrites) {
        writes_[job_.writes++] = {static_cast<uint16_t>(cycle), static_cast<uint8_t>(offset), 0, value};
    } else {
        // Out of room: from the end of the line, as a write in HBlank.
        writes_[kMaxWrites - 1] = {kVisibleCycles, static_cast<uint8_t>(offset), 0, value};
    }
}

// --- Events -----------------------------------------------------------------

// The next edge after clock.now where something happens: a visible line's
// start or VBlank's (jobs, frames, VBlank DMA), the HBlank of a line with a
// blanking or capture DMA armed, and the enabled interrupts.
void Ppu::schedule() {
    GBX_CYCLE_SPAN(14);
    auto& m = *memory_;
    const Tick now = m.clock.now;
    const unsigned at = position(now), line = at / kLineCycles, cycle = at % kLineCycles;
    const Tick line_start = now - cycle;
    Tick next = line < kVisibleLines ? line_start + kLineCycles : now - at + kFrameCycles;
    const uint16_t stat = m.io16(4);
    auto consider = [&](Tick when) {
        if (when > now && when < next) next = when;
    };
    // An edge `edge` cycles into a line: this one's if it is still to come
    // and the line wants it, else the next line's if that one does.
    const unsigned following = line + 1 == kLines ? 0 : line + 1;
    auto each_line = [&](unsigned edge, bool here, bool there) {
        if (cycle < edge && here) consider(line_start + edge);
        else if (there) consider(line_start + kLineCycles + edge);
    };
    if (m.dma_waiting(2)) each_line(kHBlankFlag, line < kVisibleLines, following < kVisibleLines);
    if (m.capture_)
        each_line(kCaptureCycle, line >= 2 && line < kVisibleLines + 2, following >= 2 && following < kVisibleLines + 2);
    if (stat & 0x10) each_line(kHBlankIrq, true, true);
    auto in_frame = [&](unsigned l, unsigned cycle) {
        Tick when = now - at + Tick{l} * kLineCycles + cycle;
        if (when <= now) when += kFrameCycles;
        return when;
    };
    if (stat & 0x08) consider(in_frame(kVisibleLines, kVBlankIrq));
    if (m.capture_) consider(in_frame(kVisibleLines + 2, 0));
    if ((stat & 0x20) && (stat >> 8) < kLines) consider(in_frame(stat >> 8, kVCountIrq));
    next_event_ = next;
    m.clock.schedule(kSlotPpu, next, event, this);
}

void Ppu::reschedule() {
    if (memory_) schedule();
}

void Ppu::event(void* context) {
#ifdef GBX_PROFILER
    ++event_counts[0];
#endif
    static_cast<Ppu*>(context)->on_event();
}

void Ppu::on_event() {
    GBX_CYCLE_SPAN(11);
    auto& m = *memory_;
    const unsigned at = position(m.clock.now), line = at / kLineCycles, cycle = at % kLineCycles;
    const uint16_t stat = m.io16(4);
    m.io_[6] = static_cast<uint8_t>(line);
    m.vcount_from = static_cast<uint32_t>(m.clock.now - cycle);
    if (cycle == 0 && line <= kVisibleLines) {
        if (line == 2) m.capture_ = m.dma_waiting(3);
        if (open_) submit();
        if (line == 0) frame_start_ = m.clock.now;
        if (line < kVisibleLines) {
            begin_line(line);
        } else {
            // VBlank: the frame is complete.
            begin_line(kVisibleLines);
            submit();
            ++frame_;
            ++m.frames;
            m.trigger_dma(1);
        }
    }
    if (cycle == kHBlankFlag) {
        if (line < kVisibleLines) m.trigger_dma(2);
    }
    if (cycle == kCaptureCycle && line >= 2 && line < kVisibleLines + 2 && m.capture_) m.trigger_dma(3, 8);
    if (line == kVisibleLines + 2 && cycle == 0) m.stop_capture();
    uint16_t requests = 0;
    if (cycle == kHBlankIrq && (stat & 0x10)) requests |= irq::hblank;
    if (line == kVisibleLines && cycle == kVBlankIrq && (stat & 0x08)) requests |= irq::vblank;
    if (line == (stat >> 8) && cycle == kVCountIrq && (stat & 0x20)) requests |= irq::vcount;
    if (requests) {
        GBX_CYCLE_SPAN(15);
        m.request_irq(requests);
    }
    schedule();
}

// --- Jobs --------------------------------------------------------------------

// Opens the job of `line`: the registers now, and the pages written since
// the last job, copied to the payload ring.
void Ppu::begin_line(unsigned line) {
    GBX_CYCLE_SPAN(12);
    auto& m = *memory_;
    job_.line = static_cast<uint8_t>(line);
    job_.frame = frame_;
    job_.drawing = drawing_;
    job_.writes = 0;
    job_.affine_written = affine_written_;
    affine_written_ = 0;
    std::memcpy(job_.registers.data(), m.io_.data(), LineJob::kRegisterBytes);
    // In bitmap modes 4 and 5 the frame DISPCNT does not show waits: its
    // pages go with the first line that shows it, so that a game drawing
    // into it hands each page over once (GBATEK "LCD VRAM Bitmap BG Modes").
    // Frame 0 is VRAM 0-9FFFh, pages 0-79; frame 1 A000h-13FFFh, 80-159.
    static constexpr std::array<std::array<uint32_t, kDirtyWords>, 2> kHidden{{
        {0, 0, 0xffff0000u, 0xffffffffu, 0xffffffffu, 0, 0, 0},
        {0xffffffffu, 0xffffffffu, 0x0000ffffu, 0, 0, 0, 0, 0},
    }};
    const unsigned dispcnt = job_.registers[0] | job_.registers[1] << 8, mode = dispcnt & 7;
    const bool bitmap = mode == 4 || mode == 5;
    for (unsigned w = 0; w < kDirtyWords; ++w) {
        const uint32_t hidden = bitmap ? kHidden[(dispcnt >> 4) & 1][w] : 0, pending = m.dirty[w] | held_[w];
        held_[w] = pending & hidden;
        m.dirty[w] = pending & ~hidden;
    }
    unsigned count = 0;
    for (const uint32_t word : m.dirty) count += count_bits(word);
    const unsigned colors = count_bits(m.dirty[kPalettePage / 32]);
    const size_t data_bytes = size_t{count - colors} * kPageBytes + size_t{colors} * kColorPageBytes;
    const size_t bytes = count + data_bytes + kMaxWrites * sizeof(TimedWrite);
    // A payload does not wrap around the ring's end.
    if (payload_head_ % kPayloadBytes + bytes > kPayloadBytes)
        payload_head_ += kPayloadBytes - payload_head_ % kPayloadBytes;
    // The renderer reads the payload of each job it has not finished: the
    // jobs whose payload starts in the part of the ring this one takes
    // must be done first. Older ones than the queue holds are.
    const uint64_t reused = payload_head_ + bytes;
    if (reused > kPayloadBytes) {
        // Starts grow with the index, and the limit with each job: the jobs
        // passed once are not looked at again.
        const uint64_t limit = reused - kPayloadBytes;
        uint32_t index = std::max(reused_before_, submitted_ - std::min<uint32_t>(submitted_, kJobs));
        while (index != submitted_ && payload_starts_[index % kJobs] < limit) ++index;
        if (index != reused_before_) {
#ifdef GBX_PROFILER
            const uint64_t start = platform::monotonic_us();
            offload_->finish_before(gb::Offload::rows, index);
            render_counters.payload_waits.fetch_add(1, std::memory_order_relaxed);
            render_counters.payload_wait_us.fetch_add(platform::monotonic_us() - start, std::memory_order_relaxed);
#else
            offload_->finish_before(gb::Offload::rows, index);
#endif
        }
        reused_before_ = index;
    }
    const auto offset = static_cast<size_t>(payload_head_ % kPayloadBytes);
    job_start_ = payload_head_;
    job_.payload = static_cast<uint32_t>(offset);
    job_.pages = 0;
    uint8_t* numbers = payload_ + offset;
    uint8_t* data = numbers + count;
    for (unsigned w = 0; w < Ppu::kDirtyWords; ++w) {
        uint32_t word = m.dirty[w];
        m.dirty[w] = 0;
        while (word) {
            const unsigned page = w * 32 + lowest_bit(word);
            word &= word - 1;
            numbers[job_.pages++] = static_cast<uint8_t>(page);
            if (page - kPalettePage < kOamPage - kPalettePage) {
                std::memcpy(data, m.palette.data() + (page - kPalettePage) * kColorPageBytes, kColorPageBytes);
                data += kColorPageBytes;
                continue;
            }
            std::memcpy(data, page < kVramPages ? m.vram + page * kPageBytes : m.oam.data() + (page - kOamPage) * kPageBytes,
                        kPageBytes);
            data += kPageBytes;
        }
    }
    payload_head_ += count + data_bytes;
    open_ = true;
    quiet();
}

// Hands the open job over, with the writes timed in its line.
void Ppu::submit() {
    GBX_CYCLE_SPAN(13);
    open_ = false;
    quiet();
    if (job_.writes) {
        std::memcpy(payload_ + payload_head_ % kPayloadBytes, writes_.data(), job_.writes * sizeof(TimedWrite));
        payload_head_ += job_.writes * sizeof(TimedWrite);
    }
    const uint32_t index = offload_->acquire(gb::Offload::rows);
    jobs_[index % kJobs] = job_;
    payload_starts_[index % kJobs] = job_start_;
    submitted_ = index + 1;
    offload_->submit(gb::Offload::rows, job_.line == kVisibleLines);
}

// A worker that always has work, as behind a CPU core that runs faster
// than the original, would starve the idle task of its core and its
// watchdog (5 s): at each frame's end it sleeps a tick if it has not slept
// for a second, which costs the CPU core at most a millisecond a second.
// The rest is here rather than in the shared worker loop, which would move
// the GB's hot code.
void Ppu::consume(void* context, uint32_t index) {
    auto& self = *static_cast<Ppu*>(context);
    const LineJob& job = self.jobs_[index % kJobs];
#ifdef GBX_PROFILER
    const uint64_t start = platform::monotonic_us();
    if (job.line == 0) render_counters.frame_part_retired = {};
    const uint32_t cycles = profile_cycles(), retired = profile_retired();
#endif
    self.renderer_->draw(job, self.payload_ + job.payload);
#ifdef GBX_PROFILER
    const uint32_t job_retired = profile_retired() - retired, job_cycles = profile_cycles() - cycles;
    render_counters.draw_retired.add(job_retired, kCountShift);
    render_counters.draw_cycles.add(job_cycles, kCountShift);
    const uint64_t end = platform::monotonic_us();
    render_counters.draw_us.add(static_cast<uint32_t>(end - start));
    static uint64_t frame_start = 0, frame_draw = 0;
    static uint32_t frame_cycles = 0, frame_retired = 0;
    if (job.line == 0) frame_start = start, frame_draw = 0, frame_cycles = 0, frame_retired = 0;
    frame_draw += end - start;
    frame_cycles += job_cycles;
    frame_retired += job_retired;
    if (job.line == kVisibleLines) {
        render_counters.frames.fetch_add(1, std::memory_order_relaxed);
        const auto keep = [](std::atomic<uint32_t>& worst, uint64_t us) {
            if (us > worst.load(std::memory_order_relaxed)) worst.store(static_cast<uint32_t>(us), std::memory_order_relaxed);
        };
        if (frame_draw > render_counters.worst_draw_us.load(std::memory_order_relaxed)) {
            render_counters.worst_draw_frame.store(job.frame, std::memory_order_relaxed);
            render_counters.worst_draw_cycles.store(frame_cycles, std::memory_order_relaxed);
            render_counters.worst_draw_retired.store(frame_retired, std::memory_order_relaxed);
            for (unsigned i = 0; i < kRenderParts; ++i)
                render_counters.worst_part_retired[i].store(render_counters.frame_part_retired[i], std::memory_order_relaxed);
        }
        keep(render_counters.worst_draw_us, frame_draw);
        if (frame_start) keep(render_counters.worst_span_us, end - frame_start);
        const uint32_t recent = render_counters.recent_count.load(std::memory_order_relaxed);
        render_counters.recent[recent % kRecentFrames] = {job.frame, static_cast<uint32_t>(frame_draw),
                                                          render_counters.frame_part_retired};
        render_counters.recent_count.store(recent + 1, std::memory_order_release);
    }
#endif
    if (job.line == kVisibleLines) {
        if (const uint64_t now = platform::monotonic_us(); now - self.rested_us_ >= 1'000'000) {
            platform::sleep_ms(1);
            self.rested_us_ = platform::monotonic_us();
        }
    }
}

void Ppu::finish() {
    if (offload_) offload_->finish(gb::Offload::rows);
}

void Ppu::detach() {
    finish();
    offload_ = nullptr;
}

std::span<const uint16_t> Ppu::pixels() {
    finish();
    return renderer_->pixels();
}

uint64_t Ppu::frame_id() {
    finish();
    return renderer_->frame_id();
}

void Ppu::set_sink(FrameSink* sink) {
    finish();
    // A PPU whose initialize() failed has no renderer to unbind.
    if (renderer_) renderer_->set_sink(sink);
}

}  // namespace gbx::gba
