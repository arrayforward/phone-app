package com.tightcast.client;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * 主界面：连接栏（IP/端口/模式）+ GLSurfaceView 画面 + 状态栏。
 * 全部转发到 C++ 核心（NativeClient）；本类只做 UI 与输入映射。
 *
 * 输入约定：
 * - 触控：单指点击/拖拽 → 相对视频画面的归一化坐标（letterbox 居中区外钳制）
 * - 物理按键：直通 Android keycode 注入到 server 手机（BACK 键留给本机退出）
 * - 三个屏幕按钮：返回/Home/最近任务（对应 KEYCODE 4/3/187）
 * - 双指纵向拖动 → SCROLL
 */
public final class MainActivity extends Activity {

    private static final String[] MODE_NAMES = {
            "double-ycocg", "double-raw", "single", "layer（分层）"
    };
    // 协议 §5 命令 0x06 位域：bit0=single bit1=ycocg bit2=layer
    private static final int[] MODE_VALUES = {0b010, 0b000, 0b001, 0b101};

    private GLSurfaceView glView;
    private EditText ipInput;
    private EditText portInput;
    private Button connectBtn;
    private TextView statusText;
    private boolean connected;
    // 模式下拉初始 onItemSelected 会自动触发——只有用户触碰过 spinner 才允许下发
    // SET_FORMAT（否则会把 server 的 --mode 启动参数覆盖回默认值）
    private boolean modeArmed;
    private boolean wasOnline;
    private MicCapture mic;
    private VideoDec baseDec;
    private VideoDec enhDec;
    private Spinner modeSpinner;
    private final Handler handler = new Handler(Looper.getMainLooper());

    // 触控状态
    private long lastMoveSent;
    private float lastX, lastY;
    private boolean twoFingerScroll;
    private float scrollLastY;

