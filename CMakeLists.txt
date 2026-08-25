cmake_minimum_required(VERSION 3.16)
project(tightcast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# tight 作为子目录构建时不会自行 find_package(Threads)，由父项目提供
find_package(Threads REQUIRED)

add_subdirectory(third_party/tight)
add_subdirectory(client)
