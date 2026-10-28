#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx {

// An encoding handed over in pieces, for one not held whole in memory, such
// as a state written to or read from a file a buffer at a time.
//
// Gives the encoding's next bytes: at least one and at most `most`, or none
// when it cannot.
using PieceSource = std::span<const uint8_t> (*)(void* context, size_t most);
// Takes the encoding's next bytes; false stops it.
using PieceSink = bool (*)(void* context, std::span<const uint8_t> bytes);

}  // namespace gbx
