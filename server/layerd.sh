#!/system/bin/sh
# layer 模式冒烟测试用常驻启动（监督循环）：server 退出（对端掉线主动退出）1s 后拉起
# 用法：adb shell "su -c 'setsid /system/bin/sh /data/local/tmp/tightcast/layerd.sh </dev/null >/dev/null 2>&1 &'"
while true; do
  CLASSPATH=/data/local/tmp/tightcast/server.jar \
    app_process /system/bin com.tightcast.server.Server --mode layer \
    >> /data/local/tmp/tightcast/server.log 2>&1
  sleep 1
done
