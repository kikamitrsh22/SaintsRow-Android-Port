// Crash reporting: logs the faulting host address (as an offset into the exe, for llvm-symbolizer),
// the guest address that was touched, and the current guest thread's registers.
#include "guest.h"
#include "kernel/kernel.h"

extern "C" [[noreturn]] void PPCIndirectCallFailed(unsigned int target, unsigned int lr)
{
    auto* t = kernel::CurrentThread();
    rt::Log("FATAL: indirect call to %08X has no recompiled function (lr=%08X, thread %u)", target, lr,
        t ? t->id : 0);
    if (t && t->ctx)
    {
        auto& c = *t->ctx;
        rt::Log("  r1=%08X r3=%08X r4=%08X r5=%08X r11=%08X r12=%08X", c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32,
            c.r11.u32, c.r12.u32);
    }
    std::_Exit(4);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rt
{
    static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
    {
        auto* rec = info->ExceptionRecord;
        // C++ exceptions (ThreadExit) are handled normally.
        if (rec->ExceptionCode == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;

        auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto pc = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
        Log("CRASH: code=%08lX pc=%p (exe+0x%llX)", rec->ExceptionCode, rec->ExceptionAddress,
            (unsigned long long)(pc - exe));
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        {
            uintptr_t target = rec->ExceptionInformation[1];
            uintptr_t gb = reinterpret_cast<uintptr_t>(guest::g_base);
            if (gb && target >= gb && target - gb < PPC_MEMORY_SIZE)
                Log("  %s guest address %08llX", rec->ExceptionInformation[0] ? "write to" : "read from",
                    (unsigned long long)(target - gb));
            else
                Log("  %s host address %p", rec->ExceptionInformation[0] ? "write to" : "read from", (void*)target);
        }

        // Host call stack (return addresses that fall inside the exe)
        auto* rsp = reinterpret_cast<uintptr_t*>(info->ContextRecord->Rsp);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(exe), &mbi, sizeof(mbi));
        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
        uintptr_t exeEnd = exe + nt->OptionalHeader.SizeOfImage;
        std::string stack;
        int found = 0;
        for (int i = 0; i < 2048 && found < 24; i++)
        {
            uintptr_t v = rsp[i];
            if (v > exe && v < exeEnd)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), " 0x%llX", (unsigned long long)(v - exe));
                stack += buf;
                found++;
            }
        }
        Log("  stack (exe offsets):%s", stack.c_str());

        if (auto* t = kernel::CurrentThread(); t && t->ctx)
        {
            auto& c = *t->ctx;
            Log("  guest thread %u: r1=%08X r3=%08X r4=%08X r5=%08X r13=%08X lr=%08llX ctr=%08X",
                t->id, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r13.u32, (unsigned long long)c.lr, c.ctr.u32);
        }
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void InstallCrashHandler()
    {
        AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* info) -> LONG {
            auto code = info->ExceptionRecord->ExceptionCode;
            if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO)
                return CrashFilter(info);
            return EXCEPTION_CONTINUE_SEARCH;
        });
    }
}
#else
namespace rt { void InstallCrashHandler() {} }
#endif
