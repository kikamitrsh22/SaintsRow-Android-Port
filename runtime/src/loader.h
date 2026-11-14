#pragma once

#include <cstdint>
#include <filesystem>

namespace loader
{
    struct LoadedTitle
    {
        uint32_t entryPoint = 0;
        uint32_t stackSize = 0x40000;
        uint32_t xexHeader = 0;    // copy of the XEX header in guest memory
        uint32_t moduleEntry = 0;  // fake LDR_DATA_TABLE_ENTRY
    };

    extern LoadedTitle g_title;

    // Loads default.xex into guest memory, fills the function lookup table and import variables.
    bool Load(const std::filesystem::path& xexPath);

    // Looks up an optional header in the guest copy of the XEX header (RtlImageXexHeaderField semantics).
    uint32_t XexHeaderField(uint32_t headerBase, uint32_t key);
}
