package com.saintsrow.port;

import android.view.Surface;

/** JNI entry points implemented in libsaintsrow.so (runtime/src/android/android_main.cpp). */
public final class NativeBridge {
    static {
        System.loadLibrary("saintsrow");
    }

    private NativeBridge() {}

    /**
     * Call before {@link #start}: where the app's native libraries are, and the custom Vulkan
     * driver library to load through adrenotools (empty = the system driver).
     */
    public static native void setDriver(String nativeLibDir, String driverLibrary);

    /** Frame rate cap: 30, 60 or 120 (the guest vblank rate the game presents on). Any time. */
    public static native void setFpsCap(int fps);

    /** Post-processing anti-aliasing: 0 off, 1 FXAA, 2 FXAA extreme. Any time. */
    public static native void setAntiAliasing(int mode);

    /** Frame generation (ZeroFG): 0 off, 1 on. Read at start; changes apply on the next start. */
    public static native void setFrameGeneration(int mode);

    /** Render resolution scale: 1 (720p), 2 (1440p), 3 (2160p). Read at start. */
    public static native void setResolutionScale(int scale);

    /**
     * Game frame statistics over the last `windowMs`: out = { fps, average ms, 99th percentile ms,
     * longest ms, frames }.
     */
    public static native void getFrameStats(int windowMs, float[] out);

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
