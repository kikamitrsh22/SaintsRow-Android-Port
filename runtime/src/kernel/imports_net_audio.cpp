// Networking (always offline) and audio (silent for now).
#include "../audio.h"
#include <atomic>
#include "kernel.h"
#include "../memory.h"

using namespace kernel;

constexpr uint32_t WSAENETDOWN = 10050;
constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;
static thread_local uint32_t t_wsaError = 0;

static void NetFail(PPCContext& ctx)
{
    t_wsaError = WSAENETDOWN;
    ctx.r3.u64 = SOCKET_ERROR;
}

GUEST_IMPORT(NetDll_WSAStartup)
{
    // (caller, version, WSADATA*)
    if (ctx.r5.u32) std::memset(guest::ptr(ctx.r5.u32), 0, 0x190);
    ctx.r3.u64 = 0;
}
GUEST_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = t_wsaError; }
GUEST_IMPORT(NetDll_WSASetLastError) { t_wsaError = ctx.r4.u32; }
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; } // cable unplugged
// XNetGetTitleXnAddr(caller, XNADDR*): loopback address with a fixed fake MAC
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    uint32_t a = ctx.r4.u32;
    if (a)
    {
        std::memset(guest::ptr(a), 0, 0x24);
        guest::w32(a + 0, 0x7F000001); // ina = 127.0.0.1
        static const uint8_t kMac[6] = { 0x00, 0x22, 0x48, 0x5A, 0x31, 0x07 };
        std::memcpy(guest::ptr(a + 10), kMac, 6); // abEnet
    }
    ctx.r3.u64 = 0x00000004; // XNET_GET_XNADDR_STATIC
}
GUEST_IMPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetRandom)
{
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; i++) guest::w8(ctx.r4.u32 + i, uint8_t(std::rand()));
    ctx.r3.u64 = 0;
}
GUEST_IMPORT(NetDll_WSACreateEvent)
{
    ctx.r3.u64 = CreateHandle(std::make_shared<Event>(true, false));
}
GUEST_IMPORT(NetDll_WSAWaitForMultipleEvents) { ctx.r3.u64 = STATUS_TIMEOUT; }
GUEST_IMPORT(NetDll___WSAFDIsSet) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_inet_addr) { ctx.r3.u64 = 0xFFFFFFFF; }

// ---------------------------------------------------------------------------
// "Silent" sockets: creation and configuration succeed, sends are swallowed, receives never
// have data. Libraries like DemonWare keep working without a real network.
// ---------------------------------------------------------------------------

constexpr uint32_t WSAEWOULDBLOCK = 10035;
static std::atomic<uint32_t> s_nextSocket{ 0x100 };

// (caller, af, type, protocol)
GUEST_IMPORT(NetDll_socket) { ctx.r3.u64 = s_nextSocket += 4; }
GUEST_IMPORT(NetDll_closesocket) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_bind) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_connect) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_listen) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_setsockopt) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_ioctlsocket) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_shutdown) { ctx.r3.u64 = 0; }

// (caller, socket, level, optname, optval, optlen)
GUEST_IMPORT(NetDll_getsockopt)
{
    uint32_t val = ctx.r7.u32, len = ctx.r8.u32;
    if (val && len) std::memset(guest::ptr(val), 0, guest::r32(len));
    ctx.r3.u64 = 0;
}

// send(caller, socket, buf, len, flags) / sendto(..., to, tolen): pretend everything was sent
GUEST_IMPORT(NetDll_send) { ctx.r3.u64 = ctx.r6.u32; }
GUEST_IMPORT(NetDll_sendto) { ctx.r3.u64 = ctx.r6.u32; }

static void NoData(PPCContext& ctx)
{
    t_wsaError = WSAEWOULDBLOCK;
    ctx.r3.u64 = SOCKET_ERROR;
}
GUEST_IMPORT(NetDll_recv) { NoData(ctx); }
GUEST_IMPORT(NetDll_recvfrom) { NoData(ctx); }

