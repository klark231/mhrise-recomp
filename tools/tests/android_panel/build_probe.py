"""Build an isolated instrumentation APK against the classes and renderer in a signed Rise Lab APK.

Give the helper's private files directory the same drivers/ layout used by the game.
am instrument -w -e mode gmem -e panel gpu dev.rise.audit.panel/.Probe
Use scene=draw or contrast for a repeated depth prepass, blended draw and texture copy.
The passes argument changes GPU work per presented picture. feedback=static selects the fixed
artwork constructor in the refactored view. lua=idle or redraw adds the APK's GLES HUD backend.
wide=1 checks the scene around the controls with the counter disabled (no_fps in the helper).
load_step=1 compares a heavy/light/heavy sequence with idle-samples.rgb from pinned linear and
FSR runs, supplied as files/linear.rgb and files/fsr.rgb. Results describe the private display.
"""
import argparse
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("apk", "sdk", "ndk", "jdk", "keystore", "out"):
        parser.add_argument("--" + arg, type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.out == args.apk.resolve():
        parser.error("Choose a separate output for the test APK")
    sources = Path(__file__).resolve().parent
    runtime = sources.parents[2] / "runtime"
    suffix = ".exe" if os.name == "nt" else ""
    tag = {"Windows": "windows", "Linux": "linux", "Darwin": "darwin"}[platform.system()] + "-x86_64"
    compiler = args.ndk.resolve() / "toolchains/llvm/prebuilt" / tag / "bin" / ("clang++" + suffix)
    jdk = args.jdk.resolve() / "bin"
    tools = args.sdk.resolve() / "build-tools/36.0.0"
    android = args.sdk.resolve() / "platforms/android-34/android.jar"
    signer = [jdk / ("java" + suffix), "-jar", tools / "lib/apksigner.jar"]
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp)

        def run(*argv):
            subprocess.run(argv, check=True, cwd=stage)

        run(*signer, "verify", args.apk.resolve())
        glslc = args.ndk.resolve() / "shader-tools" / tag / ("glslc" + suffix)
        with (stage / "scene_shaders.h").open("w", encoding="ascii") as header:
            for name, shader, options in [
                ("scene_vert", sources.parent / "texture_views.vert", []),
                ("scene_depth", sources / "scene.frag", ["-DDEPTH_PREPASS"]),
                ("scene_color", sources / "scene.frag", []),
            ]:
                run(glslc, "--target-env=vulkan1.1", *options, shader, "-o", "scene.spv")
                words = struct.iter_unpack("<I", (stage / "scene.spv").read_bytes())
                header.write(f"constexpr uint32_t {name}[] = {{" + ",".join(hex(w) for w, in words) + "};\n")
        with ZipFile(args.apk) as baseline:
            (stage / "libnnsdk.so").write_bytes(baseline.read("lib/arm64-v8a/libnnsdk.so"))
            run(compiler, "--driver-mode=g++", "--target=aarch64-linux-android30", "-std=c++20", "-O2",
                "-fPIC", "-shared", "-static-libstdc++", "-DVK_USE_PLATFORM_ANDROID_KHR", "-DVK_NO_PROTOTYPES",
                "-I", stage, "-I", runtime / "src", "-I", runtime / "recompiler", "-I", runtime / "external/volk",
                sources / "panel_device.cpp", "libnnsdk.so", "-landroid", "-llog", "-ldl", "-o", "libpanel_probe.so")
            (stage / "classes").mkdir()
            run(jdk / ("javac" + suffix), "--release", "11", "-encoding", "UTF-8", "-cp", android, "-d", "classes",
                sources / "Probe.java", sources / "LuaHudProbe.java")
            run(jdk / ("jar" + suffix), "cf", "probe.jar", "-C", "classes", ".")
            run(jdk / ("java" + suffix), "-cp", tools / "lib/d8.jar", "com.android.tools.r8.D8",
                "--min-api", "30", "--lib", android, "--output", stage, "probe.jar")
            run(tools / ("aapt2" + suffix), "link", "--manifest", sources / "AndroidManifest.xml",
                "-I", android, "--debug-mode", "-o", "base.apk")
            with ZipFile(stage / "base.apk", "a", ZIP_DEFLATED) as apk:
                apk.write(stage / "classes.dex", "classes.dex")
                for entry in baseline.infolist():
                    dex = re.fullmatch(r"classes(\d*)\.dex", entry.filename)
                    if dex:
                        apk.writestr(f"classes{int(dex[1] or 1) + 1}.dex", baseline.read(entry))
                    elif entry.filename.startswith("lib/"):
                        apk.writestr(entry, baseline.read(entry))
                apk.write(stage / "libpanel_probe.so", "lib/arm64-v8a/libpanel_probe.so")
        run(tools / ("zipalign" + suffix), "-f", "-P", "16", "4", "base.apk", "aligned.apk")
        run(*signer, "sign", "--ks", args.keystore.resolve(), "--ks-pass", "pass:android",
            "--out", args.out, "aligned.apk")
        run(*signer, "verify", args.out)


if __name__ == "__main__":
    main()
