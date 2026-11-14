#include "loader.h"
#include "guest.h"
#include "memory.h"
#include "kernel/kernel.h"
#include "xglue/xglue.h"

#include <fstream>
#include <vector>

#include <image.h>
#include <xex.h>

namespace loader
{
    LoadedTitle g_title;

    static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

    // Returns the raw value of an optional header (offset or inline value), or 0.
    static bool FindOptHeader(const uint8_t* xex, uint32_t key, uint32_t& value)
    {
        uint32_t count = be32(xex + 20);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* e = xex + 24 + i * 8;
            if (be32(e) == key)
            {
                value = be32(e + 4);
                return true;
            }
        }
        return false;
    }

    uint32_t XexHeaderField(uint32_t headerBase, uint32_t key)
    {
        uint32_t count = guest::r32(headerBase + 20);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t e = headerBase + 24 + i * 8;
            if (guest::r32(e) == key)
            {
                uint32_t v = guest::r32(e + 4);
                switch (key & 0xFF)
                {
                case 0x00: return v;          // value stored inline
                case 0x01: return e + 4;      // address of the inline value
                default:   return headerBase + v; // offset to data
                }
            }
        }
        return 0;
    }

    // Import variables (xboxkrnl.exe) - IAT slot addresses come from config/imports.tsv.
    struct VarImport { const char* name; uint32_t slot; };
    static constexpr VarImport kVarImports[] = {
        { "VdHSIOCalibrationLock", 0x8200059C },
        { "VdGpuClockInMHz", 0x820005F8 },
        { "XboxKrnlVersion", 0x82000624 },
        { "KeCertMonitorData", 0x82000638 },
        { "KeDebugMonitorData", 0x8200063C },
        { "VdGlobalDevice", 0x82000658 },
        { "ExThreadObjectType", 0x820006A4 },
        { "KeTimeStampBundle", 0x82000720 },
        { "ExLoadedCommandLine", 0x82000724 },
        { "XexExecutableModuleHandle", 0x820007AC },
    };

    static uint32_t MakeVariable(const char* name)
    {
        std::string n = name;
        if (n == "XboxKrnlVersion")
        {
            uint32_t a = mem::SystemAllocZeroed(8);
            guest::w16(a + 0, 2);     // major
            guest::w16(a + 2, 0);     // minor
            guest::w16(a + 4, 17559); // build
            guest::w16(a + 6, 0);     // qfe
            return a;
        }
        if (n == "VdGpuClockInMHz")
        {
            uint32_t a = mem::SystemAllocZeroed(4);
            guest::w32(a, 500);
            return a;
        }
        if (n == "VdHSIOCalibrationLock")
        {
            uint32_t a = mem::SystemAllocZeroed(0x1C);
            guest::w8(a, 1);                 // header type
            guest::w32(a + 0x10, 0xFFFFFFFF); // LockCount = -1
            return a;
        }
        if (n == "KeTimeStampBundle")
        {
            uint32_t a = mem::SystemAllocZeroed(0x18);
            kernel::StartTimeStampThread(a);
            return a;
        }
        if (n == "ExLoadedCommandLine")
        {
            uint32_t a = mem::SystemAllocZeroed(1024);
            std::strcpy(guest::ptr<char>(a), "\"default.xex\"");
            return a;
        }
        if (n == "XexExecutableModuleHandle")
        {
            // The variable holds a pointer to the module (LDR_DATA_TABLE_ENTRY).
            uint32_t a = mem::SystemAllocZeroed(4);
            guest::w32(a, g_title.moduleEntry);
            return a;
        }
        // KeDebugMonitorData, KeCertMonitorData, VdGlobalDevice, ExThreadObjectType: zeroed blocks
        return mem::SystemAllocZeroed(0x100);
    }

    bool Load(const std::filesystem::path& xexPath)
    {
        std::ifstream in(xexPath, std::ios::binary);
        if (!in)
        {
            LOG("cannot open %s", xexPath.string().c_str());
            return false;
        }
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (file.size() < 24 || std::memcmp(file.data(), "XEX2", 4) != 0)
        {
            LOG("not an XEX2 file");
            return false;
        }

        // Keep a pristine copy of the header (Xex2LoadImage byte-swaps parts of the image, not the header).
        uint32_t headerSize = be32(file.data() + 8);
        g_title.xexHeader = mem::SystemAllocZeroed(headerSize);
        std::memcpy(guest::ptr(g_title.xexHeader), file.data(), headerSize);

        Image image = Xex2LoadImage(file.data(), file.size());
        if (!image.data || image.base != PPC_IMAGE_BASE)
        {
            LOG("failed to load XEX image");
            return false;
        }
        // Image plus the lookup table that follows it (8 bytes per possible code address).
        uint32_t tableEnd = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2);
        if (!xglue::AllocFixed(uint32_t(image.base), tableEnd - uint32_t(image.base)))
        {
            LOG("failed to map the image range %08X-%08X", uint32_t(image.base), tableEnd);
            return false;
        }
        std::memcpy(guest::ptr(uint32_t(image.base)), image.data.get(), image.size);
        g_title.entryPoint = uint32_t(image.entry_point);
        LOG("image: base=%08X size=%X entry=%08X", uint32_t(image.base), image.size, g_title.entryPoint);

        // Function lookup table for indirect calls.
        size_t count = 0;
        for (auto* m = PPCFuncMappings; m->host; ++m, ++count)
            PPC_LOOKUP_FUNC(guest::g_base, m->guest) = m->host;
        LOG("registered %zu recompiled functions", count);

        // Optional headers we care about.
        uint32_t v;
        if (FindOptHeader(file.data(), 0x00020200, v)) // default stack size (inline)
            g_title.stackSize = v;
        if (FindOptHeader(file.data(), 0x00020104, v)) // TLS info
        {
            const uint8_t* p = file.data() + v;
            kernel::g_tls.slotCount = be32(p + 0);
            kernel::g_tls.rawDataAddress = be32(p + 4);
            kernel::g_tls.dataSize = be32(p + 8);
            kernel::g_tls.rawDataSize = be32(p + 12);
            LOG("TLS: slots=%u data=%08X size=%X raw=%X", kernel::g_tls.slotCount, kernel::g_tls.rawDataAddress,
                kernel::g_tls.dataSize, kernel::g_tls.rawDataSize);
        }

        // Fake LDR_DATA_TABLE_ENTRY: DllBase @0x18, EntryPoint @0x1C, SizeOfImage @0x20, XexHeaderBase @0x58
        g_title.moduleEntry = mem::SystemAllocZeroed(0x80);
        guest::w32(g_title.moduleEntry + 0x18, uint32_t(image.base));
        guest::w32(g_title.moduleEntry + 0x1C, g_title.entryPoint);
        guest::w32(g_title.moduleEntry + 0x20, image.size);
        guest::w32(g_title.moduleEntry + 0x58, g_title.xexHeader);

        // Import variables: each IAT slot receives the address of the variable.
        for (const auto& imp : kVarImports)
            guest::w32(imp.slot, MakeVariable(imp.name));

        return true;
    }
}
