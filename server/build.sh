#!/usr/bin/env bash
# 一键构建：javac → d8 → out/server.jar；NDK cmake → out/jni/libtight_jni.so
# 用法：从项目根执行 ./server/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SDK="${ANDROID_SDK:-/c/Users/hubinix/AppData/Local/Android/Sdk}"
NDK="$SDK/ndk/30.0.14904198"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
D8="$SDK/build-tools/34.0.0/d8.bat"

OUT="$ROOT/server/out"
CLASSES="$OUT/classes"
rm -rf "$OUT"
mkdir -p "$CLASSES"

echo "==> [1/3] javac --release 8"
SOURCES=$(find server/java -name '*.java')
javac --release 8 -cp "$ANDROID_JAR" -d "$CLASSES" $SOURCES

echo "==> [2/3] d8 -> out/server.jar"
"$D8" --release --min-api 24 --lib "$ANDROID_JAR" \
    --output "$OUT/server.jar" $(find "$CLASSES" -name '*.class')

echo "==> [3/3] NDK cmake -> out/jni/libtight_jni.so"
cmake -S server/jni -B "$OUT/jni" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$NDK/build/cmake/android.toolchain.cmake")" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/jni"

echo "==> done"
ls -l "$OUT/server.jar" "$OUT/jni/libtight_jni.so"
