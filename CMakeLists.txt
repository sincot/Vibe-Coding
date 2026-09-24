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
  src/db/leaderboard.cpp
  src/db/in_flight.cpp
  src/db/instance_lock.cpp
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
  src/judge/deadline.cpp
  src/judge/comparator.cpp
  src/judge/classification.cpp
  src/judge/workspace.cpp
  src/judge/sandbox.cpp
  src/judge/compile_gate.cpp
  src/judge/local_executor.cpp
  src/judge/judge.cpp
  src/judge/manager.cpp
  src/submit/submit.cpp
  src/submit/rejudge.cpp
  src/submit/recovery.cpp
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

# M3.2 截止时间与超时控制单元测试（gtest）：单调时钟剩余时间、有效单点时限、
# 全局硬上限跨点累计与结果保留、编译保护超时与全局裁剪区分、协作式取消。
add_executable(oj_judge_deadline_unit
  tests/unit/test_judge_deadline.cpp
)

target_link_libraries(oj_judge_deadline_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME judge_deadline_unit COMMAND oj_judge_deadline_unit)

# M3.1 判题调度器单元测试（gtest）：worker 数量规则、同步屏障证明并发、活动任务
# 上限、有界等待队列满载拒绝与并发入队、结果通道独立、异常隔离、停止排空与回收。
# 使用可控 handler 与同步机制，不触碰 HTTP/数据库/真实进程。
add_executable(oj_judge_manager_unit
  tests/unit/test_judge_manager.cpp
)

target_link_libraries(oj_judge_manager_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME judge_manager_unit COMMAND oj_judge_manager_unit)

# M3.3 沙箱与资源限制单元测试（gtest）：编译并发门限、资源上限换算、seccomp 过滤器
# 构建、能力探测与 Workspace 清理安全（不启动真实判题进程）。
add_executable(oj_sandbox_unit
  tests/unit/test_sandbox_unit.cpp
)

target_link_libraries(oj_sandbox_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME sandbox_unit COMMAND oj_sandbox_unit)

# M3.3 运行隔离与资源限制集成测试（真实 Linux 进程）：命名空间/chroot/seccomp/
# setrlimit、目录与网络隔离、CPU/内存/输出限制、ASan/UBSan 兼容、失败路径与清理。
add_executable(oj_sandbox_integration
  tests/integration/test_sandbox_integration.cpp
)

target_link_libraries(oj_sandbox_integration PRIVATE oj_core)

add_test(NAME sandbox_integration COMMAND oj_sandbox_integration)

# M1.5 判题编译与进程集成测试（真实 g++/gcc + fork/exec，隔离临时工作目录）：
# C++17/C11 的 AC/WA、CE、顺序执行、每点新进程、超时回收、超大输出、提前退出、
# 编译器缺失、临时目录清理与子进程回收。
add_executable(oj_judge_integration
  tests/integration/test_judge_integration.cpp
)

target_link_libraries(oj_judge_integration PRIVATE oj_core)

add_test(NAME judge_integration COMMAND oj_judge_integration)

# M3.4 分类与汇总逻辑单元测试（gtest，纯函数）：单点分类的确定性优先级、多迹象、
# Sanitizer 文本不单独判失败、缺失指标、总体汇总严重度与顺序无关、诊断路径清洗。
add_executable(oj_judge_classification_unit
  tests/unit/test_judge_classification.cpp
)

target_link_libraries(oj_judge_classification_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME judge_classification_unit COMMAND oj_judge_classification_unit)

# M3.4 编译与结果分类集成测试（真实 g++/gcc + 沙箱）：默认 ASan/UBSan 编译模板的
# AC/WA/CE、空白规则、越界/UB 判失败、伪 Sanitizer 文本不误判、TLE/RE/MLE、
# 输出与诊断上限、编译器故障 SYSERR 后恢复、失败继续与混合优先级、指标口径。
add_executable(oj_m34_classification_test
  tests/integration/test_m34_classification.cpp
)

target_link_libraries(oj_m34_classification_test PRIVATE oj_core)

add_test(NAME m34_classification COMMAND oj_m34_classification_test)

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

# M3.5 停止清理单元测试（gtest）：Workspace 清理失败可定位、可重复收尾、不误删他人
# 目录（不启动判题进程、不触碰数据库）。
add_executable(oj_m35_cleanup_unit
  tests/unit/test_m35_cleanup_unit.cpp
)

target_link_libraries(oj_m35_cleanup_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME m35_cleanup_unit COMMAND oj_m35_cleanup_unit)

# M3.5 持久化与停止清理集成测试（隔离临时库 + 随机端口 + 受控执行器/同步门 +
# 故障注入）：完整结果关闭重开可读、事务回滚、数据库锁竞争有界、完成/取消竞争
# 只最终处理一次、客户端断开不丢结果、各阶段停止与新提交拒绝、重复停止幂等、
# 无遗留进程/目录、失败路径无资源持续增长、停止重启后数据一致且可继续判题。
add_executable(oj_m35_persistence_shutdown_test
  tests/integration/test_m35_persistence_shutdown.cpp
)

target_link_libraries(oj_m35_persistence_shutdown_test PRIVATE oj_core)

add_test(NAME m35_persistence_shutdown COMMAND oj_m35_persistence_shutdown_test)

