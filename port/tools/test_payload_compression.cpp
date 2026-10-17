#include "setup/payload_compression.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

static bool ReadFile(const char* path, std::vector<uint8_t>& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "Usage: ttt2_test_payload_compression <game.exe> <game.exe.lzms>\n";
        return 2;
    }

    std::vector<uint8_t> source, packed, restored;
    if (!ReadFile(argv[1], source) || !ReadFile(argv[2], packed)) {
        std::cerr << "Could not read the original or packed runtime.\n";
        return 1;
    }

    std::string error;
    if (!ttt2::setup::DecompressGamePayload(packed.data(), packed.size(), restored, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (restored != source) {
        std::cerr << "Unpacked runtime differs from the built game executable.\n";
        return 1;
    }
    std::cout << "LZMS installer payload round-trip matches the built executable.\n";
    return 0;
}
