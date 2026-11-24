package io.github.iamwilmar.mhriseport;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

// First-run defaults for a MediaTek phone with a small Mali GPU (made for the Infinix GT 30 5G: Dimensity 7400, Mali-G615
// MC2, 8 GB). The port's own defaults (960x540 base, the game's shadows, LOD and draw distance) are tuned for stronger
// GPUs; a 2-core Mali-G615 can't hold 30 fps there, so it would spend most of the time in the dynamic resolution's floor
// and hitch while streaming. This writes lighter choices into mhr.ini ONCE, and only for names the ini doesn't already
// have, so anything the user picked is never overridden.
//
// - A marker file (<data>/device_profile.txt) makes it run once. "Restore default settings" in the menu deletes the ini
//   but not the marker, so it really goes back to the port's stock defaults, and the profile doesn't come back.
// - To get the profile again, delete device_profile.txt. To skip it, create that file with any content.
// - Everything it sets is an ordinary menu switch (Settings → Resolution / Graphics / Detail), so each can be undone there.
final class DeviceProfile {
    static final String MARKER = "device_profile.txt";

    // name -> value. All of these exist in the settings menu (docs/settings.md).
    private static final String[][] MALI_SMALL = {
        {"resolution_432", "1"},   // 768x432 base scene, FSR 1 scales it to the screen
        {"dynres_wide", "1"},      // dynamic resolution down to 50 % and climbing back while the GPU has room
        {"shadow_half", "1"},      // shadow maps at half size: much less fill and bandwidth work
        {"model_detail_low", "1"}, // mesh LOD rate x0.75
        {"draw_distance_75", "1"}, // small and medium objects drawn 25 % less far
        {"texture_bias_1", "1"},   // skip the largest mip level: less memory traffic, fewer ASTC re-encodes
        {"no_af", "1"},            // no anisotropic filtering
    };

    private DeviceProfile() {}

    static boolean mediaTek() {
        String soc = android.os.Build.VERSION.SDK_INT >= 31
                ? android.os.Build.SOC_MANUFACTURER + " " + android.os.Build.SOC_MODEL : "";
        String all = (soc + " " + android.os.Build.HARDWARE + " " + android.os.Build.BOARD).toLowerCase(Locale.ROOT);
        return all.contains("mediatek") || all.matches(".*\\bmt\\d{4}.*");
    }

    // The menu's choices come in groups of which only one is on (resolution_*, shadow_*...): any member counts.
    private static final String[] GROUPS = {"resolution_", "shadow_", "model_detail_", "draw_distance_", "texture_bias_"};

    private static boolean chosen(Ini ini, File folder, String name) {
        String group = name;
        for (String g : GROUPS) if (name.startsWith(g)) group = g;
        if (group.equals(name)) return ini.get(name) != null || new File(folder, name).exists();
        for (String known : MENU_NAMES) {
            if (!known.startsWith(group)) continue;
            if (ini.get(known) != null || new File(folder, known).exists()) return true;
        }
        return false;
    }

    private static final String[] MENU_NAMES = {
        "resolution_240", "resolution_360", "resolution_432", "resolution_480", "resolution_576", "resolution_648", "resolution_720",
        "shadow_half", "shadow_quarter", "shadow_cache",
        "model_detail_low", "model_detail_lowest", "model_detail_min", "model_detail_high",
        "draw_distance_75", "draw_distance_50", "draw_distance_33", "draw_distance_25",
        "texture_bias_1", "texture_bias_2",
    };

    // Called before the game starts. Returns the number of values written (0: nothing to do).
    static int applyOnce(File folder) {
        if (folder == null || !folder.isDirectory() || !mediaTek()) return 0;
        File marker = new File(folder, MARKER);
        if (marker.exists()) return 0;
        Ini ini = new Ini(folder);
        int written = 0;
        StringBuilder log = new StringBuilder("applied on ").append(android.os.Build.MODEL).append(" (")
                .append(android.os.Build.HARDWARE).append("):");
        for (String[] kv : MALI_SMALL) {
            // Respect a choice already made: in the ini, or the old empty file of that name.
            if (chosen(ini, folder, kv[0])) continue;
            ini.set(kv[0], kv[1]);
            log.append(' ').append(kv[0]);
            written++;
        }
        try {
            Files.write(marker.toPath(), (log + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
            // (storage refused: it will try again next start, which is harmless because existing names are skipped)
        }
        return written;
    }
}
