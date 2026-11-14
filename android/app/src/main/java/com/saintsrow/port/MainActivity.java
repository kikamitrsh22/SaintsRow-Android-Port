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
