#!/usr/bin/env bash
# 安装并启动 tightcast Android 客户端（需先 ./client-android/build.sh）
# 用法：./client-android/run.sh [serial]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="$ROOT/client-android/out/tightcast-client.apk"

ADB=(adb)
if [ $# -ge 1 ]; then
    ADB=(adb -s "$1")
fi

"${ADB[@]}" install -r "$APK"
"${ADB[@]}" shell am start -n com.tightcast.client/.MainActivity
echo "==> started. 默认连 192.168.43.1:8800（小米热点网关）；在界面里改 IP 后点连接"
