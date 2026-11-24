#!/usr/bin/env bash
# Builds and runs tools/astc_gpu_test.cpp (see there) on the host: shaders/astc_encode.comp's encoder as C++, checked
# against ARM's decoder.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
sed -n '/^\/\/ ASTC_CODE_BEGIN$/,/^\/\/ ASTC_CODE_END$/p' "$ROOT/runtime/src/nnsdk/nvn/shaders/astc_encode.comp" > "$TMP/astc_encode_code.inc"
mkdir -p "$TMP/nnsdk/nvn"
# bcn.cpp only needs Vulkan's format names from vk.h: the headers of the NDK's sysroot, or VULKAN_INCLUDE
printf '#pragma once\n#include <vulkan/vulkan_core.h>\n' > "$TMP/nnsdk/nvn/vk.h"
if [ -z "${VULKAN_INCLUDE:-}" ]; then
  VULKAN_INCLUDE="$(ls -d "${ANDROID_NDK_HOME:-${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/28.2.13676358}"/toolchains/llvm/prebuilt/*/sysroot/usr/include 2>/dev/null | head -1)"
fi
${CXX:-c++} -std=c++20 -O2 -w -I "$TMP" -I "$VULKAN_INCLUDE" -I "$ROOT/runtime/src" -I "$ROOT/runtime/external/astcenc" \
  -DASTCENC_NEON=0 -DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -DASTCENC_SVE=0 \
  "$ROOT/tools/astc_gpu_test.cpp" "$ROOT/runtime/src/nnsdk/nvn/bcn.cpp" "$ROOT"/runtime/external/astcenc/*.cpp \
  -o "$TMP/astc_gpu_test" -lpthread
"$TMP/astc_gpu_test"
