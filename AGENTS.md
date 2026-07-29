# AGENTS.md

## 项目概述

`tumgrd` 是一个运行在 OpenWrt 上的 C11 守护进程，用于管理 ICMP 隧道节点配置。它作为 ubus 服务运行，将状态持久化到 SQLite，并协调远程（`tuctl_client`）和本地（`ktuctl`）隧道工具。配套的 `tumgr` CLI 是一个 POSIX shell 脚本，封装了 `ubus call tumgrd ...` 调用。

## 构建

```bash
# 本地构建（需要主机上有 libubox、libubus、sqlite3 头文件）
mkdir build && cd build && cmake .. && make

# 交叉编译 OpenWrt aarch64
mkdir build-aarch64 && cd build-aarch64
cmake -DCMAKE_TOOLCHAIN_FILE=../openwrt-aarch64.cmake ..
make
```

工具链文件：`openwrt-aarch64.cmake`、`openwrt-x86_64.cmake`、`openwrt-ramips.cmake`。每个文件硬编码了 SDK 路径（`$HOME/temp/openwrt-sdk-*`），交叉编译前需要修改路径。

项目没有测试套件、没有 CI、没有 `make test`。验证方式为手动：在 OpenWrt 设备或支持 ubus 的环境中运行 `tumgrd`。

## 代码格式化与检查

```bash
./tidy          # 运行: clang-format -i src/*.[ch]
```

`.clang-format` 基于 LLVM 风格，主要配置：2 空格缩进、128 列限制、指针右对齐、`AlignConsecutiveAssignments/Declarations/Macros` 启用、不允许短块放在单行。

没有配置其他 linter 或静态分析工具。

## 架构

```
src/
  main.c        — 入口点、参数解析、uloop 初始化、信号处理
  db.c / db.h   — SQLite schema + CRUD（节点表）
  reconcile.c   — 同步逻辑：检测 IP → 比较 → 通过 tuctl_client/ktuctl 应用配置
  runner.c      — 子进程执行（fork+exec 调用外部工具）
  ubus_if.c     — ubus 对象和方法（register/deregister/refresh/status/dump）
  ipdetect.c    — HTTP 和 TCP 连接方式的公网 IP 检测
  helper.c      — URL 解析、杂项工具函数
  log_impl.c    — 基于 syslog 的日志实现
  try.h         — 错误处理宏（见下文）
  tumgrd.h      — 共享上下文结构体、常量、默认 URL
```

`tumgr`（无扩展名）是 shell 脚本，不是编译代码。

## 错误处理模式

所有 C 代码都使用 `try.h` 宏。这是本项目最重要的约定：

```c
int err = -1;

try2(some_call(), "some_call failed: %s", detail);   // 返回负值 → 跳转 err_cleanup
try2_p(alloc_thing(), "alloc_thing failed");           // 返回 NULL → 跳转 err_cleanup，err 设为 -ENOMEM

err = 0;

err_cleanup:
  // 释放资源
  return err;
```

- `try2(expr, ...)` — expr < 0 时跳转到 `err_cleanup`
- `try2_p(expr, ...)` — expr 为 NULL 时跳转到 `err_cleanup`
- `err_cleanup(ret, ...)` — 显式跳转并附带错误信息
- `ret(ret, ...)` — 立即返回错误（不跳转到 cleanup 标签）
- `strret` / `strerrno` — 分别是 `strerror(-_ret)` / `strerror(errno)` 的简写

每个可能失败的函数都遵循 `int err = -1; ... err = 0; err_cleanup: return err;` 的骨架结构。

## 节点标识

节点由 `(server_host, server_port, uid, ip_version)` 复合键唯一标识。所有数据库操作和 ubus 调用都使用此复合键。`client_port` 在同一 server + ip_version 组合内必须唯一。

## ubus 接口

守护进程注册 ubus 对象 `tumgrd`，提供方法：`register`、`deregister`、`refresh`、`status`、`dump`。详见 `src/ubus_if.c` 中的策略定义和处理函数。`tumgr` shell 脚本展示了预期的 JSON 载荷格式。

## 运行时依赖（OpenWrt）

- `libubox` / `libubus`（必需）
- `sqlite3`（必需）
- `libuci`（可选，编译时由 `TUMGRD_UCI_ENABLED` 宏控制）
- `tuctl_client`、`ktuctl`（运行时，用于隧道操作）
- `jsonfilter`（`tumgr` CLI 用于表格输出）

## 关键常量

- 默认数据库路径：`/lib/tumgrd/tumgrd.db`
- 默认 IP 检测 URL：`http://ip.3322.net/`（仅 HTTP，不支持 HTTPS）
- 默认 fwmark：`2`
- 默认检测间隔：`60s`
- 同步决策公式：`need_apply = force || ip_changed || was_error`

## 其他文件

- `to_llm.sh` — 导出所有 `src/*.[ch]` 文件内容，用于 LLM 上下文输入
- `docs/` — 详细文档（api、contributing、deployment、development-setup、quick-start、troubleshooting、user-guide）
- `contrib/etc/init.d/tumgrd` — procd init 脚本
- `contrib/etc/config/tumgrd` — UCI 默认配置

## Git 工作规范

### 提交前必须格式化和检查

1. **必须运行 `./tidy`**：提交任何 `src/*.[ch]` 文件的修改前，必须执行 `./tidy`（即 `clang-format -i src/*.[ch]`）格式化代码。
2. **必须检查合并冲突标记**：提交前用 `grep -rn '<<<<<<\|======\|>>>>>>' src/` 检查是否有残留的合并冲突标记。发现任何 `<<<<<<<`、`=======`、`>>>>>>>` 标记必须先解决再提交。

### 禁止使用 git add -A / git commit -A

**绝对不允许**使用以下命令：
- `git add -A`
- `git add .`
- `git add --all`
- `git commit -a`
- `git commit -A`

必须显式指定要添加的文件路径，例如 `git add src/main.c src/db.c`。这样可以避免意外提交不需要的文件（如构建产物、编辑器临时文件等）。

### 禁止使用交互式 git 操作

**不允许**使用需要终端交互的 git 命令，包括：
- `git rebase -i`（交互式 rebase）
- `git add -p`（交互式暂存）
- `git commit -p`（交互式提交）
- `git checkout -p`（交互式检出）

这些命令会阻塞终端等待用户输入，导致自动化流程卡死。如需 rebase，使用非交互式方式，或拆分为多步显式命令（`git reset --soft`、`git commit` 等）。
