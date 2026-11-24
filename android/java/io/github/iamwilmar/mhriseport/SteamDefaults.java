package io.github.iamwilmar.mhriseport;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

// What a Steam save has that the Switch's doesn't, for a Switch save going to Steam with no Steam save of the player's
// to fill: the PC version's options, key bindings, version check and DLC list in data00-1.bin, its character options,
// external quests, telemetry fields and a copy of the hunter's ID in each slot, all with the values a new PC save starts
// with, and the roots in the PC version's order. With a Steam save to fill (SaveData.fill), that save's own values win.
//
// The values and the order are jinghaihan's, from mhrise-save-converter (MIT, src/defaults.rs), where a Switch save
// converted this way was loaded in the PC game. Written again here for this port's save tree.
final class SteamDefaults {
    private SteamDefaults() {}

    private static final int ARRAY = -1, BOOL = 0x02, U8 = 0x04, S32 = 0x07, U32 = 0x08, S64 = 0x09, STRUCT = 0x10,
            CLASS = 0x11, STRING = 0x0f;
    private static final int VERSION = 0xef10b158;
    private static final int SYSTEM_MARKER = 0xe2a3ea98, SLOT_MARKER = 0xd6f4726d, SAVE_FILE_DETAIL = 0x85e904c1;

    private static final int[] SYSTEM_ORDER = {0xe2a3ea98, 0xb0ca70c9, 0x913ecbe8, 0x649e38ff, 0x0b19f75b, 0x4d6487d4,
        0x5a04dbf9, 0x819571e6};
    // Also the order of a Microsoft Store slot made plain (its own has the same roots, in another order)
    static final int[] SLOT_ORDER = {0xd6f4726d, 0x1322883a, 0x9ccc3b1e, 0xadc075b6, 0x20d9167c, 0x93819625, 0x8512ab74,
        0x553d33b8, 0x68344f29, 0xb2129149, 0xb0ca70c9, 0xa81238c6, 0xbb431b76, 0x1e32797d, 0x355c8c4f, 0x8c6fb4c6,
        0xe58fb6c9, 0x1f613294, 0xf2fb669a, 0xe825861b, 0x356f270b, 0xef78287f, 0xf5e018a4, 0xcf6c5091, 0x81fcc8f4,
        0xab109098, 0xdc00f45c, 0xddb8e034, 0x4423bc21, 0x5a04dbf9, 0x164d2e71, 0x51dcd6fb, 0xb9c15dcf, 0xaaca38e3,
        0x22a8b022, 0x3da5b9de, 0xa21e01ad, 0x0386f39d, 0xd5f91c48};

    // The roots of a Switch file (data00-1.bin or a slot) with what Steam's has added; anything else as it is
    static List<SaveData.Root> add(List<SaveData.Root> roots) {
        List<SaveData.Root> out = new ArrayList<>(roots);
        if (has(out, SYSTEM_MARKER)) {
            loadInfo(out);
            dlc(out);
            upsert(out, versionChecker());
            upsert(out, systemOptions());
            upsert(out, keyConfig());
            return reorder(out, SYSTEM_ORDER);
        }
        if (has(out, SLOT_MARKER)) {
            consistencyId(out);
            telemetry(out);
            upsert(out, externalQuests());
            upsert(out, characterOptions());
            return reorder(out, SLOT_ORDER);
        }
        return out;
    }

    // ---- data00-1.bin ---------------------------------------------------------------------------------------------------

    private static void loadInfo(List<SaveData.Root> roots) {
        SaveData.Cls c = top(roots, 0x0b19f75b, 0xe6a69e8a);
        if (c != null) beforeVersion(c, array(0xbba20186, U8, 1, 16));
    }

