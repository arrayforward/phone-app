package com.tightcast.server;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES30;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * GPU 重排：虚拟显示 → SurfaceTexture（external OES，帧不出 GPU）→ gather shader
 * 把画面打包成协议 §3.1 的双 YUV420 字节流 → glReadPixels 回读 → 交 ScreenEncoder
 * 喂编码器。与 CPU 路径（Repack/repack_core）输出逐字节一致（--repack-selftest 验证）。
 *
 * gather shader：FBO 绑一张 RGBA8 纹理（2W×H）当纯字节存储，fragment (fx,fy)
 * 输出字节流区间 [4·(fy·2W+fx), +4)：Y 平面（2WH 字节，左半 R、右半 B）→
 * U 平面（WH/2，左半 G(2bx,2by)、右半 G(2bx,2by+1)）→ V 平面（WH/2，
 * 左半 G(2bx+1,2by)、右半 G(2bx+1,2by+1)）。字节值 floor(c*255+0.5) 精确往返
 * （RGBA8→RGBA8 无损）；texel 采样用 NEAREST + texel 中心坐标 + SurfaceTexture
 * 变换矩阵，等价 texelFetch，行序 top-down 与 §3.1 一致。
 *
 * 回读：FBO 第 fy 行装字节流 [fy·8W, (fy+1)·8W)，故只需读前 ceil(3H/8) 行；
 * glReadPixels y=0 即 fy=0（流起点），天然正序。同步 readback（glReadPixels 直接
 * 读进 direct buffer）——实测 PBO 双缓冲异步方案在本机（Adreno 650/MIUI）反而更慢
 * （瓶颈在从 GPU 侧内存 memcpy 出来的带宽，map 等待≈0），故保留同步方案。
 */
public final class GlRepack {

    /** 重排完成帧的出口。acquireBuffer 返回 null 表示下游消费不过来（丢帧）。 */
    public interface Sink {
        ByteBuffer acquireBuffer();  // 容量须 ≥ readBufferBytes(w, h)
        void onFrame(ByteBuffer i420, long ptsUs);
    }

    /** GL 回读所需缓冲容量：ceil(3H/8) 行 × 2W × 4 字节（≥ 3WH）。 */
    public static int readBufferBytes(int w, int h) {
        return ((3 * h + 7) / 8) * (2 * w) * 4;
    }

    // 自检诊断用：直接采样 OES 源纹理的 copy shader
    private static final String FS_COPY =
            "#version 300 es\n"
            + "#extension GL_OES_EGL_image_external_essl3 : require\n"
            + "precision highp float;\n"
            + "uniform samplerExternalOES uTex;\n"
            + "uniform mat4 uTexMatrix;\n"
            + "uniform ivec2 uSize;\n"
            + "out vec4 fragColor;\n"
            + "void main() {\n"
            + "    vec2 uv = (uTexMatrix * vec4(gl_FragCoord.x / float(uSize.x),\n"
            + "                                 gl_FragCoord.y / float(uSize.y), 0.0, 1.0)).xy;\n"
            + "    fragColor = texture(uTex, uv);\n"
            + "}\n";

