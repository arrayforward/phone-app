#include "renderer.h"

#include "input_map.h"

#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // GL 1.2 常量，MinGW 的 gl.h（1.1）未定义
#endif

// --- GL 1.3/2.0 常量（MinGW 的 gl.h 仅 1.1） ---
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#endif

namespace {

constexpr UINT WM_APP_DEVICE_INFO = WM_APP + 1;
constexpr UINT WM_APP_CONNECTED   = WM_APP + 2;

// DEVICE_INFO 经 PostMessage 跨线程传递的堆结构
struct DeviceInfoMsg {
    int w, h;
    std::string model;
};

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// --- GL 2.0 入口（wglGetProcAddress 自取，不引第三方库） ---
typedef char GLchar_t;  // 1.1 头无 GLchar

typedef GLuint (APIENTRY* PFN_glCreateShader)(GLenum);
typedef void (APIENTRY* PFN_glShaderSource)(GLuint, GLsizei, const GLchar_t**, const GLint*);
typedef void (APIENTRY* PFN_glCompileShader)(GLuint);
typedef void (APIENTRY* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_t*);
typedef GLuint (APIENTRY* PFN_glCreateProgram)(void);
typedef void (APIENTRY* PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY* PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_t*);
typedef void (APIENTRY* PFN_glUseProgram)(GLuint);
typedef GLint (APIENTRY* PFN_glGetUniformLocation)(GLuint, const GLchar_t*);
typedef void (APIENTRY* PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRY* PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY* PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY* PFN_glDeleteShader)(GLuint);
typedef void (APIENTRY* PFN_glDeleteProgram)(GLuint);

PFN_glCreateShader pglCreateShader = nullptr;
PFN_glShaderSource pglShaderSource = nullptr;
PFN_glCompileShader pglCompileShader = nullptr;
PFN_glGetShaderiv pglGetShaderiv = nullptr;
PFN_glGetShaderInfoLog pglGetShaderInfoLog = nullptr;
PFN_glCreateProgram pglCreateProgram = nullptr;
PFN_glAttachShader pglAttachShader = nullptr;
PFN_glLinkProgram pglLinkProgram = nullptr;
PFN_glGetProgramiv pglGetProgramiv = nullptr;
PFN_glGetProgramInfoLog pglGetProgramInfoLog = nullptr;
PFN_glUseProgram pglUseProgram = nullptr;
PFN_glGetUniformLocation pglGetUniformLocation = nullptr;
PFN_glUniform1i pglUniform1i = nullptr;
PFN_glUniform2f pglUniform2f = nullptr;
PFN_glActiveTexture pglActiveTexture = nullptr;
PFN_glDeleteShader pglDeleteShader = nullptr;
PFN_glDeleteProgram pglDeleteProgram = nullptr;

template <typename T>
bool load_gl_proc(T& fn, const char* name) {
    fn = reinterpret_cast<T>(wglGetProcAddress(name));
    if (!fn) std::fprintf(stderr, "[renderer] missing GL entry point: %s\n", name);
    return fn != nullptr;
}

bool load_gl20() {
    bool ok = true;
    ok &= load_gl_proc(pglCreateShader, "glCreateShader");
    ok &= load_gl_proc(pglShaderSource, "glShaderSource");
    ok &= load_gl_proc(pglCompileShader, "glCompileShader");
    ok &= load_gl_proc(pglGetShaderiv, "glGetShaderiv");
    ok &= load_gl_proc(pglGetShaderInfoLog, "glGetShaderInfoLog");
    ok &= load_gl_proc(pglCreateProgram, "glCreateProgram");
    ok &= load_gl_proc(pglAttachShader, "glAttachShader");
    ok &= load_gl_proc(pglLinkProgram, "glLinkProgram");
    ok &= load_gl_proc(pglGetProgramiv, "glGetProgramiv");
    ok &= load_gl_proc(pglGetProgramInfoLog, "glGetProgramInfoLog");
    ok &= load_gl_proc(pglUseProgram, "glUseProgram");
    ok &= load_gl_proc(pglGetUniformLocation, "glGetUniformLocation");
    ok &= load_gl_proc(pglUniform1i, "glUniform1i");
    ok &= load_gl_proc(pglUniform2f, "glUniform2f");
    ok &= load_gl_proc(pglActiveTexture, "glActiveTexture");
    ok &= load_gl_proc(pglDeleteShader, "glDeleteShader");
    ok &= load_gl_proc(pglDeleteProgram, "glDeleteProgram");
    return ok;
}

// 全屏四边形直传（沿用固定管线顶点/texcoord 属性，t=0 为图像顶部）
const char* kVertexShaderSrc =
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "    v_tex = gl_MultiTexCoord0.xy;\n"
    "}\n";

// protocol §3.1/§3.2 还原：编码帧 2W×H 由两个 YUV420 左右拼接。
// §3.1（u_ycocg=0）：左半 Y=R、右半 Y=B、色度按 2×2 块携带全分辨率 G。
// §3.2（u_ycocg=1）：左半 Y=Y'（真亮度）、右半 Y=Co(+128)、色度=Cg(+128)，
//   逆变换 G=Y'+cg, B=Y'-cg-co, R=Y'-cg+co。
// UV 纹理 W×H/2（luminance=U、alpha=V），B 半帧的块右移 W/2。
// 所有采样 NEAREST + texel center ((k+0.5)/texelSize)，防插值串色。
const char* kFragmentShaderSrc =
    "uniform sampler2D tex_y;   // 编码帧 Y 平面 2W×H\n"
    "uniform sampler2D tex_uv;  // 编码帧 UV 平面 W×H/2：luminance=U，alpha=V\n"
    "uniform vec2 logical_size; // 逻辑帧尺寸 (W, H)\n"
    "uniform int ycocg;         // 1=§3.2 YCoCg，0=§3.1 原始 RGB\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    float W = logical_size.x;\n"
    "    float H = logical_size.y;\n"
    "    // 逻辑像素整数索引 (x, y)，clamp 防右/下边缘越界\n"
    "    vec2 p = min(floor(v_tex * logical_size), logical_size - 1.0);\n"
    "    float px = mod(p.x, 2.0);\n"
    "    float py = mod(p.y, 2.0);\n"
    "    // 左半 Y（R 或 Y'），右半 Y（B 或 Co）\n"
    "    vec2 y_step = vec2(0.5 / W, 1.0 / H);\n"
    "    float ya = texture2D(tex_y, (p + 0.5) * y_step).r;\n"
    "    float yb = texture2D(tex_y, (p + vec2(W, 0.0) + 0.5) * y_step).r;\n"
    "    // 2×2 色度块 (bx, by) = (x/2, y/2)；UV 纹理 W×H/2，B 半帧块右移 W/2\n"
    "    vec2 blk = floor(p * 0.5);\n"
    "    vec2 uv_step = vec2(1.0 / W, 2.0 / H);\n"
    "    vec4 ca = texture2D(tex_uv, (blk + 0.5) * uv_step);\n"
    "    vec4 cb = texture2D(tex_uv, (blk + vec2(W * 0.5, 0.0) + 0.5) * uv_step);\n"
    "    float cc;\n"
    "    if (py < 0.5) cc = (px < 0.5) ? ca.r : ca.a;  // U_A / V_A\n"
    "    else          cc = (px < 0.5) ? cb.r : cb.a;  // U_B / V_B\n"
    "    float r, g, b;\n"
    "    if (ycocg != 0) {\n"
    "        float yv = ya * 255.0;\n"
    "        float co = yb * 255.0 - 128.0;\n"
    "        float cg = cc * 255.0 - 128.0;\n"
    "        g = (yv + cg) / 255.0;\n"
    "        b = (yv - cg - co) / 255.0;\n"
    "        r = (yv - cg + co) / 255.0;\n"
    "    } else {\n"
    "        r = ya; g = cc; b = yb;\n"
    "    }\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = pglCreateShader(type);
    pglShaderSource(sh, 1, &src, nullptr);
    pglCompileShader(sh);
    GLint status = 0;
    pglGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[2048];
        GLsizei n = 0;
        pglGetShaderInfoLog(sh, sizeof(log), &n, log);
        std::fprintf(stderr, "[renderer] shader compile failed: %s\n", log);
        pglDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

Renderer::~Renderer() {
    destroy_gl();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool Renderer::init_gl() {
    m_hdc = GetDC(m_hwnd);
    if (!m_hdc) return false;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int fmt = ChoosePixelFormat(m_hdc, &pfd);
    if (fmt == 0 || !SetPixelFormat(m_hdc, fmt, &pfd)) return false;
    m_hglrc = wglCreateContext(m_hdc);
    if (!m_hglrc) return false;
    if (!wglMakeCurrent(m_hdc, m_hglrc)) return false;

    // GL 2.0 shader 管线：函数自取，编译链接失败即降级退出（不保留旧 BGRA 路径）
    if (!load_gl20()) {
        std::fprintf(stderr, "[renderer] GL 2.0 entry points unavailable\n");
        return false;
    }
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc) : 0;
    if (!fs) {
        if (vs) pglDeleteShader(vs);
        return false;
    }
    m_program = pglCreateProgram();
    pglAttachShader(m_program, vs);
    pglAttachShader(m_program, fs);
    pglLinkProgram(m_program);
    pglDeleteShader(vs);
    pglDeleteShader(fs);
    GLint linked = 0;
    pglGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        GLsizei n = 0;
        pglGetProgramInfoLog(m_program, sizeof(log), &n, log);
        std::fprintf(stderr, "[renderer] program link failed: %s\n", log);
        pglDeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    pglUseProgram(m_program);
    GLint u_tex_y = pglGetUniformLocation(m_program, "tex_y");
    GLint u_tex_uv = pglGetUniformLocation(m_program, "tex_uv");
    m_u_logical_size = pglGetUniformLocation(m_program, "logical_size");
    m_u_ycocg = pglGetUniformLocation(m_program, "ycocg");
    if (u_tex_y < 0 || u_tex_uv < 0 || m_u_logical_size < 0 || m_u_ycocg < 0) {
        std::fprintf(stderr, "[renderer] shader uniforms missing\n");
        return false;
    }
    pglUniform1i(u_tex_y, 0);   // 纹理单元 0 = Y
    pglUniform1i(u_tex_uv, 1);  // 纹理单元 1 = UV
    pglUseProgram(0);

    // 两张纹理：Y(2W×H, LUMINANCE) + UV(W×H/2, LUMINANCE_ALPHA)；
    // NEAREST——打包字节不能被双线性糊掉
    GLuint texs[2];
    glGenTextures(2, texs);
    m_tex_y = texs[0];
    m_tex_uv = texs[1];
    for (int i = 0; i < 2; ++i) {
        pglActiveTexture(i == 0 ? GL_TEXTURE0 : GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    pglActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    return true;
}

void Renderer::destroy_gl() {
    if (m_hglrc) {
        wglMakeCurrent(m_hdc, m_hglrc);
        if (m_program && pglDeleteProgram) {
            pglUseProgram(0);
            pglDeleteProgram(m_program);
            m_program = 0;
        }
        if (m_tex_y || m_tex_uv) {
            GLuint texs[2] = {m_tex_y, m_tex_uv};
            glDeleteTextures(2, texs);
            m_tex_y = m_tex_uv = 0;
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(m_hglrc);
        m_hglrc = nullptr;
    }
    if (m_hdc && m_hwnd) {
        ReleaseDC(m_hwnd, m_hdc);
        m_hdc = nullptr;
    }
}

bool Renderer::create(int w, int h, const char* title) {
    HINSTANCE inst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    // CS_OWNDC：OpenGL 要求窗口拥有私有 DC
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Renderer::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "tightcast-client";
    if (!RegisterClassExA(&wc)) return false;

    RECT rc{0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    m_hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, inst, this);
    if (!m_hwnd) return false;
    if (!init_gl()) return false;
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

HWND Renderer::hwnd() const { return m_hwnd; }

void Renderer::update_frame(VideoFrame frame) {
    {
        std::lock_guard<std::mutex> lk(m_frame_mtx);
        m_frame = std::move(frame);
        m_has_frame = true;
        m_frame_dirty = true;
    }
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Renderer::set_device_info(int w, int h, const std::string& model) {
    if (!m_hwnd) return;
    auto* msg = new DeviceInfoMsg{w, h, model};
    if (!PostMessage(m_hwnd, WM_APP_DEVICE_INFO, 0, (LPARAM)msg)) delete msg;
}

void Renderer::set_connected(bool connected) {
    if (m_hwnd) PostMessage(m_hwnd, WM_APP_CONNECTED, connected ? 1 : 0, 0);
}

LRESULT CALLBACK Renderer::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Renderer* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCT*)lp;
        self = (Renderer*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->m_hwnd = hwnd;
    } else {
        self = (Renderer*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }
    if (self) return self->handle_msg(msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

void Renderer::update_title() {
    std::string t = "tightcast";
    if (!m_model.empty()) t += " - " + m_model;
    t += m_connected ? " [connected]" : " [disconnected]";
    std::wstring wt = utf8_to_wide(t);
    SetWindowTextW(m_hwnd, wt.c_str());
}

void Renderer::apply_device_info(int w, int h, const std::string& model) {
    m_video_w = w;
    m_video_h = h;
    m_model = model;
    update_title();
    // 按画面纵横比调整窗口（client 区域 = 视频尺寸，上限 1600 等比缩小）
    if (w > 0 && h > 0) {
        int cw = w, ch = h;
        int m = std::max(cw, ch);
        if (m > 1600) {
            cw = cw * 1600 / m;
            ch = ch * 1600 / m;
        }
        RECT rc{0, 0, cw, ch};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
}

bool Renderer::normalize(int cx, int cy, float& x, float& y) const {
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return false;
    // 视频显示区（纵横比居中），无画面时按整个 client 区域；
    // 帧尺寸为编码帧 2W×H，显示/触控归一化用逻辑尺寸 W×H
    int vw = m_video_w, vh = m_video_h;
    if (m_has_frame) {
        std::lock_guard<std::mutex> lk(m_frame_mtx);
        if (m_frame.width > 0) { vw = m_frame.width / 2; vh = m_frame.height; }
    }
    RECT dr{0, 0, cw, ch};
    if (vw > 0 && vh > 0) {
        double s = std::min((double)cw / vw, (double)ch / vh);
        int dw = (int)(vw * s), dh = (int)(vh * s);
        dr.left = (cw - dw) / 2;
        dr.top = (ch - dh) / 2;
        dr.right = dr.left + dw;
        dr.bottom = dr.top + dh;
    }
    if (dr.right <= dr.left || dr.bottom <= dr.top) return false;
    x = (float)(cx - dr.left) / (float)(dr.right - dr.left);
    y = (float)(cy - dr.top) / (float)(dr.bottom - dr.top);
    x = std::min(1.0f, std::max(0.0f, x));
    y = std::min(1.0f, std::max(0.0f, y));
    return true;
}

void Renderer::on_paint() {
    PAINTSTRUCT ps;
    BeginPaint(m_hwnd, &ps);
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0 || !m_hglrc) {
        EndPaint(m_hwnd, &ps);
        return;
    }
    wglMakeCurrent(m_hdc, m_hglrc);

    // 有新帧才上传纹理（尺寸变化时重建纹理存储）
    VideoFrame frame;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(m_frame_mtx);
        if (m_frame_dirty && m_has_frame && m_frame.width > 0 &&
            !m_frame.y.empty() && !m_frame.uv.empty()) {
            frame = m_frame;  // 拷贝出来在锁外上传
            m_frame_dirty = false;
            dirty = true;
        }
    }
    if (dirty) {
        m_ycocg_mode = frame.ycocg;  // 打包模式随帧切换（§3.2 YCoCg / §3.1 raw）
        bool resize = (frame.width != m_tex_w || frame.height != m_tex_h);
        // 一次性诊断：打印首帧的平面数据指纹（排查白屏——纹理是否拿到真数据）
        if (!m_dbg_printed) {
            m_dbg_printed = true;
            std::fprintf(stderr,
                         "[renderer] first frame %dx%d ycocg=%d y[0..3]=%d,%d,%d,%d "
                         "uv[0..3]=%d,%d,%d,%d y_mid=%d uv_mid=%d\n",
                         frame.width, frame.height, (int)frame.ycocg,
                         frame.y[0], frame.y[1], frame.y[2], frame.y[3],
                         frame.uv[0], frame.uv[1], frame.uv[2], frame.uv[3],
                         frame.y[frame.y.size() / 2], frame.uv[frame.uv.size() / 2]);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // 单元 0：Y 平面（2W×H，LUMINANCE）
        pglActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_tex_y);
        if (resize) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height, 0,
                         GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.y.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.y.data());
        }
        // 单元 1：UV 平面（NV12 交错 UV，W×H/2 个 texel，luma=U alpha=V）
        pglActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_tex_uv);
        if (resize) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                         frame.width / 2, frame.height / 2, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, frame.uv.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                            GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, frame.uv.data());
        }
        if (resize) {
            m_tex_w = frame.width;
            m_tex_h = frame.height;
        }
        pglActiveTexture(GL_TEXTURE0);
    }

    // 黑底 + 纵横比居中的纹理四边形（纵横比按逻辑尺寸 W×H = 纹理宽/2 × 高）
    glViewport(0, 0, cw, ch);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_tex_w > 0 && m_tex_h > 0 && m_program) {
        int lw = m_tex_w / 2, lh = m_tex_h;
        double s = std::min((double)cw / lw, (double)ch / lh);
        float dw = (float)(lw * s), dh = (float)(lh * s);
        // 归一化到 [-1,1] 裁剪坐标
        float x0 = -dw / cw, x1 = dw / cw;
        float y0 = -dh / ch, y1 = dh / ch;
        pglUseProgram(m_program);
        pglUniform2f(m_u_logical_size, (float)lw, (float)lh);
        pglUniform1i(m_u_ycocg, m_ycocg_mode ? 1 : 0);
        pglActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_tex_y);
        pglActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_tex_uv);
        glBegin(GL_QUADS);
        // 帧数据自上而下；GL 里纹理 t=0 对应上传的第一行（图像顶部），
        // 屏幕上顶点应取 t=0（无需翻转）
        glTexCoord2f(0.f, 0.f); glVertex2f(x0, y1);
        glTexCoord2f(1.f, 0.f); glVertex2f(x1, y1);
        glTexCoord2f(1.f, 1.f); glVertex2f(x1, y0);
        glTexCoord2f(0.f, 1.f); glVertex2f(x0, y0);
        glEnd();
        pglUseProgram(0);
        pglActiveTexture(GL_TEXTURE0);
    }
    SwapBuffers(m_hdc);
    EndPaint(m_hwnd, &ps);
}

