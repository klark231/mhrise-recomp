package io.github.iamwilmar.mhriseport;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.Arrays;

public final class RiseTextureSizingTest {
    // One row of BC7 blocks per face and mip, with distinct data to detect incorrect table indexing.
    private static byte[] texture(int width, int levels, int faces, int sourceMip) {
        int bytes = 40 + 16 * levels * faces;
        for (int i = 0; i < levels; i++)
            bytes += faces * pitch(width, i);
        ByteBuffer b = RisePak.le(new byte[bytes]);
        b.putInt(0x00584554).putInt(28).putShort((short) width).putShort((short) 1).putShort((short) 1);
        b.put((byte) faces).put((byte) (16 * levels)).putInt(98).putInt(-1);
        int offset = 40 + 16 * levels * faces;
        b.position(40);
        for (int face = 0; face < faces; face++) {
            for (int i = 0; i < levels; i++) {
                int size = pitch(width, i);
                b.putLong(offset).putInt(size).putInt(size);
                Arrays.fill(b.array(), offset, offset + size, (byte) (1 + 16 * face + sourceMip + i));
                offset += size;
            }
        }
        return b.array();
    }
    private static int pitch(int width, int mip) {
        return 16 * ((Math.max(1, width >> mip) + 3) / 4);
    }
    private static byte[] convert(File dir, byte[] source, byte[] target, boolean tier) throws IOException {
        File input = File.createTempFile("tex-", ".bin", dir), output = new File(dir, "output");
        Files.write(input.toPath(), source);
        RiseTexture.convert(input, output, target, tier);
        return Files.readAllBytes(output.toPath());
    }
    private static void check(boolean ok) {
        if (!ok) throw new AssertionError();
    }
    public static void main(String[] args) throws IOException {
        File dir = Files.createTempDirectory(new File(args[0]).toPath(), "tex-sizing-").toFile();
        byte[] full = texture(6144, 13, 6, 0);
        byte[] custom = convert(dir, full, null, false);
        check(Arrays.equals(custom, convert(dir, texture(384, 9, 6, 4), null, false)));
        check(RisePak.le(convert(dir, texture(4096, 1, 1, 0), null, false)).getShort(8) == 4096);

        byte[] nativeHeader = convert(dir, texture(1536, 3, 6, 0), null, false);
        byte[] nativeSize = convert(dir, full, nativeHeader, false);
        check(Arrays.equals(nativeSize, convert(dir, texture(1536, 11, 6, 2), nativeHeader, false)));
        check(Arrays.equals(convert(dir, full, nativeHeader, true),
                convert(dir, texture(1536, 3, 6, 2), nativeHeader, true)));
        check(RisePak.le(convert(dir, texture(192, 3, 6, 0), nativeHeader, false)).getShort(8) == 192);
        try {
            convert(dir, texture(6144, 4, 1, 0), null, false);
            throw new AssertionError("Imported oversized texture without a fitting mip");
        } catch (IOException expected) {
            check(expected.getMessage().contains("512x512"));
        }
    }
}