# M3.6 Rejudge 集成测试（隔离临时库 + 随机端口 + 受控/真实执行器）：
# 管理员权限与首改限制、非法/不存在 ID、客户端字段不可替换、真实 g++/gcc 的
# C++17/C11 AC/WA 互转、原记录保留与计数不变、状态重算（最早 AC/唯一失效/通过人数）、
# 并发去重与队列满载 503、SYSERR/服务取消保留原结果、重启持久化。
add_executable(oj_rejudge_api_test
  tests/integration/test_rejudge_api.cpp
)

target_link_libraries(oj_rejudge_api_test PRIVATE oj_core)

add_test(NAME rejudge_api COMMAND oj_rejudge_api_test)

# M3.7 在途任务持久化与单实例互斥单元测试（gtest）：InFlightStore 状态机、任务标识
# 生成、分批读取/原子认领/中断/失效认领重置、InstanceLock 互斥与释放（隔离临时文件库）。
add_executable(oj_m37_in_flight_unit
  tests/unit/test_m37_in_flight_unit.cpp
)

target_link_libraries(oj_m37_in_flight_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME m37_in_flight_unit COMMAND oj_m37_in_flight_unit)

# M3.7 崩溃恢复与在途任务持久化集成测试（隔离临时库 + 随机端口 + 受控执行器）：
# 接收即持久化、容量拒绝不落库、原子结算唯一、启动恢复重放并保留原提交时间/首次 AC、
# 恢复容量等待不丢弃、无法判题标记中断、删题保护、旧库终态不误恢复、Rejudge 不涉在途。
add_executable(oj_m37_recovery_api_test
  tests/integration/test_m37_recovery_api.cpp
)

target_link_libraries(oj_m37_recovery_api_test PRIVATE oj_core)

add_test(NAME m37_recovery_api COMMAND oj_m37_recovery_api_test)

# M4.3 题目详情「本人做题状态」读取单元测试（gtest）：ProblemStore::viewer_solved
# 的 accepted/none/无记录/游客/不存在题目/用户与题目隔离语义（隔离临时文件库）。
add_executable(oj_problem_owner_status_unit
  tests/unit/test_problem_owner_status_unit.cpp
)

target_link_libraries(oj_problem_owner_status_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME problem_owner_status_unit COMMAND oj_problem_owner_status_unit)

# M4.3 题目与做题页面后端接口集成测试（隔离临时库 + 随机端口 + 受控执行器）：
# 详情 solved（游客不返回/登录返回/AC 后 true/仅 WA 仍 false/不泄露隐藏用例/401/404）、
# 提交响应新增字段（global_deadline_hit/cancelled/output_truncated、非 AC 点
# actual_output 始终存在）、全局硬上限部分执行、服务取消透传。
add_executable(oj_m43_problem_page_api_test
  tests/integration/test_m43_problem_page_api.cpp
)

target_link_libraries(oj_m43_problem_page_api_test PRIVATE oj_core)

add_test(NAME m43_problem_page_api COMMAND oj_m43_problem_page_api_test)

# M4.4 提交历史与本人状态读取（数据层）单元测试（gtest）：SubmissionStore::list_by_user
# 的分页/最新优先/用户隔离/题目筛选/摘要字段，以及 UserProblemStatusStore::list_by_user
# 的排序/字段/筛选/隔离（隔离临时文件库）。
add_executable(oj_m44_history_unit
  tests/unit/test_m44_submission_history_unit.cpp
)

target_link_libraries(oj_m44_history_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME m44_history_unit COMMAND oj_m44_history_unit)

# M4.4 提交历史 / 提交详情 / 本人题目状态后端接口集成测试（隔离临时库 + 随机端口）：
# 仅本人历史与身份不可指定、mine 解析、分页与参数校验、题目筛选；详情本人/管理员/
# 首改/越权/非法 ID/损坏 per_case/更新后保存结果/隐藏题可见性；本人状态字段与隔离。
add_executable(oj_m44_history_api_test
  tests/integration/test_m44_history_api.cpp
)

target_link_libraries(oj_m44_history_api_test PRIVATE oj_core)

add_test(NAME m44_history_api COMMAND oj_m44_history_api_test)

# M4.5 排行榜统计读取（数据层）单元测试（gtest）：LeaderboardStore::query 的聚合
# （重复 AC 不重复计数、失败提交计数、AC 后失败保留、Rejudge 重算联动）、排序级联、
# null 首次 AC、隐藏题排除、仅已结算用户、管理员同口径、分页与越界（隔离临时文件库）。
add_executable(oj_m45_leaderboard_unit
  tests/unit/test_m45_leaderboard_unit.cpp
)

target_link_libraries(oj_m45_leaderboard_unit PRIVATE oj_core GTest::gtest_main)

add_test(NAME m45_leaderboard_unit COMMAND oj_m45_leaderboard_unit)

# M4.5 排行榜接口集成测试（隔离临时库 + 随机端口）：公开访问/空库、排序与全局名次、
# 分页与参数校验、字段范围（不泄露账号/源码等）、隐藏题排除、管理员同口径，以及
# Rejudge 重算后的统计与排序联动。
add_executable(oj_m45_leaderboard_api_test
  tests/integration/test_m45_leaderboard_api.cpp
)

target_link_libraries(oj_m45_leaderboard_api_test PRIVATE oj_core)

add_test(NAME m45_leaderboard_api COMMAND oj_m45_leaderboard_api_test)
