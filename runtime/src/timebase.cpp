// Guest timebase scaling for PPC_QUERY_TIMEBASE() (ppc_indirect.h): host counter ticks -> 49.875 MHz.
#include "platform.h"
#include "guest.h"

#include <chrono>
#include <thread>

// 32.32 fixed point: guest ticks per host tick. 1.0 until InitGuestTimebase runs.
extern "C" uint64_t g_guestTimebaseMul = uint64_t(1) << 32;

void InitGuestTimebase()
{
#if defined(__aarch64__)
    const double hostFreq = double(HostTimebaseFrequency());
#else
    // TSC frequency, measured over 100 ms.
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    uint64_t c0 = HostTimebase();
    std::this_thread::sleep_for(milliseconds(100));
    uint64_t c1 = HostTimebase();
    const double hostFreq = double(c1 - c0) / duration<double>(steady_clock::now() - t0).count();
#endif
    g_guestTimebaseMul = uint64_t(double(kGuestTimebaseFrequency) / hostFreq * 4294967296.0);
    LOG("timebase: host %.3f MHz -> guest %.3f MHz", hostFreq / 1e6, kGuestTimebaseFrequency / 1e6);
}