    // D8 对匿名类/非静态内部类有内部 NPE bug：一律 static 具名嵌套 + owner 引用
    private static final class GlRenderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            NativeClient.nativeSurfaceCreated();
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int w, int h) {
            NativeClient.nativeSurfaceChanged(w, h);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            NativeClient.nativeDrawFrame();
        }
    }

    private static final class ModeSelectListener implements AdapterView.OnItemSelectedListener {
        private final MainActivity owner;

        ModeSelectListener(MainActivity owner) {
            this.owner = owner;
        }

        @Override
        public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
            // 持久化用户选择（恢复选中也会触发本回调，写回同值无害）
            owner.getPreferences(MODE_PRIVATE).edit().putInt("mode_pos", pos).apply();
            if (owner.connected && owner.modeArmed)
                NativeClient.nativeSetMode(MODE_VALUES[pos]);
        }

        @Override
        public void onNothingSelected(AdapterView<?> parent) {}
    }

    private static final class StatusTick implements Runnable {
        private final MainActivity owner;

        StatusTick(MainActivity owner) {
            this.owner = owner;
        }

        @Override
        public void run() {
            if (owner.connected) {
                boolean online = NativeClient.nativeIsOnline();
                if (online && !owner.wasOnline && owner.modeSpinner != null) {
                    // 上线沿：以本机持久化的模式为准下发 SET_FORMAT
                    int pos = owner.modeSpinner.getSelectedItemPosition();
                    if (pos >= 0 && pos < MODE_VALUES.length)
                        NativeClient.nativeSetMode(MODE_VALUES[pos]);
                }
                owner.wasOnline = online;
                int[] size = NativeClient.nativeVideoSize();
                String decStat = (owner.baseDec != null && owner.enhDec != null)
                        ? " " + owner.baseDec.status() + owner.enhDec.status() : "";
                owner.statusText.setText(online
                        ? "已连接 " + size[0] + "x" + size[1] + " "
                                + NativeClient.nativeStatsLine() + decStat
                        : "等待对端…");
            } else {
                owner.wasOnline = false;
            }
            owner.handler.postDelayed(this, 1000);
        }
    }

    // adb 自动化冒烟：am start --es host <ip> --ei port <p> --ez autoconnect true
    // [--ei mode <位域>]（mode 在连接 2s 后下发 SET_FORMAT，等 online 生效）
    private static final class AutoConnectTick implements Runnable {
        private final MainActivity owner;

        AutoConnectTick(MainActivity owner) {
            this.owner = owner;
        }

        @Override
        public void run() {
            int mode = owner.getIntent().getIntExtra("mode", -1);
            if (mode >= 0) {
                owner.modeArmed = true;  // 自动化显式指定：武装并下发
                NativeClient.nativeSetMode(mode);
            }
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log2File.init(getExternalFilesDir(null));
        Log2File.log("MainActivity onCreate");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        // 连接栏行 1：IP（弹性宽）/ 端口
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        ipInput = new EditText(this);
        // 上次成功连接的 IP 优先（热点 IP 因组网而异），否则小米热点经典默认值
        ipInput.setText(getPreferences(MODE_PRIVATE).getString("host", "192.168.43.1"));
        ipInput.setHint("server IP（热点主机 IP，如 192.168.x.x）");
        ipInput.setInputType(InputType.TYPE_CLASS_TEXT);
        bar.addView(ipInput, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        portInput = new EditText(this);
        portInput.setText("8800");
        portInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        portInput.setEms(4);
        bar.addView(portInput);
        root.addView(bar);

        // 连接栏行 2：模式下拉（弹性宽）/ 连接按钮（必须可见——此前单行被挤出屏幕）
        LinearLayout bar2 = new LinearLayout(this);
        bar2.setOrientation(LinearLayout.HORIZONTAL);
        modeSpinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, MODE_NAMES);
        modeSpinner.setAdapter(adapter);
        // 恢复上次选择的模式（状态保存）；触发的初始 onItemSelected 因未连接不会下发
        modeSpinner.setSelection(getPreferences(MODE_PRIVATE).getInt("mode_pos", 0));
        bar2.addView(modeSpinner, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        connectBtn = new Button(this);
        connectBtn.setText("连接");
        bar2.addView(connectBtn);
        root.addView(bar2);

        // 系统键栏：返回 / Home / 最近任务（物理键直通给被控机，本机用按钮发系统键）
        LinearLayout keys = new LinearLayout(this);
        keys.setOrientation(LinearLayout.HORIZONTAL);
        addKeyButton(keys, "返回", KeyEvent.KEYCODE_BACK);
        addKeyButton(keys, "Home", KeyEvent.KEYCODE_HOME);
        addKeyButton(keys, "最近", KeyEvent.KEYCODE_APP_SWITCH);
        root.addView(keys);

        glView = new GLSurfaceView(this);
        glView.setEGLContextClientVersion(2);
        glView.setPreserveEGLContextOnPause(true);
        glView.setRenderer(new GlRenderer());
        glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        glView.setOnTouchListener((v, ev) -> onGlTouch(ev));
        LinearLayout.LayoutParams glParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f);
        root.addView(glView, glParams);

        statusText = new TextView(this);
        statusText.setText("未连接");
        root.addView(statusText);

        setContentView(root);

        connectBtn.setOnClickListener(v -> toggleConnect());
        modeSpinner.setOnItemSelectedListener(new ModeSelectListener(this));
        // 用户触碰后才武装模式下拉（初始选中回调不下发，见 modeArmed 注释）
        modeSpinner.setOnTouchListener((v, ev) -> {
            modeArmed = true;
            return false;
        });

        mic = new MicCapture();
        startStatusLoop();

        // Java MediaCodec 解码器（layer 0=基础 1=增强）：NDK AImageReader 输出面
        // 在华为 EMUI 上 configure 失败（-10000），buffer 模式为主路径
        baseDec = new VideoDec(0);
        enhDec = new VideoDec(1);
        NativeClient.nativeSetVideoDecs(baseDec, enhDec);

        // adb 自动化冒烟参数（见 AutoConnectTick 注释）；enhWaitMs 调 grace 窗口
        android.content.Intent it = getIntent();
        if (it != null) {
            int enhWait = it.getIntExtra("enhWaitMs", -1);
            if (enhWait >= 0) NativeClient.nativeSetEnhWait(enhWait);
            String host = it.getStringExtra("host");
            if (host != null && !host.isEmpty()) ipInput.setText(host);
            int port = it.getIntExtra("port", -1);
            if (port > 0) portInput.setText(String.valueOf(port));
            int mode = it.getIntExtra("mode", -1);
            if (mode >= 0) {
                for (int i = 0; i < MODE_VALUES.length; ++i) {
                    if (MODE_VALUES[i] == mode) {
                        modeSpinner.setSelection(i);
                        // 显式测试参数优先于持久化值，并写回持久化（避免
                        // 上线沿的持久化下发把显式参数覆盖掉——实测曾因此
                        // 被 layer 回盖导致双模式黑屏 51s）
                        getPreferences(MODE_PRIVATE).edit().putInt("mode_pos", i).apply();
                        break;
                    }
                }
            }
            if (it.getBooleanExtra("autoconnect", false) && !connected) {
                toggleConnect();
                if (mode >= 0) handler.postDelayed(new AutoConnectTick(this), 2000);
            }
        }
    }

    private void addKeyButton(LinearLayout parent, String label, int keycode) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(v -> {
            NativeClient.nativeKey(0, keycode);
            NativeClient.nativeKey(1, keycode);
        });
        parent.addView(b);
    }

    private void toggleConnect() {
        if (connected) {
            disconnect();
        } else {
            String host = ipInput.getText().toString().trim();
            int port;
            try {
                port = Integer.parseInt(portInput.getText().toString().trim());
            } catch (NumberFormatException e) {
                statusText.setText("端口无效");
                return;
            }
            if (NativeClient.nativeStart(host, port, "tightcast")) {
                connected = true;
                connectBtn.setText("断开");
                getPreferences(MODE_PRIVATE).edit().putString("host", host).apply();
                maybeStartMic();
            } else {
                statusText.setText("启动失败（端口占用？）");
            }
        }
    }

    private void disconnect() {
        stopMic();
        NativeClient.nativeStop();
        connected = false;
        connectBtn.setText("连接");
        statusText.setText("未连接");
    }

    private void maybeStartMic() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            mic.start();
        } else {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
        }
    }

    private void stopMic() {
        if (mic != null) mic.stop();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        if (requestCode == 1 && connected && grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            mic.start();
        }
    }

    private void startStatusLoop() {
        handler.postDelayed(new StatusTick(this), 1000);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // BACK 留给本机（退出/收起），其余按键直通注入到 server 手机
        int kc = event.getKeyCode();
        if (kc == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event);
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            if (event.getRepeatCount() > 0) return true;  // 跳过自动重复
            NativeClient.nativeKey(0, kc);
            return true;
        } else if (event.getAction() == KeyEvent.ACTION_UP) {
            NativeClient.nativeKey(1, kc);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /** 视频画面在 view 内的 letterbox 显示区（与 native renderer 同一公式）。 */
    private boolean normalize(float vx, float vy, float[] out) {
        int[] vs = NativeClient.nativeVideoSize();
        int vw = glView.getWidth(), vh = glView.getHeight();
        if (vs == null || vs[0] <= 0 || vs[1] <= 0 || vw <= 0 || vh <= 0) return false;
        double s = Math.min((double) vw / vs[0], (double) vh / vs[1]);
        float dw = (float) (vs[0] * s), dh = (float) (vs[1] * s);
        float left = (vw - dw) / 2, top = (vh - dh) / 2;
        if (dw <= 0 || dh <= 0) return false;
        float x = (vx - left) / dw;
        float y = (vy - top) / dh;
        out[0] = Math.min(1.0f, Math.max(0.0f, x));
        out[1] = Math.min(1.0f, Math.max(0.0f, y));
        return true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        glView.onResume();
    }

    @Override
    protected void onPause() {
        glView.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        disconnect();
        super.onDestroy();
    }

    private boolean onGlTouch(MotionEvent ev) {
        int action = ev.getActionMasked();
        float[] n = new float[2];
        if (action == MotionEvent.ACTION_POINTER_DOWN && ev.getPointerCount() == 2) {
            // 双指 → 滚动模式
            twoFingerScroll = true;
            scrollLastY = (ev.getY(0) + ev.getY(1)) / 2;
            if (connected) NativeClient.nativeTouch(1, lastX, lastY);  // 收掉单指
            return true;
        }
        if (action == MotionEvent.ACTION_POINTER_UP && twoFingerScroll) {
            twoFingerScroll = false;
            return true;
        }
        if (twoFingerScroll && action == MotionEvent.ACTION_MOVE
                && ev.getPointerCount() >= 2) {
            float cy = (ev.getY(0) + ev.getY(1)) / 2;
            float dy = (scrollLastY - cy) / Math.max(1, glView.getHeight()) * 8.0f;
            scrollLastY = cy;
            if (Math.abs(dy) > 0.001f && normalize(ev.getX(0), ev.getY(0), n)) {
                NativeClient.nativeScroll(n[0], n[1], dy);
            }
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
                if (normalize(ev.getX(), ev.getY(), n)) {
                    lastX = n[0];
                    lastY = n[1];
                    NativeClient.nativeTouch(0, n[0], n[1]);
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                long now = ev.getEventTime();
                if (now - lastMoveSent < 8) return true;  // MOVE 节流 ≥8ms
                lastMoveSent = now;
                if (normalize(ev.getX(), ev.getY(), n)) {
                    lastX = n[0];
                    lastY = n[1];
                    NativeClient.nativeTouch(2, n[0], n[1]);
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (normalize(ev.getX(), ev.getY(), n)) {
                    NativeClient.nativeTouch(1, n[0], n[1]);
                }
                return true;
            default:
                return true;
        }
    }
}
