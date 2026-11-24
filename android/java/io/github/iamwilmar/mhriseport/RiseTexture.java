package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.util.Arrays;

// Rise stores one mip table per image. Retile the original pixels, including cubemap faces and LUT depth slices.
final class RiseTexture {
    private static final int HEADER = 40, MAGIC = 0x00584554;
    private static final int CUSTOM_TEXTURE_THRESHOLD = 4096, CUSTOM_TEXTURE_LIMIT = 512;
    private static int u16(ByteBuffer b, int p) {
        return Short.toUnsignedInt(b.getShort(p));
    }
    private static int div(int a, int b) {
        return (a + b - 1) / b;
    }
    private static int logHeight(int rows) {
        return Math.min(4, 32 - Integer.numberOfLeadingZeros(div(rows, 8) - 1));
    }
    private static int firstFittingMip(int width, int height, int levels, int maxWidth, int maxHeight)
            throws IOException {
        for (int mip = 0; mip < levels; mip++) {
            if (Math.max(1, width >> mip) <= maxWidth && Math.max(1, height >> mip) <= maxHeight)
                return mip;
        }
        throw new IOException("No mip fits " + maxWidth + "x" + maxHeight + " in " + width + "x" + height
                + " PC texture; export the texture with a full mip chain");
    }
    private static int mips(ByteBuffer b) throws IOException {
        int n = Byte.toUnsignedInt(b.get(15));
        if (b.getInt(0) != MAGIC || (b.getInt(4) != 28 && !(b.getInt(4) == 30 && u16(b, 12) > 1)) || u16(b, 12) == 0
                || b.get(14) == 0 || u16(b, 8) == 0 || u16(b, 10) == 0 || n == 0 || n % 16 != 0)
            throw new IOException("Unsupported Rise TEX header");
        return n / 16;
    }
    private static int unit(int format) throws IOException {
        switch (format) {
            case 71:
            case 72:
            case 80:
            case 81:
                return 8;
            case 74:
            case 75:
            case 77:
            case 78:
            case 83:
            case 84:
            case 95:
            case 96:
            case 98:
            case 99:
                return 16;
            case 28:
            case 29:
                return 4;
            case 10:
                return 8;
            default:
                throw new IOException("Unsupported PC texture format " + format);
        }
    }
    static void convert(File input, File output, byte[] target, boolean matchTier) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(input, "r");
                RandomAccessFile out = new RandomAccessFile(output, "rw")) {
            byte[] h = new byte[HEADER];
            in.readFully(h);
            ByteBuffer src = RisePak.le(h);
            int levels = mips(src), w = u16(src, 8), y = u16(src, 10), fmt = src.getInt(16), bpp = unit(fmt);
            if (src.getInt(20) != -1)
                throw new IOException("Expected a linear PC texture");
            int images = Byte.toUnsignedInt(h[14]), depth = u16(src, 12);
            int block = fmt == 28 || fmt == 29 || fmt == 10 ? 1 : 4;
            int first = 0, count;
            byte[] header = h.clone();
            ByteBuffer result = RisePak.le(header);
            int baseHeight = logHeight(div(y, block)), depthLog = 0, tileLog = 0;
            if (depth > 1 && target == null) {
                if (w != 32 || y != 32 || depth != 32 || levels != 1 || images != 1)
                    throw new IOException("No matching NS volume texture layout");
                // Rise's 32-cubed color grading LUTs use one GOB in Y and sixteen in Z.
                baseHeight = 0;
                depthLog = 4;
            }
            if (target != null) {
                ByteBuffer ns = RisePak.le(target);
                int targetMips = mips(ns);
                if (ns.getInt(20) != 1 || (ns.get(32) & 15) > 5 || ((ns.get(32) & 255) >> 4) > 5
                        || (ns.get(33) & 255) > 5 || u16(ns, 12) != depth || ns.get(14) != h[14])
                    throw new IOException("Unsupported NS texture layout");
                if (matchTier) {
                    while (first < levels
                            && (Math.max(1, w >> first) != u16(ns, 8) || Math.max(1, y >> first) != u16(ns, 10)))
                        first++;
                    if (first + targetMips > levels)
                        throw new IOException("PC mip chain does not cover the NS texture tier");
                } else if (depth == 1) {
                    first = firstFittingMip(w, y, levels, u16(ns, 8), u16(ns, 10));
                }
                count = matchTier ? targetMips : levels - first;
                System.arraycopy(target, 24, header, 24, 16);
                baseHeight = target[32] & 15;
                depthLog = (target[32] & 255) >> 4;
                tileLog = target[33];
            } else {
                if (depth == 1 && Math.max(w, y) > CUSTOM_TEXTURE_THRESHOLD) {
                    first = firstFittingMip(w, y, levels, CUSTOM_TEXTURE_LIMIT, CUSTOM_TEXTURE_LIMIT);
                    baseHeight = logHeight(div(Math.max(1, y >> first), block));
                }
                count = levels - first;
                result.putShort(36, (short) 7);
                result.putShort(38, (short) 1);
            }
            result.putInt(20, 1);
            result.putInt(4, 28);
            header[32] = (byte) (baseHeight | depthLog << 4);
            header[33] = (byte) tileLog;
            result.putShort(8, (short) Math.max(1, w >> first));
            result.putShort(10, (short) Math.max(1, y >> first));
            header[15] = (byte) (16 * count);
            byte[] table = new byte[16 * levels * images];
            in.readFully(table);
            ByteBuffer srcMips = RisePak.le(table);
            byte[] index = new byte[16 * count * images];
            ByteBuffer dstMips = RisePak.le(index);
            out.setLength(0);
            out.write(header);
            out.write(index);
            byte[] gob = new byte[512];
            byte[] batch = new byte[65536];
            for (int image = 0; image < images; image++) {
                for (int i = first; i < first + count; i++) {
                    int rows = div(Math.max(1, y >> i), block), width = div(Math.max(1, w >> i), block);
                    int pitch = Math.multiplyExact(width, bpp);
                    int slices = Math.max(1, depth >> i), record = 16 * (image * levels + i);
                    long sliceSize = (long) pitch * rows, size = sliceSize * slices, offset = srcMips.getLong(record);
                    if (size > Integer.MAX_VALUE || srcMips.getInt(record + 8) != pitch
                            || srcMips.getInt(record + 12) != sliceSize || offset < HEADER + table.length
                            || offset > in.length() - size)
                        throw new IOException("Invalid PC texture mip " + i);
                    ByteBuffer pixels = in.getChannel().map(FileChannel.MapMode.READ_ONLY, offset, size);
                    int bh = 1 << baseHeight;
                    while (bh > 1 && rows <= 4 * bh) bh /= 2;
                    int rowGobs = div(pitch, 64), tile = 1 << tileLog;
                    if (tile > 1 && width > (64 / bpp) * tile && rows > (1 << baseHeight) * 8)
                        rowGobs = div(rowGobs, tile) * tile;
                    long start = out.getFilePointer(), tiled = 0, stored = 1;
                    int used = 0;
                    int bd = 1 << depthLog;
                    while (bd > 1 && slices <= bd / 2) bd /= 2;
                    for (int slab = 0; slab < div(slices, bd); slab++)
                        for (int group = 0; group < div(rows, 8 * bh); group++)
                            for (int gx = 0; gx < rowGobs; gx++)
                                for (int dz = 0; dz < bd; dz++)
                                    for (int gy = 0; gy < bh; gy++) {
                                        Arrays.fill(gob, (byte) 0);
                                        // Each GOB is 64 bytes wide by 8 rows, ordered in 16-byte sectors.
                                        for (int s = 0; s < 512; s += 16) {
                                            int x = gx * 64 + ((s >> 8) & 1) * 32 + ((s >> 5) & 1) * 16;
                                            int row = (group * bh + gy) * 8 + ((s >> 6) & 3) * 2 + ((s >> 4) & 1);
                                            int z = slab * bd + dz;
                                            if (z >= slices || row >= rows || x >= pitch)
                                                continue;
                                            pixels.position((z * rows + row) * pitch + x);
                                            pixels.get(gob, s, Math.min(16, pitch - x));
                                        }
                                        for (int j = 511; j >= 0; j--)
                                            if (gob[j] != 0) {
                                                stored = tiled + j + 1;
                                                break;
                                            }
                                        System.arraycopy(gob, 0, batch, used, 512);
                                        used += 512;
                                        tiled += 512;
                                        if (used == batch.length) {
                                            out.write(batch);
                                            used = 0;
                                        }
                                    }
                    if (used != 0)
                        out.write(batch, 0, used);
                    if (tiled > Integer.MAX_VALUE)
                        throw new IOException("Texture mip exceeds TEX.28 size");
                    out.setLength(start + stored);
                    out.seek(start + stored);
                    dstMips.putLong(start).putInt((int) stored).putInt((int) tiled);
                }
            }
            out.seek(HEADER);
            out.write(index);
        }
    }
}
