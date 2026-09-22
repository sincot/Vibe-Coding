# 前端后台管理页面 DOM 级验证（M2.5）

本目录存放 M2.5 后台管理页面的可重复运行的页面级验证，**不注册到 CTest、不属于项目运行
依赖**，保持前端“无构建、无测试框架”。在真实启动的后端上，用 [jsdom](https://github.com/jsdom/jsdom)
执行仓库中真实的 `web/js` 模块（hash 路由、页面视图、API 封装）并驱动 DOM 交互。

## 前置条件

- 后端已构建：`cmake --build build --parallel 1`（需要 `build/oj_server`）。
- Node.js 与 jsdom：

  ```bash
  npm install --prefix /tmp/opencode/domtest jsdom
  ```

  可用 `JSDOM_DIR` 指向其它已安装 jsdom 的目录。

## 运行

```bash
bash tests/frontend/run.sh
```

脚本会：

1. 在 `mktemp` 临时目录创建隔离 SQLite 库并导入种子题；
2. 以随机端口启动 `oj_server`（测试密钥/管理员密码，仅内存环境变量，不落盘）；
3. 用 jsdom 执行 `admin_pages_dom.mjs`，连接该服务完成验证；
4. 结束时停止服务并清理临时目录，不影响正式 `data/oj.db`。

可选环境变量：`JSDOM_DIR`、`OJ_FRONTEND_PORT`。

## 覆盖范围

- 后台入口与访问检查（游客/普通用户/未改密管理员/无效 token/权限被撤销）；
- 题目管理：列表分页与筛选、创建/编辑、公开隐藏、删除与冲突、表单校验负路径、公开样例维护；
- 测试用例管理：增/改/删、原样文本、`ord` 规则（自动追加/显式/越界）、公开样例只读、跨题不串用；
- 用户管理：列表、重置密码（新旧登录与首改标记）、角色提升/降级、最后管理员保护、自我降级；
- 状态与安全：加载/空/保存中/失败、重复提交、网络失败不假成功、XSS 文本渲染、敏感数据不入 localStorage。

## 边界

- 该验证在 jsdom 中执行，**不等同于真实图形浏览器渲染**，也未覆盖真机窄屏布局。
- 未采集前端覆盖率。
