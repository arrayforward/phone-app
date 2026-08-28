#!/system/bin/sh
# double-ycocg（默认模式）冒烟测试用常驻启动（监督循环），日志 server_d.log
while true; do
  CLASSPATH=/data/local/tmp/tightcast/server.jar \
    app_process /system/bin com.tightcast.server.Server \
    >> /data/local/tmp/tightcast/server_d.log 2>&1
  sleep 1
done
