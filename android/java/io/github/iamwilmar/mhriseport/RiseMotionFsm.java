package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.*;

final class RiseMotionFsm {
    private static final int FLAG_TYPE = 839600709, PC_CRC = 0x1e8c1a7b, NS_CRC = 0x15902356;
    private static final Map<Long, String> LAYOUT = new HashMap<>();
    static {
        RiseFsmLayouts.fill(LAYOUT);
    }

    private static int offset(ByteBuffer b, int base, int at) throws IOException {
        long delta = b.getLong(at);
        if (delta < 0 || delta > b.limit() - base)
            throw new IOException("Invalid MOTFSM section offset");
        return base + (int) delta;
    }
    private static int align(int value, int alignment) {
        return (value + alignment - 1) & -alignment;
    }

    static void convert(File input, File output) throws IOException {
        byte[] bytes = Files.readAllBytes(input.toPath());
        ByteBuffer b = RisePak.le(bytes);
        try {
            if (b.getInt(0) != 43 || b.getInt(4) != 0x3273666d)
                throw new IOException("Expected Rise MOTFSM2.43");
            int tree = offset(b, 0, 16);
            if (b.getInt(tree) != 0x54564842)
                throw new IOException("Missing BHVT");
            Set<Integer> visited = new HashSet<>();
            for (int block = 0; block < 11; block++) {
                int base = offset(b, tree, tree + 16 + block * 8);
                if (visited.add(base))
                    convertBlock(b, base);
            }
        } catch (IndexOutOfBoundsException | ArithmeticException e) {
            throw new IOException("Invalid MOTFSM2.43 data", e);
        }
        Files.write(output.toPath(), bytes);
    }

    private static void convertBlock(ByteBuffer b, int base) throws IOException {
        if (b.getInt(base) != 0x005a5352 || b.getInt(base + 4) != 16)
            throw new IOException("Unsupported motion RSZ");
        int n = b.getInt(base + 12), users = b.getInt(base + 16);
        int table = offset(b, base, base + 24), pos = offset(b, base, base + 32);
        int userdata = offset(b, base, base + 40);
        if (n < 0 || n > (pos - table) / 8 || users < 0 || users > (pos - userdata) / 16)
            throw new IOException("Invalid motion RSZ table");
        int last = 0;
        for (int i = 1; i < n; i++)
            if (b.getInt(table + 8 * i) == FLAG_TYPE)
                last = i;
        if (last == 0)
            return;
        Set<Integer> external = new HashSet<>();
        for (int i = 0; i < users; i++) external.add(b.getInt(userdata + 16 * i));
        for (int i = 1; i <= last; i++) {
            int row = table + 8 * i, type = b.getInt(row), crc = b.getInt(row + 4);
            if (type == 0 || external.contains(i))
                continue;
            if (type == FLAG_TYPE && crc == 0x93b592a5)
                throw new IOException("TU4-era motion graph: migrating its action IDs requires the source enum "
                                      + "definitions or the matching original PC graph");
            String layout = LAYOUT.get(((long) type << 32) | Integer.toUnsignedLong(crc));
            if (layout == null)
                throw new IOException(String.format(Locale.ROOT,
                        "Motion RSZ schema %08x/%08x differs from Rise 16.0.1. A matching source schema is needed to "
                        + "migrate this action.",
                        type, crc));
            for (int j = 0; j < layout.length(); j++) {
                char op = layout.charAt(j);
                int alignment = op == 'b' ? 1 : op == 'h' ? 2 : op == 'v' ? 16 : 4;
                pos = align(pos, alignment);
                if (type == FLAG_TYPE && j == 2) {
                    if (crc != PC_CRC)
                        throw new IOException("Unrecognized player flag schema");
                    int value = b.getInt(pos);
                    if (value < 0 || value > 39 || value == 30)
                        throw new IOException("Player flag has no Switch equivalent: " + value);
                    if (value > 30)
                        b.putInt(pos, value - 1);
                    b.putInt(row + 4, NS_CRC);
                }
                if (op == 'a' || op == 's') {
                    int length = b.getInt(pos);
                    if (length < 0)
                        throw new IOException("Negative motion RSZ field length");
                    pos = Math.addExact(pos, Math.addExact(4, Math.multiplyExact(length, op == 's' ? 2 : 4)));
                } else
                    pos = Math.addExact(pos, op == 'd' ? 8 : alignment);
                if (pos > b.limit())
                    throw new IOException("Truncated motion RSZ");
            }
        }
    }
}
