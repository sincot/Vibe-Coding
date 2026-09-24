# 可选 C++ 覆盖率采集（gcov + gcovr/lcov）

> **定位**：M6.1 之外的**可选 QA 增强**。M6.1 的完成条件不包含覆盖率；本工具不注册
> CTest、不进入常规回归，也不改变 M6.1 的验证结论。未采集/未覆盖的分支不代表测试失败。

## 组成

| 文件 | 作用 |
|---|---|
| `run_coverage.sh` | 配置独立覆盖率构建 → 单并发构建 → 串行跑全部常规 CTest →（可选）instrumented 服务跑 `regression.sh` → 生成 HTML/文本报告 |
| `install_tools_local.sh` | **免 sudo** 用 `apt-get download` + `dpkg-deb -x` 把 gcovr 与 lcov/genhtml 解包到 `tests/coverage/.tools/`（不修改系统） |

## 前置

1. 已能常规构建（见根 `README.md`）。
2. 报告工具二选一：
   ```bash
   bash tests/coverage/install_tools_local.sh      # 免 sudo（推荐，本仓库已验证）
   sudo apt-get install -y lcov gcovr              # 系统安装
   ```
   本机无 sudo，故采用本地解包；`run_coverage.sh` 会自动 source `.tools/env.sh`。
3. `gcov`（随 gcc 提供）。

## 用法

```bash
bash tests/coverage/run_coverage.sh
COV_WITH_REGRESSION=0 bash tests/coverage/run_coverage.sh   # 跳过 main.cpp 覆盖
COV_TOOL=lcov bash tests/coverage/run_coverage.sh           # 只用 lcov
COV_SCOPE='src/judge/' bash tests/coverage/run_coverage.sh  # 只看判题核心
```

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `COV_BUILD_DIR` | `<repo>/build-cov` | 独立覆盖率构建目录（已加入 `.gitignore`） |
| `COV_TOOL` | `auto` | `auto`/`gcovr`/`lcov` |
| `COV_SCOPE` | `src/` | 覆盖率过滤正则 |
| `COV_WITH_REGRESSION` | `1` | 用 instrumented `oj_server` 跑 `regression.sh`，覆盖 `src/main.cpp` |
| `COVTOOL_DIR` | `tests/coverage/.tools` | 本地工具目录 |

## 产物

- gcovr：`build-cov/coverage/index.html`（行/分支明细）、`summary.txt`
- lcov：`build-cov/coverage/html-lcov/index.html`（**含函数覆盖率**）、`lcov-summary.txt`
- 日志：`build-cov/coverage-{configure,build,ctest,regression}.log`

## 最近一次实测（本机）

- 构建：`build-cov`（`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage`），单并发，
  峰值 RSS ≈ 524 MiB。
- 覆盖率下测试：CTest **49/49 通过**（243s）+ `regression.sh` **23/23**。
- 总体（`src/`）：行 **78.9%**（5395/6841）、函数 **95.8%**（385/402）、
  分支 **49.9%**（5344/10704）。
- 分目录：`judge` 行 71.1%/函数 88.6%/分支 47.5%；`db` 75.0%/98.2%/46.7%；
  `auth` 84.8%/97.1%/56.6%；`http` 88.6%/100%/50.8%；`problem` 98.7%/100%/75.6%；
  `submit` 67.6%/100%/36.7%；`user` 97.9%/100%/76.2%。
- M6.1 判题核心：`comparator.cpp` 行 100%/分支 81.8%、`classification.cpp` 95.3%/64.9%、
  `judge.cpp` 89.5%/59.9%；`local_executor.cpp` 67.1%/43.6%、`sandbox.cpp` 49.9%/34.5%
  （fork/seccomp/资源限制等错误与特权分支大量未覆盖，需 root/命名空间特定场景）。

## 注意事项

- `gcovr 5.0` 的文本/JSON 汇总只含**行与分支**；**函数覆盖率**由 `lcov --summary` 提供，
  本脚本已同时生成。
- `lcov 1.14` 对 `gcov 11` 的兼容性有限（可能丢弃部分分支/函数数据），故以 `gcovr` 为主、
  `lcov` 补函数维度；两者计数口径略有差异（如行总数 6801 vs 6841）。
- 覆盖率是**一次性、可选**采集：需独立 instrumented 构建（`build-cov/` 约 800 MiB），
  不污染常规 `build/`，不与常规构建/测试并发。
- 未采集时不得声称“全部函数/分支已覆盖”（`TESTING_GUIDE.md` 7.3）。
