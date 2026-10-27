// Runs a GBA game from an instant save on the interpreter and prints a hash
// of every frame, the PPU features the frames used and the renderer's time,
// for checking that a renderer change draws the same frames:
//
//     gbx_gba_frames --bios FILE [--state FILE] [--frames N] ROM
//
// The state is a slot file the app wrote, see components/app/src/slots.cpp;
// without one the game starts as the app loads it.
// Two builds give the same hashes when they draw the same frames; the hashes
// are compared between builds, never kept as the expected output.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include "gbx/gba/system.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        std::fclose(f);
    }
    return bytes;
}

uint64_t fnv(std::span<const uint16_t> pixels) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint16_t p : pixels) h = (h ^ p) * 0x100000001b3ull;
    return h;
}

// With GBX_SINK, frames go to RGB565 buffers as on the board: two that
// trade places at each frame, as the display's do for the renderer.
struct Sink final : gbx::gba::FrameSink {
    std::array<std::vector<uint16_t>, 2> buffers{std::vector<uint16_t>(gbx::gba::kFramePixels),
                                                 std::vector<uint16_t>(gbx::gba::kFramePixels)};
    unsigned writer = 0;
    uint16_t* frame_buffer() override { return buffers[writer].data(); }
    void frame_ready(uint64_t) override { writer ^= 1; }
};

// The slot file's header and thumbnail before the state.
constexpr size_t kStateOffset = 64 + gbx::gba::kFramePixels * 2;

}  // namespace

