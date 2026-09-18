cmake_minimum_required(VERSION 3.16)
project(oj_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# cpp-httplib（Ubuntu 22.04 的 libcpp-httplib-dev，采用 .h + .so 分离形式）
# Debian 包编译时启用了 OpenSSL / zlib / brotli，因此需要以相同特性宏编译，
# 保证 Request 等结构体与 .so 中的布局一致（见 dependence.md）。
# ---------------------------------------------------------------------------
find_path(HTTPLIB_INCLUDE_DIR httplib.h)
find_library(HTTPLIB_LIBRARY NAMES cpp-httplib)
if(NOT HTTPLIB_INCLUDE_DIR OR NOT HTTPLIB_LIBRARY)
  message(FATAL_ERROR
    "未找到 cpp-httplib。请安装 libcpp-httplib-dev（参见 dependence.md）。")
endif()

# ---------------------------------------------------------------------------
# nlohmann/json（Ubuntu 22.04 的 nlohmann-json3-dev，header-only）
# ---------------------------------------------------------------------------
find_path(JSON_INCLUDE_DIR nlohmann/json.hpp)
if(NOT JSON_INCLUDE_DIR)
  message(FATAL_ERROR
    "未找到 nlohmann/json。请安装 nlohmann-json3-dev（参见 dependence.md）。")
endif()

add_executable(oj_server
  src/main.cpp
  src/http/server.cpp
)

target_include_directories(oj_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${HTTPLIB_INCLUDE_DIR}
  ${JSON_INCLUDE_DIR}
)

target_compile_definitions(oj_server PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT
  CPPHTTPLIB_ZLIB_SUPPORT
  CPPHTTPLIB_BROTLI_SUPPORT
)

target_link_libraries(oj_server PRIVATE
  ${HTTPLIB_LIBRARY}
  Threads::Threads
)
