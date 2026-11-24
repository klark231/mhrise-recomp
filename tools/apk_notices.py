"""License files shared by the Android APK builders."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def notices(*, vendor=None, lua_deps=None):
    files = {
        "GPL-3.0.txt": ROOT / "LICENSE",
        "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md",
        "mod-archives.txt": ROOT / "android/assets/licenses/mod-archives.txt",
        "libadrenotools.txt": ROOT / "runtime/external/libadrenotools/LICENSE",
        "linkernsbypass.txt": ROOT / "runtime/external/libadrenotools/lib/linkernsbypass/LICENSE",
        "volk.txt": ROOT / "runtime/external/volk/LICENSE.md",
        "astcenc.txt": ROOT / "runtime/external/astcenc/LICENSE.txt",
        "SPIRV-Tools.txt": ROOT / "runtime/recompiler/LICENSE.SPIRV-Tools",
        "FFmpeg-LGPL-2.1.txt": ROOT / "android/assets/licenses/FFmpeg-LGPL-2.1.txt",
    }
    if vendor is not None:
        files.update({
            "fmt.txt": vendor / "fmt/LICENSE",
            "SPIRV-Headers.txt": vendor / "SPIRV-Headers/LICENSE",
            "sirit-LICENSE": vendor / "sirit/LICENSE",
            "sirit-LICENSE.txt": vendor / "sirit/LICENSE.txt",
        })
    if lua_deps is not None:
        files.update({
            "imgui.txt": lua_deps / "rise_imgui-src/LICENSE.txt",
            "freetype.txt": lua_deps / "freetype-src/docs/FTL.TXT",
            "json.txt": lua_deps / "rise_json-src/LICENSE.MIT",
        })
    result = {name: path.read_bytes() for name, path in files.items()}
    if lua_deps is not None:
        header = (lua_deps / "lua54-src/src/lua.h").read_bytes()
        start = header.rindex(b"/*")
        result["lua.txt"] = header[start:header.index(b"*/", start) + 2] + b"\n"
    return result


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, data in notices(vendor=ROOT / "third_party",
                              lua_deps=Path(sys.argv[2]) if len(sys.argv) > 2 else None).items():
        (out / name).write_bytes(data)
