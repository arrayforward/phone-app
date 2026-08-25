#pragma once

// 调试报文日志（内部使用，非公开 API）。
//
// 默认关闭（零开销）：所有 DBG/PROBE 调试输出统一走 TIGHT_DBG_PRINTF，
// 编译期由 TIGHT_DBG_PACKET_LOG 控制：
//   cmake -DCMAKE_CXX_FLAGS=-DTIGHT_DBG_PACKET_LOG ...
// 开启后输出到 stdout（每条立即 flush，便于崩溃现场保留日志）。

#include <cstdio>

#ifdef TIGHT_DBG_PACKET_LOG
#define TIGHT_DBG_PRINTF(...)                            \
    do {                                                 \
        std::printf(__VA_ARGS__);                        \
        std::fflush(stdout);                             \
    } while (0)
#else
#define TIGHT_DBG_PRINTF(...) ((void)0)
#endif
