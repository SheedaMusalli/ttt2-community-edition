// Compresses the native game executable into the LZMS resource format read by the installer.
#include "setup/payload_compression.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

namespace {

constexpr char kMagic[8] = { 'T', 'T', 'T', '2', 'L', 'Z', 'M', 'S' };

bool ReadFile(const wchar_t* path, std::vector<uint8_t>& bytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto end = file.tellg();
    if (end <= 0 || static_cast<uint64_t>(end) > std::numeric_limits<size_t>::max()) return false;
    bytes.resize(static_cast<size_t>(end));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), end));
}

bool WriteFile(const wchar_t* path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    return file && static_cast<bool>(file.write(reinterpret_cast<const char*>(bytes.data()),
                                                 static_cast<std::streamsize>(bytes.size())));
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::wcerr << L"Usage: compress_game_payload <game.exe> <game.exe.lzms>\n";
        return 2;
    }

    std::vector<uint8_t> input;
    if (!ReadFile(argv[1], input)) {
        std::wcerr << L"Could not read the game executable.\n";
        return 1;
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
        std::cerr << "CreateCompressor failed: " << GetLastError() << "\n";
        return 1;
    }

    SIZE_T capacity = 0;
    const BOOL sized = Compress(compressor, input.data(), input.size(), nullptr, 0, &capacity);
    const DWORD sizeError = sized ? ERROR_SUCCESS : GetLastError();
    if ((sized || sizeError == ERROR_INSUFFICIENT_BUFFER) && capacity > 0) {
        // ERROR_INSUFFICIENT_BUFFER is expected for the size-query call.
    } else {
        CloseCompressor(compressor);
        std::cerr << "Could not size the compressed payload: " << sizeError << "\n";
        return 1;
    }

    const uint64_t originalSize = input.size();
    std::vector<uint8_t> packed;
    packed.reserve(sizeof(kMagic) + sizeof(uint64_t) + capacity);
    packed.insert(packed.end(), std::begin(kMagic), std::end(kMagic));
    AppendLe64(packed, input.size());
    packed.resize(sizeof(kMagic) + sizeof(uint64_t) + capacity);
    SIZE_T compressedSize = 0;
    const BOOL compressed = Compress(compressor, input.data(), input.size(),
                                     packed.data() + sizeof(kMagic) + sizeof(uint64_t),
                                     capacity, &compressedSize);
    const DWORD compressError = compressed ? ERROR_SUCCESS : GetLastError();
    CloseCompressor(compressor);
    if (!compressed || compressedSize == 0) {
        std::cerr << "LZMS compression failed: " << compressError << "\n";
        return 1;
    }
    packed.resize(sizeof(kMagic) + sizeof(uint64_t) + compressedSize);

    std::vector<uint8_t> restored;
    std::string decodeError;
    if (!ttt2::setup::DecompressGamePayload(packed.data(), packed.size(), restored, decodeError) ||
        restored != input) {
        std::cerr << "Compressed payload failed its round-trip verification: "
                  << decodeError << "\n";
        return 1;
    }
    restored.clear();
    input.clear();
    input.shrink_to_fit();

    if (!WriteFile(argv[2], packed)) {
        std::wcerr << L"Could not write the compressed installer resource.\n";
        return 1;
    }

    const double ratio = static_cast<double>(packed.size()) * 100.0 / originalSize;
    std::cout << "LZMS payload: " << packed.size() << " bytes ("
              << ratio << "% of source); round-trip verified.\n";
    return 0;
}
