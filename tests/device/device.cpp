#include <array>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_task_wdt.h"
#include "gbx/esp/board.h"
#include "gbx/esp/storage.h"
#include "gbx/platform.h"
#include "gbx/systems.h"
#include "transport.h"
#include "sm83_runner.h"
#if GBX_HAS_GBA
#include "arm7_runner.h"
#include "gba_rom_test.h"
#endif
#include "test_data.h"
#include "jit_test.h"
#include "display_test.h"
#include "audio_test.h"
#include "rom_test.h"
#include "reference_identity.h"

namespace {

namespace protocol = gbx::test::protocol;
namespace transport = gbx::test::transport;
// Only the SM83 suite uses the runner. It lives in PSRAM, so its size does not
// cut into the internal RAM that the JIT code pool of a ROM test needs.
gbx::test::Sm83Runner& runner() {
    static auto* instance = [] {
        void* storage = heap_caps_malloc(sizeof(gbx::test::Sm83Runner), MALLOC_CAP_SPIRAM);
        if (!storage) std::abort();
        return new (storage) gbx::test::Sm83Runner;
    }();
    return *instance;
}
#if GBX_HAS_GBA
gbx::test::Arm7Runner arm7_runner;
#endif
std::array<uint8_t, 32> manifest_digest{}, gba_manifest_digest{}, bios_digest{};

bool hello(const protocol::Frame& request, bool board_ready, bool mounted, bool data_ready, bool arm7_ready) {
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    const auto* app = esp_app_get_description();
    std::array<uint8_t, protocol::kMaxHelloPayload> payload{};
    // Layout and capabilities are documented in execution.h.
    std::memcpy(payload.data(), request.payload.data(), protocol::kNonceSize);
    auto* info = payload.data() + protocol::kNonceSize;
    protocol::store16(info, chip.revision);
    // Both backends of each CPU in the build (gbx/systems.h). The runners
    // allocate their JIT code memory on first use and report a failure there.
    info[2] = gbx::test::execution::kInterpreter | gbx::test::execution::kJit |
              (GBX_HAS_GBA ? gbx::test::execution::kArm7Interpreter | gbx::test::execution::kArm7Jit : 0);
    info[3] = static_cast<uint8_t>((board_ready ? 1 : 0) | (mounted ? 2 : 0) | (data_ready ? 4 : 0) |
                                   (arm7_ready ? 8 : 0));
    protocol::store32(info + 4, esp_clk_cpu_freq());
    protocol::store16(info + 8, protocol::kMaxPayload);
    protocol::store16(info + 10, protocol::kWindow);
    size_t size = protocol::kNonceSize + 12;
    for (const std::string_view text : {gbx::board::name(), app->idf_ver, app->version, GBX_TEST_PRESET}) {
        if (size + 2 + text.size() > payload.size()) {
            return false;
        }
        protocol::store16(payload.data() + size, static_cast<uint16_t>(text.size()));
        size += 2;
        std::memcpy(payload.data() + size, text.data(), text.size());
        size += text.size();
    }
    if (size + 194 > payload.size()) return false;
    protocol::store16(payload.data() + size, gbx::test::execution::kVersion);
    std::memcpy(payload.data() + size + 2, manifest_digest.data(), 32);
    std::memcpy(payload.data() + size + 34, app->app_elf_sha256, 32);
    std::memcpy(payload.data() + size + 66, kSm83Identity, 32);
    std::memcpy(payload.data() + size + 98, gba_manifest_digest.data(), 32);
    std::memcpy(payload.data() + size + 130, kArm7Identity, 32);
    std::memcpy(payload.data() + size + 162, bios_digest.data(), 32);
    size += 194;
    return transport::begin_session(request.payload) &&
           transport::send(protocol::Kind::hello_reply, std::span(payload).first(size));
}

}  // namespace