    private static final String VS =
            "#version 300 es\n"
            + "void main() {\n"
            + "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
            + "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            + "}\n";

    private static final String FS_GATHER =
            "#version 300 es\n"
            + "#extension GL_OES_EGL_image_external_essl3 : require\n"
            + "precision highp float;\n"
            + "precision highp int;\n"   // 2W·H≈6.8M 超 mediump int 范围，必须 highp
            + "uniform samplerExternalOES uTex;\n"
            + "uniform mat4 uTexMatrix;\n"
            + "uniform ivec2 uSrcSize;\n"  // W, H
            + "uniform ivec2 uDstSize;\n"  // 2W, H
            + "out vec4 fragColor;\n"
            + "float fetchChan(int sx, int sy, int chan) {\n"
            + "    vec2 uv = (uTexMatrix * vec4((float(sx) + 0.5) / float(uSrcSize.x),\n"
            + "                                 1.0 - (float(sy) + 0.5) / float(uSrcSize.y),\n"
            + "                                 0.0, 1.0)).xy;\n"
            + "    vec4 c = texture(uTex, uv);\n"
            + "    if (chan == 0) return c.r;\n"
            + "    if (chan == 2) return c.b;\n"
            + "    return c.g;\n"
            + "}\n"
            + "// 协议 §3.1：2W×H I420 连续字节流的第 off 字节\n"
            + "int streamByte(int off) {\n"
            + "    int W = uSrcSize.x;\n"
            + "    int H = uSrcSize.y;\n"
            + "    int ysz = 2 * W * H;\n"
            + "    int usz = W * H / 2;\n"
            + "    if (off >= ysz + 2 * usz) return 0;\n"
            + "    if (off < ysz) {\n"
            + "        int row = off / (2 * W);\n"
            + "        int xx = off - row * 2 * W;\n"
            + "        bool left = xx < W;\n"
            + "        int sx = left ? xx : xx - W;\n"
            + "        return int(floor(fetchChan(sx, row, left ? 0 : 2) * 255.0 + 0.5));\n"
            + "    }\n"
            + "    int idx = off - ysz;\n"
            + "    bool isU = idx < usz;\n"
            + "    if (!isU) idx -= usz;\n"
            + "    int bx = idx % W;\n"
            + "    int by = idx / W;\n"
            + "    bool left = bx < W / 2;\n"
            + "    int sx = 2 * (left ? bx : bx - W / 2) + (isU ? 0 : 1);\n"
            + "    int sy = 2 * by + (left ? 0 : 1);\n"
            + "    return int(floor(fetchChan(sx, sy, 1) * 255.0 + 0.5));\n"
            + "}\n"
            + "void main() {\n"
            + "    int fx = int(gl_FragCoord.x);\n"
            + "    int fy = int(gl_FragCoord.y);\n"
            + "    int base = 4 * (fy * uDstSize.x + fx);\n"
            + "    fragColor = vec4(\n"
            + "        float(streamByte(base)) / 255.0,\n"
            + "        float(streamByte(base + 1)) / 255.0,\n"
            + "        float(streamByte(base + 2)) / 255.0,\n"
            + "        float(streamByte(base + 3)) / 255.0);\n"
            + "}\n";

    // 自检用：已知图案 R=x&255, G=y&255, B=(x+y)&255（GL 坐标，bottom-up）
    private static final String FS_PATTERN =
            "#version 300 es\n"
            + "precision highp float;\n"
            + "precision highp int;\n"
            + "uniform ivec2 uSize;\n"
            + "out vec4 fragColor;\n"
            + "void main() {\n"
            + "    int x = int(gl_FragCoord.x);\n"
            + "    int y = int(gl_FragCoord.y);\n"
            + "    int r = x & 255;\n"
            + "    int g = y & 255;\n"
            + "    int b = (x + y) & 255;\n"
            + "    fragColor = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);\n"
            + "}\n";

    private final int w;
    private final int h;
    private final int encW;      // 2W
    private final int readRows;  // ceil(3H/8)
    private final Sink sink;

    private HandlerThread thread;
    private Handler handler;
    private volatile boolean released;

    // 以下仅 GL 线程访问
    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLConfig eglConfig;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLSurface eglPbuffer = EGL14.EGL_NO_SURFACE;
    private int oesTex;
    private SurfaceTexture st;
    private Surface surface;
    private int fbo;
    private int fboTex;
    private int program;
    private int uTex, uTexMatrix, uSrcSize, uDstSize;
    private final float[] texMatrix = new float[16];
    private long lastTimestamp = -1;
    private final int readBytes;
    private long repackNanosTotal;
    private long repackCount;

    private GlRepack(int w, int h, Sink sink) {
        this.w = w;
        this.h = h;
        this.encW = 2 * w;
        this.readRows = (3 * h + 7) / 8;
        this.readBytes = readRows * encW * 4;
        this.sink = sink;
    }

    /** 创建并初始化 GL 管线（EGL/shader 失败抛异常，调用方回退 CPU 路径）。 */
    public static GlRepack create(int w, int h, Sink sink) throws Exception {
        GlRepack g = new GlRepack(w, h, sink);
        g.thread = new HandlerThread("gl-repack");
        g.thread.start();
        g.handler = new Handler(g.thread.getLooper());
        try {
            g.runSync(() -> {
                g.initGl();
                return null;
            });
        } catch (Exception e) {
            g.thread.quitSafely();
            g.thread = null;
            throw e;
        }
        return g;
    }

    /** 采集 target（虚拟显示 surface）。create 返回后即可用。 */
    public Surface getSurface() {
        return surface;
    }

    public void release() {
        released = true;
        if (thread == null) return;
        try {
            runSync(() -> {
                destroyGl();
                return null;
            });
        } catch (Exception e) {
            System.out.println("[GlRepack] destroy failed: " + e);
        }
        thread.quitSafely();
        thread = null;
        handler = null;
    }

    /** 在 GL 线程同步执行任务（供 create/release/selfTest 使用）。 */
    private <T> T runSync(Callable<T> c) throws Exception {
        final AtomicReference<T> ref = new AtomicReference<>();
        final AtomicReference<Throwable> err = new AtomicReference<>();
        final CountDownLatch latch = new CountDownLatch(1);
        handler.post(() -> {
            try {
                ref.set(c.call());
            } catch (Throwable t) {
                err.set(t);
            } finally {
                latch.countDown();
            }
        });
        boolean ok = latch.await(10, TimeUnit.SECONDS);
        if (!ok) throw new IllegalStateException("GL thread timeout");
        if (err.get() != null) {
            throw new Exception("GL thread error: " + err.get(), err.get());
        }
        return ref.get();
    }

    // ---- GL 线程 ----

    private void initGl() throws Exception {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) throw new IllegalStateException("no EGL display");
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)) {
            throw new IllegalStateException("eglInitialize failed 0x" + Integer.toHexString(EGL14.eglGetError()));
        }
        int[] cfgAttrs = {
                EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT | EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] num = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, cfgAttrs, 0, configs, 0, 1, num, 0) || num[0] < 1) {
            throw new IllegalStateException("eglChooseConfig failed 0x" + Integer.toHexString(EGL14.eglGetError()));
        }
        eglConfig = configs[0];
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT,
                new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE}, 0);
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            throw new IllegalStateException("eglCreateContext failed 0x" + Integer.toHexString(EGL14.eglGetError()));
        }
        eglPbuffer = EGL14.eglCreatePbufferSurface(eglDisplay, eglConfig,
                new int[]{EGL14.EGL_WIDTH, 16, EGL14.EGL_HEIGHT, 16, EGL14.EGL_NONE}, 0);
        if (eglPbuffer == EGL14.EGL_NO_SURFACE) {
            throw new IllegalStateException("eglCreatePbufferSurface failed");
        }
        if (!EGL14.eglMakeCurrent(eglDisplay, eglPbuffer, eglPbuffer, eglContext)) {
            throw new IllegalStateException("eglMakeCurrent failed");
        }

        // 采集源：external OES 纹理 + SurfaceTexture
        int[] ids = new int[2];
        GLES30.glGenTextures(1, ids, 0);
        oesTex = ids[0];
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
        st = new SurfaceTexture(oesTex);
        st.setDefaultBufferSize(w, h);
        surface = new Surface(st);
        st.setOnFrameAvailableListener(this::onFrame, handler);

        // 输出：RGBA8 纹理（2W×H）绑 FBO 当字节存储
        GLES30.glGenTextures(1, ids, 0);
        fboTex = ids[0];
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, fboTex);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
        GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA8, encW, h, 0,
                GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, null);
        GLES30.glGenFramebuffers(1, ids, 0);
        fbo = ids[0];
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, fbo);
        GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER, GLES30.GL_COLOR_ATTACHMENT0,
                GLES30.GL_TEXTURE_2D, fboTex, 0);
        if (GLES30.glCheckFramebufferStatus(GLES30.GL_FRAMEBUFFER) != GLES30.GL_FRAMEBUFFER_COMPLETE) {
            throw new IllegalStateException("FBO incomplete");
        }

        program = buildProgram(VS, FS_GATHER);
        uTex = GLES30.glGetUniformLocation(program, "uTex");
        uTexMatrix = GLES30.glGetUniformLocation(program, "uTexMatrix");
        uSrcSize = GLES30.glGetUniformLocation(program, "uSrcSize");
        uDstSize = GLES30.glGetUniformLocation(program, "uDstSize");
        GLES30.glDisable(GLES30.GL_BLEND);

        System.out.println("[GlRepack] GL ready: " + GLES30.glGetString(GLES30.GL_RENDERER)
                + ", gather " + encW + "x" + h + ", readback " + readRows + " rows");
    }

    private void destroyGl() {
        if (st != null) {
            st.setOnFrameAvailableListener(null);
        }
        if (surface != null) {
            surface.release();
            surface = null;
        }
        if (st != null) {
            st.release();
            st = null;
        }
        if (program != 0) {
            GLES30.glDeleteProgram(program);
            program = 0;
        }
        int[] ids = new int[1];
        if (fbo != 0) {
            ids[0] = fbo;
            GLES30.glDeleteFramebuffers(1, ids, 0);
            fbo = 0;
        }
        int[] texs = new int[2];
        int n = 0;
        if (fboTex != 0) texs[n++] = fboTex;
        if (oesTex != 0) texs[n++] = oesTex;
        if (n > 0) GLES30.glDeleteTextures(n, texs, 0);
        fboTex = 0;
        oesTex = 0;
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_CONTEXT);
            if (eglPbuffer != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglPbuffer);
                eglPbuffer = EGL14.EGL_NO_SURFACE;
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext);
                eglContext = EGL14.EGL_NO_CONTEXT;
            }
            EGL14.eglTerminate(eglDisplay);
            eglDisplay = EGL14.EGL_NO_DISPLAY;
        }
    }

    /** OnFrameAvailable（GL 线程）：取最新帧 → gather 渲染 → 同步 readback → 交 sink。 */
    private void onFrame(SurfaceTexture s) {
        if (released) return;
        try {
            GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
            st.updateTexImage();
            long ts = st.getTimestamp();
            if (ts == lastTimestamp) return;  // 重复帧跳过
            lastTimestamp = ts;
            ByteBuffer dst = sink.acquireBuffer();
            if (dst == null) return;          // 下游消费不过来，丢帧保低时延
            long t0 = System.nanoTime();
            st.getTransformMatrix(texMatrix);
            renderAndReadback(dst);
            repackNanosTotal += System.nanoTime() - t0;
            repackCount++;
            if (repackCount % 100 == 0) {
                System.out.println("[GlRepack] gl repack avg "
                        + (repackNanosTotal / repackCount / 1000) / 1000.0 + "ms over "
                        + repackCount + " frames");
            }
            sink.onFrame(dst, ts / 1000);   // 帧出生时刻 = SurfaceFlinger 合成时间戳（时延分析用）
        } catch (Exception e) {
            System.out.println("[GlRepack] onFrame error: " + e);
        }
    }

    private void renderAndReadback(ByteBuffer dst) {
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, fbo);
        GLES30.glViewport(0, 0, encW, h);
        GLES30.glUseProgram(program);
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0);
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES30.glUniform1i(uTex, 0);
        GLES30.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0);
        GLES30.glUniform2i(uSrcSize, w, h);
        GLES30.glUniform2i(uDstSize, encW, h);
        GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
        ByteBuffer view = dst.duplicate();
        view.position(0);
        GLES30.glReadPixels(0, 0, encW, readRows, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, view);
        int err = GLES30.glGetError();
        if (err != GLES30.GL_NO_ERROR) {
            throw new IllegalStateException("gl error 0x" + Integer.toHexString(err));
        }
    }

    private static int buildProgram(String vsSrc, String fsSrc) {
        int vs = compile(GLES30.GL_VERTEX_SHADER, vsSrc);
        int fs = compile(GLES30.GL_FRAGMENT_SHADER, fsSrc);
        int prog = GLES30.glCreateProgram();
        GLES30.glAttachShader(prog, vs);
        GLES30.glAttachShader(prog, fs);
        GLES30.glLinkProgram(prog);
        int[] status = new int[1];
        GLES30.glGetProgramiv(prog, GLES30.GL_LINK_STATUS, status, 0);
        GLES30.glDeleteShader(vs);
        GLES30.glDeleteShader(fs);
        if (status[0] == 0) {
            String log = GLES30.glGetProgramInfoLog(prog);
            GLES30.glDeleteProgram(prog);
            throw new IllegalStateException("program link failed: " + log);
        }
        return prog;
    }

    private static int compile(int type, String src) {
        int sh = GLES30.glCreateShader(type);
        GLES30.glShaderSource(sh, src);
        GLES30.glCompileShader(sh);
        int[] status = new int[1];
        GLES30.glGetShaderiv(sh, GLES30.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) {
            String log = GLES30.glGetShaderInfoLog(sh);
            GLES30.glDeleteShader(sh);
            throw new IllegalStateException("shader compile failed: " + log);
        }
        return sh;
    }

    // ---- 自检：同一已知图案分别走 GL / CPU 两条重排路径，逐字节比对 ----

    public static boolean selfTest(int w, int h) {
        final String tag = "[GlRepack-selftest " + w + "x" + h + "]";
        final AtomicReference<ByteBuffer> glFrame = new AtomicReference<>();
        final CountDownLatch latch = new CountDownLatch(1);
        final int readBytes = readBufferBytes(w, h);
        GlRepack g = null;
        try {
            g = GlRepack.create(w, h, new Sink() {
                @Override
                public ByteBuffer acquireBuffer() {
                    return ByteBuffer.allocateDirect(readBytes);
                }

                @Override
                public void onFrame(ByteBuffer buf, long ptsUs) {
                    glFrame.set(buf);
                    latch.countDown();
                }
            });
            final GlRepack gg = g;
            // GL 线程：把图案渲进 SurfaceTexture 的 surface（走真实采集通路），
            // 同时渲一份到普通 FBO readback 出 top-down RGBA 参考
            final byte[] rgba = g.runSync(() -> gg.injectPatternFrame());
            // 帧事件可能合并，循环补帧直到产出一帧输出
            long deadline = System.currentTimeMillis() + 5000;
            while (latch.getCount() > 0 && System.currentTimeMillis() < deadline) {
                if (!latch.await(300, TimeUnit.MILLISECONDS)) {
                    g.runSync(() -> {
                        gg.swapOnePatternFrame();
                        return null;
                    });
                }
            }
            if (latch.getCount() > 0) {
                System.out.println(tag + " FAIL: no frame within 5s");
                return false;
            }
            ByteBuffer cpu = ByteBuffer.allocateDirect(w * h * 3);
            Repack.repackArray(rgba, w, h, cpu);
            ByteBuffer gbuf = glFrame.get().duplicate();
            gbuf.position(0);
            ByteBuffer cbuf = cpu.duplicate();
            cbuf.position(0);
            int total = w * h * 3;
            int mismatches = 0;
            int firstBad = -1;
            for (int i = 0; i < total; i++) {
                if (gbuf.get(i) != cbuf.get(i)) {
                    if (firstBad < 0) firstBad = i;
                    mismatches++;
                }
            }
            if (firstBad >= 0) {
                System.out.println(tag + " FAIL: " + mismatches + "/" + total + "B mismatch");
                // 失败时 dump 源纹理，便于区分"源没到位"与"打包映射错"
                try {
                    final GlRepack gdbg = g;
                    System.out.println(tag + " source dump: "
                            + g.runSync(() -> gdbg.debugDumpSource()));
                } catch (Exception dumpErr) {
                    System.out.println(tag + " source dump failed: " + dumpErr);
                }
                StringBuilder glHex = new StringBuilder("gl :");
                StringBuilder cpuHex = new StringBuilder("cpu:");
                for (int i = 0; i < 16; i++) {
                    glHex.append(String.format(" %02x", gbuf.get(i)));
                    cpuHex.append(String.format(" %02x", cbuf.get(i)));
                }
                System.out.println(glHex.toString());
                System.out.println(cpuHex.toString());
                // 解码前 4 个失配字节的流位置含义
                int shown = 0;
                for (int i = firstBad; i < total && shown < 4; i++) {
                    if (gbuf.get(i) == cbuf.get(i)) continue;
                    System.out.println("  @" + i + " " + describeOffset(i, w, h)
                            + " gl=" + (gbuf.get(i) & 0xFF) + " cpu=" + (cbuf.get(i) & 0xFF));
                    shown++;
                }
                return false;
            }
            System.out.println(tag + " PASS: " + total + "B identical");
            return true;
        } catch (Exception e) {
            System.out.println(tag + " FAIL: " + e);
            e.printStackTrace(System.out);
            return false;
        } finally {
            if (g != null) g.release();
        }
    }

    /**
     * 自检诊断（GL 线程）：把 OES 源纹理 copy 到 FBO 再 readback，
     * 打印 GL 底行/顶行前若干像素，确认源纹理内容是否到位。
     * 须在 updateTexImage 之后调用（即收到帧后）。
     */
    private String debugDumpSource() {
        int prog = buildProgram(VS, FS_COPY);
        int uT = GLES30.glGetUniformLocation(prog, "uTex");
        int uM = GLES30.glGetUniformLocation(prog, "uTexMatrix");
        int uS = GLES30.glGetUniformLocation(prog, "uSize");
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, fbo);
        GLES30.glViewport(0, 0, w, h);
        GLES30.glUseProgram(prog);
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0);
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES30.glUniform1i(uT, 0);
        GLES30.glUniformMatrix4fv(uM, 1, false, texMatrix, 0);
        GLES30.glUniform2i(uS, w, h);
        GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
        byte[] raw = new byte[w * h * 4];
        GLES30.glReadPixels(0, 0, w, h, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE,
                ByteBuffer.wrap(raw));
        GLES30.glDeleteProgram(prog);
        StringBuilder sb = new StringBuilder("bottom row:");
        for (int x = 0; x < 8; x++) {
            sb.append(String.format(" %02x%02x%02x", raw[x * 4] , raw[x * 4 + 1], raw[x * 4 + 2]));
        }
        sb.append(" top row:");
        int base = (h - 1) * w * 4;
        for (int x = 0; x < 8; x++) {
            sb.append(String.format(" %02x%02x%02x", raw[base + x * 4], raw[base + x * 4 + 1],
                    raw[base + x * 4 + 2]));
        }
        sb.append(String.format(" matrix[0,5,12,13]=%.3f,%.3f,%.3f,%.3f",
                texMatrix[0], texMatrix[5], texMatrix[12], texMatrix[13]));
        return sb.toString();
    }

    /** 自检用（GL 线程）：再往采集 surface 里渲一帧同一图案。 */
    private void swapOnePatternFrame() {
        int prog = buildProgram(VS, FS_PATTERN);
        EGLSurface win = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface,
                new int[]{EGL14.EGL_NONE}, 0);
        try {
            EGL14.eglMakeCurrent(eglDisplay, win, win, eglContext);
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
            GLES30.glViewport(0, 0, w, h);
            GLES30.glUseProgram(prog);
            GLES30.glUniform2i(GLES30.glGetUniformLocation(prog, "uSize"), w, h);
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
            EGL14.eglSwapBuffers(eglDisplay, win);
        } finally {
            EGL14.eglMakeCurrent(eglDisplay, eglPbuffer, eglPbuffer, eglContext);
            if (win != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, win);
            GLES30.glDeleteProgram(prog);
        }
    }

    /** 自检诊断：把字节流 offset 翻译成（平面, 坐标, 通道）。 */
    private static String describeOffset(int off, int w, int h) {
        int ysz = 2 * w * h;
        int usz = w * h / 2;
        if (off < ysz) {
            int row = off / (2 * w);
            int xx = off % (2 * w);
            return "Y(row=" + row + ",x=" + xx + (xx < w ? " A/R" : " B/B") + ")";
        }
        int idx = off - ysz;
        String plane = "U";
        if (idx >= usz) {
            idx -= usz;
            plane = "V";
        }
        int bx = idx % w;
        int by = idx / w;
        return plane + "(bx=" + bx + ",by=" + by + (bx < w / 2 ? " A" : " B") + ")";
    }

    /**
     * 自检用（GL 线程）：把已知图案渲进采集 surface（EGL window surface →
     * SurfaceTexture），并把同一图案渲进普通 FBO readback，返回 top-down RGBA。
     */
    private byte[] injectPatternFrame() {
        int prog = buildProgram(VS, FS_PATTERN);
        int[] ids = new int[1];
        GLES30.glGenTextures(1, ids, 0);
        int refTex = ids[0];
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, refTex);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_NEAREST);
        GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA8, w, h, 0,
                GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, null);
        GLES30.glGenFramebuffers(1, ids, 0);
        int refFbo = ids[0];
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, refFbo);
        GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER, GLES30.GL_COLOR_ATTACHMENT0,
                GLES30.GL_TEXTURE_2D, refTex, 0);

        EGLSurface win = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface,
                new int[]{EGL14.EGL_NONE}, 0);
        try {
            int uSize = GLES30.glGetUniformLocation(prog, "uSize");
            // 1) 渲进采集通路（window surface → SurfaceTexture → Flinger 回送）
            EGL14.eglMakeCurrent(eglDisplay, win, win, eglContext);
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0); // window surface 默认帧缓冲
            GLES30.glViewport(0, 0, w, h);
            GLES30.glUseProgram(prog);
            GLES30.glUniform2i(uSize, w, h);
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
            EGL14.eglSwapBuffers(eglDisplay, win);
            // 帧事件可能合并/丢失，多换一帧提高到达率（内容相同，无影响）
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
            EGL14.eglSwapBuffers(eglDisplay, win);
            System.out.println("[GlRepack-selftest] pattern frame swapped, glErr=0x"
                    + Integer.toHexString(GLES30.glGetError()));
            // 2) 同一图案渲进参考 FBO 并 readback（bottom-up → top-down）
            EGL14.eglMakeCurrent(eglDisplay, eglPbuffer, eglPbuffer, eglContext);
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, refFbo);
            GLES30.glViewport(0, 0, w, h);
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
            byte[] raw = new byte[w * h * 4];
            ByteBuffer buf = ByteBuffer.wrap(raw);
            GLES30.glReadPixels(0, 0, w, h, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, buf);
            byte[] topDown = new byte[raw.length];
            int rowBytes = w * 4;
            for (int r = 0; r < h; r++) {
                System.arraycopy(raw, (h - 1 - r) * rowBytes, topDown, r * rowBytes, rowBytes);
            }
            return topDown;
        } finally {
            EGL14.eglMakeCurrent(eglDisplay, eglPbuffer, eglPbuffer, eglContext);
            if (win != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, win);
            GLES30.glDeleteProgram(prog);
            ids[0] = refFbo;
            GLES30.glDeleteFramebuffers(1, ids, 0);
            ids[0] = refTex;
            GLES30.glDeleteTextures(1, ids, 0);
        }
    }
}
