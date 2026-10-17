#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "profiles.h"
#include "save_store.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace profiles {
uint64_t ActiveXuid() { return 0xB13EBABEBABEBABEull; }
}

uint32_t FlushSaveFiles(const std::filesystem::path&, bool) { return ERROR_SUCCESS; }

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("TTT2SaveStoreTest-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    assert(_wputenv_s(L"TTT2_SAVE_ROOT", root.c_str()) == 0);

    SaveContentRecord record{};
    record[3] = 1; // metadata version (big-endian)
    record[7] = 1; // record version (big-endian)
    constexpr char name[] = "tag25save";
    std::memcpy(record.data() + 264, name, sizeof(name));

    uint32_t disposition = 0;
    assert(SaveCreateContent("tag25save", record, 1, disposition) == ERROR_SUCCESS);
    assert(disposition == 1);

    std::filesystem::path host, container;
    uint32_t resolveError = 0;
    assert(ResolveSaveFile("tag25save:", host, container, resolveError));
    assert(resolveError == ERROR_SUCCESS);
    assert(host == container);
    assert(std::filesystem::is_directory(container));

    std::vector<SaveContentRecord> records;
    assert(SaveListContent(records) == ERROR_SUCCESS);
    assert(records.size() == 1 && records.front() == record);

    assert(SaveCloseContent("tag25save", true) == ERROR_SUCCESS);
    assert(SaveCreateContent("tag25save", record, 3, disposition) == ERROR_SUCCESS);
    assert(disposition == 2);
    assert(SaveCloseContent("tag25save", true) == ERROR_SUCCESS);
    assert(SaveDeleteContent(record) == ERROR_SUCCESS);
    assert(SaveListContent(records) == ERROR_SUCCESS && records.empty());

    std::filesystem::remove_all(root, ec);
    assert(!ec);
    std::cout << "save-store create, mount, enumerate, reopen, and delete passed\n";
    return 0;
}
