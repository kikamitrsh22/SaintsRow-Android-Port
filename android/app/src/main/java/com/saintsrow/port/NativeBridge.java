package com.saintsrow.port;

import android.view.Surface;

/** JNI entry points implemented in libsaintsrow.so (runtime/src/android/android_main.cpp). */
public final class NativeBridge {
    static {
        System.loadLibrary("saintsrow");
    }

    private NativeBridge() {}

    /** Boots the game once: guest memory, XEX load, Vulkan GPU, then the game's main thread. */
    public static native void start(String gameDir, String userDir, Surface surface);

    /** The Surface was recreated (e.g. after the app returns to the foreground). */
    public static native void surfaceChanged(Surface surface);

    /** The Surface is going away; the presenter detaches from it. */
    public static native void surfaceDestroyed();

    /**
     * Controller state in XInput conventions: XINPUT_GAMEPAD button bits, triggers 0..255,
     * thumbsticks -32768..32767 with +Y pointing up.
     */
    public static native void setPad(int buttons, int leftTrigger, int rightTrigger,
                                     int lx, int ly, int rx, int ry);
}
