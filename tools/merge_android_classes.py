"""Merge selected Java class families into a deployed MOD build."""
import hashlib
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from urllib.request import urlopen
from zipfile import ZipFile


JARS = {
    "org/smali/dexlib2/2.5.2/dexlib2-2.5.2.jar": "5a5c8982d8bd7d6e3bb1a0713049e3c78b719ec32b20f6b619885cec30a0dd61",
    "com/google/guava/guava/32.1.3-jre/guava-32.1.3-jre.jar": "6d4e2b5a118aab62e6e5e29d185a0224eed82c85c40ac3d33cf04a270c3b3744",
    "com/google/guava/failureaccess/1.0.1/failureaccess-1.0.1.jar": "a171ee4c734dd2da837e4b16be9df4661afab72a41adaf31eb84dfdaf936ca26",
}


def merge_classes(root, staging, baseline, updated, jdk, families):
    cache = root / "build/mod-dependencies"
    cache.mkdir(parents=True, exist_ok=True)
    jars = []
    for path, digest in JARS.items():
        jar = cache / Path(path).name
        if not jar.is_file() or hashlib.sha256(jar.read_bytes()).hexdigest() != digest:
            with TemporaryDirectory(dir=cache) as tmp:
                local = Path(tmp) / jar.name
                with urlopen("https://repo.maven.apache.org/maven2/" + path, timeout=60) as source:
                    local.write_bytes(source.read())
                if hashlib.sha256(local.read_bytes()).hexdigest() != digest:
                    raise ValueError(f"Unexpected checksum for {jar.name}")
                local.replace(jar)
        jars.append(str(jar))
    original = staging / "baseline.dex"
    with ZipFile(baseline) as apk:
        original.write_bytes(apk.read("classes.dex"))
    result = staging / "merged.dex"
    subprocess.run([jdk / "java.exe", "-cp", os.pathsep.join(jars), root / "tools/MergeAndroidClasses.java",
                    original, updated, result, *families], check=True)
    return result
