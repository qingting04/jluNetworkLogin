# jluNetworkLogin

吉林大学校园网（DrCOM）认证客户端 —— OpenWrt 插件 + LuCI 界面（C 版）。

守护进程常驻运行，自动完成 challenge → login → keepalive 认证循环，让路由器下的所有设备无需各自登录即可上网；支持 GitHub Actions 云端编译（无需本地搭建 OpenWrt 环境）。

## 关于 DrCOM

DrCOM（Dr.COM）是深澜软件出品的高校校园网认证系统，吉林大学用它做准入认证。本插件在路由器上以守护进程形式接入校园网，认证成功后转发整个局域网的上网流量。

> 为什么用纯 C：OpenWrt 原生就是 C 交叉编译工具链（`$(TARGET_CC)` + musl），用 C 可以做到「menuconfig 选架构 → make 直接交叉编译」，不依赖任何预编译二进制；而且只链接 OpenWrt 自带的 `libubus` / `libubox` / `libuci`，不需要 libcurl / OpenSSL 之类的额外依赖，包体很小。

## 特性

- **轻量高效**：纯 C（C11，单文件 `drcomd.c`），仅链接 OpenWrt 自带的 `libubus` / `libubox` / `libuci`
- **procd 托管**：由 OpenWrt 原生进程管理器管理，断线自动 respawn，UCI 变更自动 reload
- **ubus RPC 接口**：暴露 `status` / `reconnect` / `reload` 三个方法，可被脚本或其它服务调用
- **系统日志**：经 ulog 写入 logd，`logread` 即可查看
- **LuCI 网页配置**：设置、状态轮询、一键配置（静态 IP / MAC / 网关 / DNS），JS 框架需 LuCI 23.05+
- **中英双语界面**：文案走 LuCI 标准 i18n（英文 msgid + `po/zh_Hans` 翻译包），中文界面由 `luci-i18n-jluNetworkLogin-zh-cn` 提供
- **GitHub Actions 云端编译**，本地零环境

## 快速开始：fork 编译（推荐，无需本地环境）

### 1. Fork 本仓库

点右上角 **Fork**。

### 2. 改成你自己的路由器架构

打开 `.github/workflows/build.yml`，顶部四个变量默认是**小米路由器 3G（MT7621）**：

```yaml
env:
  SDK_VERSION: "25.12.2"   # ImmortalWrt 版本
  TARGET: "ramips"         # 目标平台
  SUBTARGET: "mt7621"      # 子目标
  GCC: "14.3.0"            # gcc 版本
```

