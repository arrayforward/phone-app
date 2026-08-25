package com.tightcast.server;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * 设备信息：主屏分辨率/旋转/layerStack（反射 DisplayManagerGlobal 隐藏 API），
 * 以及设备型号名。app_process 以 root/shell 身份运行，隐藏 API 可用。
 */
public final class DeviceInfo {

    public static final class Display {
        public int width;       // 主屏逻辑宽（已按旋转调整）
        public int height;      // 主屏逻辑高
        public int rotation;    // Surface.ROTATION_*
        public int layerStack;  // SurfaceControl.setDisplayLayerStack 所需
    }

    private DeviceInfo() {}

    /** 取主屏（displayId 0）当前信息。失败抛异常并打日志。 */
    public static Display getMainDisplay() throws Exception {
        Class<?> dmgClass = Class.forName("android.hardware.display.DisplayManagerGlobal");
        Method getInstance = dmgClass.getMethod("getInstance");
        Object dmg = getInstance.invoke(null);
        Method getDisplayInfo = dmgClass.getMethod("getDisplayInfo", int.class);
        Object info = getDisplayInfo.invoke(dmg, 0); // android.view.DisplayInfo
        if (info == null) {
            throw new IllegalStateException("DisplayManagerGlobal.getDisplayInfo(0) returned null");
        }
        Class<?> infoClass = info.getClass();
        Display d = new Display();
        d.width = getIntField(infoClass, info, "logicalWidth");
        d.height = getIntField(infoClass, info, "logicalHeight");
        d.rotation = getIntField(infoClass, info, "rotation");
        d.layerStack = getIntFieldOrDefault(infoClass, info, "layerStack", 0);
        return d;
    }

    public static String model() {
        return android.os.Build.MODEL;
    }

    private static int getIntField(Class<?> cls, Object obj, String name) throws Exception {
        Field f = cls.getField(name);
        return f.getInt(obj);
    }

    private static int getIntFieldOrDefault(Class<?> cls, Object obj, String name, int def) {
        try {
            Field f = cls.getField(name);
            return f.getInt(obj);
        } catch (Exception e) {
            System.out.println("[DeviceInfo] field " + name + " unavailable, fallback " + def);
            return def;
        }
    }
}
