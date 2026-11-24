package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class PcLooseMod {
    static final String MARKER = ".rise-pc-loose";
    static final String LUA_ORDER = ".rise-lua-order";
    private static final String NSW = "natives/NSW/";
    private static final Pattern PC = Pattern.compile("^(.*?)(?:^|/)natives/stm/(.+)$", Pattern.CASE_INSENSITIVE);
    private static final Pattern PATCH =
            Pattern.compile("re_chunk_000\\.pak\\.patch_([0-9]{3})\\.pak", Pattern.CASE_INSENSITIVE);
    // Same resource versions as the PC -> NS conversions verified on Rise 16.0.1.
    private static final Pattern SHARED =
            Pattern.compile(".*\\.(?:user\\.2|pfb\\.17|rcol\\.20|motlist\\.528|efx\\.2818730|mesh\\.2109148288|"
                    + "mdf2\\.23|chain\\.48|scn\\.20|clip\\.43|uvs\\.7|jcns\\.21|motfsm2\\.43|bnk\\.2\\.x64)$");

    static Map<String, File> select(Map<String, File> files) throws IOException {
        String root = null;
        Map<String, File> selected = new TreeMap<>();
        Map<String, String> alternates = new LinkedHashMap<>();
        boolean nsw = false;
        for (Map.Entry<String, File> entry : files.entrySet()) {
            String path = entry.getKey();
            if (("/" + path.toLowerCase(Locale.ROOT)).contains("/natives/nsw/"))
                nsw = true;
            Matcher m = PC.matcher(path);
            if (!m.matches())
                continue;
            if (root != null && !root.equalsIgnoreCase(m.group(1)))
                throw new IOException(
                        "Archive contains multiple PC variants. Import one natives/STM tree at a time.");
            root = m.group(1);
            String rel = m.group(2).toLowerCase(Locale.ROOT);
            if (rel.endsWith(".txt") || rel.endsWith(".md") || rel.endsWith("/.ds_store") || rel.equals(".ds_store"))
                continue;
            // Author-provided alternates keep their non-game names and cannot shadow the active file.
            String active = rel.replaceFirst("(\\.(?:user\\.2|mdf2\\.23|chain\\.48))(?:-[^/]+|\\.bak)$", "$1")
                                    .replaceFirst("(\\.(?:mdf2|chain)) ?- ?[^/]+(\\.(?:23|48))$", "$1$2")
                                    .replaceFirst("(^|/)bak-([^/]+)$", "$1$2");
            if (!active.equals(rel)) {
                alternates.put(rel, active);
                continue;
            }
            if (!SHARED.matcher(rel).matches() && !rel.endsWith(".tex.28") && !rel.endsWith(".dds"))
                throw new IOException("PC resource needs a separate converter: " + path);
            if (selected.put(rel, entry.getValue()) != null)
                throw new IOException("Duplicate PC resource: " + rel);
        }
        if (root != null && (selected.isEmpty() || nsw))
            throw new IOException("Empty or mixed PC/Switch resource tree");
        for (Map.Entry<String, String> variant : alternates.entrySet())
            if (!selected.containsKey(variant.getValue()))
                throw new IOException(
                        "Select the active version of " + variant.getValue() + " (found " + variant.getKey() + ")");
        for (String rel : selected.keySet())
            if (rel.endsWith(".dds") && !selected.containsKey(rel.substring(0, rel.length() - 4) + ".tex.28"))
                throw new IOException("DDS source has no companion game TEX: " + rel);
        return selected;
    }

    static int convert(Map<String, File> input, File romfs, File work, File output) throws IOException {
        Set<Long> wanted = new HashSet<>();
        for (String rel : input.keySet())
            if (rel.endsWith(".tex.28")) {
                wanted.add(RisePak.hash(NSW + rel));
                if (rel.startsWith("streaming/"))
                    wanted.add(RisePak.hash(NSW + rel.substring(10)));
            }
        for (String rel : input.keySet())
            if (rel.endsWith(".bnk.2.x64"))
                wanted.add(RisePak.hash(NSW + switchBank(rel)));
        Map<Long, RisePak.Entry> originals = RisePak.find(romfs, wanted);
        Map<String, File> result = new TreeMap<>();
        File compressed = new File(work, "base-entry");
        for (Map.Entry<String, File> e : input.entrySet()) {
            String rel = e.getKey();
            File file = e.getValue();
            if (rel.endsWith(".motfsm2.43") || rel.endsWith(".bnk.2.x64")) {
                File converted = File.createTempFile("converted-", ".bin", work);
                try {
                    if (rel.endsWith(".motfsm2.43"))
                        RiseMotionFsm.convert(file, converted);
                    else {
                        rel = switchBank(rel);
                        RiseSoundBank.convert(file, originals.get(RisePak.hash(NSW + rel)), compressed, converted);
                    }
                } catch (IOException error) {
                    throw new IOException(rel + ": " + error.getMessage(), error);
                }
                result.put(NSW + rel, converted);
                continue;
            }
            if (!rel.endsWith(".tex.28")) {
                result.put(NSW + rel, file);
                continue;
            }
            boolean streaming = rel.startsWith("streaming/");
            RisePak.Entry target = originals.get(RisePak.hash(NSW + rel));
            if (!streaming || target != null)
                texture(result, rel, file, target, streaming, compressed, work);
            if (streaming) {
                String resident = rel.substring(10);
                RisePak.Entry base = originals.get(RisePak.hash(NSW + resident));
                if (!input.containsKey(resident) && base != null)
                    texture(result, resident, file, base, true, compressed, work);
                else if (target == null && base == null)
                    throw new IOException("No NS streaming or resident target for " + rel);
            }
        }
        if (result.isEmpty())
            throw new IOException("No installable PC resources");
        RisePak.write(output, result);
        return result.size();
    }
    private static String switchBank(String rel) {
        return rel.substring(0, rel.length() - 3) + "nsw";
    }
    private static void texture(Map<String, File> resources, String rel, File input, RisePak.Entry base, boolean tier,
            File compressed, File work) throws IOException {
        File output = File.createTempFile("tex-", ".bin", work);
        try {
            RiseTexture.convert(input, output, base == null ? null : base.prefix(40, compressed), tier);
        } catch (IOException e) {
            throw new IOException(rel + ": " + (e instanceof EOFException ? "truncated texture" : e.getMessage()), e);
        }
        resources.put(NSW + rel, output);
    }

    static File romfs(File data) throws IOException {
        DumpScanner.Result config = DumpScanner.load(data);
        File root = config != null && config.romfs != null ? new File(config.romfs) : new File(data, "romfs");
        if (!new File(root, "re_chunk_000.pak").isFile())
            throw new IOException("Select the game's RomFS before importing PC mods");
        return root;
    }
    private static void reserve(File dir, BitSet slots) throws IOException {
        if (!dir.exists())
            return;
        File[] files = dir.listFiles();
        if (files == null)
            throw new IOException("Cannot inspect patch numbers in " + dir);
        for (File file : files) {
            Matcher m = PATCH.matcher(file.getName());
            if (m.matches())
                slots.set(Integer.parseInt(m.group(1)));
        }
    }
    static String name(int slot) throws IOException {
        if (slot > 999)
            throw new IOException("No free patch numbers");
        return String.format(Locale.ROOT, "re_chunk_000.pak.patch_%03d.pak", slot);
    }

    static int nextSlot(File data, File game) throws IOException {
        BitSet slots = new BitSet();
        reserve(game, slots);
        int first = Math.max(1, slots.length());
        for (ModFiles.Mod mod : ModFiles.list(data)) reserve(new File(mod.dir, "romfs"), slots);
        return slots.nextClearBit(first);
    }

    static long order(File dir) throws IOException {
        File marker = new File(dir, MARKER);
        if (!marker.isFile())
            marker = new File(dir, LUA_ORDER);
        String value = new String(Files.readAllBytes(marker.toPath()), java.nio.charset.StandardCharsets.UTF_8);
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException e) {
            throw new IOException("Invalid PC mod order: " + dir);
        }
    }
    static long nextOrder(File data) throws IOException {
        long latest = 0;
        for (ModFiles.Mod m : ModFiles.list(data))
            if (new File(m.dir, MARKER).isFile() || new File(m.dir, LUA_ORDER).isFile())
                latest = Math.max(latest, order(m.dir));
        return Math.addExact(latest, 1);
    }
    static void prepareSlots(File data) throws IOException {
        List<File> managed = new ArrayList<>();
        BitSet occupied = new BitSet();
        for (ModFiles.Mod m : ModFiles.list(data)) {
            File root = new File(m.dir, "romfs");
            if (new File(m.dir, MARKER).isFile()) {
                if (m.on)
                    managed.add(root);
            } else if (m.on)
                reserve(root, occupied);
        }
        if (managed.isEmpty())
            return;
        BitSet official = new BitSet();
        reserve(romfs(data), official);
        int slot = Math.max(1, official.length());
        // Fill free slots in import order after official updates; leave hand-installed packages in place.
        Map<File, Long> order = new HashMap<>();
        for (File root : managed) order.put(root, order(root.getParentFile()));
        managed.sort(Comparator.comparingLong(order::get));
        for (File root : managed) {
            File[] paks = root.listFiles(f -> PATCH.matcher(f.getName()).matches());
            if (paks == null || paks.length != 1)
                throw new IOException("Expected one converted PAK in " + root);
            slot = occupied.nextClearBit(slot);
            File dest = new File(root, name(slot++));
            if (!paks[0].equals(dest))
                Files.move(paks[0].toPath(), dest.toPath());
        }
    }
}
