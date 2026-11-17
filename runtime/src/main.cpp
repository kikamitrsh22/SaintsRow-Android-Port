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

// Keyboard and mouse controls, laid out like Saints Row 2 on PC (the layout and the game addresses
// are Saints Reborn's). Keys become controller input on top of what a real controller reports,
// only while the game window has focus. Some keys depend on what the game is showing.
//
// On foot                           In a vehicle
//   W A S D   move                    W / S      accelerate / brake, reverse
//   Mouse     camera                  A / D      steer
//   LMB       fire / attack           LMB        drive-by attack
//   RMB       secondary attack        Space      handbrake
//   Space     jump                    Shift      nitrous
//   Shift     sprint                  Ctrl       hydraulics
//   E         action / enter car      E          exit car
//   R         reload / pick up        Z / C      look left / right (X: back)
//   F         kick     C  crouch      MMB / V    right stick click
// Everywhere
//   Q (hold)  weapon wheel (pick with W A S D)
//   Esc / M   pause          Tab     map / back button
//   Arrows    D-pad (menus, recruit, radio)
//   Enter     A (select)     Backspace  B (back)
// Menus (main menu, pause menu, player creation)
//   Arrows or W / S  up / down      Enter or LMB  select      Esc / Backspace / RMB  back
//   Q / E     previous / next tab (LT / RT)      X / Y  the X / Y actions (Y = Begin Game, Resume)
namespace
{
    constexpr uint32_t kPlayerPtr = 0x8309ABEC;     // the player object, null in the front end
    constexpr uint32_t kMenuScreen = 0x839E0DF8;    // active menu screen object
    constexpr uint32_t kScreenCreation = 0x82FFB7BC;
    constexpr uint32_t kCreationFlag = 0x827B04D4;  // 1 in player creation

    bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    // The game's own "is the player in a vehicle" test, done with memory reads.
    bool PlayerInVehicle()
    {
        const uint32_t player = guest::r32(kPlayerPtr);
        if (player < 0x10000) return false;
        if (guest::r32(player + 3456) != 0) return false;
        const uint32_t handle = guest::r32(player + 2496);
        if (!handle) return false;
        if (guest::r8(player + 2569) & 0x10) return false; // getting in / out
        const uint32_t state = guest::r32(player + 508);
        if (state == 9 || state == 12) return false;
        const uint32_t index = handle & 0xFFFF;
        if (index >= 4096) return false;
        const uint32_t object = guest::r32(0x830866C8 + 12 + index * 16);
        if (object < 0x10000) return false;
        return guest::r32(object + 68) == handle && guest::r32(object + 72) == 5;
    }

