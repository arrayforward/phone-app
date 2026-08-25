#!/usr/bin/env bash
# 部署并启动 server：push 到 /data/local/tmp/tightcast/ 后用 su app_process 启动。
# 用法：./server/run.sh [--no-su] [server 参数...]
#   --no-su   用 shell 身份跑（备用），默认 su（Magisk root）
set -euo pipefail

# Git Bash 会把 adb 的 /data/... 参数误转成 Windows 路径，禁止参数路径转换
export MSYS2_ARG_CONV_EXCL='*'

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE="${DEVICE:-f47720f0}"
REMOTE_DIR=/data/local/tmp/tightcast

USE_SU=1
ARGS=()
for a in "$@"; do
    if [ "$a" = "--no-su" ]; then
        USE_SU=0
    else
        ARGS+=("$a")
    fi
done

adb -s "$DEVICE" shell "mkdir -p $REMOTE_DIR"
adb -s "$DEVICE" push "$ROOT/server/out/server.jar" "$REMOTE_DIR/server.jar"
adb -s "$DEVICE" push "$ROOT/server/out/jni/libtight_jni.so" "$REMOTE_DIR/libtight_jni.so"

CMD="CLASSPATH=$REMOTE_DIR/server.jar app_process /system/bin com.tightcast.server.Server ${ARGS[*]:-}"
# 监督循环：对端掉线后 server 会主动退出（内核把 UDP socket connect 到旧对端，
# 必须新进程/新 socket 才能接新客户端），由 while 循环 1s 后拉起
LOOP="while true; do $CMD; sleep 1; done"
if [ "$USE_SU" = "1" ]; then
    echo "==> starting with su (supervised): $CMD"
    adb -s "$DEVICE" shell "su -c '$LOOP'"
else
    echo "==> starting as shell (supervised): $CMD"
    adb -s "$DEVICE" shell "$LOOP"
fi
