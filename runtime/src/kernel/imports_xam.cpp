// XAM imports: memory, loader, user/profile, content (saves), notifications, UI, input.
#include "kernel.h"
#include <chrono>
#include <thread>
#include "../loader.h"
#include "../memory.h"
#include "../input.h"
#include "../vfs.h"

#include <set>
#include <tuple>
#include <unordered_map>

using namespace kernel;

constexpr uint32_t kTitleId = 0x545107D1;

// Logs the first occurrence of each (what, a, b) combination - cheap tracing for hot XAM calls.
static void TraceOnce(const char* what, uint32_t a, uint32_t b = 0, uint32_t result = 0)
{
    static std::mutex s_lock;
    static std::set<std::tuple<std::string, uint32_t, uint32_t, uint32_t>> s_seen;
    std::lock_guard lock(s_lock);
    if (s_seen.emplace(what, a, b, result).second)
        LOG("trace %s(%08X, %08X) -> %08X (lr=%08X)", what, a, b, result, kernel::CurrentThread() && kernel::CurrentThread()->ctx ? uint32_t(kernel::CurrentThread()->ctx->lr) : 0);
}
constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;

// XAM_OVERLAPPED: +0 Result, +4 Length, +8 Context, +0xC hEvent, +0x10 CompletionRoutine,
// +0x14 CompletionContext, +0x18 ExtendedError
static uint32_t CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length = 0, uint32_t extended = 0)
{
    if (!ov) return result;
    guest::w32(ov + 0x00, result);
    guest::w32(ov + 0x04, length);
    guest::w32(ov + 0x18, extended ? extended : (result ? 0x80070000 | result : 0));
    if (uint32_t ev = guest::r32(ov + 0x0C))
    {
        if (auto e = GetHandleAs<Event>(ev)) SetEvent(*e);
    }
    if (guest::r32(ov + 0x10)) LOG("XAM overlapped completion routine not supported");
    return ERROR_IO_PENDING;
}

// ---------------------------------------------------------------------------
// Memory / loader / system
// ---------------------------------------------------------------------------

GUEST_IMPORT(XamAlloc)
{
    uint32_t a = mem::SystemAllocZeroed(ctx.r4.u32);
    guest::w32(ctx.r5.u32, a);
    ctx.r3.u64 = a ? ERROR_SUCCESS : 0x8007000E;
}

GUEST_IMPORT(XamFree) { mem::g_system.Free(ctx.r3.u32); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamGetSystemVersion) { ctx.r3.u64 = 0x20096B00; } // 2.0.17559.0

GUEST_IMPORT(XamGetExecutionId)
{
    static uint32_t s_id = [] {
        uint32_t a = mem::SystemAllocZeroed(0x18);
        guest::w32(a + 0x00, 0x24367742); // media id
        guest::w32(a + 0x04, 1);          // version
        guest::w32(a + 0x08, 1);          // base version
        guest::w32(a + 0x0C, kTitleId);
        guest::w8(a + 0x10, 2);           // platform
        guest::w8(a + 0x11, 1);           // executable type
        guest::w8(a + 0x12, 1);           // disc number
        guest::w8(a + 0x13, 1);           // disc count
        return a;
    }();
    guest::w32(ctx.r3.u32, s_id);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(XamLoaderGetLaunchDataSize) { guest::w32(ctx.r3.u32, 0); ctx.r3.u64 = 0x80070490; } // not found
GUEST_IMPORT(XamLoaderGetLaunchData) { ctx.r3.u64 = 0x80070490; }
GUEST_IMPORT(XamLoaderSetLaunchData) { ctx.r3.u64 = 0; }
// Saints Reborn: never let the game terminate itself (it does so on some error paths) - park the
// calling thread instead so the rest of the game keeps running.
GUEST_IMPORT(XamLoaderTerminateTitle)
{
    LOG("XamLoaderTerminateTitle - parking the calling thread");
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
GUEST_IMPORT(XamLoaderLaunchTitle) { LOG("XamLoaderLaunchTitle(%s) - exiting", guest::cstr(ctx.r3.u32).c_str()); std::exit(0); }

GUEST_IMPORT(XGetAVPack) { ctx.r3.u64 = 8; }        // HDMI
GUEST_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x00FF; } // North America
GUEST_IMPORT(XGetLanguage) { ctx.r3.u64 = 1; }        // English

GUEST_IMPORT(XGetVideoMode)
{
    uint32_t m = ctx.r3.u32;
    std::memset(guest::ptr(m), 0, 0x30);
    guest::w32(m + 0x00, 1280);
    guest::w32(m + 0x04, 720);
    guest::w32(m + 0x0C, 1);
    guest::w32(m + 0x10, 1);
    float hz = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &hz, 4);
    guest::w32(m + 0x14, bits);
    guest::w32(m + 0x18, 1);
    guest::w32(m + 0x1C, 0x8A);
    guest::w32(m + 0x20, 1);
}

