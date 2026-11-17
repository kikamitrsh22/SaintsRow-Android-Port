package com.saintsrow.port;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
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

        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
        hideSystemBars();
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
            NativeBridge.start(gameDir.getAbsolutePath(), userDir.getAbsolutePath(), holder.getSurface());
        } else {
            NativeBridge.surfaceChanged(holder.getSurface());
        }
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
