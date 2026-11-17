package com.saintsrow.port;

import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Performance overlay. Frame numbers come from the native side: the exact time of every game frame
 * (VdSwap), over the last second - frames per second, average frame time, "1% low" (the 99th
 * percentile frame time, shown as FPS) and the longest frame. Device numbers are read from sysfs /
 * procfs: CPU clocks per cluster, the app's CPU use, GPU load and clock (kgsl), temperatures.
 *
 * Mode 1 (simple): FPS and frame time. Mode 2 (detailed): everything.
 */
final class PerfOverlay {
    private static final long PERIOD_MS = 500;
    private static final int WINDOW_MS = 1000;

    private final TextView view;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float[] frame = new float[5];
    private final int cpuCount = Runtime.getRuntime().availableProcessors();
    private final List<int[]> clusters = new ArrayList<>(); // {first cpu, last cpu}
    private final List<String> cpuTempZones = new ArrayList<>(), gpuTempZones = new ArrayList<>();
    private String batteryTempZone;
    private int mode;
    private long lastCpuTicks = -1, lastWallNs;
    private final long ticksPerSecond = 100; // USER_HZ on Android

    PerfOverlay(TextView view) {
        this.view = view;
        view.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        view.setTextColor(Color.WHITE);
        view.setTextSize(12);
        view.setShadowLayer(3, 1, 1, Color.BLACK);
        view.setBackgroundColor(0x80000000);
        view.setPadding(14, 8, 14, 8);
        findClusters();
        findThermalZones();
    }

    void setMode(int mode) {
        this.mode = mode;
        handler.removeCallbacks(tick);
        view.setVisibility(mode == 0 ? TextView.GONE : TextView.VISIBLE);
        if (mode != 0) handler.post(tick);
    }

    void stop() { handler.removeCallbacks(tick); }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            try { view.setText(build()); } catch (Throwable ignored) {}
            handler.postDelayed(this, PERIOD_MS);
        }
    };

    private String build() {
        NativeBridge.getFrameStats(WINDOW_MS, frame);
        float fps = frame[0], avg = frame[1], p99 = frame[2], max = frame[3];
        StringBuilder sb = new StringBuilder();
        if (frame[4] < 1) {
            sb.append("FPS   --  (no frames)");
        } else {
            sb.append(String.format(Locale.US, "FPS %5.1f  %5.1f ms", fps, avg));
            if (mode == 2)
                sb.append(String.format(Locale.US, "\n1%% low %4.0f FPS  max %4.0f ms", p99 > 0 ? 1000 / p99 : 0, max));
        }
        if (mode != 2) return sb.toString();

        // CPU: clock of each cluster (fastest core of it), this app's CPU use, hottest CPU sensor.
        sb.append("\nCPU");
        for (int[] c : clusters) {
            long khz = 0;
            for (int i = c[0]; i <= c[1]; i++)
                khz = Math.max(khz, readLong("/sys/devices/system/cpu/cpu" + i + "/cpufreq/scaling_cur_freq"));
            sb.append(String.format(Locale.US, " %.2f", khz / 1e6));
        }
        sb.append(" GHz");
        float cpu = appCpuPercent();
        if (cpu >= 0) sb.append(String.format(Locale.US, "  %3.0f%%", cpu));
        float ct = maxTemp(cpuTempZones);
        if (ct > 0) sb.append(String.format(Locale.US, "  %2.0f°C", ct));

        // GPU (Adreno kgsl): load, clock, temperature.
        long busy = readLong("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
        long gclk = readLong("/sys/class/kgsl/kgsl-3d0/gpuclk");
        sb.append(String.format(Locale.US, "\nGPU %4d MHz  %3d%%", gclk / 1000000, busy));
        float gt = maxTemp(gpuTempZones);
        if (gt > 0) sb.append(String.format(Locale.US, "  %2.0f°C", gt));

        // Memory (this app) and battery temperature.
        long rssKb = readStatusKb("VmRSS:");
        sb.append(String.format(Locale.US, "\nRAM %.2f GB", rssKb / 1048576.0));
        if (batteryTempZone != null) {
            long b = readLong(batteryTempZone);
            if (b > 0) sb.append(String.format(Locale.US, "  BAT %2.0f°C", b > 1000 ? b / 1000.0 : b / 10.0));
        }
        return sb.toString();
    }

    /** This app's CPU use since the last update, as a share of all cores (100% = every core busy). */
    private float appCpuPercent() {
        String stat = readLine("/proc/self/stat");
        if (stat == null) return -1;
        String[] f = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
        long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]); // utime + stime
        long now = System.nanoTime();
        float pct = -1;
        if (lastCpuTicks >= 0) {
            double busy = (ticks - lastCpuTicks) / (double) ticksPerSecond;
            double wall = (now - lastWallNs) / 1e9;
            if (wall > 0) pct = (float) (busy / wall / cpuCount * 100);
        }
        lastCpuTicks = ticks;
        lastWallNs = now;
        return pct;
    }

    /** CPU clusters: cores sharing a maximum frequency. */
    private void findClusters() {
        long prev = -1;
        int start = 0;
        for (int i = 0; i < cpuCount; i++) {
            long max = readLong("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq");
            if (i > 0 && max != prev) { clusters.add(new int[] { start, i - 1 }); start = i; }
            prev = max;
        }
        clusters.add(new int[] { start, cpuCount - 1 });
    }

    private void findThermalZones() {
        File[] zones = new File("/sys/class/thermal").listFiles();
        if (zones == null) return;
        for (File z : zones) {
            if (!z.getName().startsWith("thermal_zone")) continue;
            String type = readLine(z.getPath() + "/type");
            if (type == null) continue;
            String temp = z.getPath() + "/temp";
            if (type.startsWith("cpu-") || type.startsWith("cpuss-")) cpuTempZones.add(temp);
            else if (type.startsWith("gpuss-")) gpuTempZones.add(temp);
            else if (type.equals("battery")) batteryTempZone = temp;
        }
        if (batteryTempZone == null && new File("/sys/class/power_supply/battery/temp").canRead())
            batteryTempZone = "/sys/class/power_supply/battery/temp";
    }

    private static float maxTemp(List<String> zones) {
        long max = 0;
        for (String z : zones) max = Math.max(max, readLong(z));
        return max > 1000 ? max / 1000f : max;
    }

    private static long readStatusKb(String key) {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/self/status"))) {
            for (String line; (line = r.readLine()) != null; )
                if (line.startsWith(key)) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception ignored) {}
        return 0;
    }

    private static String readLine(String path) {
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            return r.readLine();
        } catch (Exception e) {
            return null;
        }
    }

    private static long readLong(String path) {
        String s = readLine(path);
        if (s == null) return 0;
        s = s.trim().split(" ")[0];
        try { return Long.parseLong(s); } catch (NumberFormatException e) { return 0; }
    }
}