    // The DLC flags: the PC version has a second list (what's available) after the first two
    private static void dlc(List<SaveData.Root> roots) {
        SaveData.Cls c = top(roots, 0x4d6487d4, 0xa0a6441d);
        if (c == null) return;
        SaveData.Field owned = field(c, 0x3811b01d);
        SaveData.Field available = owned != null ? new SaveData.Field(0x0a50d2dc, owned.type, owned.value)
                : classField(0x0a50d2dc, cls(0xfdb0c267, array(0x6829aab6, U32, 4, 0)));
        List<SaveData.Field> fields = new ArrayList<>();
        for (int h : new int[] {0x1fc1b1b3, 0x3811b01d, 0x4f127348, VERSION}) {
            SaveData.Field f = field(c, h);
            if (f != null) fields.add(f);
        }
        SaveData.Field version = fields.isEmpty() ? null : fields.remove(fields.size() - 1);
        fields.add(Math.min(2, fields.size()), available);
        if (version != null) fields.add(version);
        c.fields = fields;
    }

    private static SaveData.Root versionChecker() {
        return root(0x649e38ff, cls(0xf3a7e6a4, u32(VERSION, 827392000)));
    }

    private static SaveData.Root systemOptions() {
        return root(0x5a04dbf9, cls(0x609f5577, array(0x7b7aa988, S32, 4, 5), s32(0x66d8c850, 50), s32(0x25c4fa4e, 0),
                s32(0x6546cbdd, 50), s32(0x7e68451a, 0), bool(0x7216682b, true), bool(0xafa1edf9, false), u32(VERSION, 1001)));
    }

    private static SaveData.Root keyConfig() {
        SaveData.Cls user = cls(0x1d7e2ded,
                classField(0x4d3752f4, cls(0x0d2b2d0d, classArray(0x4d3752f4))),
                classField(0x73a07131, cls(0x33ad5ad6, string(0xbcf6bc33, "Default"), classArray(0x7680621f))));
        return root(0x819571e6, cls(0xd6331b34,
                classField(0x049bafc9, user),
                classField(0x64822a5f, cls(0xbf550512, classArray(0xe6665f54), classArray(0x912df7a2), classArray(0x73a07131))),
                classField(0x953d1f7b, cls(0x10c111cc, classArray(0xb998da40))),
                u32(VERSION, 1004)));
    }

    // ---- the slots ------------------------------------------------------------------------------------------------------

    // The PC version keeps the hunter's ID in the slot's system data too
    private static void consistencyId(List<SaveData.Root> roots) {
        byte[] id = new byte[16];
        SaveData.Cls record = top(roots, 0x355c8c4f, 0xaf6ea643);
        SaveData.Field f = record == null ? null : field(record, 0x0a960102);
        if (f != null && f.value instanceof SaveData.Val && ((SaveData.Val) f.value).data.length == 16) id = ((SaveData.Val) f.value).data.clone();
        SaveData.Cls system = top(roots, 0x356f270b, 0x5766f30b);
        if (system != null) beforeVersion(system, new SaveData.Field(0xe40fc0cd, STRUCT, new SaveData.Val(id)));
    }

    private static void telemetry(List<SaveData.Root> roots) {
        SaveData.Cls snapshot = top(roots, 0x1322883a, 0x8b6c3ddc);
        SaveData.Field f = snapshot == null ? null : field(snapshot, 0x0d011e4c);
        if (f == null || !(f.value instanceof SaveData.Cls) || ((SaveData.Cls) f.value).hash != 0x95320424) return;
        SaveData.Cls product = (SaveData.Cls) f.value;
        for (int h : new int[] {0xff28f5f3, 0x85b77a5a, 0x503b7ec1, 0x698ae462}) pushMissing(product, classArray(h));
        pushMissing(product, s64(0x6e232145));
        for (int h : new int[] {0x0a360ef6, 0xcb7a2960, 0x4627d929}) pushMissing(product, classArray(h));
        pushMissing(product, s64(0x640af498));
    }

    private static SaveData.Root externalQuests() {
        return root(0xbb431b76, cls(0xef94546d, array(0x014d9d98, BOOL, 1, 100), array(0x42ffa9f2, BOOL, 1, 100),
                array(0x26bc7c4a, BOOL, 1, 100), array(0xba5c3e3f, S64, 8, 100), u32(VERSION, 1)));
    }

    private static SaveData.Root characterOptions() {
        return root(0x5a04dbf9, cls(0xd71a55bd, array(0xf4f5060e, S32, 4, 2), array(0xe33aee58, S32, 4, 11),
                array(0x437130de, S32, 4, 7), u32(VERSION, 1000)));
    }

