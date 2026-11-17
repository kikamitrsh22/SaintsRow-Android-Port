// Small portability layer for the runtime core (Windows x64 and Android arm64).
#pragma once

#include <cstdint>
#include <cstdio>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
// Guest timebase (mftb) = host cycle counter.
inline uint64_t HostTimebase() { return __rdtsc(); }
#elif defined(__aarch64__)
inline uint64_t HostTimebase()
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
// Frequency of cntvct_el0 (fixed, usually 19.2 MHz on Snapdragon).
inline uint64_t HostTimebaseFrequency()
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
#else
#error "Unsupported host architecture"
#endif

// Guest timebase (mftb, KeQueryPerformanceFrequency): the console's fixed 49.875 MHz.
constexpr uint64_t kGuestTimebaseFrequency = 49875000;
// Sets the host counter -> guest timebase factor. Call once at start, before guest code runs.
void InitGuestTimebase();

inline int FileSeek64(std::FILE* f, int64_t offset, int origin)
{
#if defined(_WIN32)
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, off_t(offset), origin);
#endif
}