如果路由器不是 MT7621，改成对应的值（常见映射见文末[架构表](#架构映射)）。

### 3. 触发编译

- 直接 push 到 `main` 分支，自动触发；或
- 仓库 **Actions** 标签页 → 左侧 `build` → **Run workflow** 手动触发。

约 5~10 分钟完成。

### 4. 下载产物

Actions → 最近一次运行 → **Summary** → 下载 `jluNetworkLogin` artifact，解压得到三个 `.apk`：

```
jluNetworkLogin_1.0.0-1_mipsel_24kc.apk           # 守护进程（C）
luci-app-jluNetworkLogin_1.0.0-1_all.apk          # LuCI 页面
luci-i18n-jluNetworkLogin-zh-cn_*_all.apk         # 中文界面翻译包（可选）
```

> 不装翻译包时界面是英文（msgid 即英文原文），装了就是中文 —— 这是 LuCI 的标准做法。

### 5. 装到路由器

```sh
scp jluNetworkLogin_*.apk luci-app-jluNetworkLogin_*.apk luci-i18n-jluNetworkLogin-*.apk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
apk add --allow-untrusted /tmp/jluNetworkLogin_*.apk
apk add --allow-untrusted /tmp/luci-app-jluNetworkLogin_*.apk
apk add --allow-untrusted /tmp/luci-i18n-jluNetworkLogin-*.apk      # 中文界面（可选）
/etc/init.d/jlu-network-login enable
/etc/init.d/jlu-network-login start
```

> `--allow-untrusted` 是因为本地/CI 编译的包没有官方签名。

刷新 LuCI，入口在 **服务 → JLU Network Login**（中文界面下显示为「吉林大学校园网自动登录」）。

> **固件版本说明**：默认面向 ImmortalWrt 25.12（使用 `apk` 包管理）。若你的固件是旧版 `opkg` 系统，把 `SDK_VERSION` 改成对应的大版本即可产出 `.ipk`，安装时改用 `opkg install jluNetworkLogin_*.ipk`。

## 本地编译（有 OpenWrt / ImmortalWrt 源码树时）

```sh
cp -r jluNetworkLogin            immortalwrt/package/
cp -r luci-app-jluNetworkLogin   immortalwrt/package/
cd immortalwrt

./scripts/feeds update -a && ./scripts/feeds install -a   # 首次，拉 feeds 源码

make menuconfig   # 选架构 + 勾选 Network → jluNetworkLogin，LuCI → Applications → luci-app-jluNetworkLogin
make package/jluNetworkLogin/compile V=s
make package/luci-app-jluNetworkLogin/compile V=s
```

> **注意**：OpenWrt / ImmortalWrt 不同大版本之间 ABI 包名（`libubus` / `libubox` / `libuci` 的版本化依赖）可能不同，**必须使用目标路由器对应版本与 target 的 SDK 或源码树编译**，不可跨版本安装旧的 ipk / apk。

## 使用

页面自上而下三块（版式与 hustNetworkLogin 一致）：

**① 服务状态**（顶部，每几秒自动刷新）：连接状态（空闲 / 已在线 / 获取挑战中 / 登录中 / 保持在线）与最近错误

**② 操作按钮**（紧跟状态区）：

| 按钮 | 功能 |
|------|------|
| 一键配置 | 将所选接口改为静态地址，写入 IP / MAC / 网关 / DNS，关闭 DNS 重绑定保护；执行前自动备份。**接口 / IP / 网关 / MAC 未填齐时点击不做任何配置**（细节见下节） |
| 一键恢复 | 恢复上次「一键配置」前的接口设置，重新启用 DNS 重绑定保护 |
| 重连 | 断开当前连接并重新发起 challenge → login 流程 |

**③ 设置**：启用开关、账号、密码、网络接口（下拉）、IP 地址、网关、MAC 地址（改完点页面底部「保存并应用」；IP / 网关 / MAC 框内灰字仅为**占位示例**，不是默认值，需要自己填）

### 一键配置做了什么

1. 备份当前 `jlu-network-login.main` 的 UCI 配置
2. 将 `/etc/config/network` 中选中接口改为 `static`，写入 IP、MAC、网关；DNS 固定为 `10.10.10.10`、`202.98.18.3`，若无掩码则补 `255.255.255.0`
3. 关闭 `/etc/config/dhcp` 中 dnsmasq 的 `rebind_protection`
4. `ubus call network reload` 并重启 dnsmasq
5. 自动备份修改前的状态，供「一键恢复」使用

### 可配置项

配置文件：`/etc/config/jlu-network-login`

```conf
config main 'main'
    option enabled '0'
    option username '你的学号'
    option password '你的密码'
    option interface 'wan'
    option ip '10.100.61.100'
    option mac 'aa:bb:cc:dd:ee:ff'
    option gateway '10.100.61.1'
```

| 选项 | 必填 | 默认值 | 说明 |
|------|:---:|--------|------|
| `enabled` | 是 | `0` | 是否启用认证 |
| `username` | 是 | — | 学号 |
| `password` | 是 | — | 密码（建议不超过 16 位，见下文） |
| `interface` | 是 | `wan` | 接入校园网的接口 |
| `ip` | 是 | — | 认证时绑定的源 IP |
| `mac` | 是 | — | 认证时绑定的源 MAC |
| `gateway` | 一键配置时 | — | 网关（仅「一键配置」需要） |

> **密码长度**：登录包对密码做混淆时使用 16 字节的密钥，超过 16 位的密码会按 16 位循环使用密钥。建议使用不超过 16 位的密码；如遇登录失败，请先缩短密码再试。

> **必填项由守护进程提示**：界面不再做必填拦截（留空也能保存）。缺少哪项由守护进程写进「最近错误」：`Username is required` / `Password is required` / `IP address is required` / `MAC address is required`（中文界面显示为「未填写 …」）。

命令行等价：

```sh
uci set jlu-network-login.main.username='你的学号'
uci set jlu-network-login.main.password='你的密码'
uci set jlu-network-login.main.interface='wan'
uci set jlu-network-login.main.ip='10.100.61.100'
uci set jlu-network-login.main.mac='aa:bb:cc:dd:ee:ff'
uci set jlu-network-login.main.enabled='1'
uci commit jlu-network-login && /etc/init.d/jlu-network-login reload
```

### 命令行（ubus / 日志）

```sh
# 查看状态（连接状态、IP、MAC、最后错误等）
ubus call jlu-network-login status

# 手动重连
ubus call jlu-network-login reconnect

# 重新加载 UCI 配置并重连（force 可跳过 UCI 加载失败检查）
ubus call jlu-network-login reload
ubus call jlu-network-login reload '{ "force": true }'

# 看日志（守护进程的 syslog 标识是 jlu-network-login）
logread -e jlu-network-login
```

## 协议流程

```
challenge  →  login  →  keepalive (stage 0 → 1 → 2 → 循环)
    ↓ 超时       ↓ 失败        ↓ 超时
  断线重试     断线重试      断线重试
```

- 向服务器发送 UDP challenge 包（`0x01`），请求 4 字节 salt
- 收到 salt 后构造 login 包（含 MD5 校验、口令混淆、MAC 绑定、主机名等字段）
- 登录成功后进入三段式 keepalive 保活，约每 20 秒一轮，断线自动重试

## 目录结构

```
.
├── jluNetworkLogin/                   # C 软件包（守护进程）
│   ├── Makefile                       #   标准 C 包，$(TARGET_CC) 交叉编译
│   ├── src/drcomd.c                   #   单文件实现（challenge / login / keepalive + ubus）
│   └── files/
│       ├── jlu-network-login.init                # procd 服务脚本
│       └── jlu-network-login.config              # UCI 默认配置
├── luci-app-jluNetworkLogin/          # LuCI 插件（JS 版）
│   ├── Makefile
│   ├── po/zh_Hans/luci-app-jluNetworkLogin.po   # 中文翻译（英文 msgid → 中文）
│   └── root/
│       ├── www/luci-static/resources/view/jluNetworkLogin.js
│       ├── usr/share/luci/menu.d/luci-app-jluNetworkLogin.json
│       └── usr/share/rpcd/acl.d/luci-app-jluNetworkLogin.json
└── .github/workflows/build.yml        # GitHub Actions 云端编译
```

## 架构映射

fork 后按自己路由器改 workflow 顶部 4 个变量（`TARGET` / `SUBTARGET` 尤其重要）：

| 路由器 | TARGET / SUBTARGET |
|---|---|
| 小米 3G / 4A 千兆 / AC2100 等（MT7621） | `ramips` / `mt7621` |
| 小米 4A 百兆 / 4C 等（MT7628） | `ramips` / `mt76x8` |
| 红米 AC2100（MT7621） | `ramips` / `mt7621` |
| x86_64 软路由 | `x86` / `64` |
| 树莓派 4 | `bcm27xx` / `bcm2711` |
| 斐讯 N1 | `rockchip` / `armv8`（或对应） |

> 不确定时：在 [ImmortalWrt 固件下载站](https://downloads.immortalwrt.org) 找到你的机型，看它在 `releases/<版本>/targets/<TARGET>/<SUBTARGET>/` 的哪一层，把这两段填进去即可。`GCC` 版本看该目录下 `immortalwrt-sdk-*.tar.zst` 文件名里的 `gcc-xx.x.x`。

## 常见问题

| 现象 | 原因 / 处理 |
|------|-------------|
| 「最近错误」提示 `... is required` | 对应项未填写（`username` / `password` / `ip` / `mac`），补齐后点「保存并应用」即可 |
| `login failed: wrong credentials` | 账号或密码错误，或密码过长 |
| `kicked: other device logged in` | 该账号已在其它设备登录 |
| 安装后始终无法在线 | 确认接口为接入校园网的接口，且 IP / MAC 与校园网分配的静态信息一致 |

## 说明

- **包名/显示名是小驼峰 `jluNetworkLogin`**；但 UCI config 名、init.d 脚本名、ubus 对象名、守护进程的 syslog 标识保持 kebab-case `jlu-network-login`（OpenWrt 系统机制约定）。
- **中英双语界面**：LuCI 文案用英文 msgid，中文由 `luci-i18n-jluNetworkLogin-zh-cn` 翻译包提供，不装则是英文。
- **LuCI 版本**：JS 框架需 LuCI 23.05+，ImmortalWrt 23.05 / 24.10 / 25.x 均支持。

## 免责声明

本项目仅用于学习研究与校园网接入自动化，请遵守学校网络使用规范。
