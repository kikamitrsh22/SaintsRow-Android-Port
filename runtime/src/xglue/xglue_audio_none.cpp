// xglue audio for builds without the Xenia APU (Windows for now): the game runs silently.
#include "xglue.h"

namespace xglue
{
    bool InitAudio() { return false; }
    uint32_t XmaCreateContext() { return 0; }
    void XmaReleaseContext(uint32_t) {}
    int AudioOpen() { return -1; }
    bool AudioWaitSlot(int, uint32_t) { return false; }
    void AudioSubmit(int, uint32_t) {}
    void AudioClose(int) {}
    void AudioPause(bool) {}
}