int main(int argc, char** argv) {
    const char *bios_path = nullptr, *state_path = nullptr, *rom_path = nullptr;
    unsigned frames = 600;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bios" && i + 1 < argc) bios_path = argv[++i];
        else if (arg == "--state" && i + 1 < argc) state_path = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else rom_path = argv[i];
    }
    if (!bios_path || !rom_path) {
        std::fprintf(stderr, "usage: %s --bios FILE [--state FILE] [--frames N] ROM\n", argv[0]);
        return 2;
    }
    const auto bios = read_file(bios_path);
    const auto slot = state_path ? read_file(state_path) : std::vector<uint8_t>{};
    if (bios.size() != gbx::gba::kBiosBytes || (state_path && slot.size() < kStateOffset + 4)) {
        std::fprintf(stderr, "cannot read the BIOS or the slot\n");
        return 1;
    }
    gbx::gba::RomCache cache;
    auto system = std::unique_ptr<gbx::gba::System>(new (std::nothrow) gbx::gba::System);
    if (!cache.open(rom_path, 64u << 20) || !system || !system->initialize() ||
        !system->load(std::span<const uint8_t, gbx::gba::kBiosBytes>(bios.data(), gbx::gba::kBiosBytes), cache)) {
        std::fprintf(stderr, "cannot start the game\n");
        return 1;
    }
    if (state_path) {
        uint32_t state_bytes;
        std::memcpy(&state_bytes, slot.data() + 52, 4);
        gbx::gba::Snapshot snapshot;
        if (!snapshot.allocate() ||
            snapshot.decode({slot.data() + kStateOffset, state_bytes}) != gbx::gba::Snapshot::Status::ok ||
            !system->restore(snapshot)) {
            std::fprintf(stderr, "cannot start the game from the slot\n");
            return 1;
        }
    }
    Sink sink;
    if (std::getenv("GBX_SINK")) system->set_frame_sink(&sink);
    // GBX_SKIP=<n>: of every n frames, only the first is drawn, as at 2X.
    const unsigned skip = std::getenv("GBX_SKIP") ? static_cast<unsigned>(std::atoi(std::getenv("GBX_SKIP"))) : 1;
    const unsigned every =
        std::getenv("GBX_HASH_EVERY") ? static_cast<unsigned>(std::atoi(std::getenv("GBX_HASH_EVERY"))) : 1;
    // GBX_SPLIT=<cycles> runs that long first, and with GBX_ROUNDTRIP also
    // saves the state there and restores it, through its encoding: the
    // frames after must not change.
    if (const char* split = std::getenv("GBX_SPLIT")) {
        system->run_until(system->now() + std::strtoull(split, nullptr, 10));
        if (std::getenv("GBX_ROUNDTRIP")) {
            gbx::gba::Snapshot saved, loaded;
            std::vector<uint8_t> bytes;
            if (!saved.allocate() || !loaded.allocate() || !system->capture(saved)) return 1;
            bytes.resize(saved.max_encoded());
            bytes.resize(saved.encode(bytes));
            if (bytes.empty() || loaded.decode(bytes) != gbx::gba::Snapshot::Status::ok || !system->restore(loaded)) {
                std::fprintf(stderr, "the round trip failed\n");
                return 1;
            }
        }
    }
    // Lines by DISPCNT mode, enabled BGs and OBJs, windows, BLDCNT's effect
    // and targets, as sampled at each frame's end.
    std::map<std::tuple<unsigned, unsigned, unsigned, unsigned>, unsigned> features;
    double run_ms = 0;
    // GBX_AUDIO_HASH: the PCM at 48000 Hz, read after each frame, hashed.
    const bool audio = std::getenv("GBX_AUDIO_HASH");
    uint64_t audio_hash = 0xcbf29ce484222325ull;
    size_t audio_frames = 0;
    std::vector<int16_t> pcm(2 * 4096);
    const auto take_audio = [&] {
        for (size_t n; (n = system->read_audio(pcm)) != 0; audio_frames += n)
            for (size_t i = 0; i < 2 * n; ++i) audio_hash = (audio_hash ^ static_cast<uint16_t>(pcm[i])) * 0x100000001b3ull;
    };
    if (audio) system->set_audio_rate(48000);
    // GBX_KEYS=<frame>:<keys>,...: from each frame on, the KEYINPUT bits
    // in hex are held, until the next entry; 0 releases them.
    std::vector<std::pair<unsigned, uint16_t>> keys;
    if (const char* text = std::getenv("GBX_KEYS")) {
        for (char* p = const_cast<char*>(text); *p;) {
            const unsigned frame = static_cast<unsigned>(std::strtoul(p, &p, 10));
            if (*p++ != ':') break;
            keys.emplace_back(frame, static_cast<uint16_t>(std::strtoul(p, &p, 16)));
            if (*p == ',') ++p;
        }
    }
    for (unsigned f = 0, next_keys = 0; f < frames; ++f) {
        const auto start = std::chrono::steady_clock::now();
        while (next_keys < keys.size() && keys[next_keys].first <= f) system->set_buttons(keys[next_keys++].second);
        if (skip > 1) system->set_drawing(f % skip == 0);
        system->run_frame();
        if (audio) take_audio();
        // GBX_HASH_EVERY=<n>: the frame of every n-th only; with GBX_SINK
        // it is the buffer last handed to the sink, back in BGR555.
        if (every > 1 && f % every) continue;
        if (std::getenv("GBX_SINK")) {
            system->memory.ppu.finish();
            std::vector<uint16_t> shown(gbx::gba::kFramePixels);
            const auto& from = sink.buffers[sink.writer ^ 1];
            for (size_t i = 0; i < shown.size(); ++i) {
                const unsigned c = from[i];
                shown[i] = static_cast<uint16_t>((c >> 11) | ((c >> 6) & 31) << 5 | (c & 31) << 10);
            }
            std::printf("frame %u %016llx\n", f, static_cast<unsigned long long>(fnv(shown)));
            continue;
        }
        const auto view = system->frame();
        run_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("frame %u %016llx\n", f, static_cast<unsigned long long>(fnv(view.pixels)));
        const auto& m = system->memory;
        ++features[{m.io16(0) & 0xff47u, m.io16(0) >> 8, m.io16(0x50), m.io16(0x52)}];
    }
    if (audio) {
        system->finish_audio();
        take_audio();
        std::printf("audio %016llx %zu frames %llu dropped\n", static_cast<unsigned long long>(audio_hash), audio_frames,
                    static_cast<unsigned long long>(system->memory.audio_dropped()));
    }
    if (std::getenv("GBX_STATE_HASH")) {
        // The machine at the end: the CPU, the clock and the memories.
        auto& m = system->memory;
        uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&](const void* data, size_t bytes) {
            for (size_t i = 0; i < bytes; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001b3ull;
        };
        mix(system->cpu.r.data(), 64);
        mix(&system->cpu.cpsr, 4);
        mix(system->cpu.pipeline.data(), 8);
        const auto now = system->now();
        mix(&now, sizeof now);
        mix(m.iwram.data(), m.iwram.size());
        mix(m.ewram, 256 * 1024);
        mix(m.vram, 96 * 1024);
        mix(m.palette.data(), m.palette.size());
        mix(m.oam.data(), m.oam.size());
        std::printf("state %016llx now %llu r15 %08x\n", static_cast<unsigned long long>(h),
                    static_cast<unsigned long long>(now), system->cpu.r[15]);
    }
    for (const auto& [key, count] : features)
        std::printf("dispcnt %04x layers %02x bldcnt %04x bldalpha %04x: %u frames\n", std::get<0>(key),
                    std::get<1>(key), std::get<2>(key), std::get<3>(key), count);
    std::printf("%u frames in %.1f ms\n", frames, run_ms);
    if (std::getenv("GBX_OBJ_STATS")) {
        // The last frame's OBJs: kind, size and the lines they cover.
        static constexpr uint8_t kSize[4][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                                   {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                                   {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
                                                   {{0, 0}, {0, 0}, {0, 0}, {0, 0}}};
        const auto& oam = system->memory.oam;
        unsigned pixels = 0, objs = 0;
        for (unsigned i = 0; i < 128; ++i) {
            const uint16_t a0 = oam[i * 8] | oam[i * 8 + 1] << 8, a1 = oam[i * 8 + 2] | oam[i * 8 + 3] << 8,
                           a2 = oam[i * 8 + 4] | oam[i * 8 + 5] << 8;
            const bool affine = a0 & 0x100;
            if (!affine && (a0 & 0x200)) continue;
            const unsigned w = kSize[a0 >> 14][a1 >> 14][0], h = kSize[a0 >> 14][a1 >> 14][1];
            const unsigned scale = affine && (a0 & 0x200) ? 2 : 1;
            ++objs;
            pixels += w * h * scale * scale;
            std::printf("obj %3u %s%s %2ux%-2u y %3u x %3u mode %u %s prio %u\n", i, affine ? "affine" : "normal",
                        scale == 2 ? "x2" : "", w, h, a0 & 0xff, a1 & 0x1ff, (a0 >> 10) & 3, a0 & 0x2000 ? "256" : "16",
                        (a2 >> 10) & 3);
        }
        std::printf("%u OBJs, %u pixels\n", objs, pixels);
    }
    if (const char* ppm = std::getenv("GBX_FRAME_PPM")) {
        // The last frame, for looking at it.
        if (FILE* f = std::fopen(ppm, "wb")) {
            std::fprintf(f, "P6 %u %u 255\n", gbx::gba::kScreenWidth, gbx::gba::kScreenHeight);
            for (const uint16_t p : system->frame().pixels) {
                const uint8_t rgb[3] = {static_cast<uint8_t>((p & 31) << 3), static_cast<uint8_t>(((p >> 5) & 31) << 3),
                                        static_cast<uint8_t>(((p >> 10) & 31) << 3)};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    return 0;
}
