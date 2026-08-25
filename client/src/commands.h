#pragma once
// docs/protocol.md 第 5 节控制命令的字节流构造（全部大端）。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tc {

enum : std::uint8_t {
    CMD_TOUCH        = 0x01,
    CMD_KEY          = 0x02,
    CMD_TEXT         = 0x03,
    CMD_SCROLL       = 0x04,
    CMD_REQ_KEYFRAME = 0x05,
};

inline void put_u32be(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

inline void put_f32be(std::vector<std::uint8_t>& v, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    put_u32be(v, u);
}

// 0x01 TOUCH: action 0=DOWN 1=UP 2=MOVE，slot 固定 0（v1 单点触控）
inline std::vector<std::uint8_t> build_touch(std::uint8_t action, float x, float y) {
    std::vector<std::uint8_t> v;
    v.reserve(11);
    v.push_back(CMD_TOUCH);
    v.push_back(action);
    v.push_back(0); // slot
    put_f32be(v, x);
    put_f32be(v, y);
    return v;
}

// 0x02 KEY: action 0=DOWN 1=UP，androidKeycode 大端 s32
inline std::vector<std::uint8_t> build_key(std::uint8_t action, std::int32_t android_keycode) {
    std::vector<std::uint8_t> v;
    v.reserve(6);
    v.push_back(CMD_KEY);
    v.push_back(action);
    put_u32be(v, static_cast<std::uint32_t>(android_keycode));
    return v;
}

// 0x03 TEXT: UTF-8 文本
inline std::vector<std::uint8_t> build_text(const std::string& utf8) {
    std::vector<std::uint8_t> v;
    v.reserve(1 + utf8.size());
    v.push_back(CMD_TEXT);
    v.insert(v.end(), utf8.begin(), utf8.end());
    return v;
}

// 0x04 SCROLL: dy > 0 = 滚轮向上
inline std::vector<std::uint8_t> build_scroll(float x, float y, float dy) {
    std::vector<std::uint8_t> v;
    v.reserve(13);
    v.push_back(CMD_SCROLL);
    put_f32be(v, x);
    put_f32be(v, y);
    put_f32be(v, dy);
    return v;
}

// 0x05 REQ_KEYFRAME
inline std::vector<std::uint8_t> build_req_keyframe() {
    return { CMD_REQ_KEYFRAME };
}

} // namespace tc