    // ---- the tree -------------------------------------------------------------------------------------------------------

    static List<SaveData.Root> reorder(List<SaveData.Root> roots, int[] order) {
        List<SaveData.Root> rest = new ArrayList<>(roots), out = new ArrayList<>();
        for (int h : order)
            for (Iterator<SaveData.Root> it = rest.iterator(); it.hasNext(); ) {
                SaveData.Root r = it.next();
                if (r.hash == h) {
                    out.add(r);
                    it.remove();
                    break;
                }
            }
        out.addAll(rest);
        // The file's details always last
        SaveData.Root detail = null;
        for (Iterator<SaveData.Root> it = out.iterator(); it.hasNext(); ) {
            SaveData.Root r = it.next();
            if (r.hash == SAVE_FILE_DETAIL) {
                detail = r;
                it.remove();
                break;
            }
        }
        if (detail != null) out.add(detail);
        return out;
    }

    private static boolean has(List<SaveData.Root> roots, int hash) {
        for (SaveData.Root r : roots)
            if (r.hash == hash) return true;
        return false;
    }

    private static SaveData.Cls top(List<SaveData.Root> roots, int hash, int clsHash) {
        for (SaveData.Root r : roots)
            if (r.hash == hash && r.cls.hash == clsHash) return r.cls;
        return null;
    }

    private static SaveData.Field field(SaveData.Cls c, int hash) {
        for (SaveData.Field f : c.fields)
            if (f.hash == hash) return f;
        return null;
    }

    private static void upsert(List<SaveData.Root> roots, SaveData.Root r) {
        for (int i = 0; i < roots.size(); i++)
            if (roots.get(i).hash == r.hash) {
                roots.set(i, r);
                return;
            }
        roots.add(r);
    }

    private static void beforeVersion(SaveData.Cls c, SaveData.Field f) {
        if (field(c, f.hash) != null) return;
        int at = c.fields.size();
        for (int i = 0; i < c.fields.size(); i++)
            if (c.fields.get(i).hash == VERSION) {
                at = i;
                break;
            }
        c.fields.add(at, f);
    }

    private static void pushMissing(SaveData.Cls c, SaveData.Field f) {
        if (field(c, f.hash) == null) c.fields.add(f);
    }

    private static SaveData.Root root(int hash, SaveData.Cls c) {
        return new SaveData.Root(hash, c);
    }

    private static SaveData.Cls cls(int hash, SaveData.Field... fields) {
        SaveData.Cls c = new SaveData.Cls();
        c.hash = hash;
        for (SaveData.Field f : fields) c.fields.add(f);
        return c;
    }

    private static SaveData.Field classField(int hash, SaveData.Cls c) {
        return new SaveData.Field(hash, CLASS, c);
    }

    // count zeroed values of memberSize bytes each
    private static SaveData.Field array(int hash, int memberType, int memberSize, int count) {
        SaveData.Arr a = new SaveData.Arr();
        a.memberType = memberType;
        a.memberSize = memberSize;
        a.count = count;
        a.packed = new byte[count * memberSize];
        return new SaveData.Field(hash, ARRAY, a);
    }

    private static SaveData.Field classArray(int hash) {
        SaveData.Arr a = new SaveData.Arr();
        a.memberType = CLASS;
        a.memberSize = 8;
        a.kind = 1;
        return new SaveData.Field(hash, ARRAY, a);
    }

    private static SaveData.Field scalar(int hash, int type, byte[] bytes) {
        return new SaveData.Field(hash, type, new SaveData.Val(bytes));
    }

    private static SaveData.Field u32(int hash, int v) {
        return scalar(hash, U32, ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array());
    }

    private static SaveData.Field s32(int hash, int v) {
        return scalar(hash, S32, ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array());
    }

    private static SaveData.Field s64(int hash) {
        return scalar(hash, S64, new byte[8]);
    }

    private static SaveData.Field bool(int hash, boolean v) {
        return scalar(hash, BOOL, new byte[] {(byte) (v ? 1 : 0)});
    }

    private static SaveData.Field string(int hash, String s) {
        return new SaveData.Field(hash, STRING, new SaveData.Str(s.getBytes(StandardCharsets.UTF_16LE)));
    }
}
