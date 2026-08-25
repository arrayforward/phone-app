add_executable(tightcast-client
    src/main.cpp
    src/transport_glue.cpp
    src/decoder.cpp
    src/vpl_decoder.cpp
    src/renderer.cpp
    src/audio.cpp
    src/input_map.cpp
)

# oneVPL 头文件（运行时 dlopen libvpl.dll，无需链接导入库）
target_include_directories(tightcast-client PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/libvpl/api
)

target_link_libraries(tightcast-client PRIVATE
    tight
    mfplat
    mf
    mfuuid
    uuid
    ole32
    winmm
    opengl32
    d3d11
    gdi32
    user32
)

if(MSVC)
    target_compile_options(tightcast-client PRIVATE /W4 /utf-8)
else()
    target_compile_options(tightcast-client PRIVATE -Wall -Wextra)
endif()
