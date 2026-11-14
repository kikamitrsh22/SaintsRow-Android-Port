package com.saintsrow.port;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Turns Android gamepad events (built-in controller of handhelds like the AYN Odin 3, or a Bluetooth
 * pad) into one XInput-style state and pushes it to native code whenever it changes.
 */
final class Gamepad {
    // XINPUT_GAMEPAD button bits
    static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
    static final int START = 0x0010, BACK = 0x0020, LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080;
    static final int LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200, GUIDE = 0x0400;
    static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    private int motionLogs;
    private int keyButtons;      // from KeyEvents
    private int hatButtons;      // d-pad reported as AXIS_HAT_X/Y
    private int leftTriggerKey;  // L2/R2 reported as keys (digital)
    private int rightTriggerKey;
    private int leftTriggerAxis, rightTriggerAxis;
    private int lx, ly, rx, ry;

    private int loggedButtons = -1, loggedLt = -1, loggedRt = -1;
    private int sentButtons = -1, sentLt = -1, sentRt = -1, sentLx, sentLy, sentRx, sentRy;

    static boolean isGamepad(InputDevice device) {
        if (device == null) return false;
        int s = device.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return A;
            case KeyEvent.KEYCODE_BUTTON_B: return B;
            case KeyEvent.KEYCODE_BUTTON_X: return X;
            case KeyEvent.KEYCODE_BUTTON_Y: return Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return LEFT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1: return RIGHT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return RIGHT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_START: return START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return BACK;
            case KeyEvent.KEYCODE_BACK: return BACK;         // some pads send BACK for Select
            case KeyEvent.KEYCODE_BUTTON_MODE: return GUIDE;
            case KeyEvent.KEYCODE_DPAD_UP: return DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return DPAD_RIGHT;
            default: return 0;
        }
    }

    /** Returns true if the event was a controller event we consumed. */
    boolean onKey(KeyEvent e) {
        if (!isGamepad(e.getDevice()) && !KeyEvent.isGamepadButton(e.getKeyCode())) return false;
        android.util.Log.i("SaintsRowInput", "key " + KeyEvent.keyCodeToString(e.getKeyCode())
                + (e.getAction() == KeyEvent.ACTION_DOWN ? " down" : " up")
                + " repeat=" + e.getRepeatCount() + " dev=" + (e.getDevice() != null ? e.getDevice().getName() : "none"));
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_L2) {
            leftTriggerKey = down ? 255 : 0;
        } else if (code == KeyEvent.KEYCODE_BUTTON_R2) {
            rightTriggerKey = down ? 255 : 0;
        } else {
            int bit = buttonFor(code);
            if (bit == 0) return false;
            keyButtons = down ? (keyButtons | bit) : (keyButtons & ~bit);
        }
        push();
        return true;
    }

    private static float axis(MotionEvent e, int axis) {
        InputDevice.MotionRange range = e.getDevice() != null ? e.getDevice().getMotionRange(axis, e.getSource()) : null;
        float v = e.getAxisValue(axis);
        if (range != null && Math.abs(v) < range.getFlat()) v = 0;
        return v;
    }

    private static int stick(float v) {
        return Math.max(-32768, Math.min(32767, Math.round(v * 32767f)));
    }

    private static int trigger(float v) {
        return Math.max(0, Math.min(255, Math.round(v * 255f)));
    }

    /** Returns true if the event was a controller motion event we consumed. */
    boolean onMotion(MotionEvent e) {
        if (!isGamepad(e.getDevice()) || e.getAction() != MotionEvent.ACTION_MOVE) return false;
        if (motionLogs < 40) {
            motionLogs++;
            android.util.Log.i("SaintsRowInput", "motion dev=" + e.getDevice().getName() + " x=" + e.getAxisValue(MotionEvent.AXIS_X)
                    + " y=" + e.getAxisValue(MotionEvent.AXIS_Y) + " lt=" + e.getAxisValue(MotionEvent.AXIS_LTRIGGER)
                    + " rt=" + e.getAxisValue(MotionEvent.AXIS_RTRIGGER) + " hatx=" + e.getAxisValue(MotionEvent.AXIS_HAT_X)
                    + " haty=" + e.getAxisValue(MotionEvent.AXIS_HAT_Y) + " brake=" + e.getAxisValue(MotionEvent.AXIS_BRAKE)
                    + " gas=" + e.getAxisValue(MotionEvent.AXIS_GAS));
        }
        lx = stick(axis(e, MotionEvent.AXIS_X));
        ly = stick(-axis(e, MotionEvent.AXIS_Y));   // Android: +Y is down; XInput: +Y is up
        rx = stick(axis(e, MotionEvent.AXIS_Z));
        ry = stick(-axis(e, MotionEvent.AXIS_RZ));
        leftTriggerAxis = trigger(Math.max(axis(e, MotionEvent.AXIS_LTRIGGER), axis(e, MotionEvent.AXIS_BRAKE)));
        rightTriggerAxis = trigger(Math.max(axis(e, MotionEvent.AXIS_RTRIGGER), axis(e, MotionEvent.AXIS_GAS)));

        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hatButtons = 0;
        if (hx < -0.5f) hatButtons |= DPAD_LEFT;
        if (hx > 0.5f) hatButtons |= DPAD_RIGHT;
        if (hy < -0.5f) hatButtons |= DPAD_UP;
        if (hy > 0.5f) hatButtons |= DPAD_DOWN;
        push();
        return true;
    }

    private void push() {
        int buttons = keyButtons | hatButtons;
        int lt = Math.max(leftTriggerKey, leftTriggerAxis);
        int rt = Math.max(rightTriggerKey, rightTriggerAxis);
        if (buttons == sentButtons && lt == sentLt && rt == sentRt
                && lx == sentLx && ly == sentLy && rx == sentRx && ry == sentRy) return;
        sentButtons = buttons; sentLt = lt; sentRt = rt;
        sentLx = lx; sentLy = ly; sentRx = rx; sentRy = ry;
        if (buttons != loggedButtons || lt != loggedLt || rt != loggedRt) {
            android.util.Log.i("SaintsRowInput", String.format("pad buttons=%04X lt=%d rt=%d lx=%d ly=%d rx=%d ry=%d",
                    buttons, lt, rt, lx, ly, rx, ry));
            loggedButtons = buttons; loggedLt = lt; loggedRt = rt;
        }
        NativeBridge.setPad(buttons, lt, rt, lx, ly, rx, ry);
    }
}
