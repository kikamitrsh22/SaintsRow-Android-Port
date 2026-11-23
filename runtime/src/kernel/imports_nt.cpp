// Nt*/Ob* imports: handle-based sync objects, timers, virtual memory, files.
#include "kernel.h"
#include <chrono>
#include <set>
#include <string>
#include "../memory.h"
#include "../platform.h"
#include "../vfs.h"
#include "../xglue/xglue.h"

#include <chrono>
#include <thread>
#include <unordered_map>

using namespace kernel;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Events, semaphores, mutants
// ---------------------------------------------------------------------------

// NtCreateEvent(pHandle, objAttr, eventType, initialState)
GUEST_IMPORT(NtCreateEvent)
{
    bool manual = ctx.r5.u32 == 0; // NotificationEvent
    guest::w32(ctx.r3.u32, CreateHandle(std::make_shared<Event>(manual, ctx.r6.u32 != 0)));
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtSetEvent)
{
    auto e = GetHandleAs<Event>(ctx.r3.u32);
    if (!e) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t prev = SetEvent(*e);
    if (ctx.r4.u32) guest::w32(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtClearEvent)
{
    auto e = GetHandleAs<Event>(ctx.r3.u32);
    if (!e) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ResetEvent(*e);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtCreateSemaphore(pHandle, objAttr, count, limit)
GUEST_IMPORT(NtCreateSemaphore)
{
    guest::w32(ctx.r3.u32, CreateHandle(std::make_shared<Semaphore>(int32_t(ctx.r5.u32), int32_t(ctx.r6.u32))));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseSemaphore(handle, count, pPrevious)
GUEST_IMPORT(NtReleaseSemaphore)
{
    auto s = GetHandleAs<Semaphore>(ctx.r3.u32);
    if (!s) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t status;
    int32_t prev = ReleaseSemaphore(*s, int32_t(ctx.r4.u32), &status);
    if (ctx.r5.u32) guest::w32(ctx.r5.u32, uint32_t(prev));
    ctx.r3.u64 = status;
}

// NtCreateMutant(pHandle, objAttr, initialOwner)
GUEST_IMPORT(NtCreateMutant)
{
    auto m = std::make_shared<Mutant>();
    if (ctx.r5.u32) { m->owner = CurrentThread(); m->recursion = 1; }
    guest::w32(ctx.r3.u32, CreateHandle(m));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseMutant(handle, pPrevious)
GUEST_IMPORT(NtReleaseMutant)
{
    auto m = GetHandleAs<Mutant>(ctx.r3.u32);
    if (!m) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t status;
    int32_t prev = ReleaseMutant(*m, CurrentThread(), &status);
    if (ctx.r4.u32) guest::w32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = status;
}

// NtWaitForSingleObjectEx(handle, mode, alertable, pTimeout)
GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    auto obj = GetHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = Wait({ obj }, false, DeadlineFromGuest(ctx.r6.u32));
}

// NtWaitForMultipleObjectsEx(count, handles[], waitType, mode, alertable, pTimeout)
GUEST_IMPORT(NtWaitForMultipleObjectsEx)
{
    std::vector<std::shared_ptr<Object>> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto o = GetHandle(guest::r32(ctx.r4.u32 + i * 4));
        if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
        objs.push_back(o);
    }
    ctx.r3.u64 = Wait(objs, ctx.r5.u32 == 0, DeadlineFromGuest(ctx.r8.u32));
}

// ---------------------------------------------------------------------------
// Timers: an event signalled by a host thread
// ---------------------------------------------------------------------------

struct Timer : Event
{
    std::atomic<uint32_t> generation{ 0 };
    Timer(bool manual) : Event(manual, false) {}
};

// NtCreateTimer(pHandle, objAttr, type)  type 0 = notification, 1 = synchronization
GUEST_IMPORT(NtCreateTimer)
{
    guest::w32(ctx.r3.u32, CreateHandle(std::make_shared<Timer>(ctx.r5.u32 == 0)));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtSetTimerEx(handle, pDueTime, apcRoutine, apcMode, apcArg, resume, period(ms), pPreviousState)
GUEST_IMPORT(NtSetTimerEx)
{
    auto t = GetHandleAs<Timer>(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto deadline = DeadlineFromGuest(ctx.r4.u32).value_or(steady_clock::now());
    uint32_t period = ctx.r9.u32;
    if (ctx.r5.u32) LOG("NtSetTimerEx: APC routine %08X not supported", ctx.r5.u32);
    ResetEvent(*t);
    uint32_t gen = ++t->generation;
    std::thread([t, deadline, period, gen]() mutable {
        auto next = deadline;
        for (;;)
        {
            std::this_thread::sleep_until(next);
            if (t->generation != gen) return;
            SetEvent(*t);
            if (!period) return;
            next += milliseconds(period);
        }
    }).detach();
    if (ctx.r10.u32) guest::w32(ctx.r10.u32, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtCancelTimer)
{
    auto t = GetHandleAs<Timer>(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ++t->generation;
    if (ctx.r4.u32) guest::w32(ctx.r4.u32, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Handles / objects
// ---------------------------------------------------------------------------

GUEST_IMPORT(NtClose)
{
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// NtDuplicateObject(handle, pNewHandle, options)
GUEST_IMPORT(NtDuplicateObject)
{
    uint32_t h = DuplicateHandle(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r4.u32) guest::w32(ctx.r4.u32, h);
    if (ctx.r5.u32 & 1) CloseHandle(ctx.r3.u32); // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ObReferenceObjectByHandle(handle, objectType, pObject)
GUEST_IMPORT(ObReferenceObjectByHandle)
{
    auto obj = GetHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t guestObj = 0;
    if (auto t = std::dynamic_pointer_cast<GuestThread>(obj))
    {
        guestObj = t->kthread;
    }
    else
    {
        // Give the object a guest-side body so Ke* functions can find it again.
        static std::mutex s_lock;
        static std::unordered_map<Object*, uint32_t> s_bodies;
        std::lock_guard lock(s_lock);
        auto& body = s_bodies[obj.get()];
        if (!body)
        {
            body = mem::SystemAllocZeroed(0x20);
            if (auto e = std::dynamic_pointer_cast<Event>(obj)) guest::w8(body, e->manualReset ? 0 : 1);
            else if (std::dynamic_pointer_cast<Semaphore>(obj)) guest::w8(body, 5);
            else if (std::dynamic_pointer_cast<Mutant>(obj)) guest::w8(body, 2);
            RegisterGuestObject(body, obj);
        }
        guestObj = body;
    }
    if (ctx.r5.u32) guest::w32(ctx.r5.u32, guestObj);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ObDereferenceObject) {}
GUEST_IMPORT(ObCreateSymbolicLink) { ctx.r3.u64 = STATUS_SUCCESS; }
GUEST_IMPORT(ObDeleteSymbolicLink) { ctx.r3.u64 = STATUS_SUCCESS; }

// ---------------------------------------------------------------------------
// Virtual memory
// ---------------------------------------------------------------------------

constexpr uint32_t MEM_COMMIT = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;
constexpr uint32_t MEM_DECOMMIT = 0x4000;
constexpr uint32_t MEM_RELEASE = 0x8000;
constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;

// NtAllocateVirtualMemory(pBaseAddress, pRegionSize, allocationType, protect, debug)
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t va = guest::r32(pBase), size = guest::r32(pSize);
    uint32_t status = xglue::NtAllocateVirtualMemory(va, size, ctx.r5.u32, ctx.r6.u32);
    if (status == STATUS_SUCCESS)
    {
        guest::w32(pBase, va);
        guest::w32(pSize, size);
    }
    else
    {
        LOG("NtAllocateVirtualMemory(%08X, %X, type %X) failed: %08X", guest::r32(pBase), guest::r32(pSize), ctx.r5.u32, status);
    }
    ctx.r3.u64 = status;
}

// NtFreeVirtualMemory(pBaseAddress, pRegionSize, freeType, debug)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t va = guest::r32(pBase), size = guest::r32(pSize);
    uint32_t status = xglue::NtFreeVirtualMemory(va, size, ctx.r5.u32);
    if (status == STATUS_SUCCESS)
    {
        guest::w32(pBase, va);
        guest::w32(pSize, size);
    }
    ctx.r3.u64 = status;
}

// NtQueryVirtualMemory(address, pMemoryBasicInformation, regionType)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    uint32_t info[7];
    uint32_t status = xglue::NtQueryVirtualMemory(ctx.r3.u32, info);
    if (status == STATUS_SUCCESS)
        for (int i = 0; i < 7; i++) guest::w32(ctx.r4.u32 + i * 4, info[i]);
    ctx.r3.u64 = status;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static void SetIosb(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb) return;
    guest::w32(iosb, status);
    guest::w32(iosb + 4, info);
}

static std::string ObjectPath(uint32_t objAttr)
{
    // OBJECT_ATTRIBUTES { RootDirectory, ObjectName (ANSI_STRING*), Attributes }
    std::string name = guest::ansi(guest::r32(objAttr + 4));
    uint32_t root = guest::r32(objAttr);
    if (root && root != 0xFFFFFFFD)
    {
        if (auto dir = GetHandleAs<vfs::File>(root))
        {
            std::filesystem::path p = dir->path / name;
            return p.string(); // already a host path
        }
    }
    return name;
}

static uint32_t OpenFile(uint32_t pHandle, uint32_t objAttr, uint32_t iosb, uint32_t disposition, uint32_t options,
    uint32_t access)
{
    std::string guestName = ObjectPath(objAttr);
    std::filesystem::path host;
    bool writable = false;
    uint32_t root = guest::r32(objAttr);
    if (root && root != 0xFFFFFFFD && GetHandleAs<vfs::File>(root))
    {
        host = guestName;
        writable = true;
    }
    else if (!vfs::Translate(guestName, host, &writable))
    {
        LOG("open: unknown device for '%s'", guestName.c_str());
        SetIosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    std::error_code ec;
    bool exists = std::filesystem::exists(host, ec);
    bool isDir = exists && std::filesystem::is_directory(host, ec);
    bool wantDir = (options & 1) != 0; // FILE_DIRECTORY_FILE
    bool wantWrite = (access & 0x40000000) || (access & 0x2) || disposition == 2 || disposition == 4 || disposition == 5 || disposition == 0;

    uint32_t info = 1; // FILE_OPENED
    if (!exists)
    {
        if (disposition == 1 || disposition == 4) // OPEN / OVERWRITE require existence
        {
            LOG("open: not found %s -> %s", guestName.c_str(), host.string().c_str());
            SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (!writable)
        {
            SetIosb(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        if (wantDir)
            std::filesystem::create_directories(host, ec);
        else
        {
            std::filesystem::create_directories(host.parent_path(), ec);
            if (std::FILE* f = std::fopen(host.string().c_str(), "wb")) std::fclose(f);
        }
        isDir = wantDir;
        info = 2; // FILE_CREATED
    }
    else if (disposition == 2) // CREATE on existing
    {
        SetIosb(iosb, STATUS_OBJECT_NAME_COLLISION, 0);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    auto f = std::make_shared<vfs::File>();
    f->path = host;
    f->isDirectory = isDir;
    if (!isDir)
    {
        bool truncate = exists && (disposition == 0 || disposition == 4 || disposition == 5);
        const char* mode = (wantWrite && writable) ? (truncate ? "w+b" : "r+b") : "rb";
        f->fp = std::fopen(host.string().c_str(), mode);
        if (!f->fp)
        {
            LOG("open: fopen failed %s", host.string().c_str());
            SetIosb(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        if (truncate) info = 3; // FILE_OVERWRITTEN
    }

    uint32_t h = CreateHandle(f);
    guest::w32(pHandle, h);
    SetIosb(iosb, STATUS_SUCCESS, info);
    // Each successful open used to be logged; the game reopens its stream packfiles constantly
    // (2000+ lines in two minutes), and every log line is a logcat write plus a flushed write to
    // external storage, on the thread doing the streaming. Log each file the first time only.
    {
        static std::mutex s_seenLock;
        static std::set<std::string> s_seen;
        std::lock_guard lock(s_seenLock);
        if (s_seen.insert(guestName).second)
            LOG("open: %s -> %s (handle %08X)", guestName.c_str(), host.string().c_str(), h);
    }
    g_io.opens++;
    return STATUS_SUCCESS;
}

// NtCreateFile(pHandle, access, objAttr, iosb, allocSize, attributes, share, disposition, options)
GUEST_IMPORT(NtCreateFile)
{
    uint32_t disposition = ctx.r10.u32;
    uint32_t options = StackArg32(ctx, 0); // 9th argument
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, disposition, options, ctx.r4.u32);
}

// NtOpenFile(pHandle, access, objAttr, iosb, share, options)
GUEST_IMPORT(NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32, ctx.r4.u32);
}

static void SignalCompletion(uint32_t eventHandle)
{
    if (!eventHandle) return;
    if (auto e = GetHandleAs<Event>(eventHandle)) SetEvent(*e);
}

// NtReadFile(handle, event, apcRoutine, apcContext, iosb, buffer, length, pByteOffset)
GUEST_IMPORT(NtReadFile)
{
    auto f = GetHandleAs<vfs::File>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    if (!f || !f->fp) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (pOffset)
    {
        int64_t off = int64_t(guest::r64(pOffset));
        if (off >= 0) f->position = uint64_t(off); // -2 = FILE_USE_FILE_POINTER_POSITION
    }
    const auto readStart = std::chrono::steady_clock::now();
    FileSeek64(f->fp, int64_t(f->position), SEEK_SET);
    size_t n = std::fread(guest::ptr(buffer), 1, length, f->fp);
    f->position += n;
    g_io.reads++;
    g_io.readBytes += n;
    g_io.readUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - readStart).count());
    uint32_t status = (n == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    SetIosb(iosb, status, uint32_t(n));
    SignalCompletion(ctx.r4.u32);
    if (ctx.r5.u32) LOG("NtReadFile: APC completion not supported");
    ctx.r3.u64 = status;
}

// NtWriteFile(handle, event, apcRoutine, apcContext, iosb, buffer, length, pByteOffset)
GUEST_IMPORT(NtWriteFile)
{
    auto f = GetHandleAs<vfs::File>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    if (!f || !f->fp) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (pOffset)
    {
        int64_t off = int64_t(guest::r64(pOffset));
        if (off >= 0) f->position = uint64_t(off);
    }
    FileSeek64(f->fp, int64_t(f->position), SEEK_SET);
    size_t n = std::fwrite(guest::ptr(buffer), 1, length, f->fp);
    f->position += n;
    SetIosb(iosb, STATUS_SUCCESS, uint32_t(n));
    SignalCompletion(ctx.r4.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    if (auto f = GetHandleAs<vfs::File>(ctx.r3.u32); f && f->fp) std::fflush(f->fp);
    SetIosb(ctx.r4.u32, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

static uint64_t FileSize(vfs::File& f)
{
    std::error_code ec;
    if (f.isDirectory) return 0;
    if (f.fp) std::fflush(f.fp);
    auto s = std::filesystem::file_size(f.path, ec);
    return ec ? 0 : s;
}

// NtQueryInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtQueryInformationFile)
{
    auto f = GetHandleAs<vfs::File>(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t size = FileSize(*f);
    uint64_t now = SystemTime();
    std::memset(guest::ptr(info), 0, length);
    uint32_t written = 0;
    switch (cls)
    {
    case 5: // FileStandardInformation
        guest::w64(info + 0, (size + 0xFFF) & ~0xFFFull);
        guest::w64(info + 8, size);
        guest::w32(info + 16, 1);
        guest::w8(info + 21, f->isDirectory ? 1 : 0);
        written = 24;
        break;
    case 14: // FilePositionInformation
        guest::w64(info, f->position);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        guest::w64(info + 0, now);
        guest::w64(info + 8, now);
        guest::w64(info + 16, now);
        guest::w64(info + 24, now);
        guest::w64(info + 32, (size + 0xFFF) & ~0xFFFull);
        guest::w64(info + 40, size);
        guest::w32(info + 48, f->isDirectory ? 0x10 : 0x80);
        written = 56;
        break;
    case 4: // FileBasicInformation
        guest::w64(info + 0, now);
        guest::w64(info + 8, now);
        guest::w64(info + 16, now);
        guest::w64(info + 24, now);
        guest::w32(info + 32, f->isDirectory ? 0x10 : 0x80);
        written = 40;
        break;
    default:
        LOG("NtQueryInformationFile: class %u not implemented", cls);
        SetIosb(iosb, STATUS_INVALID_INFO_CLASS, 0);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    SetIosb(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtSetInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtSetInformationFile)
{
    auto f = GetHandleAs<vfs::File>(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    switch (cls)
    {
    case 14: // FilePositionInformation
        f->position = guest::r64(info);
        break;
    case 20: // FileEndOfFileInformation
    {
        std::error_code ec;
        if (f->fp) std::fflush(f->fp);
        std::filesystem::resize_file(f->path, guest::r64(info), ec);
        break;
    }
    case 4:  // FileBasicInformation
    case 13: // FileDispositionInformation (delete on close) - ignored
        break;
    default:
        LOG("NtSetInformationFile: class %u not implemented", cls);
        break;
    }
    SetIosb(iosb, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtQueryVolumeInformationFile(handle, iosb, info, length, class)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    std::memset(guest::ptr(info), 0, length);
    switch (cls)
    {
    case 1: // FileFsVolumeInformation
        guest::w32(info + 8, 0x12345678); // serial
        break;
    case 3: // FileFsSizeInformation
        guest::w64(info + 0, 0x100000);  // total allocation units
        guest::w64(info + 8, 0x80000);   // available
        guest::w32(info + 16, 32);       // sectors per unit
        guest::w32(info + 20, 512);      // bytes per sector
        break;
    case 5: // FileFsAttributeInformation
        guest::w32(info + 0, 0);
        guest::w32(info + 4, 255);
        guest::w32(info + 8, 4);
        std::memcpy(guest::ptr(info + 12), "FATX", 4);
        break;
    default:
        LOG("NtQueryVolumeInformationFile: class %u", cls);
        break;
    }
    SetIosb(iosb, STATUS_SUCCESS, length);
    ctx.r3.u64 = STATUS_SUCCESS;
}

static bool WildcardMatch(const char* pat, const char* str)
{
    for (; *pat; ++pat, ++str)
    {
        if (*pat == '*')
        {
            for (const char* s = str;; ++s)
            {
                if (WildcardMatch(pat + 1, s)) return true;
                if (!*s) return false;
            }
        }
        if (!*str) return false;
        if (*pat != '?' && std::tolower((unsigned char)*pat) != std::tolower((unsigned char)*str)) return false;
    }
    return *str == 0;
}

// NtQueryDirectoryFile(handle, event, apc, apcCtx, iosb, info, length, fileName(ANSI_STRING*), restartScan)
GUEST_IMPORT(NtQueryDirectoryFile)
{
    auto f = GetHandleAs<vfs::File>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32, pName = ctx.r10.u32;
    uint32_t restart = StackArg32(ctx, 0);
    if (!f || !f->isDirectory) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }

    std::string pattern = pName ? guest::ansi(pName) : "*";
    if (pattern.empty()) pattern = "*";
    if (!f->enumStarted || restart)
    {
        f->entries.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(f->path, ec))
            if (WildcardMatch(pattern.c_str(), e.path().filename().string().c_str()))
                f->entries.push_back(e);
        f->enumIndex = 0;
        f->enumStarted = true;
    }

    if (f->enumIndex >= f->entries.size())
    {
        SetIosb(iosb, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }

    // One FILE_DIRECTORY_INFORMATION entry per call.
    auto& e = f->entries[f->enumIndex++];
    std::string name = e.path().filename().string();
    uint32_t need = 0x40 + uint32_t(name.size());
    if (need > length) { ctx.r3.u64 = 0x80000005; return; } // STATUS_BUFFER_OVERFLOW
    std::error_code ec;
    uint64_t size = e.is_directory(ec) ? 0 : e.file_size(ec);
    uint64_t now = SystemTime();
    std::memset(guest::ptr(info), 0, need);
    guest::w32(info + 0x00, 0); // NextEntryOffset
    guest::w32(info + 0x04, uint32_t(f->enumIndex));
    guest::w64(info + 0x08, now);
    guest::w64(info + 0x10, now);
    guest::w64(info + 0x18, now);
    guest::w64(info + 0x20, now);
    guest::w64(info + 0x28, size);
    guest::w64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    guest::w32(info + 0x38, e.is_directory(ec) ? 0x10 : 0x80);
    guest::w32(info + 0x3C, uint32_t(name.size()));
    std::memcpy(guest::ptr(info + 0x40), name.data(), name.size());
    SetIosb(iosb, STATUS_SUCCESS, need);
    ctx.r3.u64 = STATUS_SUCCESS;
}