LRESULT Renderer::handle_msg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        on_paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;  // 全部在 WM_PAINT 自绘，避免闪烁

    case WM_APP_DEVICE_INFO: {
        std::unique_ptr<DeviceInfoMsg> m((DeviceInfoMsg*)lp);
        apply_device_info(m->w, m->h, m->model);
        return 0;
    }
    case WM_APP_CONNECTED:
        m_connected = (wp != 0);
        update_title();
        return 0;

    case WM_LBUTTONDOWN: {
        SetCapture(m_hwnd);
        float x, y;
        if (m_handlers.on_touch && normalize(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y))
            m_handlers.on_touch(0, x, y);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!(wp & MK_LBUTTON)) return 0;
        // MOVE 节流：≥8ms
        std::uint64_t now = GetTickCount64();
        if (now - m_last_move_ms < 8) return 0;
        m_last_move_ms = now;
        float x, y;
        if (m_handlers.on_touch && normalize(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y))
            m_handlers.on_touch(2, x, y);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x, y;
        if (m_handlers.on_touch && normalize(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y))
            m_handlers.on_touch(1, x, y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(m_hwnd, &pt);
        float x, y;
        if (m_handlers.on_scroll && normalize(pt.x, pt.y, x, y)) {
            // WHEEL_DELTA 滚轮向上为正，与 protocol 约定 dy>0=向上 一致
            float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
            m_handlers.on_scroll(x, y, dy);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        if (down && (lp & (1 << 30))) return 0;  // 跳过自动重复
        int kc = vk_to_android_keycode((unsigned)wp);
        if (kc >= 0 && m_handlers.on_key)
            m_handlers.on_key(down ? 0 : 1, kc);
        // Alt 组合留给系统（F4 系统键等已映射，仍放行 DefWindowProc）
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
            return DefWindowProc(m_hwnd, msg, wp, lp);
        return 0;
    }
    case WM_CHAR: {
        // 仅可打印 ASCII 走 TEXT，避免与 KEY 双重注入
        wchar_t c = (wchar_t)wp;
        if (c >= 0x20 && c <= 0x7E && m_handlers.on_text)
            m_handlers.on_text(std::string(1, (char)c));
        return 0;
    }

    case WM_DESTROY:
        destroy_gl();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(m_hwnd, msg, wp, lp);
}
