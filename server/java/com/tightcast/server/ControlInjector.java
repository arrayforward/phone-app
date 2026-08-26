package com.tightcast.server;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * 控制命令注入（协议第 5 节）：TOUCH / KEY / TEXT / SCROLL / REQ_KEYFRAME。
 * 经 InputManager.injectInputEvent 隐藏 API 注入（root/shell uid 有 INJECT_EVENTS 权限）。
 *
 * 坐标约定：client 发归一化坐标（相对视频画面），视频为主屏全屏镜像，故乘
 * 主屏逻辑分辨率得到注入像素坐标。
 */
public final class ControlInjector {

    private static final int INJECT_INPUT_EVENT_MODE_ASYNC = 0;

    private final ScreenEncoder encoder;

    private Object inputManager;
    private Method injectInputEvent;

    // 单点触控状态机
    private boolean touchDown;
    private float lastX;
    private float lastY;

    public ControlInjector(ScreenEncoder encoder) {
        this.encoder = encoder;
        try {
            Class<?> imClass = Class.forName("android.hardware.input.InputManager");
            try {
                Method getInstance = imClass.getMethod("getInstance");
                inputManager = getInstance.invoke(null);
            } catch (NoSuchMethodException e) {
                // getInstance 缺失时反射构造
                inputManager = imClass.getDeclaredConstructor().newInstance();
            }
            injectInputEvent = imClass.getMethod("injectInputEvent", InputEvent.class, int.class);
            System.out.println("[ControlInjector] InputManager ready");
        } catch (Exception e) {
            System.out.println("[ControlInjector] InputManager init failed: " + e);
            e.printStackTrace(System.out);
        }
    }

    /** native 线程回调入口：只做解析与注入，快速返回。 */
    public void handle(byte[] cmd) {
        if (cmd == null || cmd.length == 0) return;
        try {
            switch (cmd[0] & 0xFF) {
                case 0x01:
                    onTouch(cmd);
                    break;
                case 0x02:
                    onKey(cmd);
                    break;
                case 0x03:
                    onText(cmd);
                    break;
                case 0x04:
                    onScroll(cmd);
                    break;
                case 0x05:
                    encoder.requestKeyframe();
                    break;
                case 0x06:  // SET_FORMAT（协议 §5）：切换视频格式模式
                    if (cmd.length >= 2) encoder.setFormat(cmd[1] & 0xFF);
                    break;
                case 0x07:  // REQ_ENH_KEYFRAME（协议 §3.4）：增强层断链恢复
                    encoder.requestEnhKeyframe();
                    break;
                default:
                    System.out.println("[ControlInjector] unknown command 0x"
                            + Integer.toHexString(cmd[0] & 0xFF));
            }
        } catch (Exception e) {
            System.out.println("[ControlInjector] handle error: " + e);
        }
    }

    private void inject(InputEvent event) {
        if (inputManager == null || injectInputEvent == null) return;
        try {
            injectInputEvent.invoke(inputManager, event, INJECT_INPUT_EVENT_MODE_ASYNC);
        } catch (Exception e) {
            System.out.println("[ControlInjector] inject failed: " + e);
        }
    }

    /** 归一化坐标 → 主屏像素坐标。 */
    private static float[] toDevicePixels(float nx, float ny) throws Exception {
        DeviceInfo.Display d = DeviceInfo.getMainDisplay();
        return new float[] {nx * d.width, ny * d.height};
    }

    // ---- 0x01 TOUCH：u8 action(0=DOWN 1=UP 2=MOVE) u8 slot f32 x f32 y ----

    private void onTouch(byte[] cmd) throws Exception {
        if (cmd.length < 11) return;
        ByteBuffer buf = ByteBuffer.wrap(cmd).order(ByteOrder.BIG_ENDIAN);
        buf.get();                    // type
        int action = buf.get() & 0xFF;
        buf.get();                    // slot（v1 仅 0）
        float nx = buf.getFloat();
        float ny = buf.getFloat();
        float[] px = toDevicePixels(nx, ny);
        float x = px[0];
        float y = px[1];
        long now = SystemClock.uptimeMillis();

        int motionAction;
        switch (action) {
            case 0:
                motionAction = MotionEvent.ACTION_DOWN;
                touchDown = true;
                break;
            case 1:
                motionAction = MotionEvent.ACTION_UP;
                touchDown = false;
                break;
            case 2:
                if (!touchDown) return; // 无 DOWN 的 MOVE 忽略
                motionAction = MotionEvent.ACTION_MOVE;
                break;
            default:
                return;
        }
        lastX = x;
        lastY = y;
        MotionEvent event = obtainPointerEvent(now, motionAction, x, y);
        inject(event);
        event.recycle();
    }

