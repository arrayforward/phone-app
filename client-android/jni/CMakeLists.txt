cmake_minimum_required(VERSION 3.16)
project(tightcast_client_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# tight 作为子目录引入（其 CMakeLists 在子目录模式下不自设 C++ 标准）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tight tight_build)

add_library(tightcast_client SHARED
    client_jni.cpp
    client.cpp
    transport.cpp
    decoder.cpp
    renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/layered/entropy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/layered/residual.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/layered/camouflage.cpp
)
target_include_directories(tightcast_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)
target_link_libraries(tightcast_client PRIVATE
    tight
    android
    log
    GLESv2
    mediandk
)
