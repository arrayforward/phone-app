#pragma once
// GLES2 渲染器（Android 客户端）：移植 Windows client/src/renderer.cpp 的
// shader 还原逻辑到 GLSL ES，支持三种帧路径（逐帧 flags 自描述）：
//   double-raw（§3.1）/ double-ycocg（§3.2）/ single BT.601（§3.3）
// layer 模式（§3.4）：基础纹理 + 残差纹理（4 纹理单元），
// val = clamp(base + res − 128/255) 后再做对应路径还原。
//
// GL 调用全部发生在 GLSurfaceView 的渲染线程（on_draw_frame 等）；
// update_frame 由调度线程调用（内部持锁拷贝，渲染线程取帧上传纹理）。

#include "decoder.h"

#include <cstdint>
#include <mutex>

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    // ---- GL 线程（GLSurfaceView.Renderer 回调经 JNI 转入）----
    void on_surface_created();
    void on_surface_changed(int w, int h);
    void on_draw_frame();

    // ---- 调度线程：提交最新一帧 ----
    void update_frame(VideoFrame frame);

    // 逻辑视频尺寸（DEVICE_INFO 优先，其次最新帧推导）；未知返回 false
    bool video_size(int& w, int& h);
    void set_device_info(int w, int h);
    void set_connected(bool connected);
    bool connected() const { return m_connected; }

private:
    bool compile_program();
    void upload_textures(const VideoFrame& f, bool resize);

    // 纹理与 program（仅 GL 线程）
    unsigned int m_tex_y = 0;
    unsigned int m_tex_uv = 0;
    unsigned int m_res_y = 0;
    unsigned int m_res_uv = 0;
    unsigned int m_program = 0;
    int m_u_logical_size = -1;
    int m_u_mode = -1;
    int m_u_has_res = -1;
    int m_tex_w = 0;   // 当前纹理尺寸（编码帧）
    int m_tex_h = 0;
    // 残差纹理独立的尺寸跟踪：首帧基础帧分配了基础纹理（resize），同尺寸的
    // 首个残差帧 resize=false——若共用 m_tex_w，残差纹理从未被 glTexImage2D
    // 分配，glTexSubImage2D 静默失败 → shader 采到未定义内容（闪红根因）
    int m_res_w = 0;
    int m_res_h = 0;
    int m_view_w = 0;  // 视口
    int m_view_h = 0;

    // 最新帧（调度线程写、GL 线程读）
    std::mutex m_frame_mtx;
    VideoFrame m_frame;
    bool m_has_frame = false;
    bool m_frame_dirty = false;
    // 当前纹理内容的模式（随帧）
    int m_mode = 0;
    bool m_frame_has_res = false;

    // DEVICE_INFO 逻辑尺寸
    std::mutex m_info_mtx;
    int m_info_w = 0;
    int m_info_h = 0;
    bool m_connected = false;
};
