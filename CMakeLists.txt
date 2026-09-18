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

# ---------------------------------------------------------------------------
# SQLite（Ubuntu 22.04 的 libsqlite3-dev，提供 sqlite3.h 与 libsqlite3）
# ---------------------------------------------------------------------------
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR
    "未找到 SQLite。请安装 libsqlite3-dev（参见 dependence.md）。")
endif()

# ---------------------------------------------------------------------------
# argon2（Ubuntu 22.04 的 libargon2-dev，提供 argon2.h 与 libargon2）
# ---------------------------------------------------------------------------
find_path(ARGON2_INCLUDE_DIR argon2.h)
find_library(ARGON2_LIBRARY NAMES argon2)
if(NOT ARGON2_INCLUDE_DIR OR NOT ARGON2_LIBRARY)
  message(FATAL_ERROR
    "未找到 argon2。请安装 libargon2-dev（参见 dependence.md）。")
endif()

# ---------------------------------------------------------------------------
# OpenSSL libcrypto（JWT HS256 签名所需 HMAC，jwt-cpp header-only 依赖）
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# jwt-cpp（header-only，源码安装于 /usr/local/include，见 dependence.md）
# 使用 nlohmann-json 作为其 JSON traits（与项目 JSON 依赖保持一致）。
# ---------------------------------------------------------------------------
find_path(JWT_CPP_INCLUDE_DIR jwt-cpp/jwt.h)
if(NOT JWT_CPP_INCLUDE_DIR)
  message(FATAL_ERROR
    "未找到 jwt-cpp。请按 dependence.md 安装到 /usr/local/include。")
endif()

# ---------------------------------------------------------------------------
# 核心静态库：数据库、认证与 HTTP 层（除程序入口外），供服务与测试复用。
# ---------------------------------------------------------------------------
add_library(oj_core STATIC
  src/http/server.cpp
  src/db/database.cpp
  src/db/schema.cpp
  src/db/users.cpp
  src/auth/password.cpp
  src/auth/account.cpp
  src/auth/validation.cpp
  src/auth/register.cpp
  src/auth/jwt.cpp
  src/auth/rate_limit.cpp
  src/auth/login.cpp
  src/auth/context.cpp
  src/auth/authorize.cpp
  src/auth/password_change.cpp
)

target_include_directories(oj_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${HTTPLIB_INCLUDE_DIR}
  ${JSON_INCLUDE_DIR}
  ${SQLITE3_INCLUDE_DIR}
  ${ARGON2_INCLUDE_DIR}
  ${JWT_CPP_INCLUDE_DIR}
)

target_compile_definitions(oj_core PUBLIC
  CPPHTTPLIB_OPENSSL_SUPPORT
  CPPHTTPLIB_ZLIB_SUPPORT
  CPPHTTPLIB_BROTLI_SUPPORT
)

target_link_libraries(oj_core PUBLIC
  ${HTTPLIB_LIBRARY}
  ${SQLITE3_LIBRARY}
  ${ARGON2_LIBRARY}
  OpenSSL::Crypto
  Threads::Threads
)

add_executable(oj_server
  src/main.cpp
)

target_link_libraries(oj_server PRIVATE oj_core)

# ---------------------------------------------------------------------------
# 测试（CTest）
# ---------------------------------------------------------------------------
enable_testing()

add_executable(oj_db_test
  tests/integration/test_db.cpp
  src/db/database.cpp
  src/db/schema.cpp
  src/auth/password.cpp
)

target_include_directories(oj_db_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${SQLITE3_INCLUDE_DIR}
  ${ARGON2_INCLUDE_DIR}
)

target_link_libraries(oj_db_test PRIVATE
  ${SQLITE3_LIBRARY}
  ${ARGON2_LIBRARY}
  Threads::Threads
)

add_test(NAME db_integration COMMAND oj_db_test)

# 注册核心逻辑单元测试（账号生成、校验、注册服务，含确定性账号碰撞模拟）。
add_executable(oj_register_test
  tests/unit/test_register.cpp
)

target_link_libraries(oj_register_test PRIVATE oj_core)

add_test(NAME register_unit COMMAND oj_register_test)

# 注册接口与数据库集成测试（通过 httplib::Client 访问真实 HTTP 服务）。
add_executable(oj_register_api_test
  tests/integration/test_register_api.cpp
)

target_link_libraries(oj_register_api_test PRIVATE oj_core)

add_test(NAME register_api COMMAND oj_register_api_test)

# JWT 签发/验证单元测试（算法固定、claims、过期、伪造/篡改/无签名等）。
add_executable(oj_jwt_test
  tests/unit/test_jwt.cpp
)

target_link_libraries(oj_jwt_test PRIVATE oj_core)

add_test(NAME jwt_unit COMMAND oj_jwt_test)

# 登录限速单元测试（可注入时钟，验证阈值、窗口、解除、并发不可绕过）。
add_executable(oj_rate_limit_test
  tests/unit/test_rate_limit.cpp
)

target_link_libraries(oj_rate_limit_test PRIVATE oj_core)

add_test(NAME rate_limit_unit COMMAND oj_rate_limit_test)

# 登录与身份验证集成测试（真实 HTTP 服务 + 隔离临时库 + 测试密钥）。
add_executable(oj_login_api_test
  tests/integration/test_login_api.cpp
)

target_link_libraries(oj_login_api_test PRIVATE oj_core)

add_test(NAME login_api COMMAND oj_login_api_test)

# 改密与权限检查单元测试（改密校验、首次改密检查、管理员权限检查、改密服务）。
add_executable(oj_password_test
  tests/unit/test_password_change.cpp
)

target_link_libraries(oj_password_test PRIVATE oj_core)

add_test(NAME password_unit COMMAND oj_password_test)

# 改密与权限检查集成测试（真实 HTTP 服务 + 隔离临时库 + 测试密钥 + 测试路由）。
add_executable(oj_password_api_test
  tests/integration/test_password_api.cpp
)

target_link_libraries(oj_password_api_test PRIVATE oj_core)

add_test(NAME password_api COMMAND oj_password_api_test)
