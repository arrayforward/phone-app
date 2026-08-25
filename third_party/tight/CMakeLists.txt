# ============================================================
# tight 可靠 UDP 传输协议库（独立构建 / 父项目 add_subdirectory 两用）
#
# 单独构建：
#   cmake -B build -S tight
#   cmake --build build
# 被引用：
#   add_subdirectory(tight)
#   target_link_libraries(your_target PRIVATE tight)
#   #include "tight/tight.hpp"
# ============================================================

# 独立构建时的引导（作为子目录构建时跳过，继承父项目设置）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(tight VERSION 2.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(MSVC)
        add_compile_options(/W4 /utf-8)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
    find_package(Threads REQUIRED)
    enable_testing()
endif()

if(WIN32)
    set(TIGHT_PLATFORM_LIBS ws2_32)
else()
    set(TIGHT_PLATFORM_LIBS)
endif()

# 库本体：公共接口头在 include/tight/，私有实现细节在 src/（不对外暴露）。
# src/ 按职责分目录（include 根仍为 src/，内部用限定路径包含）：
#   core/     传输编排（transport）、每对端状态（peer）、带宽估计（bandwidth）
#   channel/  数据通道：分片（fragmenter）、重组（reassembler）、报告（report）、命令（command）
#   protocol/ 线格式：报文编解码（packet_codec）、大端转换（wire_format）、CRC32
#   fec/      Reed-Solomon 擦除码与内部编码接口
#   crypto/   X25519 / HKDF / AES-256-GCM
#   net/      地址解析、socket 平台层、WSA 管理、ECN/L4S 平台层
#   util/     缓冲池、小栈线程
add_library(tight STATIC
    src/core/transport.cpp
    src/core/bandwidth.cpp
    src/channel/reassembler.cpp
    src/channel/fragmenter.cpp
    src/channel/report.cpp
    src/channel/command.cpp
    src/crypto/crypto.cpp
    src/protocol/packet_codec.cpp
    src/protocol/crc32.cpp
    src/fec/fec.cpp
    src/net/address.cpp
    src/net/wsa.cpp
)

target_include_directories(tight
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include   # 对外：#include "tight/tight.hpp"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src       # 对内：实现细节，不导出
)

target_link_libraries(tight PUBLIC
    Threads::Threads
    ${TIGHT_PLATFORM_LIBS}
)

# 安装导出：供其他项目直接引用
install(TARGETS tight ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# 单元测试（仅独立构建时启用，作为子目录构建时不污染父项目）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(tests)
endif()
