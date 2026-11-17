// xglue, audio half: Xenia's XMA decoder (hardware XMA contexts behind MMIO registers at
// 0x7FEA0000) plus the host output driver the game's XAudio render client feeds.
//
// The game mixes in software: every 256 samples (5.3 ms at 48 kHz) its render callback runs and
// submits one frame of 6 x 256 big-endian floats (5.1). The output driver releases one credit on
// a semaphore for each frame it has played, which paces the callback.
#include "xglue.h"
#include "xglue_internal.h"

#include <memory>
#include <mutex>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xglue
{
    // Platform half (xglue_android.cpp / xglue_win.cpp): the output device for one client.
    xe::apu::AudioDriver* CreateAudioDriver(xe::Memory* memory, xe::threading::Semaphore* semaphore);

    static std::unique_ptr<xe::apu::XmaDecoder> s_xma;

    // Frames the game may run ahead of the device (Xenia's apu_max_queued_frames default).
    constexpr uint32_t kQueuedFrames = 8;
    constexpr int kMaxClients = 8;

    struct AudioClient
    {
        std::unique_ptr<xe::threading::Semaphore> semaphore;
        xe::apu::AudioDriver* driver = nullptr;
    };
    static std::mutex s_clientLock;
    static AudioClient s_clients[kMaxClients];

    bool InitAudio()
    {
        s_xma = std::make_unique<xe::apu::XmaDecoder>(XMemory());
        if (uint32_t(s_xma->Setup(nullptr)) != 0)
        {
            XELOGE("xglue: XMA decoder setup failed");
            s_xma.reset();
            return false;
        }
        return true;
    }

    uint32_t XmaCreateContext() { return s_xma ? s_xma->AllocateContext() : 0; }

    void XmaReleaseContext(uint32_t context)
    {
        if (s_xma && context) s_xma->ReleaseContext(context);
    }

    int AudioOpen()
    {
        std::lock_guard lock(s_clientLock);
        for (int i = 0; i < kMaxClients; i++)
        {
            AudioClient& c = s_clients[i];
            if (c.driver) continue;
            c.semaphore = xe::threading::Semaphore::Create(0, kQueuedFrames);
            c.driver = CreateAudioDriver(XMemory(), c.semaphore.get());
            if (!c.driver || !c.driver->Initialize())
            {
                XELOGE("xglue: audio output could not be opened");
                if (c.driver) c.driver->Shutdown();
                delete c.driver;
                c.driver = nullptr;
                c.semaphore.reset();
                return -1;
            }
            c.semaphore->Release(kQueuedFrames, nullptr);
            return i;
        }
        return -1;
    }

    bool AudioWaitSlot(int index, uint32_t timeoutMs)
    {
        if (index < 0 || index >= kMaxClients || !s_clients[index].semaphore) return false;
        return xe::threading::Wait(s_clients[index].semaphore.get(), false,
                   std::chrono::milliseconds(timeoutMs)) == xe::threading::WaitResult::kSuccess;
    }

    void AudioSubmit(int index, uint32_t samples)
    {
        if (index < 0 || index >= kMaxClients || !s_clients[index].driver || !samples) return;
        s_clients[index].driver->SubmitFrame(XMemory()->TranslateVirtual<float*>(samples));
    }

    void AudioClose(int index)
    {
        std::lock_guard lock(s_clientLock);
        if (index < 0 || index >= kMaxClients || !s_clients[index].driver) return;
        s_clients[index].driver->Shutdown();
        delete s_clients[index].driver;
        s_clients[index].driver = nullptr;
        s_clients[index].semaphore.reset();
    }

    void AudioPause(bool paused)
    {
        std::lock_guard lock(s_clientLock);
        for (auto& c : s_clients)
            if (c.driver) paused ? c.driver->Pause() : c.driver->Resume();
    }
}
