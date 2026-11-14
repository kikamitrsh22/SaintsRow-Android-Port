// Saints Row native runtime - entry point.
#include "guest.h"
#include "loader.h"
#include "memory.h"
#include "vfs.h"
#include "kernel/kernel.h"
#include "gpu/gpu.h"
#include "input.h"
#include "xglue/xglue.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <thread>
#include <vector>
#include <cstdlib>

#include <chrono>
#include <cstdarg>
#include <mutex>

namespace rt
{
    static std::mutex s_logLock;
    static auto s_start = std::chrono::steady_clock::now();

    void Log(const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        std::lock_guard lock(s_logLock);
        std::fprintf(stderr, "[%9.3f][t%02u] %s\n", t, CurrentThreadId(), buf);
        std::fflush(stderr);
    }
}

namespace rt { void InstallCrashHandler(); }

// Polls the first XInput controller into the shared pad state (Windows test builds).
static void StartXInputPolling()
{
    // SR_AUTOPRESS=<seconds>[,<seconds>...]: press A at those times (for testing without a pad).
    std::vector<double> autoPress;
    if (const char* env = std::getenv("SR_AUTOPRESS"))
        for (const char* p = env; *p;)
        {
            autoPress.push_back(std::atof(p));
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    std::thread([autoPress] {
        auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            for (double at : autoPress)
                if (t >= at && t < at + 0.3)
                {
                    input::PadState p;
                    p.buttons = input::kA;
                    input::SetPad(p);
                    goto next;
                }
            XINPUT_STATE st{};
            if (XInputGetState(0, &st) == ERROR_SUCCESS)
            {
                input::PadState p;
                p.buttons = st.Gamepad.wButtons;
                p.leftTrigger = st.Gamepad.bLeftTrigger;
                p.rightTrigger = st.Gamepad.bRightTrigger;
                p.thumbLX = st.Gamepad.sThumbLX;
                p.thumbLY = st.Gamepad.sThumbLY;
                p.thumbRX = st.Gamepad.sThumbRX;
                p.thumbRY = st.Gamepad.sThumbRY;
                input::SetPad(p);
            }
            else if (!autoPress.empty())
            {
                input::SetPad(input::PadState{});
            }
        next:
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }).detach();
}

int main(int argc, char** argv)
{
    std::fputs("[startup] main reached\n", stderr);
    std::fflush(stderr);
    rt::InstallCrashHandler();
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: SaintsRow <game folder>\n");
        return 1;
    }

    std::filesystem::path gameDir = argv[1];
    if (!mem::Init())
    {
        LOG("failed to reserve 4 GB of guest memory");
        return 1;
    }

    vfs::Init(gameDir, std::filesystem::current_path() / "userdata");

    if (!loader::Load(gameDir / "default.xex"))
        return 1;

    if (!xglue::InitGraphics(gpu::DispatchInterrupt))
    {
        LOG("failed to initialize the window / Vulkan GPU");
        return 1;
    }

    // The game runs on its own thread; this thread runs the window's message loop.
    StartXInputPolling();
    std::thread game([] {
        kernel::RunMainThread(loader::g_title.entryPoint, std::max<uint32_t>(loader::g_title.stackSize, 0x100000));
        LOG("main thread returned");
        xglue::RequestQuit();
    });
    game.detach();
    return xglue::RunUiLoop();
}
