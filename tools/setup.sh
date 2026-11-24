#!/usr/bin/env bash
# Fetches third_party/ (not in git): what Eden's shader recompiler (runtime/eden) builds with, all pinned to known
# versions.
# Prepara third_party/ (no va en git): lo que usa el recompilador de shaders de Eden (runtime/eden), fijado a versiones
# conocidas.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

clone() {  # <folder> <repo> <tag>: shallow clone of a fixed version
  [ -d "$TP/$1" ] || git clone -q --depth 1 --branch "$3" "$2" "$TP/$1"
}

# Eden's shader recompiler isn't fetched any more: it's kept, with this project's changes, in runtime/eden.
# Same versions as Eden's cpmfile.json.
clone fmt https://github.com/fmtlib/fmt.git 12.1.0
clone sirit https://github.com/eden-emulator/sirit.git v1.0.5
clone SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers.git vulkan-sdk-1.4.341.0
# sirit expects SPIRV-Headers as a submodule in externals/.
rmdir "$TP/sirit/externals/SPIRV-Headers" 2> /dev/null || true
[ -e "$TP/sirit/externals/SPIRV-Headers" ] || ln -s ../../SPIRV-Headers "$TP/sirit/externals/SPIRV-Headers"

# Official Boost 1.92.0: only each module's headers (everything used is header-only).
if [ ! -d "$TP/boost" ]; then
  tmp="$TP/boost-dl"
  mkdir -p "$tmp"
  # GitHub sometimes answers 502 for a moment: retry instead of failing the build.
  curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors -o "$tmp/boost.tar.xz" \
    https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
  if tar --version 2> /dev/null | grep -q GNU; then
    tar xf "$tmp/boost.tar.xz" -C "$tmp" --wildcards --wildcards-match-slash '*/libs/*/include/*'
  else
    tar xf "$tmp/boost.tar.xz" -C "$tmp" --include='*/libs/*/include/*'
  fi
  mv "$tmp/boost-1.92.0" "$TP/boost"
  rm -rf "$tmp"
fi
echo "third_party ready"
