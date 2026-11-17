// xglue, Windows platform half: Win32 window + VulkanGraphicsSystem.
#include "xglue.h"
#include "xglue_internal.h"

#include <cstdlib>
#include <memory>

#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context_win.h"

namespace xglue
{
    // Windows: the vblank rate stays at 60 Hz (the patched Xenia here has no setter yet).
    void SetFrameRateCap(int) {}
    int FrameRateCap() { return 0; }
    void SetAntiAliasing(int) {}
    void SetFrameGeneration(int) {}
    void SetResolutionScale(int) {}
    static std::unique_ptr<xe::ui::Win32WindowedAppContext> s_app;
    static std::unique_ptr<xe::ui::Window> s_window;
    static std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> s_gpu;

    // ------------------------------------------------------------------------
    // Window + GPU
    // ------------------------------------------------------------------------

    class QuitListener final : public xe::ui::WindowListener
    {
    public:
        void OnClosing(xe::ui::UIEvent& e) override
        {
            s_app->QuitFromUIThread();
            std::exit(0);
        }
    };
    static QuitListener s_quitListener;

    bool InitGraphics(InterruptHook hook)
    {
        s_app = std::make_unique<xe::ui::Win32WindowedAppContext>(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
        if (!s_app->Initialize())
        {
            XELOGE("xglue: app context init failed");
            return false;
        }

        s_window = xe::ui::Window::Create(*s_app, "Saints Row (native)", 1280, 720);
        s_window->AddListener(&s_quitListener);
        if (!s_window->Open())
        {
            XELOGE("xglue: window open failed");
            return false;
        }

        s_gpu = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
        s_gpu->SetHostMode(XMemory(), std::move(hook));
        if (s_gpu->Setup(nullptr, nullptr, s_app.get(), true) != 0)
        {
            XELOGE("xglue: graphics system setup failed");
            return false;
        }
        s_window->SetPresenter(s_gpu->presenter());
        g_gpu = s_gpu.get();
        return true;
    }

    int RunUiLoop() { return s_app->RunMainMessageLoop(); }

    void RequestQuit()
    {
        if (s_app) s_app->CallInUIThread([] { s_app->QuitFromUIThread(); });
    }

}
