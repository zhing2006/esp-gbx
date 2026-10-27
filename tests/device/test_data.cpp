#include "test_data.h"

#include <cstdio>

#include "esp_attr.h"
#include "psa/crypto.h"

namespace gbx::test {
namespace {

// File reads only: PSRAM leaves internal RAM to the JIT code pool.
EXT_RAM_BSS_ATTR std::array<uint8_t, 16384> buffer;

bool hash_file(const char* path, std::array<uint8_t, 32>& digest, uint64_t& size) {
    auto* file = std::fopen(path, "rb");
    if (!file) return false;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    size = 0;
    size_t count;
    while (ok && (count = std::fread(buffer.data(), 1, buffer.size(), file)) != 0) {
        size += count;
        ok = psa_hash_update(&operation, buffer.data(), count) == PSA_SUCCESS;
    }
    size_t written = 0;
    ok = ok && !std::ferror(file) &&
        psa_hash_finish(&operation, digest.data(), digest.size(), &written) == PSA_SUCCESS && written == 32;
    psa_hash_abort(&operation);
    std::fclose(file);
    return ok;
}

}  // namespace

bool read_manifest_digest(const char* path, std::array<uint8_t, 32>& manifest_digest) {
    manifest_digest.fill(0);
    uint64_t size = 0;
    std::array<uint8_t, 32> digest;
    if (psa_crypto_init() != PSA_SUCCESS || !hash_file(path, digest, size)) return false;
    manifest_digest = digest;
    return true;
}

}  // namespace gbx::test
