package com.tightcast.client;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * 文件日志（华为 ROM 默认抑制应用 logcat）：写 app 私有目录 files/client.log，
 * 无需权限。调试用：`adb pull /sdcard/Android/data/com.tightcast.client/files/client.log`。
 */
public final class Log2File {

    private static volatile File file;
    private static final SimpleDateFormat TS = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US);

    private Log2File() {}

    public static synchronized void init(File filesDir) {
        file = new File(filesDir, "client.log");
        log("=== log start ===");
    }

    public static void log(String msg) {
        File f = file;
        if (f == null) return;
        String line = TS.format(new Date()) + " " + msg + "\n";
        synchronized (Log2File.class) {
            try (FileWriter w = new FileWriter(f, true)) {
                w.write(line);
            } catch (IOException ignored) {}
        }
    }
}
