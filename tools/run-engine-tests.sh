#!/bin/sh
# Build the OWIS engine test suite with the Android NDK and run it on a connected device/emulator.
# It exercises the engine against the simulator: scan, fault + freeze-frame decode, coding writes
# (0x27 unlock + 0x2E, wrong-key rejection), K-line KWP2000 SecurityAccess, guided jobs, VW TP 2.0, live data.
# This is the same software verification the desktop OWIS uses; it does not need a car or an adapter.
#
# Usage: tools/run-engine-tests.sh <path-to-a-data-pack.json>   (e.g. a G1 pack built with build_data.py)
set -e
PACK="${1:?give a data pack path, e.g. data/G1.json}"
: "${ANDROID_NDK:=$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/ndk/* 2>/dev/null | tail -1)}"
HOST=$(ls "$ANDROID_NDK/toolchains/llvm/prebuilt/" | head -1)
ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
case "$ABI" in
  arm64-v8a)  TRIPLE=aarch64-linux-android ;;
  x86_64)     TRIPLE=x86_64-linux-android ;;
  armeabi-v7a)TRIPLE=armv7a-linux-androideabi ;;
  *) echo "unsupported ABI $ABI"; exit 1 ;;
esac
CXX="$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST/bin/${TRIPLE}26-clang++"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT=$(mktemp -d)
"$CXX" -std=c++17 -O2 -I"$DIR/app/src/main/cpp/engine" "$DIR/tools/engine_test/test_engine.cpp" -o "$OUT/test_engine"
LIBCXX="$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST/sysroot/usr/lib/$TRIPLE/libc++_shared.so"
adb push "$OUT/test_engine" "$LIBCXX" /data/local/tmp/ >/dev/null
adb push "$PACK" /data/local/tmp/pack.json >/dev/null
adb shell chmod 755 /data/local/tmp/test_engine
adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./test_engine /data/local/tmp/pack.json /data/local/tmp"
