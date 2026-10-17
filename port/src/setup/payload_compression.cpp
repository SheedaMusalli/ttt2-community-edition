#include "setup/payload_compression.h"

#include <windows.h>
#include <compressapi.h>

#include <cstring>
#include <limits>

namespace ttt2::setup {
namespace {

constexpr char kMagic[8] = { 'T', 'T', 'T', '2', 'L', 'Z', 'M', 'S' };
constexpr size_t kHeaderSize = sizeof(kMagic) + sizeof(uint64_t);

uint64_t ReadLe64(const uint8_t* p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}

std::string ApiError(const char* operation)
{
    return std::string("Could not unpack the installer game payload (") + operation +
           ", Windows error " + std::to_string(GetLastError()) + "). Please download the installer again.";
}

} // namespace

bool GamePayloadOriginalSize(const void* packed, size_t packedSize, uint64_t& originalSize)
{
    if (!packed || packedSize < kHeaderSize) return false;
    const auto* bytes = static_cast<const uint8_t*>(packed);
    if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) return false;
    originalSize = ReadLe64(bytes + sizeof(kMagic));
    return originalSize != 0 && originalSize <= (std::numeric_limits<size_t>::max)() &&
           packedSize > kHeaderSize;
}

bool DecompressGamePayload(const void* packed, size_t packedSize,
                          std::vector<uint8_t>& output, std::string& error)
{
    uint64_t originalSize = 0;
    if (!GamePayloadOriginalSize(packed, packedSize, originalSize)) {
        error = "The installer game payload is invalid. Please download the installer again.";
        return false;
    }

    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) {
        error = ApiError("opening the Windows LZMS decoder");
        return false;
    }

    try {
        output.resize(static_cast<size_t>(originalSize));
    } catch (...) {
        CloseDecompressor(decompressor);
        error = "There is not enough memory to unpack the game executable.";
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(packed);
    SIZE_T actualSize = 0;
    const BOOL ok = Decompress(decompressor, bytes + kHeaderSize, packedSize - kHeaderSize,
                               output.data(), output.size(), &actualSize);
    const DWORD decodeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseDecompressor(decompressor);
    if (!ok || actualSize != originalSize) {
        output.clear();
        SetLastError(decodeError);
        error = ApiError("decoding the compressed game executable");
        return false;
    }
    return true;
}

} // namespace ttt2::setup