extern "C" void app_main(void) {
#if CONFIG_FREERTOS_UNICORE
    // One core runs the cases, the transport and the idle task. A check that
    // runs for seconds leaves the idle task no time, and the task watchdog's
    // report would arrive as text inside the binary session.
    const esp_task_wdt_config_t watchdog{
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = false};
    esp_task_wdt_reconfigure(&watchdog);
#endif
    const bool board_ready = gbx::board::init();
    // Buffer HELLO retries while mounting and hashing the SD data. The UART's
    // hardware FIFO alone cannot retain requests throughout initialization.
    if (!transport::start()) {
        return;
    }
    const bool mounted = board_ready && gbx::platform::storage::mount_sd();
    // The manifests identify the data; the packs are not hashed on the board.
    const bool manifest_ready =
        mounted && gbx::test::read_manifest_digest("/sdcard/tests/gb/manifest.tsv", manifest_digest);
    const bool data_ready = manifest_ready && runner().open("/sdcard/tests/gb/sm83/cases.bin");
#if GBX_HAS_GBA
    const bool arm7_ready =
        mounted && gbx::test::read_manifest_digest("/sdcard/tests/gba/manifest.tsv", gba_manifest_digest) &&
        arm7_runner.open("/sdcard/tests/gba/arm7tdmi/cases.bin");
    if (mounted) bios_digest = gbx::test::gba_rom_test::load_bios();
#else
    const bool arm7_ready = false;
#endif
    ESP_LOGI("test", "SM83 test data: %s; ARM7TDMI test data: %s", data_ready ? "ready" : "unavailable",
             arm7_ready ? "ready" : "unavailable");
    for (;;) {
        protocol::Frame request{};
        const auto result = transport::receive(request, 100);
        if (result == transport::Receive::timeout) {
            continue;
        }
        if (result == transport::Receive::error) {
            transport::abandon_session();
            gbx::platform::log(gbx::platform::LogLevel::error, "test", "Protocol error; reconnect required");
            continue;
        }
        bool ok;
        if (request.kind == protocol::Kind::hello) {
            ok = hello(request, board_ready, mounted, data_ready, arm7_ready);
        } else if (request.kind == protocol::Kind::ping) {
            ok = transport::send(protocol::Kind::pong, request.payload);
        } else if (request.kind == protocol::Kind::jit_test && board_ready &&
                   request.payload.size() == 8 &&
                   protocol::load16(request.payload.data()) == gbx::test::jit_test::kVersion &&
                   protocol::load16(request.payload.data() + 2) <= 2 &&
                   protocol::load32(request.payload.data() + 4) != 0) {
            const auto id = protocol::load32(request.payload.data() + 4);
            ok = gbx::test::jit_test::run(id, protocol::load16(request.payload.data() + 2));
        } else if (request.kind == protocol::Kind::display_test && board_ready &&
                   request.payload.size() == 8 &&
                   protocol::load16(request.payload.data()) == gbx::test::display_test::kVersion &&
                   protocol::load16(request.payload.data() + 2) == 0 &&
                   protocol::load32(request.payload.data() + 4) != 0) {
            ok = gbx::test::display_test::run(protocol::load32(request.payload.data() + 4));
        } else if (request.kind == protocol::Kind::audio_test && board_ready &&
                   request.payload.size() == 8 &&
                   protocol::load16(request.payload.data()) == gbx::test::audio_test::kVersion &&
                   protocol::load16(request.payload.data() + 2) == 0 &&
                   protocol::load32(request.payload.data() + 4) != 0) {
            ok = gbx::test::audio_test::run(protocol::load32(request.payload.data() + 4));
        } else if (request.kind == protocol::Kind::rom_test && manifest_ready) {
            ok = gbx::test::rom_test::run(request.payload);
#if GBX_HAS_GBA
        } else if (request.kind == protocol::Kind::gba_rom_test && mounted) {
            ok = gbx::test::gba_rom_test::run(request.payload);
        } else if (request.kind == protocol::Kind::arm7_run && arm7_ready) {
            ok = arm7_runner.run(request.payload);
#endif
        } else if (request.kind == protocol::Kind::run && data_ready) {
            ok = runner().run(request.payload);
        } else {
            constexpr std::string_view message = "Unsupported request or test data unavailable";
            ok = transport::send(protocol::Kind::error,
                {reinterpret_cast<const uint8_t*>(message.data()), message.size()});
        }
        if (!ok) {
            transport::abandon_session();
        }
    }
}
