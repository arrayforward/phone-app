#include "renderer.h"

#include <GLES2/gl2.h>

#include <android/log.h>
#include <algorithm>

#define TAG "tightcast-render"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

const char* kVertexShaderSrc =
    "attribute vec4 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "    gl_Position = a_pos;\n"
    "    v_tex = a_tex;\n"
    "}\n";

// 帧路径还原（对应 Windows client shader 的 GLSL ES 移植 + 残差相加）：
// mode 0 = double-raw（§3.1）：左半 Y=R、右半 Y=B、色度按 2×2 块装全分辨率 G
// mode 1 = double-ycocg（§3.2）：左半 Y=Y'、右半 Y=Co(+128)、色度=Cg(+128)
// mode 2 = single BT.601（§3.3）：标准 4:2:0 limited range YUV→RGB
// has_res=1（§3.4 layer）：val = clamp(base + res − 128/255)（纹理域残差相加）
// 所有采样 NEAREST + texel center，防插值串色。
const char* kFragmentShaderSrc =
    "precision mediump float;\n"
    "uniform sampler2D tex_y;   // 基础 Y 平面（double: 2W×H；single: W×H）\n"
    "uniform sampler2D tex_uv;  // 基础 UV 平面（NV12 交错，luminance=U alpha=V）\n"
    "uniform sampler2D res_y;   // 残差 Y（128/255 = 零残差）\n"
    "uniform sampler2D res_uv;  // 残差 UV（同布局）\n"
    "uniform vec2 logical_size; // 逻辑帧尺寸 (W, H)\n"
    "uniform int mode;          // 0=double-raw 1=double-ycocg 2=single-bt601\n"
    "uniform int has_res;\n"
    "varying vec2 v_tex;\n"
    "const float BIAS = 128.0 / 255.0;\n"
    "void main() {\n"
    "    float W = logical_size.x;\n"
    "    float H = logical_size.y;\n"
    "    vec2 p = min(floor(v_tex * logical_size), logical_size - 1.0);\n"
    "    float r, g, b;\n"
    "    if (mode == 2) {\n"
    "        // single：Y 全分辨率，UV 半分辨率（NV12 交错）\n"
    "        vec2 y_step = vec2(1.0 / W, 1.0 / H);\n"
    "        float yv = texture2D(tex_y, (p + 0.5) * y_step).r;\n"
    "        vec2 blk = floor(p * 0.5);\n"
    "        vec2 uv_step = vec2(2.0 / W, 2.0 / H);\n"
    "        vec4 c = texture2D(tex_uv, (blk + 0.5) * uv_step);\n"
    "        if (has_res != 0) {\n"
    "            yv = clamp(yv + texture2D(res_y, (p + 0.5) * y_step).r - BIAS, 0.0, 1.0);\n"
    "            vec4 rc = texture2D(res_uv, (blk + 0.5) * uv_step);\n"
    "            c.r = clamp(c.r + rc.r - BIAS, 0.0, 1.0);\n"
    "            c.a = clamp(c.a + rc.a - BIAS, 0.0, 1.0);\n"
    "        }\n"
    "        float Y = (yv - 16.0 / 255.0) * (255.0 / 219.0);\n"
    "        float U = (c.r - BIAS) * (255.0 / 224.0);\n"
    "        float V = (c.a - BIAS) * (255.0 / 224.0);\n"
    "        r = Y + 1.596 * V;\n"
    "        g = Y - 0.813 * V - 0.391 * U;\n"
    "        b = Y + 2.018 * U;\n"
    "    } else {\n"
    "        // double：左右拼接 2W×H，色度按 2×2 块奇偶\n"
    "        float px = mod(p.x, 2.0);\n"
    "        float py = mod(p.y, 2.0);\n"
    "        vec2 y_step = vec2(0.5 / W, 1.0 / H);\n"
    "        float ya = texture2D(tex_y, (p + 0.5) * y_step).r;\n"
    "        float yb = texture2D(tex_y, (p + vec2(W, 0.0) + 0.5) * y_step).r;\n"
    "        vec2 blk = floor(p * 0.5);\n"
    "        vec2 uv_step = vec2(1.0 / W, 2.0 / H);\n"
    "        vec4 ca = texture2D(tex_uv, (blk + 0.5) * uv_step);\n"
    "        vec4 cb = texture2D(tex_uv, (blk + vec2(W * 0.5, 0.0) + 0.5) * uv_step);\n"
    "        float cc;\n"
    "        if (py < 0.5) cc = (px < 0.5) ? ca.r : ca.a;\n"
    "        else          cc = (px < 0.5) ? cb.r : cb.a;\n"
    "        if (has_res != 0) {\n"
    "            ya = clamp(ya + texture2D(res_y, (p + 0.5) * y_step).r - BIAS, 0.0, 1.0);\n"
    "            yb = clamp(yb + texture2D(res_y, (p + vec2(W, 0.0) + 0.5) * y_step).r - BIAS, 0.0, 1.0);\n"
    "            vec4 rca = texture2D(res_uv, (blk + 0.5) * uv_step);\n"
    "            vec4 rcb = texture2D(res_uv, (blk + vec2(W * 0.5, 0.0) + 0.5) * uv_step);\n"
    "            float rcc;\n"
    "            if (py < 0.5) rcc = (px < 0.5) ? rca.r : rca.a;\n"
    "            else          rcc = (px < 0.5) ? rcb.r : rcb.a;\n"
    "            cc = clamp(cc + rcc - BIAS, 0.0, 1.0);\n"
    "        }\n"
    "        if (mode == 1) {\n"
    "            float yv = ya * 255.0;\n"
    "            float co = yb * 255.0 - 128.0;\n"
    "            float cg = cc * 255.0 - 128.0;\n"
    "            g = (yv + cg) / 255.0;\n"
    "            b = (yv - cg - co) / 255.0;\n"
    "            r = (yv - cg + co) / 255.0;\n"
    "        } else {\n"
    "            r = ya; g = cc; b = yb;\n"
    "        }\n"
    "    }\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