GUEST_IMPORT(XNetLogonGetTitleID) { ctx.r3.u64 = kTitleId; }
GUEST_IMPORT(XCustomSetAction) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XCustomGetLastActionPress) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XNotifyPositionUI) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

struct NotifyListener : Event
{
    std::vector<std::pair<uint32_t, uint32_t>> queue; // (id, param)
    NotifyListener() : Event(true, false) {}
};

static std::shared_ptr<NotifyListener> s_listener;

GUEST_IMPORT(XamNotifyCreateListener)
{
    LOG("XamNotifyCreateListener(mask %016llX)", (unsigned long long)ctx.r3.u64);
    auto l = std::make_shared<NotifyListener>();
    // Tell the game a user is signed in and storage is present, like the dashboard does at boot.
    l->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED (user 0 bit)
    l->queue.push_back({ 0x0000000B, 0 }); // XN_SYS_STORAGEDEVICESCHANGED
    l->signaled = true;
    s_listener = l;
    ctx.r3.u64 = CreateHandle(l);
}

// XNotifyGetNext(handle, matchId, pId, pParam)
GUEST_IMPORT(XNotifyGetNext)
{
    auto l = GetHandleAs<NotifyListener>(ctx.r3.u32);
    if (!l) { ctx.r3.u64 = 0; return; }
    std::lock_guard lock(g_lock);
    uint32_t match = ctx.r4.u32;
    for (size_t i = 0; i < l->queue.size(); i++)
    {
        if (match && l->queue[i].first != match) continue;
        guest::w32(ctx.r5.u32, l->queue[i].first);
        guest::w32(ctx.r6.u32, l->queue[i].second);
        LOG("XNotifyGetNext -> id %08X param %08X", l->queue[i].first, l->queue[i].second);
        l->queue.erase(l->queue.begin() + i);
        if (l->queue.empty()) l->signaled = false;
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Users / profiles
// ---------------------------------------------------------------------------

GUEST_IMPORT(XamUserGetSigninState) // signed in locally
{
    uint32_t user = ctx.r3.u32;
    ctx.r3.u64 = user == 0 ? 1 : 0;
    TraceOnce("XamUserGetSigninState", user, 0, ctx.r3.u32);
}

GUEST_IMPORT(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    guest::w64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamUserGetName)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    const char* name = "Player";
    uint32_t len = std::min<uint32_t>(ctx.r5.u32, uint32_t(std::strlen(name) + 1));
    std::memcpy(guest::ptr(ctx.r4.u32), name, len);
    if (len) guest::w8(ctx.r4.u32 + len - 1, 0);
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamUserCheckPrivilege)
{
    guest::w32(ctx.r5.u32, 1);
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamUserAreUsersFriends)
{
    if (ctx.r6.u32) guest::w32(ctx.r6.u32, 0);
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, ERROR_SUCCESS);
}

// XamUserReadProfileSettings(titleId, userIndex, xuidCount, xuids, settingCount, settingIds, pBufferSize, buffer, overlapped)
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    uint32_t pSize = ctx.r9.u32, buffer = ctx.r10.u32, ov = StackArg32(ctx, 0);
    uint32_t users = xuids ? std::max<uint32_t>(xuidCount, 1) : 1;

    uint32_t header = 0, data = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = guest::r32(ids + i * 4);
        uint32_t type = id >> 28, size = (id >> 16) & 0xFFF;
        header += 40;
        if (type == 4 || type == 6) data += size; // WSTRING / BINARY
    }
    uint32_t needed = 8 + (header + data) * users;
    uint32_t have = guest::r32(pSize);
    if (!buffer || have < needed)
    {
        if (!have) guest::w32(pSize, needed);
        ctx.r3.u64 = ERROR_INSUFFICIENT_BUFFER;
        return;
    }

    std::memset(guest::ptr(buffer), 0, needed);
    uint32_t settings = buffer + 8;
    uint32_t extra = settings + count * 40;
    guest::w32(buffer, count);
    guest::w32(buffer + 4, settings);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = guest::r32(ids + i * 4);
        uint32_t type = id >> 28, size = (id >> 16) & 0xFFF;
        uint32_t s = settings + i * 40;
        guest::w32(s + 0x00, 1); // source: default
        guest::w32(s + 0x08, 0); // user index (union with xuid)
        guest::w32(s + 0x10, id);
        guest::w8(s + 0x18, uint8_t(type));
        if (type == 4 || type == 6)
        {
            guest::w32(s + 0x20, size);
            guest::w32(s + 0x24, extra);
            extra += size;
        }
    }
    ctx.r3.u64 = CompleteOverlapped(ov, ERROR_SUCCESS);
}

