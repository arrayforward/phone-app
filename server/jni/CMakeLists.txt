cmake_minimum_required(VERSION 3.16)
project(tightcast_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# tight 作为子目录引入（其 CMakeLists 在子目录模式下不自设 C++ 标准）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tight tight_build)

add_library(tight_jni SHARED tight_jni.cpp repack.cpp repack_core.cpp)
target_link_libraries(tight_jni PRIVATE tight log)