Renderer::~Renderer() {
    // GL 资源由 GL 线程生命周期管理（surface 销毁时进程内泄漏可接受；
    // v1 不支持 surface 重建后复用旧 program）
}

bool Renderer::compile_program() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc) : 0;
    if (!fs) {
        if (vs) glDeleteShader(vs);
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(m_program, sizeof(log), &n, log);
        LOGE("program link failed: %s", log);
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    glUseProgram(m_program);
    GLint u_tex_y = glGetUniformLocation(m_program, "tex_y");
    GLint u_tex_uv = glGetUniformLocation(m_program, "tex_uv");
    GLint u_res_y = glGetUniformLocation(m_program, "res_y");
    GLint u_res_uv = glGetUniformLocation(m_program, "res_uv");
    m_u_logical_size = glGetUniformLocation(m_program, "logical_size");
    m_u_mode = glGetUniformLocation(m_program, "mode");
    m_u_has_res = glGetUniformLocation(m_program, "has_res");
    if (u_tex_y < 0 || u_tex_uv < 0 || u_res_y < 0 || u_res_uv < 0
            || m_u_logical_size < 0 || m_u_mode < 0 || m_u_has_res < 0) {
        LOGE("shader uniforms missing");
        return false;
    }
    glUniform1i(u_tex_y, 0);
    glUniform1i(u_tex_uv, 1);
    glUniform1i(u_res_y, 2);
    glUniform1i(u_res_uv, 3);
    return true;
}

void Renderer::on_surface_created() {
    if (!compile_program()) {
        LOGE("compile_program failed");
        return;
    }
    GLuint texs[4];
    glGenTextures(4, texs);
    m_tex_y = texs[0];
    m_tex_uv = texs[1];
    m_res_y = texs[2];
    m_res_uv = texs[3];
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    m_tex_w = m_tex_h = 0;
    m_res_w = m_res_h = 0;
}

void Renderer::on_surface_changed(int w, int h) {
    m_view_w = w;
    m_view_h = h;
}

void Renderer::update_frame(VideoFrame frame) {
    {
        std::lock_guard<std::mutex> lk(m_frame_mtx);
        m_frame = std::move(frame);
        m_has_frame = true;
        m_frame_dirty = true;
    }
}

