// Build-time LZMS packing and installer-side unpacking for the game's native executable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ttt2::setup {

// The packed resource starts with a fixed magic plus an eight-byte little-endian original size.
bool GamePayloadOriginalSize(const void* packed, size_t packedSize, uint64_t& originalSize);
bool DecompressGamePayload(const void* packed, size_t packedSize,
                          std::vector<uint8_t>& output, std::string& error);

} // namespace ttt2::setup
