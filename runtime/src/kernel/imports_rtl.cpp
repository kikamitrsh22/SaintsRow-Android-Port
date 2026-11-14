// Rtl* string/time helpers, sprintf/_vsnprintf, Dbg*, Mm*, Xex*, Hal*, ExGetXConfigSetting.
#include "kernel.h"
#include "../loader.h"
#include "../memory.h"
#include "../xglue/xglue.h"

#include <ctime>
#include <functional>

using namespace kernel;

// ---------------------------------------------------------------------------
// printf-style formatting with guest arguments
// ---------------------------------------------------------------------------

// Formats a guest format string. nextArg() returns the next 64-bit argument slot.
static std::string GuestFormat(uint32_t fmtAddr, const std::function<uint64_t()>& nextArg)
{
    std::string out;
    const char* f = guest::ptr<const char>(fmtAddr);
    while (*f)
    {
        if (*f != '%') { out.push_back(*f++); continue; }
        if (f[1] == '%') { out.push_back('%'); f += 2; continue; }

        // Copy the spec, resolving '*' widths from the argument list.
        std::string spec = "%";
        ++f;
        while (*f && std::strchr("-+ #0", *f)) spec.push_back(*f++);
        if (*f == '*') { spec += std::to_string(int32_t(nextArg())); ++f; }
        while (*f >= '0' && *f <= '9') spec.push_back(*f++);
        if (*f == '.')
        {
            spec.push_back(*f++);
            if (*f == '*') { spec += std::to_string(int32_t(nextArg())); ++f; }
            while (*f >= '0' && *f <= '9') spec.push_back(*f++);
        }
        // Length modifiers
        bool is64 = false, isShort = false, isWide = false;
        for (;;)
        {
            if (*f == 'l') { if (f[1] == 'l') { is64 = true; f += 2; } else { isWide = true; ++f; } }
            else if (*f == 'h') { isShort = true; ++f; }
            else if (*f == 'I' && f[1] == '6' && f[2] == '4') { is64 = true; f += 3; }
            else if (*f == 'I' && f[1] == '3' && f[2] == '2') { f += 3; }
            else if (*f == 'L' || *f == 'z' || *f == 't' || *f == 'j') { ++f; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        char buf[512];
        switch (conv)
        {
        case 'd': case 'i':
        {
            uint64_t v = nextArg();
            int64_t s = is64 ? int64_t(v) : isShort ? int16_t(v) : int32_t(v);
            snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)s);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o':
        {
            uint64_t v = nextArg();
            uint64_t u = is64 ? v : isShort ? uint16_t(v) : uint32_t(v);
            snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)u);
            out += buf;
            break;
        }
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", uint32_t(nextArg()));
            out += buf;
            break;
        case 'c':
            out.push_back(char(nextArg()));
            break;
        case 'C':
            out.push_back(char(nextArg()));
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
        {
            uint64_t bits = nextArg();
            double d;
            std::memcpy(&d, &bits, 8);
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 's': case 'S':
        {
            uint32_t p = uint32_t(nextArg());
            bool wide = (conv == 'S') != isWide; // %S / %ls are wide in MS CRT
            std::string s = p ? (wide ? guest::wstr(p) : guest::cstr(p)) : "(null)";
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            out += (s.size() < sizeof(buf) - 1) ? std::string(buf) : s;
            break;
        }
        case 'n':
            nextArg();
            break;
        default:
            out += spec;
            if (conv) out.push_back(conv);
            break;
        }
    }
    return out;
}

// Argument slot i (0-based over the whole parameter list): r3..r10, then the stack at r1+0x54.
static uint64_t StackArg(PPCContext& ctx, uint32_t index)
{
    switch (index)
    {
    case 0: return ctx.r3.u64; case 1: return ctx.r4.u64; case 2: return ctx.r5.u64; case 3: return ctx.r6.u64;
    case 4: return ctx.r7.u64; case 5: return ctx.r8.u64; case 6: return ctx.r9.u64; case 7: return ctx.r10.u64;
    default: return guest::r64(ctx.r1.u32 + 0x54 + (index - 8) * 8);
    }
}

// sprintf(buffer, format, ...)
GUEST_IMPORT(sprintf)
{
    uint32_t index = 2;
    std::string s = GuestFormat(ctx.r4.u32, [&] { return StackArg(ctx, index++); });
    std::memcpy(guest::ptr(ctx.r3.u32), s.c_str(), s.size() + 1);
    ctx.r3.u64 = uint32_t(s.size());
}

