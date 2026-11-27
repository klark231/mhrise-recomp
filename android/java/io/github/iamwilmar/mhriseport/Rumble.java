package io.github.iamwilmar.mhriseport;

import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;

import java.util.ArrayList;
import java.util.List;

// Controller haptics: the game's vibration (the Switch's HD rumble, which loader/android_main.cpp reads from nn::hid and
// hands to GameActivity.rumble) played on the motors of the connected gamepads. The game gives an amplitude for a low and a
// high band: a pad with two motors (Android 12+ lists them) gets the low band on the first and the high band on the
// second, and one with a single motor gets the louder of the two. Android has no frequencies to play, only amplitudes.
//
// The strength is the player's (Settings → Controller → Haptic strength): 100 % plays the game's amplitudes as they
// are, 0 is off, up to 200 for a pad whose motors are weak. Each push starts a short vibration (DURATION_MS), which the
// loader renews every 40 ms while the game keeps asking, so one that is never renewed (the game paused, the app sent to the
// background) ends by itself within a blink.
final class Rumble {
    private static final int DURATION_MS = 60;
    private static final long SCAN_EVERY_MS = 2000;

    private final Object lock = new Object();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Vibrator[]> pads = new ArrayList<>();  // one entry per controller: its motors
    private long scannedAt = -SCAN_EVERY_MS;
    private boolean active;
    private final Runnable endTest = this::stop;

    // low, high: the game's amplitudes (0..1); percent: the player's strength
    void set(float low, float high, int percent) {
        synchronized (lock) {
            if (percent <= 0 || (low <= 0f && high <= 0f)) {
                stopLocked();
                return;
            }
            long now = SystemClock.uptimeMillis();
            if (!active || now - scannedAt >= SCAN_EVERY_MS) {  // (a controller plugged in later is found too)
                scan();
                scannedAt = now;
            }
            active = true;
            for (Vibrator[] pad : pads) {
                if (pad.length >= 2) {
                    play(pad[0], low, percent);
                    play(pad[1], high, percent);
                } else {
                    play(pad[0], Math.max(low, high), percent);
                }
            }
        }
    }

    // Lets every motor go (the game stopped asking, or the app is going to the background)
    void stop() {
        synchronized (lock) {
            stopLocked();
        }
    }

    // Settings' Test: a firm pulse on both bands, so the strength can be felt while it is changed
    void test(int percent) {
        main.removeCallbacks(endTest);
        set(0.9f, 0.9f, percent);
        main.postDelayed(endTest, 350);
    }

    // Whether any connected controller has a motor (for the menu's note)
    boolean available() {
        synchronized (lock) {
            scan();
            scannedAt = SystemClock.uptimeMillis();
            return !pads.isEmpty();
        }
    }

    private void stopLocked() {
        if (!active) return;
        active = false;
        for (Vibrator[] pad : pads)
            for (Vibrator v : pad) v.cancel();
    }

    private void play(Vibrator v, float amplitude, int percent) {
        int a = amplitude(amplitude, percent);
        boolean control = v.hasAmplitudeControl();
        if (a <= 0 || (!control && a < 48)) {  // (a motor with no strength control is on or off)
            v.cancel();
            return;
        }
        v.vibrate(VibrationEffect.createOneShot(DURATION_MS, control ? a : VibrationEffect.DEFAULT_AMPLITUDE));
    }

    // 0..255 for the game's 0..1 at the player's strength. The curve lifts the weak amplitudes the game uses for
    // light touches, which a small motor wouldn't show at all in a straight line.
    static int amplitude(float value, int percent) {
        if (!(value > 0f)) return 0;
        float x = (float) Math.pow(Math.min(1f, value), 0.7) * (percent / 100f);
        return Math.round(Math.min(1f, x) * 255f);
    }

    // The motors of every gamepad there is (under lock)
    private void scan() {
        if (active) {  // (don't leave the old ones vibrating if they are not found again)
            for (Vibrator[] pad : pads)
                for (Vibrator v : pad) v.cancel();
        }
        pads.clear();
        try {
            for (int id : InputDevice.getDeviceIds()) {
                InputDevice d = InputDevice.getDevice(id);
                if (d == null || d.isVirtual()) continue;
                int sources = d.getSources();
                if ((sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                        && (sources & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) continue;
                List<Vibrator> motors = new ArrayList<>();
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    VibratorManager manager = d.getVibratorManager();
                    for (int vid : manager.getVibratorIds()) {
                        Vibrator v = manager.getVibrator(vid);
                        if (v.hasVibrator()) motors.add(v);
                    }
                } else {
                    Vibrator v = d.getVibrator();
                    if (v != null && v.hasVibrator()) motors.add(v);
                }
                if (!motors.isEmpty()) pads.add(motors.toArray(new Vibrator[0]));
            }
        } catch (RuntimeException e) {
            System.err.println("[rumble] couldn't list the controllers: " + e);
            pads.clear();
        }
    }
}