GUEST_IMPORT(XamUserWriteProfileSettings) { ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, ERROR_SUCCESS); }


// ---------------------------------------------------------------------------
// Enumerators
// ---------------------------------------------------------------------------

struct Enumerator : Object
{
    std::vector<std::vector<uint8_t>> items;
    size_t next = 0;
    uint32_t itemSize = 0;
    bool IsWaitable() const override { return false; }
};

// XamEnumerate(handle, flags, buffer, bufferLength, pItemsReturned, overlapped)
GUEST_IMPORT(XamEnumerate)
{
    auto e = GetHandleAs<Enumerator>(ctx.r3.u32);
    uint32_t buffer = ctx.r5.u32, length = ctx.r6.u32, pReturned = ctx.r7.u32, ov = ctx.r8.u32;
    if (!e) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    uint32_t n = 0, off = 0;
    while (e->next < e->items.size() && off + e->items[e->next].size() <= length)
    {
        auto& it = e->items[e->next++];
        std::memcpy(guest::ptr(buffer + off), it.data(), it.size());
        off += uint32_t(it.size());
        ++n;
    }
    if (pReturned) guest::w32(pReturned, n);
    uint32_t result = n ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, result, n) : result;
}

GUEST_IMPORT(XamCreateEnumeratorHandle)
{
    auto e = std::make_shared<Enumerator>();
    guest::w32(ctx.r10.u32, CreateHandle(e)); // 8th argument: out handle
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle)
{
    LOG_STUB_ONCE("XamGetPrivateEnumStructureFromHandle");
    ctx.r3.u64 = ERROR_INVALID_PARAMETER;
}

// ---------------------------------------------------------------------------
// Content (save games). XCONTENT_DATA: DeviceID u32, ContentType u32, DisplayName wchar[128], FileName char[42]
// ---------------------------------------------------------------------------

constexpr uint32_t kDeviceId = 1; // hard drive

static std::string ContentFileName(uint32_t contentData)
{
    std::string name;
    const char* p = guest::ptr<const char>(contentData + 8 + 256);
    for (int i = 0; i < 42 && p[i]; i++) name.push_back(p[i]);
    return name;
}

GUEST_IMPORT(XamContentGetDeviceState) { ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, ERROR_SUCCESS); }

// XamContentGetDeviceData(deviceId, X_CONTENT_DEVICE_DATA*)
GUEST_IMPORT(XamContentGetDeviceData)
{
    uint32_t d = ctx.r4.u32;
    std::memset(guest::ptr(d), 0, 0x50);
    guest::w32(d + 0x00, kDeviceId);
    guest::w32(d + 0x04, 1);                      // device type: HDD
    guest::w64(d + 0x08, 20ull * 1024 * 1024 * 1024); // total bytes
    guest::w64(d + 0x10, 10ull * 1024 * 1024 * 1024); // free bytes
    const char16_t* name = u"Hard Drive";
    for (int i = 0; name[i]; i++) guest::w16(d + 0x18 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// XamContentCreate(user, rootName, contentData, flags, pDisposition, pLicense, overlapped)
GUEST_IMPORT(XamContentCreate)
{
    std::string root = guest::cstr(ctx.r4.u32);
    std::string file = ContentFileName(ctx.r5.u32);
    uint32_t flags = ctx.r6.u32;
    std::filesystem::path host = std::filesystem::current_path() / "userdata" / "save" / file;
    std::error_code ec;
    bool existed = std::filesystem::exists(host, ec);
    uint32_t disposition = flags & 0xF; // 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS
    uint32_t result = ERROR_SUCCESS;
    if (!existed && disposition == 3) result = ERROR_FILE_NOT_FOUND;
    else if (existed && disposition == 1) result = 80; // ERROR_FILE_EXISTS
    else std::filesystem::create_directories(host, ec);

    if (result == ERROR_SUCCESS)
    {
        vfs::AddMount(root, host);
        if (ctx.r7.u32) guest::w32(ctx.r7.u32, existed ? 2 : 1); // opened / created
        if (ctx.r8.u32) guest::w32(ctx.r8.u32, 0);
    }
    LOG("XamContentCreate(%s, %s, flags=%X) -> %u", root.c_str(), file.c_str(), flags, result);
    ctx.r3.u64 = CompleteOverlapped(ctx.r9.u32, result);
}

GUEST_IMPORT(XamContentClose)
{
    vfs::RemoveMount(guest::cstr(ctx.r3.u32));
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, ERROR_SUCCESS);
}

GUEST_IMPORT(XamContentDelete)
{
    std::string file = ContentFileName(ctx.r4.u32);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::current_path() / "userdata" / "save" / file, ec);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, ERROR_SUCCESS);
}

