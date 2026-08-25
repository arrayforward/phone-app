#pragma once
// Win32 窗口 + OpenGL 渲染：NV12 帧（protocol §3.1 双 YUV420 左右拼接打包）
// 经 GL 2.0 shader 合并还原为 RGB888，全屏四边形绘制（保持纵横比居中、
// 黑边填充，GPU 缩放）；鼠标/键盘事件经 Handlers 回调转成控制命令
// （protocol.md 第 5 节）。

#include "decoder.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class Renderer {
public:
    struct Handlers {
        // action: 0=DOWN 1=UP 2=MOVE；x/y 为相对视频画面的归一化坐标
        std::function<void(std::uint8_t action, float x, float y)> on_touch;
        std::function<void(float x, float y, float dy)>            on_scroll;
        // action: 0=DOWN 1=UP；android 键码
        std::function<void(std::uint8_t action, std::int32_t keycode)> on_key;
        std::function<void(const std::string& utf8)>               on_text;
    };

    Renderer() = default;
    ~Renderer();

    // 在 GUI 线程调用：创建窗口（client 区域 w×h）并初始化 OpenGL 上下文
    bool create(int w, int h, const char* title);
    void set_handlers(Handlers h) { m_handlers = std::move(h); }
    HWND hwnd() const;

    // 解码线程调用：提交最新一帧并触发重绘
    void update_frame(VideoFrame frame);
    // 任意线程调用（内部 PostMessage 到 GUI 线程）：
    void set_device_info(int w, int h, const std::string& model);
    void set_connected(bool connected);

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg(UINT msg, WPARAM wp, LPARAM lp);

    bool init_gl();
    void destroy_gl();
    void on_paint();
    void update_title();
    void apply_device_info(int w, int h, const std::string& model);
    // client 坐标 → 视频画面归一化坐标
    bool normalize(int cx, int cy, float& x, float& y) const;

    HWND m_hwnd = nullptr;

    // OpenGL（仅 GUI 线程访问）
    HDC  m_hdc = nullptr;
    HGLRC m_hglrc = nullptr;
    unsigned int m_tex_y = 0;   // Y 平面 GL_LUMINANCE（2W×H）
    unsigned int m_tex_uv = 0;  // UV 平面 GL_LUMINANCE_ALPHA（W×H/2，luma=U alpha=V）
    unsigned int m_program = 0;
    int  m_u_logical_size = -1;
    int  m_u_ycocg = -1;          // shader 的 ycocg uniform
    bool m_ycocg_mode = false;    // 当前纹理内容的打包模式（随帧 flags bit1）
    bool m_dbg_printed = false;   // 一次性诊断（首帧数据指纹）
    int  m_tex_w = 0;  // 编码帧尺寸 2W×H（纹理重建依据；逻辑宽 = m_tex_w/2）
    int  m_tex_h = 0;
    bool m_frame_dirty = false;

    // 最近一帧（解码线程写、GUI 线程读）
    mutable std::mutex m_frame_mtx;
    VideoFrame   m_frame;
    bool         m_has_frame = false;

    // 视频画面纵横比（DEVICE_INFO，GUI 线程，逻辑尺寸 W×H）
    int m_video_w = 0;
    int m_video_h = 0;
    std::string m_model;
    bool m_connected = false;

    Handlers m_handlers;
    std::uint64_t m_last_move_ms = 0;
};
