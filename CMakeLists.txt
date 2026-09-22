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
  src/config.cpp
  src/http/server.cpp
  src/db/database.cpp
  src/db/schema.cpp
  src/db/users.cpp
  src/db/problems.cpp
  src/db/problem_admin.cpp
  src/db/testcase_admin.cpp
  src/db/submissions.cpp
  src/db/seed.cpp
  src/db/user_admin.cpp
  src/problem/validation.cpp
  src/problem/testcase_validation.cpp
  src/problem/list_query.cpp
  src/user/admin_user_validation.cpp
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
  src/judge/executor.cpp
  src/judge/comparator.cpp
  src/judge/workspace.cpp
  src/judge/local_executor.cpp
  src/judge/judge.cpp
  src/judge/manager.cpp
  src/submit/submit.cpp
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

# ---------------------------------------------------------------------------
# gtest（Ubuntu 22.04 的 libgtest-dev，提供头文件与静态库 GTest::gtest_main）
# ---------------------------------------------------------------------------
find_package(GTest REQUIRED)

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

# 注册接口与数据库集成测试（通过 httplib::Client 访问真实 HTTP 服务）。
add_executable(oj_register_api_test
  tests/integration/test_register_api.cpp
)

target_link_libraries(oj_register_api_test PRIVATE oj_core)

add_test(NAME register_api COMMAND oj_register_api_test)

# 注册功能 gtest 单元测试（账号生成、昵称/密码校验、用户数据访问、注册服务）。
add_executable(oj_register_gtest
  tests/unit/test_register_gtest.cpp
)

target_link_libraries(oj_register_gtest PRIVATE oj_core GTest::gtest_main)

add_test(NAME register_gtest COMMAND oj_register_gtest)

# M1.2 JWT 签发/验证 gtest 单元测试（算法固定、claims、过期、伪造/篡改/无签名等）。
add_executable(oj_jwt_gtest
  tests/unit/test_jwt_gtest.cpp
)

target_link_libraries(oj_jwt_gtest PRIVATE oj_core GTest::gtest_main)

add_test(NAME jwt_gtest COMMAND oj_jwt_gtest)

# M1.2 登录限速 gtest 单元测试（可注入时钟，验证阈值、窗口、解除、并发不可绕过）。
add_executable(oj_rate_limit_gtest
  tests/unit/test_rate_limit_gtest.cpp
)

target_link_libraries(oj_rate_limit_gtest PRIVATE oj_core GTest::gtest_main)

add_test(NAME rate_limit_gtest COMMAND oj_rate_limit_gtest)

# M1.2 登录服务与鉴权上下文 gtest 单元测试（登录结果、Bearer 解析、鉴权回查）。
add_executable(oj_login_gtest
  tests/unit/test_login_gtest.cpp
)

target_link_libraries(oj_login_gtest PRIVATE oj_core GTest::gtest_main)

add_test(NAME login_gtest COMMAND oj_login_gtest)

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

# M1.4 题目数据与查询：纯函数单元测试（标签解析）。
add_executable(oj_problems_unit
  tests/unit/test_problems_unit.cpp
)

target_link_libraries(oj_problems_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME problems_unit COMMAND oj_problems_unit)

# M1.4 题目种子数据、可见性与查询接口集成测试（隔离临时库 + 随机端口真实 HTTP）。
add_executable(oj_problems_api_test
  tests/integration/test_problems_api.cpp
)

target_link_libraries(oj_problems_api_test PRIVATE oj_core)

add_test(NAME problems_api COMMAND oj_problems_api_test)

# 配置管理单元测试（命令行参数解析、端口校验、环境变量读取、JWT 配置校验）。
add_executable(oj_config_test
  tests/unit/test_config.cpp
)

target_link_libraries(oj_config_test PRIVATE oj_core GTest::gtest_main)

add_test(NAME config_unit COMMAND oj_config_test)

# 项目结构单元测试（M0.1）：验证约定目录、关键文件与忽略规则（纯文件系统检查）。
add_executable(oj_project_structure_test
  tests/unit/test_project_structure.cpp
)

