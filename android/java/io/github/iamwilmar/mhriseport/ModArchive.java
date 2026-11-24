package io.github.iamwilmar.mhriseport;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.*;
import me.zhanghai.android.libarchive.Archive;
import me.zhanghai.android.libarchive.ArchiveEntry;

// Spool regular entries to private numeric filenames until the archive layout is known.
final class ModArchive {
    private static void open(long a, File path) throws IOException {
        Archive.setCharset(a, "UTF-8".getBytes(StandardCharsets.US_ASCII));
        Archive.readSupportFilterAll(a);
        Archive.readOpenFileName(a, path.toString().getBytes(StandardCharsets.UTF_8), 65536);
    }

    static Map<String, File> unpack(File path, File staging) throws IOException {
        Map<String, File> files = new LinkedHashMap<>();
        long a = Archive.readNew();
        try {
            Archive.readSupportFormatZip(a);
            Archive.readSupportFormat7zip(a);
            Archive.readSupportFormatRar(a);
            Archive.readSupportFormatRar5(a);
            Archive.readSupportFormatTar(a);
            open(a, path);
            ByteBuffer buf = ByteBuffer.allocateDirect(65536);
            byte[] bytes = new byte[buf.capacity()];
            long e;
            while ((e = Archive.readNextHeader(a)) != 0) {
                if (ArchiveEntry.isEncrypted(e))
                    throw new IOException("Password-protected mod archive");
                if (ArchiveEntry.filetype(e) == ArchiveEntry.AE_IFDIR)
                    continue;
                if (ArchiveEntry.filetype(e) != ArchiveEntry.AE_IFREG || ArchiveEntry.hardlinkIsSet(e))
                    throw new IOException("Mod archive contains a link or special file");
                String name = ArchiveEntry.pathnameUtf8(e);
                if (name == null) {
                    byte[] raw = ArchiveEntry.pathname(e);
                    if (raw == null) throw new IOException("Unreadable archive filename");
                    name = StandardCharsets.UTF_8.newDecoder().decode(ByteBuffer.wrap(raw)).toString();
                }
                name = name.replace('\\', '/');
                while (name.startsWith("./"))
                    name = name.substring(2);
                if (name.startsWith("__MACOSX/") || name.endsWith("/.DS_Store"))
                    continue;
                for (String component : name.split("/", -1))
                    if (component.isEmpty() || component.equals(".") || component.equals("..")
                        || component.indexOf(':') >= 0)
                        throw new IOException("Invalid archive path: " + name);
                File file = new File(staging, "entry-" + files.size());
                if (files.put(name, file) != null)
                    throw new IOException("Duplicate archive path: " + name);
                try (OutputStream out = new FileOutputStream(file)) {
                    while (true) {
                        buf.clear();
                        Archive.readData(a, buf);
                        int n = buf.position();
                        if (n == 0)
                            break;
                        buf.flip();
                        buf.get(bytes, 0, n);
                        out.write(bytes, 0, n);
                    }
                }
                if (ArchiveEntry.sizeIsSet(e) && ArchiveEntry.size(e) != file.length())
                    throw new IOException("Incomplete archive entry: " + name);
            }
        } finally {
            Archive.free(a);
        }
        if (files.isEmpty())
            throw new IOException("Empty or unsupported mod archive");
        return files;
    }

    static byte[] zstdPrefix(File path, int count) throws IOException {
        long a = Archive.readNew();
        try {
            Archive.readSupportFormatRaw(a);
            open(a, path);
            if (Archive.readNextHeader(a) == 0)
                throw new EOFException("Empty Zstandard frame");
            ByteBuffer buf = ByteBuffer.allocateDirect(count);
            while (buf.hasRemaining()) {
                int last = buf.position();
                Archive.readData(a, buf);
                if (buf.position() == last)
                    throw new EOFException("Incomplete Zstandard payload");
            }
            byte[] result = new byte[count];
            buf.flip();
            buf.get(result);
            return result;
        } finally {
            Archive.free(a);
        }
    }
}
