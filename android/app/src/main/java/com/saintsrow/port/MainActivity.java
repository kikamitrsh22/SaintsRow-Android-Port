package com.saintsrow.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.Surface;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Full-screen Vulkan surface for the game. Game files are read from the app's external files
 * directory (Android/data/com.saintsrow.port/files/game), which needs no storage permission and
 * can be filled with `adb push`.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "SaintsRow";
    private boolean started;
    private final Gamepad gamepad = new Gamepad();
    private File gameDir;
    private File userDir;
    private SurfaceView surfaceView;

    // Graphics menu: the small "MENU" button in the top right corner, or L3 + R3 on a controller.
    private static final int[] FPS_CHOICES = { 30, 60, 120 };
    private static final String[] AA_LABELS = { "Off", "FXAA", "FXAA Extreme (sharper edges, slower)" };
    private static final String[] FG_LABELS = { "Off", "On (ZeroFG: 60 FPS shown as 120 on a 120 Hz screen)" };
    private static final String[] RES_LABELS = {
            "1280 x 720 (native)", "2560 x 1440 (2x, sharper, heavier)", "3840 x 2160 (3x, heaviest)" };
    private static final String PREFS = "settings", PREF_FPS = "fps_cap", PREF_AA = "anti_aliasing",
            PREF_FG = "frame_generation", PREF_RES = "resolution_scale", PREF_OVERLAY = "perf_overlay";
    private static final String[] OVERLAY_LABELS = { "Off", "Simple (FPS, frame time)",
            "Detailed (FPS, 1% low, CPU, GPU, temperatures, RAM)" };
    private int overlayMode = 0;
    private PerfOverlay perfOverlay;
    private int fpsCap = 60, antiAliasing = 0, frameGeneration = 0, frameGenerationAtStart = 0;
    private int resolutionScale = 1, resolutionScaleAtStart = 1;
    private boolean thumbL, thumbR;
    private AlertDialog menuDialog;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        File base = getExternalFilesDir(null);
        gameDir = new File(base, "game");
        userDir = new File(base, "userdata");
        gameDir.mkdirs();
        userDir.mkdirs();

        if (!new File(gameDir, "default.xex").isFile()) {
            showMissingGame();
            return;
        }

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        fpsCap = prefs.getInt(PREF_FPS, 60);
        antiAliasing = prefs.getInt(PREF_AA, 0);
        frameGeneration = frameGenerationAtStart = prefs.getInt(PREF_FG, 0);
        resolutionScale = resolutionScaleAtStart = Math.max(1, Math.min(3, prefs.getInt(PREF_RES, 1)));
        overlayMode = prefs.getInt(PREF_OVERLAY, 0);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        FrameLayout root = new FrameLayout(this);
        root.addView(surfaceView);

        Button fpsButton = new Button(this);
        fpsButton.setText("MENU");
        fpsButton.setAlpha(0.35f);
        fpsButton.setFocusable(false); // never steals the controller's focus from the game
        fpsButton.setOnClickListener(v -> showMenu());
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        lp.setMargins(0, 16, 16, 0);
        root.addView(fpsButton, lp);

        TextView overlayText = new TextView(this);
        FrameLayout.LayoutParams olp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        olp.setMargins(16, 16, 0, 0);
        root.addView(overlayText, olp);
        perfOverlay = new PerfOverlay(overlayText);
        perfOverlay.setMode(0); // shown once the game runs (surfaceCreated)

        setContentView(root);
        applyDisplayRate();
        hideSystemBars();
    }

    /** Graphics menu: frame rate limit, anti-aliasing and frame generation, remembered between runs. */
    private void showMenu() {
        if (menuDialog != null && menuDialog.isShowing()) return;
        NativeBridge.setPad(0, 0, 0, 0, 0, 0, 0); // release whatever the controller was holding
        String fg = FG_LABELS[frameGeneration].split(" \\(")[0]
                + (frameGeneration != frameGenerationAtStart ? " (after restart)" : "");
        String res = RES_LABELS[resolutionScale - 1].split(" \\(")[0]
                + (resolutionScale != resolutionScaleAtStart ? " (after restart)" : "");
        String[] items = {
                "Frame rate limit: " + fpsCap + " FPS",
                "Resolution: " + res,
                "Anti-aliasing: " + AA_LABELS[antiAliasing].split(" \\(")[0],
                "Frame generation: " + fg,
                "Performance overlay: " + OVERLAY_LABELS[overlayMode].split(" \\(")[0],
        };
        menuDialog = dialog("Graphics")
                .setItems(items, (d, which) -> {
                    if (which == 0) showFpsChoice();
                    else if (which == 1) showResolutionChoice();
                    else if (which == 2) showAaChoice();
                    else if (which == 3) showFgChoice();
                    else showOverlayChoice();
                })
                .setNegativeButton("Close", null)
                .create();
        menuDialog.show();
    }

    private AlertDialog.Builder dialog(String title) {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setOnDismissListener(d -> hideSystemBars());
    }

    private void showFpsChoice() {
        String[] labels = new String[FPS_CHOICES.length];
        int checked = 1;
        for (int i = 0; i < FPS_CHOICES.length; i++) {
            labels[i] = FPS_CHOICES[i] + " FPS" + (FPS_CHOICES[i] == 120 ? "  (needs a 120 Hz screen)" : "");
            if (FPS_CHOICES[i] == fpsCap) checked = i;
        }
        dialog("Frame rate limit").setSingleChoiceItems(labels, checked, (d, which) -> {
            setFpsCap(FPS_CHOICES[which]);
            d.dismiss();
        }).setNegativeButton("Back", null).show();
    }

    private void showResolutionChoice() {
        dialog("Resolution").setSingleChoiceItems(RES_LABELS, resolutionScale - 1, (d, which) -> {
            resolutionScale = which + 1;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_RES, resolutionScale).apply();
            if (resolutionScale != resolutionScaleAtStart)
                Toast.makeText(this, "Resolution changes after the game is restarted", Toast.LENGTH_LONG).show();
            d.dismiss();
        }).setNegativeButton("Back", null).show();
    }

    private void showOverlayChoice() {
        dialog("Performance overlay").setSingleChoiceItems(OVERLAY_LABELS, overlayMode, (d, which) -> {
            overlayMode = which;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_OVERLAY, which).apply();
            if (started) perfOverlay.setMode(which);
            d.dismiss();
        }).setNegativeButton("Back", null).show();
    }

    private void showAaChoice() {
        dialog("Anti-aliasing").setSingleChoiceItems(AA_LABELS, antiAliasing, (d, which) -> {
            antiAliasing = which;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_AA, which).apply();
            if (started) NativeBridge.setAntiAliasing(which);
            d.dismiss();
        }).setNegativeButton("Back", null).show();
    }

    private void showFgChoice() {
        dialog("Frame generation").setSingleChoiceItems(FG_LABELS, frameGeneration, (d, which) -> {
            frameGeneration = which;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FG, which).apply();
            applyDisplayRate();
            if (which != frameGenerationAtStart)
                Toast.makeText(this, "Frame generation changes after the game is restarted", Toast.LENGTH_LONG).show();
            d.dismiss();
        }).setNegativeButton("Back", null).show();
    }

    private void setFpsCap(int fps) {
        fpsCap = fps;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FPS, fps).apply();
        if (started) NativeBridge.setFpsCap(fps);
        applyDisplayRate();
        Log.i(TAG, "frame rate cap " + fps);
    }

    /**
     * Screen refresh rate to go with the cap: 120 Hz for 120 FPS (when the screen has such a mode),
     * otherwise 60 Hz, which also saves power at 30 and 60 FPS.
     */
    private void applyDisplayRate() {
        // Frame generation shows each game frame twice as often: it needs the 120 Hz mode.
        float want = fpsCap > 60 || frameGenerationAtStart != 0 ? 120f : 60f;
        Display display = getWindow().getDecorView().getDisplay();
        if (display == null) display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode(), best = null;
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth()
                    || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
            if (best == null || Math.abs(m.getRefreshRate() - want) < Math.abs(best.getRefreshRate() - want))
                best = m;
        }
        if (best != null) {
            WindowManager.LayoutParams p = getWindow().getAttributes();
            p.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(p);
            Log.i(TAG, "display mode " + best.getRefreshRate() + " Hz for " + fpsCap + " FPS");
        }
        if (Build.VERSION.SDK_INT >= 30 && surfaceView != null) {
            Surface s = surfaceView.getHolder().getSurface();
            if (s != null && s.isValid())
                s.setFrameRate(want, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        }
    }

    /**
     * Optional custom Vulkan driver (an adrenotools package such as Turnip: meta.json plus the
     * driver .so) placed in Android/data/com.saintsrow.port/files/driver. Libraries can only be
     * loaded from the app's private storage, so the folder is copied there first.
     *
     * @return the path of the driver library to load, or "" for the system driver.
     */
    private String prepareCustomDriver() {
        try {
            File source = new File(getExternalFilesDir(null), "driver");
            File target = new File(getFilesDir(), "driver");
            source.mkdirs();
            String library = null;
            File meta = new File(source, "meta.json");
            if (meta.isFile()) {
                String json = new String(readAll(meta), StandardCharsets.UTF_8);
                Matcher m = Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"/]+)\"").matcher(json);
                if (m.find()) library = m.group(1);
            }
            File[] files = source.listFiles();
            if (library == null && files != null) {
                for (File f : files) if (f.getName().endsWith(".so")) library = f.getName();
            }
            if (library == null || !new File(source, library).isFile()) {
                Log.i(TAG, "no custom Vulkan driver in " + source + ", using the system driver");
                return "";
            }
            target.mkdirs();
            for (File f : files) {
                if (!f.isFile()) continue;
                File out = new File(target, f.getName());
                if (out.length() == f.length() && out.lastModified() >= f.lastModified()) continue;
                try (FileOutputStream os = new FileOutputStream(out)) { os.write(readAll(f)); }
            }
            File lib = new File(target, library);
            Log.i(TAG, "custom Vulkan driver: " + lib);
            return lib.getAbsolutePath();
        } catch (Exception e) {
            Log.w(TAG, "custom Vulkan driver not usable, using the system driver", e);
            return "";
        }
    }

    private static byte[] readAll(File f) throws java.io.IOException {
        byte[] data = new byte[(int) f.length()];
        try (FileInputStream in = new FileInputStream(f)) {
            int off = 0;
            while (off < data.length) {
                int n = in.read(data, off, data.length - off);
                if (n < 0) break;
                off += n;
            }
        }
        return data;
    }

    private void showMissingGame() {
        TextView text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setBackgroundColor(Color.BLACK);
        text.setGravity(Gravity.CENTER);
        text.setPadding(48, 48, 48, 48);
        text.setTextSize(16);
        text.setText("Game files not found.\n\nCopy your own Saints Row dump (the folder that contains default.xex) to:\n\n"
                + gameDir.getAbsolutePath()
                + "\n\nFrom a PC:\nadb push \"<your Saints Row folder>/.\" " + gameDir.getAbsolutePath()
                + "\n\nThen start the app again.");
        setContentView(text);
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // Controller input goes to the game; everything else keeps its normal behaviour.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // L3 + R3 together open the frame rate menu.
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_THUMBL || code == KeyEvent.KEYCODE_BUTTON_THUMBR) {
            boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
            if (code == KeyEvent.KEYCODE_BUTTON_THUMBL) thumbL = down; else thumbR = down;
            if (started && thumbL && thumbR && event.getRepeatCount() == 0) {
                thumbL = thumbR = false;
                showMenu();
                return true;
            }
        }
        if (started && gamepad.onKey(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (started && gamepad.onMotion(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && started) hideSystemBars();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (!started) {
            started = true;
            Log.i(TAG, "starting: game=" + gameDir + " user=" + userDir);
            NativeBridge.setDriver(getApplicationInfo().nativeLibraryDir, prepareCustomDriver());
            NativeBridge.setFpsCap(fpsCap);
            NativeBridge.setAntiAliasing(antiAliasing);
            NativeBridge.setFrameGeneration(frameGenerationAtStart);
            NativeBridge.setResolutionScale(resolutionScaleAtStart);
            NativeBridge.start(gameDir.getAbsolutePath(), userDir.getAbsolutePath(), holder.getSurface());
            perfOverlay.setMode(overlayMode);
        } else {
            NativeBridge.surfaceChanged(holder.getSurface());
        }
        applyDisplayRate();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surface " + width + "x" + height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (started) NativeBridge.surfaceDestroyed();
    }
}
