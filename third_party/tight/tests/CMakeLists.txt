# tight 单元测试（ctest）
# 测试通过 src/ 下的内部实现（crypto/peer/wire_format 等）直连库内部
# 接口，覆盖加密向量、FEC 纠错、报文编解码、带宽估计、分片重组与
# 命令通道保序等纯逻辑组件。

set(TIGHT_TEST_SOURCES
    test_crc32.cpp
    test_packet_codec.cpp
    test_fec.cpp
    test_bandwidth.cpp
    test_crypto.cpp
    test_address.cpp
    test_blocking_queue.cpp
    test_command.cpp
    test_reassembler.cpp
    test_fragmenter.cpp
    test_external_socket.cpp
    test_report.cpp
    test_buffer_pool.cpp
    test_small_thread.cpp
    test_transport_e2e.cpp
    test_transport_modes.cpp
)

foreach(src ${TIGHT_TEST_SOURCES})
    get_filename_component(tname ${src} NAME_WE)
    add_executable(${tname} ${src} test_main.cpp)
    # 内部头（core/peer.hpp、crypto/crypto.hpp、protocol/wire_format.hpp 等）以 src/ 为根限定包含
    target_include_directories(${tname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${tname} PRIVATE tight)
    add_test(NAME ${tname} COMMAND ${tname})
endforeach()
