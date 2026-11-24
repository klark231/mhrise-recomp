"""Prepare the pinned Java/JNI archive reader for the two non-Gradle APK builders."""
import hashlib
from pathlib import Path
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen
from zipfile import ZipFile

VERSION = "1.1.6"
SHA256 = "06d97a37872a9574ceb2bf94a3ad8dc38fcc89a60f2424067b20eb5efaa62189"
PATH = f"me/zhanghai/android/libarchive/library/{VERSION}/library-{VERSION}.aar"
# Maven Central, then Google's copy of it: Central answers 429 to CI runners now and then. The checksum is the same.
MIRRORS = ("https://repo.maven.apache.org/maven2/", "https://maven-central.storage-download.googleapis.com/maven2/")


def fetch():
    errors = []
    for attempt in range(4):
        for mirror in MIRRORS:
            try:
                with urlopen(mirror + PATH, timeout=60) as response:
                    return response.read()
            except (HTTPError, URLError, TimeoutError) as e:
                errors.append(f"{mirror}: {e}")
        time.sleep(10 * (attempt + 1))
    raise SystemExit("libarchive Android artifact unavailable:\n  " + "\n  ".join(errors))

def prepare(staging):
    cache = Path(__file__).resolve().parents[1] / "build/mod-dependencies"
    cache.mkdir(parents=True, exist_ok=True)
    aar = cache / f"libarchive-{VERSION}.aar"
    if not aar.is_file() or hashlib.sha256(aar.read_bytes()).hexdigest() != SHA256:
        with tempfile.TemporaryDirectory(dir=cache) as tmp:
            download = Path(tmp) / "archive.aar"
            download.write_bytes(fetch())
            if hashlib.sha256(download.read_bytes()).hexdigest() != SHA256:
                raise ValueError("Unexpected libarchive Android artifact checksum")
            download.replace(aar)
    with ZipFile(aar) as source:
        (staging / "mod-archive.jar").write_bytes(source.read("classes.jar"))
        native = staging / "lib/arm64-v8a/libarchive-jni.so"
        native.parent.mkdir(parents=True, exist_ok=True)
        native.write_bytes(source.read("jni/arm64-v8a/libarchive-jni.so"))

if __name__ == "__main__":
    prepare(Path(sys.argv[1]))
