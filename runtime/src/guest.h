// Shared helpers for the Saints Row runtime: guest memory access, logging, import macros.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <ppc_context.h>

// ---------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------

namespace guest
{
    extern uint8_t* g_base; // host pointer to guest address 0

    inline uint8_t* ptr(uint32_t addr) { return g_base + addr; }
    template <typename T> inline T* ptr(uint32_t addr) { return reinterpret_cast<T*>(g_base + addr); }
    inline uint32_t addr(const void* host) { return uint32_t(static_cast<const uint8_t*>(host) - g_base); }

    inline uint8_t  r8 (uint32_t a) { return g_base[a]; }
    inline uint16_t r16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
    inline uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
    inline uint64_t r64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
    inline void w8 (uint32_t a, uint8_t v)  { g_base[a] = v; }
    inline void w16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
    inline void w32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
    inline void w64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

    inline std::string cstr(uint32_t a) { return a ? std::string(reinterpret_cast<const char*>(g_base + a)) : std::string(); }

    // ANSI_STRING { u16 Length; u16 MaximumLength; u32 Buffer; }
    inline std::string ansi(uint32_t strAddr)
    {
        if (!strAddr) return {};
        uint16_t len = r16(strAddr);
        uint32_t buf = r32(strAddr + 4);
        return buf ? std::string(reinterpret_cast<const char*>(g_base + buf), len) : std::string();
    }

    // Big-endian UTF-16 string -> UTF-8 (ASCII-safe; enough for logging and paths)
    inline std::string wstr(uint32_t a)
    {
        std::string out;
        if (!a) return out;
        for (;; a += 2)
        {
            uint16_t c = r16(a);
            if (!c) break;
            out.push_back(c < 0x80 ? char(c) : '?');
        }
        return out;
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

namespace rt
{
    uint32_t CurrentThreadId();
    void Log(const char* fmt, ...);
}

#define LOG(...) ::rt::Log(__VA_ARGS__)

// Log an unimplemented import once per name.
#define LOG_STUB_ONCE(name)                                       \
    do {                                                          \
        static std::atomic<bool> s_logged{ false };               \
        if (!s_logged.exchange(true)) LOG("STUB %s", name);       \
    } while (0)

// ---------------------------------------------------------------------------
// Import implementation macros
// ---------------------------------------------------------------------------

// Defines the host implementation of a guest import (strong symbol, overrides the weak stub).
#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)

// Stack-passed argument N (0 = the 9th parameter). Each slot is 8 bytes at r1+0x54; we want the low word.
inline uint32_t StackArg32(PPCContext& ctx, uint32_t slot)
{
    return guest::r32(ctx.r1.u32 + 0x54 + slot * 8 + 4);
}

// Calls a guest function by address on the given context.
inline void GuestCall(PPCContext& ctx, uint32_t address)
{
    PPCFunc* fn = PPC_LOOKUP_FUNC(guest::g_base, address);
    if (!fn)
    {
        rt::Log("GuestCall: no recompiled function at %08X", address);
        std::abort();
    }
    fn(ctx, guest::g_base);
}

// NTSTATUS values
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_WAIT_0 = 0x00000000;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t STATUS_USER_APC = 0x000000C0;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_PENDING = 0x00000103;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;

// Win32 error codes used by XAM
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_NO_SUCH_USER = 1317;
constexpr uint32_t ERROR_FUNCTION_FAILED = 1627;
constexpr uint32_t ERROR_CANCELLED = 1223;
