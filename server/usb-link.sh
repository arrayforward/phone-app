#!/usr/bin/env bash
# 配置手机端 USB RNDIS 点对点链路（root 手机，Git Bash 下执行）。
#
# 为什么需要它：企业/公共 WiFi 可能阻断客户端间 UDP（本项目的实测环境即如此），
# USB RNDIS 提供 PC↔手机点对点链路，延迟 ~2ms，不受 AP 策略影响。
#
# 该脚本做三件事：
#   1. 开启手机 USB RNDIS 网卡（rndis,adb，不断开 adb）
#   2. 给 rndis0 配 link-local 地址 169.254.0.1/16（PC 侧 RNDIS 网卡会自动
#      拿到 169.254.x.x，Windows 对 169.254/16 有 on-link 路由，免管理员改 IP）
#   3. 在 table wlan0 里加到 169.254.0.0/16 的路由——Android 策略路由把本机
#      发出的无 fwmark 包全部导向 table wlan0（见 ip rule 31000），不加这条
#      路由，手机回包会从 wlan0 走掉，单向不通
#
# 注意：以上配置重启手机后失效，需要重新执行本脚本。
set -euo pipefail

SERIAL="${1:-}"
ADB="adb ${SERIAL:+-s $SERIAL}"

echo "==> 开启 rndis,adb USB 功能"
$ADB shell "su -c 'setprop sys.usb.config rndis,adb'" || true
sleep 4
$ADB wait-for-device

echo "==> 配置 rndis0 地址与路由"
$ADB shell "su -c 'ip link set rndis0 up; \
    ip addr replace 169.254.0.1/16 dev rndis0; \
    ip route replace 169.254.0.0/16 dev rndis0 scope link src 169.254.0.1 table wlan0'"

echo "==> 状态："
$ADB shell "su -c 'ip -4 addr show rndis0 | grep inet; ip route get 169.254.159.20 2>/dev/null || true'"
echo
echo "完成。PC 侧应出现一个 RNDIS 网卡（自动获取 169.254.x.x 地址）。"
echo "客户端连接地址：169.254.0.1（手机）"
