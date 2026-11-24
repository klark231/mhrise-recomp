import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.jf.dexlib2.dexbacked.DexBackedDexFile;
import org.jf.dexlib2.iface.ClassDef;
import org.jf.dexlib2.writer.io.MemoryDataStore;
import org.jf.dexlib2.writer.pool.DexPool;

class MergeAndroidClasses {
    private static boolean selected(String type, Set<String> families) {
        int nested = type.indexOf('$');
        return families.contains(type.substring(0, nested >= 0 ? nested : type.length() - 1));
    }

    private static DexBackedDexFile load(Path path) throws IOException {
        try (var in = new BufferedInputStream(Files.newInputStream(path))) {
            return DexBackedDexFile.fromInputStream(null, in);
        }
    }

    private static byte[] encode(DexBackedDexFile original, Collection<? extends ClassDef> classes) throws IOException {
        var pool = new DexPool(original.getOpcodes());
        classes.stream().sorted(Comparator.comparing(ClassDef::getType)).forEach(pool::internClass);
        var store = new MemoryDataStore();
        pool.writeTo(store);
        return store.getData();
    }

    public static void main(String[] args) throws Exception {
        var baseline = load(Path.of(args[0]));
        var updated = load(Path.of(args[1]));
        var merged = new TreeMap<String, ClassDef>();
        for (var definition : baseline.getClasses()) merged.put(definition.getType(), definition);
        var families = new TreeSet<String>();
        for (int i = 3; i < args.length; ++i) {
            var family = "Lio/github/iamwilmar/mhriseport/" + args[i];
            if (!merged.containsKey(family + ";")) throw new IOException("Missing baseline " + family);
            families.add(family);
        }
        merged.keySet().removeIf(type -> selected(type, families));
        var retained = encode(baseline, merged.values());
        for (var definition : updated.getClasses())
            if (selected(definition.getType(), families)) merged.put(definition.getType(), definition);
        for (var family : families)
            if (!merged.containsKey(family + ";")) throw new IOException("Missing rebuilt " + family);
        var out = Path.of(args[2]);
        Files.write(out, encode(baseline, merged.values()));
        var roundTrip = load(out);
        var others = new ArrayList<ClassDef>();
        for (var definition : roundTrip.getClasses())
            if (!selected(definition.getType(), families)) others.add(definition);
        if (!Arrays.equals(retained, encode(baseline, others))) throw new IOException("Baseline class verification failed");
        System.out.printf("Verified %d retained DEX classes, rebuilt %d classes%n",
                          others.size(), merged.size() - others.size());
    }
}