target_compile_definitions(oj_project_structure_test PRIVATE
  OJ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(oj_project_structure_test PRIVATE GTest::gtest_main)

add_test(NAME project_structure_unit COMMAND oj_project_structure_test)

# HTTP 服务单元测试（M0.2）：健康检查、启动/停止生命周期、端口占用检测与
# 同端口重启（隔离临时库 + 环回随机端口，不触碰正式数据库）。
add_executable(oj_http_server_test
  tests/unit/test_http_server.cpp
)

target_link_libraries(oj_http_server_test PRIVATE oj_core GTest::gtest_main)

add_test(NAME http_server_unit COMMAND oj_http_server_test)

# M1.7 静态资源托管单元测试（gtest）：仅挂载给定 web_root、MIME、目录穿越防护、
# 敏感文件不可下载、空/缺失 web_root 降级，以及既有 API 不受影响（隔离临时库 +
# 临时 web 目录 + 随机端口）。
add_executable(oj_static_files_test
  tests/unit/test_static_files.cpp
)

target_link_libraries(oj_static_files_test PRIVATE oj_core GTest::gtest_main)

add_test(NAME static_files_unit COMMAND oj_static_files_test)

# M1.5 判题核心单元测试（gtest）：输出归一化/比对、状态汇总、语言解析，以及用
# FakeExecutor 驱动 JudgeEngine 的编排逻辑（不启动真实进程，不触碰数据库/网络）。
add_executable(oj_judge_unit
  tests/unit/test_judge_unit.cpp
)

target_link_libraries(oj_judge_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME judge_unit COMMAND oj_judge_unit)

# M3.1 判题调度器单元测试（gtest）：worker 数量规则、同步屏障证明并发、活动任务
# 上限、有界等待队列满载拒绝与并发入队、结果通道独立、异常隔离、停止排空与回收。
# 使用可控 handler 与同步机制，不触碰 HTTP/数据库/真实进程。
add_executable(oj_judge_manager_unit
  tests/unit/test_judge_manager.cpp
)

target_link_libraries(oj_judge_manager_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME judge_manager_unit COMMAND oj_judge_manager_unit)

# M1.5 判题编译与进程集成测试（真实 g++/gcc + fork/exec，隔离临时工作目录）：
# C++17/C11 的 AC/WA、CE、顺序执行、每点新进程、超时回收、超大输出、提前退出、
# 编译器缺失、临时目录清理与子进程回收。
add_executable(oj_judge_integration
  tests/integration/test_judge_integration.cpp
)

target_link_libraries(oj_judge_integration PRIVATE oj_core)

add_test(NAME judge_integration COMMAND oj_judge_integration)

# M1.6 提交校验与状态计算单元测试（gtest）：语言取值、源码校验、做题状态计算。
add_executable(oj_submit_unit
  tests/unit/test_submit_unit.cpp
)

target_link_libraries(oj_submit_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME submit_unit COMMAND oj_submit_unit)

# M1.6 提交接口集成测试（隔离临时库 + 随机端口真实 HTTP + 可注入执行器）：
# 认证/首改/可见性/参数校验、真实 g++/gcc 的 AC/WA/CE、隐藏用例不泄露、
# 提交与状态持久化、并发计数、SYSERR 记录、事务回滚与重启持久化。
add_executable(oj_submit_api_test
  tests/integration/test_submit_api.cpp
)

target_link_libraries(oj_submit_api_test PRIVATE oj_core)

add_test(NAME submit_api COMMAND oj_submit_api_test)

# M3.1 判题任务调度 HTTP 集成测试（隔离临时库 + 随机端口 + 受控/真实执行器）：
# 队列满载 503 且未接收不落库、繁忙时健康检查/题目查询可响应、执行器异常转 SYSERR、
# 并发首次 AC 最早时间、排队时间不计入运行耗时、真实 C++17/C11 并发判题与子进程回收、
# 优雅停止排空已接收任务。
add_executable(oj_submit_scheduling_api_test
  tests/integration/test_submit_scheduling_api.cpp
)

target_link_libraries(oj_submit_scheduling_api_test PRIVATE oj_core)

add_test(NAME submit_scheduling_api COMMAND oj_submit_scheduling_api_test)

# M2.1 管理员题目写入字段校验单元测试（gtest）：创建/部分更新的字段校验与规范
# 化（必填、类型、长度、难度枚举、标签结构、数值范围与默认值）。
add_executable(oj_problem_admin_unit
  tests/unit/test_problem_admin_unit.cpp
)

target_link_libraries(oj_problem_admin_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME problem_admin_unit COMMAND oj_problem_admin_unit)

# M2.1 管理员题目接口集成测试（隔离临时库 + 随机端口真实 HTTP + 可注入执行器）：
# 建题/改题/删题的权限、字段校验、可见性、默认值与部分更新语义、删除关联数据
# 规则、并发删除与提交一致性、重启持久化。
add_executable(oj_admin_problems_api_test
  tests/integration/test_admin_problems_api.cpp
)

target_link_libraries(oj_admin_problems_api_test PRIVATE oj_core)

add_test(NAME admin_problems_api COMMAND oj_admin_problems_api_test)

# M2.2 管理员测试用例写入字段校验与 ord 规则单元测试（gtest）：创建/部分更新的
# 字段校验（必填、类型、空串与缺失区分、长度、ord 范围）。
add_executable(oj_testcase_admin_unit
  tests/unit/test_testcase_admin_unit.cpp
)

target_link_libraries(oj_testcase_admin_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME testcase_admin_unit COMMAND oj_testcase_admin_unit)

# M2.2 管理员测试用例接口集成测试（隔离临时库 + 随机端口真实 HTTP + 可注入执行器）：
# 读/增/改/删的权限、字段与归属校验、ord 规则与判题执行顺序一致、公开样例与隐藏
# 用例隔离、空测试集不判 AC、修改对新旧提交的影响、并发删除与重启持久化。
add_executable(oj_admin_testcases_api_test
  tests/integration/test_admin_testcases_api.cpp
)

target_link_libraries(oj_admin_testcases_api_test PRIVATE oj_core)

add_test(NAME admin_testcases_api COMMAND oj_admin_testcases_api_test)

# M2.3 题目列表查询参数解析单元测试（gtest）：空白裁剪、LIKE 转义、标题/标签
# 匹配模式、难度取值、q/difficulty/tag/page/visible 归一化与非法值拒绝。
add_executable(oj_problem_list_query_unit
  tests/unit/test_problem_list_query.cpp
)

target_link_libraries(oj_problem_list_query_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME problem_list_query_unit COMMAND oj_problem_list_query_unit)

# M2.3 题目列表查询集成测试（隔离临时库 + 随机端口真实 HTTP + 可注入执行器）：
# 搜索/难度/标签筛选、组合 AND、分页稳定性与边界、total 可见性、通过人数口径、
# 本人状态、游客/普通/管理员可见范围、管理员 visible 筛选、不泄露隐藏内容。
add_executable(oj_problem_list_api_test
  tests/integration/test_problem_list_query.cpp
)

target_link_libraries(oj_problem_list_api_test PRIVATE oj_core)

add_test(NAME problems_list_api COMMAND oj_problem_list_api_test)

# M2.4 管理员用户接口请求参数校验单元测试（gtest）：action/user_id/new_password/
# role 的解析与校验、未知操作与错误类型拒绝、page 分页参数边界。
add_executable(oj_user_admin_unit
  tests/unit/test_user_admin_unit.cpp
)

target_link_libraries(oj_user_admin_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME user_admin_unit COMMAND oj_user_admin_unit)

# M2.4 管理员用户接口集成测试（隔离临时库 + 随机端口真实 HTTP + 可注入执行器）：
# 用户列表字段/排序/分页与敏感字段不泄露、权限（未登录/普通/未改密管理员）、
# 重置密码与 old/new 登录、首改标记与实际提交限制、改角色与权限实时生效、
# 最后管理员保护与并发、越权字段忽略、失败不部分更新、重启持久化。
add_executable(oj_admin_users_api_test
  tests/integration/test_admin_users_api.cpp
)

target_link_libraries(oj_admin_users_api_test PRIVATE oj_core)

add_test(NAME admin_users_api COMMAND oj_admin_users_api_test)