// _vsnprintf(buffer, count, format, va_list) - va_list points at 8-byte argument slots
GUEST_IMPORT(_vsnprintf)
{
    uint32_t buffer = ctx.r3.u32, count = ctx.r4.u32, args = ctx.r6.u32;
    uint32_t index = 0;
    std::string s = GuestFormat(ctx.r5.u32, [&] { return guest::r64(args + 8 * index++); });
    if (s.size() < count)
    {
        std::memcpy(guest::ptr(buffer), s.c_str(), s.size() + 1);
        ctx.r3.u64 = uint32_t(s.size());
    }
    else
    {
        if (count) std::memcpy(guest::ptr(buffer), s.c_str(), count);
        ctx.r3.u64 = uint32_t(-1);
    }
}

GUEST_IMPORT(DbgPrint)
{
    uint32_t index = 1;
    std::string s = GuestFormat(ctx.r3.u32, [&] { return StackArg(ctx, index++); });
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG("DbgPrint: %s", s.c_str());
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(DbgBreakPoint) { LOG("DbgBreakPoint"); }

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// RtlInitAnsiString(ANSI_STRING* dest, const char* src)
GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? uint16_t(std::strlen(guest::ptr<const char>(src))) : 0;
    guest::w16(dst, len);
    guest::w16(dst + 2, src ? len + 1 : 0);
    guest::w32(dst + 4, src);
}

// RtlInitUnicodeString(UNICODE_STRING* dest, const wchar_t* src)
GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t chars = 0;
    if (src) while (guest::r16(src + chars * 2)) ++chars;
    guest::w16(dst, chars * 2);
    guest::w16(dst + 2, src ? (chars + 1) * 2 : 0);
    guest::w32(dst + 4, src);
}

GUEST_IMPORT(RtlFreeAnsiString)
{
    uint32_t buf = guest::r32(ctx.r3.u32 + 4);
    if (buf) mem::g_system.Free(buf);
    guest::w32(ctx.r3.u32 + 4, 0);
    guest::w16(ctx.r3.u32, 0);
    guest::w16(ctx.r3.u32 + 2, 0);
}

// RtlUnicodeStringToAnsiString(ANSI_STRING* dst, UNICODE_STRING* src, BOOLEAN alloc)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t bytes = guest::r16(src);
    uint32_t sbuf = guest::r32(src + 4);
    uint16_t chars = bytes / 2;
    uint32_t dbuf;
    if (ctx.r5.u32 & 0xFF)
    {
        dbuf = mem::SystemAllocZeroed(chars + 1);
        guest::w16(dst + 2, chars + 1);
        guest::w32(dst + 4, dbuf);
    }
    else
    {
        dbuf = guest::r32(dst + 4);
        uint16_t max = guest::r16(dst + 2);
        if (chars >= max) { ctx.r3.u64 = 0x80000005; return; }
    }
    for (uint16_t i = 0; i < chars; i++)
    {
        uint16_t c = guest::r16(sbuf + i * 2);
        guest::w8(dbuf + i, c < 0x100 ? uint8_t(c) : '?');
    }
    guest::w8(dbuf + chars, 0);
    guest::w16(dst, chars);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// RtlMultiByteToUnicodeN(dst, maxBytes, pResultBytes, src, srcBytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t dst = ctx.r3.u32, max = ctx.r4.u32, res = ctx.r5.u32, src = ctx.r6.u32, n = ctx.r7.u32;
    uint32_t count = std::min(n, max / 2);
    for (uint32_t i = 0; i < count; i++) guest::w16(dst + i * 2, guest::r8(src + i));
    if (res) guest::w32(res, count * 2);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// RtlUnicodeToMultiByteN(dst, maxBytes, pResultBytes, src, srcBytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t dst = ctx.r3.u32, max = ctx.r4.u32, res = ctx.r5.u32, src = ctx.r6.u32, n = ctx.r7.u32;
    uint32_t count = std::min(n / 2, max);
    for (uint32_t i = 0; i < count; i++)
    {
        uint16_t c = guest::r16(src + i * 2);
        guest::w8(dst + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (res) guest::w32(res, count);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

// RtlCompareMemoryUlong(src, length, pattern) -> number of matching bytes
GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, len = ctx.r4.u32, pat = ctx.r5.u32;
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4)
        if (guest::r32(src + i) != pat) break;
    ctx.r3.u64 = i;
}

