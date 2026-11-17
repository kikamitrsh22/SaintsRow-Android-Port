// XAudio render driver clients: one host thread per client calls the game's render callback
// whenever the output device has room for another frame; the callback mixes and submits the frame
// through XAudioSubmitRenderDriverFrame.
#include "audio.h"

#include "guest.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "xglue/xglue.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace audio
{
    namespace
    {
        constexpr int kMaxClients = 8;

        struct Client
        {
            std::atomic<bool> running{ false };
            std::thread thread;
            int device = -1;
        };
        std::mutex s_lock;
        Client s_clients[kMaxClients];
        bool s_available = false;
    }

    void Init()
    {
        s_available = xglue::InitAudio();
        LOG("audio: %s", s_available ? "XMA decoder ready" : "unavailable - sound is silent");
    }

    int RegisterClient(uint32_t callback, uint32_t callbackArg)
    {
        if (!s_available) return -1;
        std::lock_guard lock(s_lock);
        int index = -1;
        for (int i = 0; i < kMaxClients; i++)
            if (!s_clients[i].running) { index = i; break; }
        if (index < 0) return -1;

        int device = xglue::AudioOpen();
        if (device < 0) return -1;

        // The callback gets a pointer to a dword holding its argument, as on the console.
        uint32_t wrappedArg = mem::SystemAllocZeroed(4);
        guest::w32(wrappedArg, callbackArg);

        Client& c = s_clients[index];
        if (c.thread.joinable()) c.thread.join();
        c.device = device;
        c.running = true;
        c.thread = std::thread([&c, callback, wrappedArg, index] {
            auto host = kernel::CreateHostContext("audio client");
            kernel::BindCurrentThread(host.get());
            PPCContext& ctx = *host->ctx;
            LOG("audio: client %d running (callback %08X)", index, callback);
            while (c.running)
            {
                if (!xglue::AudioWaitSlot(c.device, 100)) continue;
                if (!c.running) break;
                ctx.r1.u64 = host->stackBase - 0x100;
                ctx.r3.u64 = wrappedArg;
                GuestCall(ctx, callback);
            }
        });
        LOG("audio: render client %d registered (device %d)", index, device);
        return index;
    }

    void UnregisterClient(int index)
    {
        if (index < 0 || index >= kMaxClients) return;
        std::lock_guard lock(s_lock);
        Client& c = s_clients[index];
        if (!c.running) return;
        c.running = false;
        if (c.thread.joinable() && c.thread.get_id() != std::this_thread::get_id()) c.thread.join();
        xglue::AudioClose(c.device);
        c.device = -1;
    }

    void SubmitFrame(int index, uint32_t samples)
    {
        if (index < 0 || index >= kMaxClients) return;
        xglue::AudioSubmit(s_clients[index].device, samples);
    }

    uint32_t CreateXmaContext() { return s_available ? xglue::XmaCreateContext() : 0; }
    void ReleaseXmaContext(uint32_t context) { xglue::XmaReleaseContext(context); }
}