// select(caller, nfds, readfds, writefds, exceptfds, timeout): nothing readable; writes always ready
GUEST_IMPORT(NetDll_select)
{
    // fd_set: u32 count, u32 sockets[64]
    uint32_t readfds = ctx.r5.u32, writefds = ctx.r6.u32, exceptfds = ctx.r7.u32;
    uint32_t ready = writefds ? guest::r32(writefds) : 0;
    if (readfds) guest::w32(readfds, 0);
    if (exceptfds) guest::w32(exceptfds, 0);
    ctx.r3.u64 = ready;
}

// Everything that needs Xbox Live fails with WSAENETDOWN.
#define NET_FAIL(name) GUEST_IMPORT(name) { NetFail(ctx); }
NET_FAIL(NetDll_XNetConnect)
NET_FAIL(NetDll_XNetCreateKey)
NET_FAIL(NetDll_XNetRegisterKey)
NET_FAIL(NetDll_XNetUnregisterKey)
NET_FAIL(NetDll_XNetUnregisterInAddr)
NET_FAIL(NetDll_XNetXnAddrToInAddr)
NET_FAIL(NetDll_XNetInAddrToXnAddr)
NET_FAIL(NetDll_XNetServerToInAddr)
NET_FAIL(NetDll_XNetInAddrToString)
NET_FAIL(NetDll_XNetQosListen)
NET_FAIL(NetDll_XNetQosLookup)
NET_FAIL(NetDll_XNetQosRelease)
NET_FAIL(NetDll_XNetQosServiceLookup)

// ---------------------------------------------------------------------------
// Audio (audio.cpp): the render client is called back whenever the output device has room;
// driver handles are 0x4155xxxx with the client index in the low half, as on the console.
// ---------------------------------------------------------------------------

// XAudioRegisterRenderDriverClient(pCallback, pDriver)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t cb = guest::r32(ctx.r3.u32), arg = guest::r32(ctx.r3.u32 + 4);
    int index = cb ? audio::RegisterClient(cb, arg) : -1;
    LOG("XAudioRegisterRenderDriverClient(callback=%08X, arg=%08X) -> client %d%s", cb, arg, index,
        index < 0 ? " (silent)" : "");
    // Without an output device the game still gets a handle and simply never gets called back.
    guest::w32(ctx.r4.u32, 0x41550000 | uint32_t(index < 0 ? 0xFFFF : index));
    ctx.r3.u64 = 0;
}
GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    if (index != 0xFFFF) audio::UnregisterClient(int(index));
    ctx.r3.u64 = 0;
}
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    if (index != 0xFFFF) audio::SubmitFrame(int(index), ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolume(category, pVolume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    float one = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &one, 4);
    guest::w32(ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}
GUEST_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) { guest::w32(ctx.r4.u32, 0); ctx.r3.u64 = 0; }

// Hardware XMA contexts come from the decoder (audio.cpp); without one, a plain zeroed block so
// the game keeps running silently.
GUEST_IMPORT(XMACreateContext)
{
    uint32_t c = audio::CreateXmaContext();
    bool hardware = c != 0;
    if (!c) c = mem::SystemAllocZeroed(0x40);
    guest::w32(ctx.r3.u32, c);
    static std::atomic<int> s_logged{ 0 };
    if (s_logged.fetch_add(1) < 4) LOG("XMACreateContext -> %08X%s", c, hardware ? "" : " (no decoder)");
    ctx.r3.u64 = c ? 0 : 0xC0000017; // STATUS_NO_MEMORY
}
GUEST_IMPORT(XMAReleaseContext)
{
    uint32_t c = ctx.r3.u32;
    if (c >= 0xA0000000) audio::ReleaseXmaContext(c); // decoder contexts are physical memory
    else if (c) mem::g_system.Free(c);
    ctx.r3.u64 = 0;
}
