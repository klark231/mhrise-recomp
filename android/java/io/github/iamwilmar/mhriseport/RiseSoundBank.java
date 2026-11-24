package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.*;

// Wwise media-only banks: preserve platform media for unchanged PC tracks and replace compatible WEMs by ID.
final class RiseSoundBank {
    private static final int BKHD = 0x44484b42, DIDX = 0x58444944, DATA = 0x41544144;
    private static final class Bank {
        byte[] header;
        Map<Integer, byte[]> media = new LinkedHashMap<>();
        Bank(byte[] bytes) throws IOException {
            ByteBuffer b = RisePak.le(bytes);
            header = chunk(b, BKHD);
            ByteBuffer index = RisePak.le(chunk(b, DIDX));
            byte[] samples = chunk(b, DATA);
            if (b.hasRemaining() || header.length < 16 || index.remaining() % 12 != 0)
                throw new IOException("Unsupported Wwise bank structure");
            while (index.hasRemaining()) {
                int id = index.getInt(), pos = index.getInt(), size = index.getInt();
                if (pos < 0 || size < 22 || size > samples.length - pos)
                    throw new IOException("Invalid WEM extent");
                byte[] wem = Arrays.copyOfRange(samples, pos, pos + size);
                ByteBuffer wav = RisePak.le(wem);
                if (wav.getInt(0) != 0x46464952 || wav.getInt(4) != size - 8 || wav.getInt(8) != 0x45564157
                        || wav.getInt(12) != 0x20746d66 || media.put(id, wem) != null)
                    throw new IOException("Invalid Wwise RIFF media or duplicate ID");
            }
        }
    }
    private static byte[] chunk(ByteBuffer b, int tag) throws IOException {
        if (b.remaining() < 8 || b.getInt() != tag)
            throw new IOException("Expected a media-only BKHD/DIDX/DATA sound bank");
        int n = b.getInt();
        if (n < 0 || n > b.remaining())
            throw new IOException("Incomplete Wwise chunk");
        byte[] bytes = new byte[n];
        b.get(bytes);
        return bytes;
    }
    private static void writeChunk(OutputStream out, int tag, byte[] bytes) throws IOException {
        out.write(RisePak.le(new byte[8]).putInt(tag).putInt(bytes.length).array());
        out.write(bytes);
    }
    static void convert(File input, RisePak.Entry target, File scratch, File output) throws IOException {
        if (target == null || target.size > Integer.MAX_VALUE)
            throw new IOException("No matching NS sound bank");
        Bank pc = new Bank(Files.readAllBytes(input.toPath()));
        Bank ns = new Bank(target.prefix((int) target.size, scratch));
        if (!Arrays.equals(Arrays.copyOf(pc.header, 16), Arrays.copyOf(ns.header, 16))
                || !pc.media.keySet().equals(ns.media.keySet()))
            throw new IOException("Wwise bank identity, version or media IDs differ");
        ByteArrayOutputStream samples = new ByteArrayOutputStream();
        ByteBuffer index = RisePak.le(new byte[ns.media.size() * 12]);
        MessageDigest hash;
        try {
            hash = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        }
        for (Map.Entry<Integer, byte[]> item : ns.media.entrySet()) {
            int id = item.getKey();
            byte[] base = item.getValue(), mod = pc.media.get(id);
            // The ID + PC content digest identifies stock tracks in Rise's platform-specific banks.
            String digest = java.util.Base64.getEncoder().encodeToString(hash.digest(mod));
            boolean stock = RiseStockMedia.matches(id, digest);
            byte[] use = stock ? base : mod;
            if (!stock && RisePak.le(base).getShort(20) != RisePak.le(mod).getShort(20))
                throw new IOException(String.format(Locale.ROOT,
                        "WEM %s needs codec conversion (%04x to %04x); re-encode this track in Wwise",
                        Integer.toUnsignedString(id), Short.toUnsignedInt(RisePak.le(mod).getShort(20)),
                        Short.toUnsignedInt(RisePak.le(base).getShort(20))));
            while (samples.size() % 16 != 0) samples.write(0);
            index.putInt(id).putInt(samples.size()).putInt(use.length);
            samples.write(use);
        }
        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(output))) {
            writeChunk(out, BKHD, ns.header);
            writeChunk(out, DIDX, index.array());
            writeChunk(out, DATA, samples.toByteArray());
        }
    }
}
