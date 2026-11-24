package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

final class RisePak {
    static final int MAGIC = 0x414b504b;
    static ByteBuffer le(byte[] b) {
        return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static int murmur(String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_16LE);
        ByteBuffer words = le(b);
        int h = -1;
        while (words.remaining() >= 4) {
            int k = words.getInt();
            h ^= Integer.rotateLeft(k * 0xcc9e2d51, 15) * 0x1b873593;
            h = Integer.rotateLeft(h, 13) * 5 + 0xe6546b64;
        }
        if (words.hasRemaining()) {
            int k = Short.toUnsignedInt(words.getShort());
            h ^= Integer.rotateLeft(k * 0xcc9e2d51, 15) * 0x1b873593;
        }
        h ^= b.length;
        h = (h ^ (h >>> 16)) * 0x85ebca6b;
        h = (h ^ (h >>> 13)) * 0xc2b2ae35;
        return h ^ (h >>> 16);
    }
    static long hash(String path) {
        return ((long) murmur(path.toUpperCase(Locale.ROOT)) << 32)
            | Integer.toUnsignedLong(murmur(path.toLowerCase(Locale.ROOT)));
    }

    static void write(File pak, Map<String, File> resources) throws IOException {
        Set<Long> hashes = new HashSet<>();
        long offset = 16L + 48L * resources.size();
        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(pak))) {
            ByteBuffer row = le(new byte[48]);
            row.putInt(MAGIC).putInt(4).putInt(resources.size()).putInt(0);
            out.write(row.array(), 0, 16);
            for (Map.Entry<String, File> entry : resources.entrySet()) {
                long key = hash(entry.getKey()), size = entry.getValue().length();
                if (!hashes.add(key))
                    throw new IOException("Conflicting PAK path: " + entry.getKey());
                row.clear();
                row.putLong(key).putLong(offset).putLong(size).putLong(size).putLong(0).putLong(0);
                out.write(row.array());
                offset = Math.addExact(offset, size);
            }
            for (File file : resources.values())
                Files.copy(file.toPath(), out);
        }
        if (pak.length() != offset)
            throw new IOException("Incomplete PAK");
    }

    static final class Entry {
        File pak;
        long offset, packed, size, flags;
        byte[] prefix(int n, File temp) throws IOException {
            if (size < n || (flags & 0x00ff0070L) != 0)
                throw new IOException("Unsupported base texture entry in " + pak.getName());
            try (RandomAccessFile in = new RandomAccessFile(pak, "r")) {
                in.seek(offset);
                byte[] bytes = new byte[n];
                if ((flags & 15) == 0) {
                    in.readFully(bytes);
                    return bytes;
                }
                if ((flags & 15) != 1 && (flags & 15) != 2)
                    throw new IOException("Unknown base PAK compression");
                try (OutputStream out = new FileOutputStream(temp)) {
                    byte[] block = new byte[65536];
                    for (long left = packed; left > 0;) {
                        int k = (int) Math.min(left, block.length);
                        in.readFully(block, 0, k);
                        out.write(block, 0, k);
                        left -= k;
                    }
                }
                if ((flags & 15) == 2)
                    return ModArchive.zstdPrefix(temp, n);
                Inflater inflater = new Inflater(true);
                try (DataInputStream stream =
                         new DataInputStream(new InflaterInputStream(new FileInputStream(temp), inflater))) {
                    stream.readFully(bytes);
                    return bytes;
                } finally {
                    inflater.end();
                }
            }
        }
    }

    // Scan each official index sequentially once. Later patch rows replace older rows.
    static Map<Long, Entry> find(File romfs, Set<Long> wanted) throws IOException {
        Map<Long, Entry> result = new HashMap<>();
        File[] paks = romfs.listFiles(f
            -> f.isFile()
                && (f.getName().equals("re_chunk_000.pak")
                    || f.getName().matches("re_chunk_000\\.pak\\.patch_[0-9]{3}\\.pak")));
        if (paks == null || !new File(romfs, "re_chunk_000.pak").isFile())
            throw new IOException("Cannot locate the game's RomFS PAKs");
        Arrays.sort(paks);
        if (wanted.isEmpty())
            return result;
        for (File pak : paks)
            try (
                DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(pak)))) {
                byte[] header = new byte[16];
                in.readFully(header);
                ByteBuffer h = le(header);
                if (h.getInt() != MAGIC || h.getInt() != 4)
                    throw new IOException("Unsupported Rise PAK index: " + pak.getName());
                int count = h.getInt();
                if (count < 0 || 16L + 48L * count > pak.length())
                    throw new IOException("Invalid PAK index: " + pak.getName());
                byte[] block = new byte[48 * 1024];
                for (int i = 0; i < count;) {
                    int n = Math.min(count - i, 1024);
                    in.readFully(block, 0, 48 * n);
                    ByteBuffer table = le(block);
                    for (int j = 0; j < n; j++) {
                        int at = j * 48;
                        long k = table.getLong(at);
                        if (!wanted.contains(k))
                            continue;
                        Entry e = new Entry();
                        e.pak = pak;
                        e.offset = table.getLong(at + 8);
                        e.packed = table.getLong(at + 16);
                        e.size = table.getLong(at + 24);
                        e.flags = table.getLong(at + 32);
                        if (e.offset < 16L + 48L * count || e.packed < 0 || e.size < 0
                            || e.offset > pak.length() - e.packed)
                            throw new IOException("Invalid texture location in " + pak.getName());
                        result.put(k, e);
                    }
                    i += n;
                }
            }
        return result;
    }
}