    private static MotionEvent obtainPointerEvent(long now, int action, float x, float y) {
        MotionEvent.PointerProperties props = new MotionEvent.PointerProperties();
        props.id = 0;
        props.toolType = MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords coords = new MotionEvent.PointerCoords();
        coords.x = x;
        coords.y = y;
        coords.pressure = 1.0f;
        coords.size = 1.0f;
        return MotionEvent.obtain(now, now, action, 1,
                new MotionEvent.PointerProperties[] {props},
                new MotionEvent.PointerCoords[] {coords},
                0, 0, 1.0f, 1.0f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
    }

    // ---- 0x02 KEY：u8 action(0=DOWN 1=UP) s32 keycode ----

    private void onKey(byte[] cmd) {
        if (cmd.length < 6) return;
        ByteBuffer buf = ByteBuffer.wrap(cmd).order(ByteOrder.BIG_ENDIAN);
        buf.get();
        int action = buf.get() & 0xFF;
        int keycode = buf.getInt();
        long now = SystemClock.uptimeMillis();
        int eventAction = action == 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP;
        inject(new KeyEvent(now, now, eventAction, keycode, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
    }

    // ---- 0x03 TEXT：UTF-8 文本，可打印 ASCII → (keycode, shift) 逐字符 DOWN+UP ----

    private void onText(byte[] cmd) {
        if (cmd.length < 2) return;
        String text = new String(cmd, 1, cmd.length - 1, StandardCharsets.UTF_8);
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            int keycode = keycodeFor(c);
            if (keycode < 0) continue; // 无法映射的字符忽略
            int meta = needsShift(c) ? KeyEvent.META_SHIFT_ON : 0;
            inject(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keycode, 0, meta,
                    KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
            inject(new KeyEvent(now, now, KeyEvent.ACTION_UP, keycode, 0, meta,
                    KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
        }
    }

    private static boolean needsShift(char c) {
        return (c >= 'A' && c <= 'Z') || "!@#$%^&*()_+{}|:\"<>?~".indexOf(c) >= 0;
    }

    private static int keycodeFor(char c) {
        if (c >= 'a' && c <= 'z') return KeyEvent.KEYCODE_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return KeyEvent.KEYCODE_A + (c - 'A');
        if (c >= '0' && c <= '9') return KeyEvent.KEYCODE_0 + (c - '0');
        switch (c) {
            case ' ': return KeyEvent.KEYCODE_SPACE;
            case '\n': case '\r': return KeyEvent.KEYCODE_ENTER;
            case '\t': return KeyEvent.KEYCODE_TAB;
            case '!': return KeyEvent.KEYCODE_1;
            case '@': return KeyEvent.KEYCODE_2;
            case '#': return KeyEvent.KEYCODE_3;
            case '$': return KeyEvent.KEYCODE_4;
            case '%': return KeyEvent.KEYCODE_5;
            case '^': return KeyEvent.KEYCODE_6;
            case '&': return KeyEvent.KEYCODE_7;
            case '*': return KeyEvent.KEYCODE_8;
            case '(': return KeyEvent.KEYCODE_9;
            case ')': return KeyEvent.KEYCODE_0;
            case '-': case '_': return KeyEvent.KEYCODE_MINUS;
            case '=': case '+': return KeyEvent.KEYCODE_EQUALS;
            case '[': case '{': return KeyEvent.KEYCODE_LEFT_BRACKET;
            case ']': case '}': return KeyEvent.KEYCODE_RIGHT_BRACKET;
            case '\\': case '|': return KeyEvent.KEYCODE_BACKSLASH;
            case ';': case ':': return KeyEvent.KEYCODE_SEMICOLON;
            case '\'': case '"': return KeyEvent.KEYCODE_APOSTROPHE;
            case ',': case '<': return KeyEvent.KEYCODE_COMMA;
            case '.': case '>': return KeyEvent.KEYCODE_PERIOD;
            case '/': case '?': return KeyEvent.KEYCODE_SLASH;
            case '`': case '~': return KeyEvent.KEYCODE_GRAVE;
            default: return -1;
        }
    }

    // ---- 0x04 SCROLL：f32 x f32 y f32 dy → ACTION_SCROLL + AXIS_VSCROLL ----

    private void onScroll(byte[] cmd) throws Exception {
        if (cmd.length < 13) return;
        ByteBuffer buf = ByteBuffer.wrap(cmd).order(ByteOrder.BIG_ENDIAN);
        buf.get();
        float nx = buf.getFloat();
        float ny = buf.getFloat();
        float dy = buf.getFloat();
        float[] px = toDevicePixels(nx, ny);
        long now = SystemClock.uptimeMillis();

        MotionEvent.PointerProperties props = new MotionEvent.PointerProperties();
        props.id = 0;
        props.toolType = MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords coords = new MotionEvent.PointerCoords();
        coords.x = px[0];
        coords.y = px[1];
        coords.setAxisValue(MotionEvent.AXIS_VSCROLL, dy);
        MotionEvent event = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1,
                new MotionEvent.PointerProperties[] {props},
                new MotionEvent.PointerCoords[] {coords},
                0, 0, 1.0f, 1.0f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        inject(event);
        event.recycle();
    }
}
