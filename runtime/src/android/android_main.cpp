// Android entry points (JNI) for libsaintsrow.so. Java side: com.saintsrow.port.NativeBridge.
#include "../guest.h"
#include "../gpu/gpu.h"
#include "../input.h"
#include "../audio.h"
#include "../perf_hint.h"
#include "../kernel/kernel.h"
#include "../loader.h"
#include "../memory.h"
#include "../vfs.h"
#include "../xglue/xglue.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace rt
{
    static std::mutex s_logLock;
    static auto s_start = std::chrono::steady_clock::now();
    static std::FILE* s_logFile = nullptr;

    void Log(const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        __android_log_print(ANDROID_LOG_INFO, "SaintsRow", "[t%02u] %s", CurrentThreadId(), buf);
        std::lock_guard lock(s_logLock);
        if (s_logFile)
        {
            std::fprintf(s_logFile, "[%9.3f][t%02u] %s\n", t, CurrentThreadId(), buf);
            std::fflush(s_logFile);
        }
    }
}

static std::atomic<bool> s_started{ false };

static std::string JStr(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(s, c);
    return r;
}

static void UiThreadMain(std::string gameDir, std::string userDir)
{
    std::filesystem::create_directories(userDir);
    rt::s_logFile = std::fopen((std::filesystem::path(userDir) / "saintsrow.log").string().c_str(), "w");
    LOG("Saints Row native runtime (Android) - game=%s user=%s", gameDir.c_str(), userDir.c_str());
    perf::SetConfigDir(userDir.c_str());

    if (!mem::Init())
    {
        LOG("failed to reserve guest memory");
        return;
    }
    vfs::Init(gameDir, userDir);
    if (!loader::Load(std::filesystem::path(gameDir) / "default.xex"))
        return;
    xglue::SetGpuConfigFile((std::filesystem::path(userDir) / "gpu.toml").string().c_str());
    audio::Init();
    if (!xglue::InitGraphics(gpu::DispatchInterrupt))
    {
        LOG("failed to initialize Vulkan");
        return;
    }

    std::thread game([] {
        kernel::RunMainThread(loader::g_title.entryPoint, std::max<uint32_t>(loader::g_title.stackSize, 0x100000));
        LOG("main thread returned");
        xglue::RequestQuit();
    });
    game.detach();
    xglue::RunUiLoop();
    LOG("UI loop exited");
}

extern "C" JNIEXPORT void JNICALL Java_com_saintsrow_port_NativeBridge_setDriver(
    JNIEnv* env, jclass, jstring nativeLibDir, jstring driverLibrary)
{
    xglue::SetCustomDriver(JStr(env, nativeLibDir).c_str(), JStr(env, driverLibrary).c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_saintsrow_port_NativeBridge_start(
    JNIEnv* env, jclass, jstring gameDir, jstring userDir, jobject surface)
{
    if (s_started.exchange(true)) return;
    xglue::SetNativeWindow(ANativeWindow_fromSurface(env, surface));
    std::thread(UiThreadMain, JStr(env, gameDir), JStr(env, userDir)).detach();
}

extern "C" JNIEXPORT void JNICALL Java_com_saintsrow_port_NativeBridge_surfaceChanged(
    JNIEnv* env, jclass, jobject surface)
{
    xglue::SetNativeWindow(ANativeWindow_fromSurface(env, surface));
    xglue::OnSurfaceChanged();
}

extern "C" JNIEXPORT void JNICALL Java_com_saintsrow_port_NativeBridge_surfaceDestroyed(JNIEnv*, jclass)
{
    xglue::OnSurfaceDestroyed();
}

// Gamepad state from MainActivity (XINPUT_GAMEPAD layout, already in XInput conventions).
extern "C" JNIEXPORT void JNICALL Java_com_saintsrow_port_NativeBridge_setPad(
    JNIEnv*, jclass, jint buttons, jint leftTrigger, jint rightTrigger, jint lx, jint ly, jint rx, jint ry)
{
    input::PadState p;
    p.buttons = uint16_t(buttons);
    p.leftTrigger = uint8_t(leftTrigger);
    p.rightTrigger = uint8_t(rightTrigger);
    p.thumbLX = int16_t(lx);
    p.thumbLY = int16_t(ly);
    p.thumbRX = int16_t(rx);
    p.thumbRY = int16_t(ry);
    input::SetPad(p);
}