void Renderer::upload_textures(const VideoFrame& f, bool resize) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // 单元 0：基础 Y（encW×H，LUMINANCE）
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex_y);
    if (resize) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f.width, f.height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, f.y.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, f.y.data());
    }
    // 单元 1：基础 UV（NV12 交错 → LUMINANCE_ALPHA，encW/2 × H/2 texel）
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_tex_uv);
    if (resize) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, f.width / 2, f.height / 2, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, f.uv.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width / 2, f.height / 2,
                        GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, f.uv.data());
    }
    // 单元 2/3：残差（has_residual 时上传；否则沿用旧内容但不会采样）
    if (f.has_residual) {
        // 残差纹理独立分配判断（见 renderer.h 注释：不能共用 base 的 resize）
        bool res_resize = (f.width != m_res_w || f.height != m_res_h);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_res_y);
        if (res_resize) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, f.width, f.height, 0,
                         GL_LUMINANCE, GL_UNSIGNED_BYTE, f.res_y.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, f.res_y.data());
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_res_uv);
        if (res_resize) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, f.width / 2, f.height / 2, 0,
                         GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, f.res_uv.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width / 2, f.height / 2,
                            GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, f.res_uv.data());
        }
        if (res_resize) {
            m_res_w = f.width;
            m_res_h = f.height;
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::on_draw_frame() {
    glViewport(0, 0, m_view_w, m_view_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_program || m_view_w <= 0 || m_view_h <= 0) return;

    // 取最新帧（锁内拷贝，锁外上传）
    VideoFrame frame;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(m_frame_mtx);
        if (m_frame_dirty && m_has_frame && m_frame.width > 0
                && !m_frame.y.empty() && !m_frame.uv.empty()) {
            frame = m_frame;  // 拷贝（swap 亦可，拷贝保持语义简单）
            m_frame_dirty = false;
            dirty = true;
        }
    }
    if (dirty) {
        bool resize = (frame.width != m_tex_w || frame.height != m_tex_h);
        upload_textures(frame, resize);
        m_tex_w = frame.width;
        m_tex_h = frame.height;
        m_mode = frame.single ? 2 : (frame.ycocg ? 1 : 0);
        m_frame_has_res = frame.has_residual;
    }
    if (m_tex_w <= 0 || m_tex_h <= 0) return;

    // 逻辑尺寸：single = 编码帧尺寸；double = 编码帧宽/2
    int lw = m_mode == 2 ? m_tex_w : m_tex_w / 2;
    int lh = m_tex_h;
    double s = std::min((double)m_view_w / lw, (double)m_view_h / lh);
    float dw = (float)(lw * s), dh = (float)(lh * s);
    float x0 = -dw / m_view_w, x1 = dw / m_view_w;
    float y0 = -dh / m_view_h, y1 = dh / m_view_h;

    glUseProgram(m_program);
    glUniform2f(m_u_logical_size, (float)lw, (float)lh);
    glUniform1i(m_u_mode, m_mode);
    glUniform1i(m_u_has_res, m_frame_has_res ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex_y);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_tex_uv);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_res_y);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_res_uv);
    glActiveTexture(GL_TEXTURE0);

    // 帧数据自上而下；纹理 t=0 对应图像顶部（同 Windows 端约定）
    const float verts[] = {x0, y1, x1, y1, x1, y0, x0, y0};
    const float texs[]  = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    GLint a_pos = glGetAttribLocation(m_program, "a_pos");
    GLint a_tex = glGetAttribLocation(m_program, "a_tex");
    glEnableVertexAttribArray((GLuint)a_pos);
    glEnableVertexAttribArray((GLuint)a_tex);
    glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer((GLuint)a_tex, 2, GL_FLOAT, GL_FALSE, 0, texs);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray((GLuint)a_pos);
    glDisableVertexAttribArray((GLuint)a_tex);
}

bool Renderer::video_size(int& w, int& h) {
    {
        std::lock_guard<std::mutex> lk(m_info_mtx);
        if (m_info_w > 0 && m_info_h > 0) {
            w = m_info_w;
            h = m_info_h;
            return true;
        }
    }
    std::lock_guard<std::mutex> lk(m_frame_mtx);
    if (m_has_frame && m_frame.width > 0) {
        w = m_frame.single ? m_frame.width : m_frame.width / 2;
        h = m_frame.height;
        return true;
    }
    return false;
}

void Renderer::set_device_info(int w, int h) {
    std::lock_guard<std::mutex> lk(m_info_mtx);
    m_info_w = w;
    m_info_h = h;
}

void Renderer::set_connected(bool connected) {
    m_connected = connected;
}