// RtlFillMemoryUlong(dst, length, pattern)
GUEST_IMPORT(RtlFillMemoryUlong)
{
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32, pat = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= len; i += 4) guest::w32(dst + i, pat);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (u16 each)
GUEST_IMPORT(RtlTimeToTimeFields)
{
    uint64_t t = guest::r64(ctx.r3.u32);
    uint32_t out = ctx.r4.u32;
    constexpr uint64_t kEpochDiff = 116444736000000000ull;
    time_t secs = time_t((t - kEpochDiff) / 10000000ull);
    uint32_t ms = uint32_t((t / 10000ull) % 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    guest::w16(out + 0, uint16_t(tm.tm_year + 1900));
    guest::w16(out + 2, uint16_t(tm.tm_mon + 1));
    guest::w16(out + 4, uint16_t(tm.tm_mday));
    guest::w16(out + 6, uint16_t(tm.tm_hour));
    guest::w16(out + 8, uint16_t(tm.tm_min));
    guest::w16(out + 10, uint16_t(tm.tm_sec));
    guest::w16(out + 12, uint16_t(ms));
    guest::w16(out + 14, uint16_t(tm.tm_wday));
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t in = ctx.r3.u32;
    std::tm tm{};
    tm.tm_year = guest::r16(in + 0) - 1900;
    tm.tm_mon = guest::r16(in + 2) - 1;
    tm.tm_mday = guest::r16(in + 4);
    tm.tm_hour = guest::r16(in + 6);
    tm.tm_min = guest::r16(in + 8);
    tm.tm_sec = guest::r16(in + 10);
#ifdef _WIN32
    time_t secs = _mkgmtime(&tm);
#else
    time_t secs = timegm(&tm);
#endif
    constexpr uint64_t kEpochDiff = 116444736000000000ull;
    uint64_t t = kEpochDiff + uint64_t(secs) * 10000000ull + uint64_t(guest::r16(in + 12)) * 10000ull;
    guest::w64(ctx.r4.u32, t);
    ctx.r3.u64 = 1;
}

// ---------------------------------------------------------------------------
// Exceptions / misc Rtl
// ---------------------------------------------------------------------------

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = ctx.r3.u32;
    uint32_t r;
    switch (s)
    {
    case STATUS_SUCCESS: r = 0; break;
    case STATUS_OBJECT_NAME_NOT_FOUND: r = 2; break;
    case STATUS_OBJECT_PATH_NOT_FOUND: r = 3; break;
    case STATUS_ACCESS_DENIED: r = 5; break;
    case STATUS_INVALID_HANDLE: r = 6; break;
    case STATUS_NO_MEMORY: r = 8; break;
    case STATUS_END_OF_FILE: r = 38; break;
    case STATUS_NO_MORE_FILES: r = 18; break;
    case STATUS_OBJECT_NAME_COLLISION: r = 183; break;
    case STATUS_PENDING: r = 997; break;
    case STATUS_TIMEOUT: r = 1460; break;
    default: r = (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317; break;
    }
    ctx.r3.u64 = r;
}

// RtlRaiseException(EXCEPTION_RECORD*)
GUEST_IMPORT(RtlRaiseException)
{
    uint32_t rec = ctx.r3.u32;
    uint32_t code = guest::r32(rec);
    if (code == 0x406D1388) // MSVC "set thread name"
    {
        uint32_t info = rec + 0x18; // ExceptionInformation[]
        uint32_t namePtr = guest::r32(info + 4);
        LOG("thread name: %s", guest::cstr(namePtr).c_str());
        return;
    }
    LOG("RtlRaiseException(code=%08X, flags=%08X, address=%08X)", code, guest::r32(rec + 4), guest::r32(rec + 0xC));
}

GUEST_IMPORT(RtlCaptureContext) { LOG_STUB_ONCE("RtlCaptureContext"); }
GUEST_IMPORT(RtlUnwind) { LOG("RtlUnwind(%08X, %08X)", ctx.r3.u32, ctx.r4.u32); }
GUEST_IMPORT(__C_specific_handler) { LOG_STUB_ONCE("__C_specific_handler"); ctx.r3.u64 = 1; }

// RtlImageXexHeaderField(xexHeaderBase, key)
GUEST_IMPORT(RtlImageXexHeaderField)
{
    ctx.r3.u64 = loader::XexHeaderField(ctx.r3.u32, ctx.r4.u32);
}

// ---------------------------------------------------------------------------
// Mm*
// ---------------------------------------------------------------------------

// MmAllocatePhysicalMemoryEx(flags, size, protect, minAddress, maxAddress, alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t a = xglue::MmAllocatePhysicalMemoryEx(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    if (!a) LOG("MmAllocatePhysicalMemoryEx(size=%X protect=%X) failed", ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = a;
}