GUEST_IMPORT(XamContentGetCreator)
{
    if (ctx.r5.u32) guest::w32(ctx.r5.u32, 1);
    if (ctx.r6.u32) guest::w64(ctx.r6.u32, kXuid);
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, ERROR_SUCCESS);
}

// XamContentCreateEnumerator(user, deviceId, contentType, flags, itemsPerEnum, pBufferSize, pHandle)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    constexpr uint32_t kItem = 0x134; // sizeof(XCONTENT_DATA)
    auto e = std::make_shared<Enumerator>();
    std::error_code ec;
    for (auto& d : std::filesystem::directory_iterator(std::filesystem::current_path() / "userdata" / "save", ec))
    {
        if (!d.is_directory(ec)) continue;
        std::vector<uint8_t> item(kItem, 0);
        auto put32 = [&](size_t o, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(&item[o], &v, 4); };
        put32(0, kDeviceId);
        put32(4, ctx.r5.u32);
        std::string fn = d.path().filename().string();
        for (size_t i = 0; i < fn.size() && i < 127; i++) { item[8 + i * 2 + 1] = uint8_t(fn[i]); }
        std::memcpy(&item[8 + 256], fn.c_str(), std::min<size_t>(fn.size(), 41));
        e->items.push_back(std::move(item));
    }
    if (ctx.r8.u32) guest::w32(ctx.r8.u32, kItem * std::max<uint32_t>(ctx.r7.u32, 1));
    guest::w32(ctx.r9.u32, CreateHandle(e));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// System UI - answer immediately
// ---------------------------------------------------------------------------

// XamShowMessageBoxUIEx(user, title, text, buttonCount, buttons, activeButton, flags, unused, pResult, overlapped)
GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    LOG("MessageBox: %s / %s", guest::wstr(ctx.r4.u32).c_str(), guest::wstr(ctx.r5.u32).c_str());
    uint32_t pResult = StackArg32(ctx, 0), ov = StackArg32(ctx, 1);
    if (pResult) guest::w32(pResult, ctx.r8.u32); // pick the active button
    ctx.r3.u64 = CompleteOverlapped(ov, ERROR_SUCCESS);
}

// XamShowDeviceSelectorUI(user, contentType, contentFlags, totalRequested, pDeviceId, overlapped)
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    if (ctx.r7.u32) guest::w32(ctx.r7.u32, kDeviceId);
    ctx.r3.u64 = CompleteOverlapped(ctx.r8.u32, ERROR_SUCCESS);
}

GUEST_IMPORT(XamShowSigninUI)
{
    if (s_listener)
    {
        std::lock_guard lock(g_lock);
        s_listener->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI closed
        s_listener->signaled = true;
        g_cv.notify_all();
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamShowKeyboardUI) { LOG_STUB_ONCE("XamShowKeyboardUI"); ctx.r3.u64 = CompleteOverlapped(ctx.r10.u32, ERROR_CANCELLED); }
GUEST_IMPORT(XamShowAchievementsUI) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowFriendsUI) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowGamerCardUIForXUID) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowMarketplaceUI) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowMessageComposeUI) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowPlayerReviewUI) { ctx.r3.u64 = ERROR_SUCCESS; }
GUEST_IMPORT(XamShowDirtyDiscErrorUI) { LOG("XamShowDirtyDiscErrorUI - a game file failed to read"); std::exit(1); }

// ---------------------------------------------------------------------------
// XMsg
// ---------------------------------------------------------------------------