    // Mouse movement -> right stick while playing. The cursor is kept at the window centre.
    void MouseCamera(HWND window, bool capture, input::PadState& p)
    {
        static bool s_captured = false;
        static double s_x = 0, s_y = 0; // smoothed stick position, -1..1
        if (!capture || !window)
        {
            if (s_captured) { ClipCursor(nullptr); s_captured = false; }
            s_x = s_y = 0;
            return;
        }
        RECT rc;
        GetClientRect(window, &rc);
        POINT centre{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(window, &centre);
        POINT cur;
        GetCursorPos(&cur);
        int dx = 0, dy = 0;
        if (s_captured) { dx = cur.x - centre.x; dy = cur.y - centre.y; }
        else
        {
            RECT clip{ centre.x - 200, centre.y - 200, centre.x + 200, centre.y + 200 };
            ClipCursor(&clip);
            s_captured = true;
        }
        SetCursorPos(centre.x, centre.y);

        // Polled every 4 ms: low-pass the deltas so the stick does not flicker between samples.
        constexpr double kGain = 0.06, kSmooth = 0.75;
        s_x = s_x * kSmooth + dx * kGain;
        s_y = s_y * kSmooth + dy * kGain;
        auto toStick = [](double v, int16_t& out) {
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            double a = v < 0 ? -v : v;
            if (a < 0.01) return;
            // Start just outside the game's dead zone so slow movement still turns the camera.
            double mag = 0.26 + a * 0.74;
            out = int16_t((v < 0 ? -mag : mag) * 32767);
        };
        toStick(s_x, p.thumbRX);
        toStick(-s_y, p.thumbRY);
    }
}

static void AddKeyboard(input::PadState& p)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!fg || pid != GetCurrentProcessId())
    {
        MouseCamera(nullptr, false, p);
        return;
    }

    auto press = [&](bool down, uint16_t button) { if (down) p.buttons |= button; };
    constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010,
        kBack = 0x0020, kLS = 0x0040, kRS = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000,
        kX = 0x4000, kY = 0x8000;

    const uint32_t screen = guest::r32(kMenuScreen);
    const bool frontEnd = guest::r32(kPlayerPtr) == 0;
    const bool creation = screen == kScreenCreation || guest::r32(kCreationFlag) == 1;
    const bool menu = frontEnd || creation || screen != 0;

    // Esc pauses while playing and goes back inside menus. The press that opened a menu has to be
    // released before it counts as "back", or the menu would close again at once.
    static bool s_escReleased = true;
    const bool esc = KeyDown(VK_ESCAPE);
    if (!menu) s_escReleased = false;
    else if (!esc) s_escReleased = true;

    press(KeyDown('M') || (esc && !menu), kStart);
    press(KeyDown(VK_TAB) && !creation, kBack);
    press(KeyDown(VK_RETURN), kA);
    press(KeyDown(VK_BACK) || (esc && menu && s_escReleased), kB);
    press(KeyDown(VK_UP), kUp);
    press(KeyDown(VK_DOWN), kDown);
    press(KeyDown(VK_LEFT), kLeft);
    press(KeyDown(VK_RIGHT), kRight);

    if (menu)
    {
        MouseCamera(nullptr, false, p);
        press(KeyDown('W'), kUp);
        press(KeyDown('S'), kDown);
        press(KeyDown('A'), kLeft);
        press(KeyDown('D'), kRight);
        press(KeyDown(VK_LBUTTON) || KeyDown(VK_SPACE), kA);
        press(KeyDown(VK_RBUTTON), kB);
        press(KeyDown('X'), kX);
        press(KeyDown('Y'), kY);
        if (KeyDown('Q')) p.leftTrigger = 255;
        if (KeyDown('E')) p.rightTrigger = 255;
        return;
    }

    MouseCamera(fg, !KeyDown(VK_MENU), p); // hold Alt to free the mouse

    // Cutscenes: a click or Space skips (Y, or A in some scenes).
    if (guest::r8(0x8370D991) || guest::r8(0x8370D990))
    {
        if (KeyDown(VK_LBUTTON) || KeyDown(VK_SPACE)) p.buttons |= kY | kA;
        return;
    }

    if (KeyDown('D') != KeyDown('A')) p.thumbLX = KeyDown('D') ? 32767 : -32767;
    press(KeyDown('Q'), kB);
    press(KeyDown('E'), kY);
    if (KeyDown(VK_LBUTTON)) p.rightTrigger = 255;
    press(KeyDown(VK_MBUTTON) || KeyDown('V'), kRS);

    if (PlayerInVehicle())
    {
        press(KeyDown('W'), kA);                    // accelerate
        press(KeyDown('S'), kX);                    // brake / reverse
        if (KeyDown(VK_SPACE)) p.leftTrigger = 255; // handbrake
        press(KeyDown(VK_SHIFT), kRS);              // nitrous
        press(KeyDown(VK_CONTROL), kLS);            // hydraulics
        press(KeyDown('Z') || KeyDown('X'), kLB);
        press(KeyDown('C') || KeyDown('X'), kRB);
    }
    else
    {
        if (KeyDown('W') != KeyDown('S')) p.thumbLY = KeyDown('W') ? 32767 : -32767;
        if (KeyDown(VK_RBUTTON)) p.leftTrigger = 255; // secondary attack
        press(KeyDown(VK_SPACE), kX);               // jump
        press(KeyDown(VK_SHIFT), kRB);              // sprint
        press(KeyDown('R'), kA);                    // reload / pick up
        press(KeyDown('F'), kLB);                   // kick
        press(KeyDown('C'), kLS);                   // crouch
    }
}

// Polls the first XInput controller and the keyboard into the shared pad state (Windows test builds).
static void StartXInputPolling()
{
    // SR_AUTOPRESS=<seconds>[button][,...]: press a button at those times (for testing without a
    // pad). Button: A (default), B, X, Y, S (Start), K (Back). Example: 14S,20A,45Y
    std::vector<std::pair<double, uint16_t>> autoPress;
    if (const char* env = std::getenv("SR_AUTOPRESS"))
        for (const char* p = env; *p;)
        {
            char* end = nullptr;
            double at = std::strtod(p, &end);
            uint16_t button = 0x1000; // A
            switch (*end)
            {
            case 'B': button = 0x2000; break;
            case 'X': button = 0x4000; break;
            case 'Y': button = 0x8000; break;
            case 'S': button = 0x0010; break;
            case 'K': button = 0x0020; break;
            }
            autoPress.emplace_back(at, button);
            p = end;
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    std::thread([autoPress] {
        auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            for (auto [at, button] : autoPress)
                if (t >= at && t < at + 0.3)
                {
                    input::PadState p;
                    p.buttons = button;
                    input::SetPad(p);
                    goto next;
                }
            {
                input::PadState p;
                XINPUT_STATE st{};
                if (XInputGetState(0, &st) == ERROR_SUCCESS)
                {
                    p.buttons = st.Gamepad.wButtons;
                    p.leftTrigger = st.Gamepad.bLeftTrigger;
                    p.rightTrigger = st.Gamepad.bRightTrigger;
                    p.thumbLX = st.Gamepad.sThumbLX;
                    p.thumbLY = st.Gamepad.sThumbLY;
                    p.thumbRX = st.Gamepad.sThumbRX;
                    p.thumbRY = st.Gamepad.sThumbRY;
                }
                AddKeyboard(p);
                input::SetPad(p);
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
