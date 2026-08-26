#include "camouflage.h"

#include <cstddef>

namespace layered {

namespace {

// 单平面逐行模 256 一阶差分：d[0] = s[0]，d[i] = (s[i] − s[i−1]) & 0xFF
// 用 prev 缓存左侧原值，同址（in==out）安全
void diff_rows(const std::uint8_t* in, std::uint8_t* out, int width, int rows) {
    for (int r = 0; r < rows; ++r) {
        const std::uint8_t* s = in + (std::size_t)r * width;
        std::uint8_t* d = out + (std::size_t)r * width;
        if (width <= 0) continue;
        std::uint8_t prev = s[0];
        d[0] = prev;
        for (int x = 1; x < width; ++x) {
            std::uint8_t cur = s[x];
            d[x] = (std::uint8_t)((cur - prev) & 0xFF);
            prev = cur;
        }
    }
}

// 单平面逐行前prefix和（mod 256）：s[i] = (d[0] + … + d[i]) & 0xFF
void sum_rows(const std::uint8_t* in, std::uint8_t* out, int width, int rows) {
    for (int r = 0; r < rows; ++r) {
        const std::uint8_t* d = in + (std::size_t)r * width;
        std::uint8_t* s = out + (std::size_t)r * width;
        int acc = 0;
        for (int x = 0; x < width; ++x) {
            acc = (acc + d[x]) & 0xFF;
            s[x] = (std::uint8_t)acc;
        }
    }
}

}  // namespace

void camouflage_forward(const std::uint8_t* in, int w, int h, std::uint8_t* out) {
    const std::size_t y_bytes = (std::size_t)w * h;
    const int cw = w / 2, ch = h / 2;
    diff_rows(in, out, w, h);                            // Y 平面
    diff_rows(in + y_bytes, out + y_bytes, cw, ch);      // U 平面
    diff_rows(in + y_bytes + (std::size_t)cw * ch,
              out + y_bytes + (std::size_t)cw * ch, cw, ch);  // V 平面
}

void camouflage_inverse(const std::uint8_t* in, int w, int h, std::uint8_t* out) {
    const std::size_t y_bytes = (std::size_t)w * h;
    const int cw = w / 2, ch = h / 2;
    sum_rows(in, out, w, h);
    sum_rows(in + y_bytes, out + y_bytes, cw, ch);
    sum_rows(in + y_bytes + (std::size_t)cw * ch,
             out + y_bytes + (std::size_t)cw * ch, cw, ch);
}

}  // namespace layered
