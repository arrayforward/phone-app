#!/usr/bin/env bash
# 一键构建 tightcast Android 客户端 APK：
#   NDK cmake → libtightcast_client.so；javac → d8 → classes.dex；
#   aapt2 → base.apk；python zipfile 塞入 dex/so（.so 不压缩）→ zipalign → apksigner
# 用法：从项目根执行 ./client-android/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SDK="${ANDROID_SDK:-/c/Users/hubinix/AppData/Local/Android/Sdk}"
NDK="$SDK/ndk/30.0.14904198"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
BUILD_TOOLS="$SDK/build-tools/34.0.0"
D8="$BUILD_TOOLS/d8.bat"
AAPT2="$BUILD_TOOLS/aapt2.exe"
ZIPALIGN="$BUILD_TOOLS/zipalign.exe"
APKSIGNER="$BUILD_TOOLS/apksigner.bat"

OUT="$ROOT/client-android/out"
CLASSES="$OUT/classes"
rm -rf "$OUT"
mkdir -p "$CLASSES" "$OUT/dex" "$OUT/jni"

echo "==> [1/5] NDK cmake -> libtightcast_client.so"
cmake -S client-android/jni -B "$OUT/jni" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$NDK/build/cmake/android.toolchain.cmake")" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/jni"

echo "==> [2/5] javac --release 8"
SOURCES=$(find client-android/app/java -name '*.java')
javac --release 8 -cp "$ANDROID_JAR" -d "$CLASSES" $SOURCES

echo "==> [3/5] d8 -> classes.dex"
"$D8" --release --min-api 24 --lib "$ANDROID_JAR" \
    --output "$OUT/dex" $(find "$CLASSES" -name '*.class')

echo "==> [4/5] aapt2 link -> base.apk"
"$AAPT2" link -o "$OUT/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest client-android/app/AndroidManifest.xml \
    --min-sdk-version 24 \
    --target-sdk-version 34

echo "==> [5/5] package + zipalign + sign"
# classes.dex 压缩、libtightcast_client.so 不压缩（zipalign -p 对齐页边界）
python - "$OUT/base.apk" "$OUT/dex/classes.dex" "$OUT/jni/libtightcast_client.so" <<'EOF'
import sys, zipfile
apk, dex, so = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(apk, 'a') as z:
    z.write(dex, 'classes.dex', zipfile.ZIP_DEFLATED)
    z.write(so, 'lib/arm64-v8a/libtightcast_client.so', zipfile.ZIP_STORED)
print("packaged classes.dex + libtightcast_client.so")
EOF

"$ZIPALIGN" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"

KEYSTORE="$ROOT/client-android/debug.keystore"  # 放 out/ 之外，避免每次构建重新生成
if [ ! -f "$KEYSTORE" ]; then
    # keytool 未必在 PATH：优先 JAVA_HOME，其次常见 JDK 安装目录
    KEYTOOL="keytool"
    if ! command -v keytool >/dev/null 2>&1; then
        for c in "${JAVA_HOME:-}/bin/keytool.exe" \
                 "/c/Program Files/Java/jdk-21.0.10/bin/keytool.exe" \
                 "/c/Program Files/Android/Android Studio/jbr/bin/keytool.exe"; do
            if [ -f "$c" ]; then KEYTOOL="$c"; break; fi
        done
    fi
    "$KEYTOOL" -genkeypair -v -keystore "$KEYSTORE" -alias android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass android -keypass android \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null
    [ -f "$KEYSTORE" ] || { echo "keytool failed to create keystore"; exit 1; }
fi
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/tightcast-client.apk" "$OUT/aligned.apk"
# apksigner.bat 失败也可能返回 0：显式校验产物存在且可验证
[ -f "$OUT/tightcast-client.apk" ] || { echo "apksigner produced no apk"; exit 1; }
"$APKSIGNER" verify --print-certs "$OUT/tightcast-client.apk" | head -1

echo "==> done"
ls -l "$OUT/tightcast-client.apk"
