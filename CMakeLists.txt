cmake_minimum_required(VERSION 3.16)
project(oj_system CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

find_path(HTTPLIB_INCLUDE_DIR NAMES httplib.h REQUIRED)
find_library(HTTPLIB_LIBRARY NAMES cpp-httplib REQUIRED)
message(STATUS "cpp-httplib found at: ${HTTPLIB_INCLUDE_DIR} (library ${HTTPLIB_LIBRARY})")

# The Debian libcpp-httplib ships an implementation compiled against system
# OpenSSL/zlib/brotli; the header must use matching feature macros so the ABI
# matches the compiled library.
find_package(nlohmann_json QUIET)

set(OJ_SOURCES
  src/main.cpp
)

add_executable(oj-server ${OJ_SOURCES})
target_compile_definitions(oj-server PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT
  CPPHTTPLIB_ZLIB_SUPPORT
  CPPHTTPLIB_BROTLI_SUPPORT)
target_include_directories(oj-server PRIVATE ${HTTPLIB_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(oj-server PRIVATE Threads::Threads ${HTTPLIB_LIBRARY})

if(nlohmann_json_FOUND)
  target_link_libraries(oj-server PRIVATE nlohmann_json::nlohmann_json)
  message(STATUS "nlohmann_json linked")
else()
  message(WARNING "nlohmann_json not found, JSON helpers disabled")
endif()