// MmFreePhysicalMemory(type, address)
GUEST_IMPORT(MmFreePhysicalMemory) { xglue::MmFreePhysicalMemory(ctx.r4.u32); }

GUEST_IMPORT(MmGetPhysicalAddress) { ctx.r3.u64 = xglue::GetPhysicalAddress(ctx.r3.u32); }
GUEST_IMPORT(MmSetAddressProtect) { xglue::MmSetAddressProtect(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }

// MmQueryStatistics(MM_STATISTICS*)
GUEST_IMPORT(MmQueryStatistics)
{
    uint32_t s = ctx.r3.u32;
    uint32_t size = guest::r32(s);
    std::memset(guest::ptr(s + 4), 0, size > 4 ? size - 4 : 0);
    guest::w32(s + 4, 0x20000);  // TotalPhysicalPages (512 MB / 4K)
    guest::w32(s + 8, 0x1000);   // KernelPages
    guest::w32(s + 12, 0x8000);  // TitleAvailablePages
    guest::w32(s + 16, 0x1C000000); // TitleTotalVirtualMemoryBytes
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Xex*, Hal*, Ex*
// ---------------------------------------------------------------------------

GUEST_IMPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

// XexGetModuleHandle(name, pHandle)
GUEST_IMPORT(XexGetModuleHandle)
{
    std::string name = guest::cstr(ctx.r3.u32);
    if (!ctx.r3.u32 || name == "default.xex")
    {
        guest::w32(ctx.r4.u32, loader::g_title.moduleEntry);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    LOG("XexGetModuleHandle(%s) - not found", name.c_str());
    guest::w32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0xC0000225; // STATUS_NOT_FOUND
}

// XexGetProcedureAddress(module, ordinal, pAddress)
GUEST_IMPORT(XexGetProcedureAddress)
{
    LOG("XexGetProcedureAddress(%08X, %u)", ctx.r3.u32, ctx.r4.u32);
    guest::w32(ctx.r5.u32, 0);
    ctx.r3.u64 = 0xC0000225;
}

GUEST_IMPORT(HalReturnToFirmware)
{
    LOG("HalReturnToFirmware(%u) - exiting", ctx.r3.u32);
    std::exit(0);
}

// ExGetXConfigSetting(category, setting, buffer, bufferSize, pRequiredSize)
GUEST_IMPORT(ExGetXConfigSetting)
{
    uint16_t cat = uint16_t(ctx.r3.u32), setting = uint16_t(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, pReq = ctx.r7.u32;
    uint32_t value = 0, valueSize = 4;
    bool known = true;
    if (cat == 0x0003) // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x0009: value = 1; break;          // language: English
        case 0x000A: value = 0x00040000; break; // video flags: widescreen
        case 0x000C: value = 0; break;          // retail flags
        case 0x0011: value = 0x00FF; valueSize = 4; break; // country
        default: known = false; break;
        }
    }
    else if (cat == 0x0002 && setting == 0x0002) // XCONFIG_SECURED: AV region
    {
        value = 0x00001000; // NTSC-U
    }
    else
    {
        known = false;
    }
    if (!known) LOG("ExGetXConfigSetting(%04X, %04X) -> zeros", cat, setting);
    if (buffer && size)
    {
        std::memset(guest::ptr(buffer), 0, size);
        if (size >= 4) guest::w32(buffer, value);
        else if (size >= 2) guest::w16(buffer, uint16_t(value));
        else guest::w8(buffer, uint8_t(value));
    }
    if (pReq) guest::w16(pReq, uint16_t(valueSize));
    ctx.r3.u64 = STATUS_SUCCESS;
}
