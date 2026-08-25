#pragma once

// 内部专用 FEC 接口（非公开 API）：RS 编码到调用方原始缓冲。
// 供 fragmenter 配合 POD thread_local 缓冲使用（规避 MinGW emutls 并发
// 线程退出时非 POD thread_local 析构竞态，详见 fragmenter.cpp 注释）。

#include "tight/fec.hpp"

#include <cstddef>
#include <cstdint>

namespace tight::tight_detail {

// out 容量须 ≥ parity_count × width（每校验片 width 字节，连续排布）。
void rs_encode_into_raw(const ReedSolomon::Span* fragments,
                        std::size_t fragment_count,
                        std::size_t parity_count, std::size_t width,
                        std::uint8_t* out);

} // namespace tight::tight_detail