GUEST_IMPORT(XMsgInProcessCall) { LOG_STUB_ONCE("XMsgInProcessCall"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XMsgStartIORequest) { LOG("XMsgStartIORequest(%08X, %08X)", ctx.r3.u32, ctx.r4.u32); ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, ERROR_SUCCESS); }
GUEST_IMPORT(XMsgStartIORequestEx) { LOG("XMsgStartIORequestEx(%08X, %08X)", ctx.r3.u32, ctx.r4.u32); ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, ERROR_SUCCESS); }
GUEST_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------------
// Sessions / voice
// ---------------------------------------------------------------------------

GUEST_IMPORT(XamSessionCreateHandle)
{
    guest::w32(ctx.r3.u32, CreateHandle(std::make_shared<Event>(true, false)));
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamSessionRefObjByHandle)
{
    auto obj = GetHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    uint32_t body = mem::SystemAllocZeroed(0x20);
    RegisterGuestObject(body, obj);
    guest::w32(ctx.r4.u32, body);
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamVoiceCreate) { if (ctx.r5.u32) guest::w32(ctx.r5.u32, 0); ctx.r3.u64 = ERROR_ACCESS_DENIED; }
GUEST_IMPORT(XamVoiceClose) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XamVoiceSubmitPacket) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------------
// Input: user 0 is the host controller (Android gamepad / XInput), fed through input::SetPad.
// ---------------------------------------------------------------------------

// XamInputGetState(userIndex, flags, X_INPUT_STATE*)
// X_INPUT_STATE: u32 packet; u16 buttons; u8 lt; u8 rt; s16 lx, ly, rx, ry (big-endian)
GUEST_IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    if (user != 0 || !input::IsConnected())
    {
        TraceOnce("XamInputGetState", user, ctx.r4.u32, ERROR_DEVICE_NOT_CONNECTED);
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    TraceOnce("XamInputGetState", user, ctx.r4.u32, 0);
    uint32_t packet;
    input::PadState p = input::GetPad(&packet);
    guest::w32(state + 0, packet);
    guest::w16(state + 4, p.buttons);
    guest::w8(state + 6, p.leftTrigger);
    guest::w8(state + 7, p.rightTrigger);
    guest::w16(state + 8, uint16_t(p.thumbLX));
    guest::w16(state + 10, uint16_t(p.thumbLY));
    guest::w16(state + 12, uint16_t(p.thumbRX));
    guest::w16(state + 14, uint16_t(p.thumbRY));
    ctx.r3.u64 = ERROR_SUCCESS;
}

GUEST_IMPORT(XamInputSetState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED; }
GUEST_IMPORT(XamInputGetKeystrokeEx)
{
    TraceOnce("XamInputGetKeystrokeEx", ctx.r3.u32, ctx.r4.u32, 0x10D2);
    ctx.r3.u64 = 0x10D2; // ERROR_EMPTY
}

// ---------------------------------------------------------------------------
// Achievements / stats: valid but empty enumerators (no Xbox Live profile data).
// Returning an error here makes the New Game loader thread bail out halfway.
// ---------------------------------------------------------------------------

// XamUserCreateAchievementEnumerator(titleId, user, xuid, flags, offset, count, pBufferSize, pHandle)
GUEST_IMPORT(XamUserCreateAchievementEnumerator)
{
    uint32_t user = ctx.r4.u32, flags = ctx.r6.u32, count = ctx.r8.u32, pSize = ctx.r9.u32, pHandle = ctx.r10.u32;
    if (!count || !pSize || !pHandle || user >= 4) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    uint32_t entry = 36 + ((flags & 7) ? 464 : 0); // X_ACHIEVEMENT_DETAILS (+ string buffer)
    guest::w32(pSize, entry * count);
    auto e = std::make_shared<Enumerator>();
    e->itemSize = entry;
    guest::w32(pHandle, CreateHandle(e));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// XamUserCreateStatsEnumerator(titleId, user, count, flags, size, pStats, pBufferSize, pHandle)
GUEST_IMPORT(XamUserCreateStatsEnumerator)
{
    uint32_t count = ctx.r5.u32, pSize = ctx.r9.u32, pHandle = ctx.r10.u32;
    if (!count || !pSize || !pHandle) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    guest::w32(pSize, 0x40 * count);
    guest::w32(pHandle, CreateHandle(std::make_shared<Enumerator>()));
    ctx.r3.u64 = ERROR_SUCCESS;
}
