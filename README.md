# tumgrd

tumgrd 是一个运行在 OpenWrt 上的守护进程，用于控制 [tutuicmptunnel-kmod](https://github.com/hrimfaxi/tutuicmptunnel-kmod) 内核模块。

## 概述

tumgrd 通过 ubus 接口管理 ICMP 隧道节点，提供以下能力：

- **节点管理**：注册/注销隧道节点，每个节点关联一个远程服务器和本地客户端端口
- **公网 IP 自动检测**：通过 HTTP 或 IPv6 连接探测当前 WAN IP
- **自动同步**：当检测到公网 IP 变化或 WAN 接口 up 时，自动更新隧道配置
- **周期性检查**：定时检测 IP 变化并自愈（reconcile），保持隧道状态正确

### 架构

```
tumgr (CLI) ──ubus──▶ tumgrd (daemon) ──▶ tuctl_client ──▶ 远程服务器
                           │
                           └──▶ ktuctl ──▶ tutuicmptunnel-kmod (内核模块)
```

- `tumgrd`：守护进程，暴露 ubus 对象 `tumgrd`，管理 desired state（SQLite）
- `tumgr`：shell CLI 客户端，封装 ubus 调用
- `tuctl_client`：向远程 tutuicmptunnel 服务器添加/删除 tunnel
- `ktuctl`：控制本地 tutuicmptunnel-kmod 内核模块的客户端连接

### 自动同步机制

- 启动 3 秒后执行一次全量 reconcile
- 此后每 `interval` 秒周期性检查
- 当 WAN 接口触发 `ifup` 事件时，强制全量 reconcile
- reconcile 逻辑：检测公网 IP → 与上次记录比较 → 若变化或处于 error 状态则重新应用配置

## 安装

### 依赖

- OpenWrt 系统，需安装以下包：
  - `libubus` / `libubox`（OpenWrt 核心库）
  - `sqlite3` / `libsqlite3`
  - `libuci`（可选，用于 WAN 接口检测）
- `tutuicmptunnel-kmod` 内核模块及配套工具 `tuctl_client`、`ktuctl`

### 编译

```bash
mkdir build && cd build
cmake ..
make
```

交叉编译（以 aarch64 为例）：

```bash
mkdir build-aarch64 && cd build-aarch64
cmake -DCMAKE_TOOLCHAIN_FILE=../openwrt-aarch64.cmake ..
make
```

项目提供了以下 toolchain 文件：
- `openwrt-aarch64.cmake`
- `openwrt-x86_64.cmake`
- `openwrt-ramips.cmake`

### 部署到 OpenWrt

1. 将编译产物和文件复制到路由器：

```bash
# 守护进程
scp tumgrd root@<router>:/usr/sbin/tumgrd

# CLI 客户端
scp tumgr root@<router>:/usr/bin/tumgr
chmod +x /usr/bin/tumgr

# init 脚本和配置文件
scp contrib/etc/init.d/tumgrd root@<router>:/etc/init.d/tumgrd
scp contrib/etc/config/tumgrd root@<router>:/etc/config/tumgrd
```

2. 启动服务：

```bash
/etc/init.d/tumgrd enable
/etc/init.d/tumgrd start
```

### UCI 配置

`/etc/config/tumgrd`：

```
config tumgrd 'main'
    option enabled '1'
    option database '/lib/tumgrd/tumgrd.db'
    option interval '60'
    option log_level 'info'
    option enable_xor '1'
    option use_client_ip '1'
```

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `1` | 是否启用服务 |
| `database` | `/lib/tumgrd/tumgrd.db` | SQLite 数据库路径 |
| `interval` | `60` | 周期性检查间隔（秒） |
| `log_level` | `info` | 日志级别：error/warn/info/debug/trace |
| `enable_xor` | `0` | 新注册节点是否自动生成 XOR 密钥 |
| `use_client_ip` | `1` | 是否使用 @client_ip@ 占位符（启用后服务端自动解析客户端 IP） |

## 使用

### tumgr CLI

#### 注册节点

```bash
tumgr register -uid my-node-01 \
  -server-host 192.168.1.100 \
  -server-port 14801 \
  -client-port 1443 \
  -psk my-secret-password \
  -description "主服务器" \
  -client-comment "my-node-01"
```

可选参数：`-memlimit <bytes>`、`-ip-check-url <url>`、`-ip-version <ipv4|ipv6>`

#### 注销节点

```bash
tumgr deregister -uid my-node-01 \
  -server-host 192.168.1.100 \
  -server-port 14801 \
  -ip-version ipv4
```

#### 刷新（手动触发同步）

```bash
# 刷新单个节点
tumgr refresh -uid my-node-01 \
  -server-host 192.168.1.100 \
  -server-port 14801 \
  -ip-version ipv4 -force

# 刷新所有节点
tumgr refresh -all -force
```

#### 查看状态

```bash
tumgr status          # 表格格式
tumgr status -json    # JSON 格式
tumgr dump            # 原始 JSON dump
```

### 直接调用 ubus

也可以绕过 `tumgr` 直接使用 ubus：

```bash
# 注册
ubus call tumgrd register '{"uid":"my-node-01","server_host":"192.168.1.100","server_port":14801,"client_port":1443,"psk":"my-secret","memlimit":1024768}'

# 查看状态
ubus call tumgrd status

# 刷新所有
ubus call tumgrd refresh '{"all":true,"force":true}'

# 注销
ubus call tumgrd deregister '{"uid":"my-node-01","server_host":"192.168.1.100","server_port":14801,"ip_version":"ipv4"}'
```

## 命令行参数（tumgrd 守护进程）

```
Usage: tumgrd [options]

Options:
  -d, --database PATH      SQLite 数据库路径（默认：/lib/tumgrd/tumgrd.db）
  -i, --interval SEC       监控间隔秒数（10-3600，默认：60）
  -s, --socket PATH        ubus socket 路径（默认：系统默认）
      --log-level LEVEL    日志级别：debug|info|warn|error（默认：info）
      --enable-xor         新节点自动生成 XOR 密钥
      --disable-xor        禁用自动生成 XOR 密钥（默认）
      --fwmark NUM         IP 检测的 SO_MARK 值（0-255，默认：2）
  -h, --help               显示帮助
```

## 许可证

本项目基于 [GNU General Public License v2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) 发布。
