# Slhbond-Monitor

纯 C 写的轻量级 Linux 系统监控。单个可执行文件、只依赖 **libc + pthread**，
自带仪表盘网页（**深色 / 浅色两套主题可一键切换**）：系统信息、CPU 总览、GPU、
网络波形、NPU、以及每块硬盘一个方块的存储设备区。

**不绑定任何板子或发行版** —— 同一份源码在 x86_64 服务器、树莓派、
RK3568 这类 ARM 板子上都能原生编译运行，各自识别出自己有的硬件。

![仪表盘（深色）](docs/screenshot.png)

![仪表盘（浅色）](docs/screenshot-light.png)

---

## 目录

- [特性](#特性)
- [资源占用](#资源占用)
- [快速开始](#快速开始)
- [部署脚本](#部署脚本)
- [支持平台与识别范围](#支持平台与识别范围)
- [面板说明](#面板说明)
- [HTTP 接口](#http-接口)
- [配置](#配置)
- [接入 GitHub 自动更新](#接入-github-自动更新)
- [代码地图](#代码地图)
- [安全说明](#安全说明)
- [排错](#排错)
- [设计取舍](#设计取舍)

---

## 特性

- **零第三方依赖** —— 不链接 OpenSSL、不引入任何 JSON 库，`make` 即可。
  整个二进制约 490 KB。
- **跨架构跨发行版** —— 只读标准的 `/proc` 与 `/sys`，不依赖厂商 SDK、
  不做板子型号白名单。ARM 与 x86 都是原生编译。
- **模块化源码** —— 每个 `.c` 只负责一件事，接口写在同名 `.h` 里，
  见[代码地图](#代码地图)。
- **前端不编译** —— HTML/CSS/JS 放在 `web/`，改完刷新浏览器即可。
- **实时** —— 前端定时轮询，CPU 逐核负载、网络吞吐、磁盘读写都带历史序列，
  波形能看出突发。
- **超限自动记录** —— 阈值可调，指标连续超标就自动记下时间、硬件与峰值，
  面板上直接显示每个指标距离记录还差多久。
- **每秒溯源** —— 每秒硬件快照存本地，可拖时间轴回看任意一刻，一天约 3.6 MB。
- **命令留痕** —— 记录用户执行的命令，删除类操作单独标红。
- **深色 / 浅色主题** —— 右上角一键切换，选择记在浏览器本地；
  两套配色都调过对比度，不是简单反色。
- **开机自启** —— 部署脚本自动适配 systemd / OpenRC / SysV / cron。
- **不编造数据** —— 拿不到的指标返回 `null` 并说明原因（GPU 负载计数器、
  未加载的 NPU 驱动、读不到的温度），而不是给一个恒定的假值。

---

## 资源占用

在 RK3568（4× Cortex-A55，Armbian 26.8.3，内核 6.18.37）上实测，
**v1.20 重新测量**（加入了命令记录与事件轮询之后）：

| 场景 | CPU（单核口径） | 占整机 4 核 | 常驻内存 |
| --- | --- | --- | --- |
| 空闲（无任何客户端） | 3.9% | **0.98%** | 3.1 MB |
| 12 客户端并发压测（持续 30 秒） | 4.2% | **1.05%** | 3.5 MB |
| 内存峰值（VmHWM） | — | — | 3.5 MB |
| 触发一次硬盘健康检测 | 10 毫秒 | — | 不变 |

> **和 v1.10 相比变了，原因说清楚：** 之前记录的是 0.27%（整机口径）／2.7 MB。
> 现在整机约 1%／3.1 MB。增加的部分**不是**来自命令记录的扫描频率 ——
> 做过对照实验，把 `/proc` 扫描从每秒 4 次降到 1 次，
> CPU 从 3.93% 变成 4.03%（噪声级差异），说明扫描几乎是免费的。
>
> 逐线程测量显示开销**全部集中在唯一的采样线程**上：它每秒读取约 145 次
> `/proc` 与 sysfs 文件（CPU 频率、热区、GPU、网络、磁盘等），
> 在这块板子上单次 sysfs 读取偏慢。命令记录、事件轮询、告警判定加起来
> 只占该线程开销的一小部分。要压更低就调大 `poll_ms`。

- **内存不随负载增长。** 空闲 3.1 MB、并发压测后 3.5 MB，峰值 3.5 MB。
  并发请求靠 `cache_ms`（默认 900 毫秒）复用同一份采集快照，
  不会让每个连接各读一遍 `/proc`。
- **线程数固定 9 个**，长时间运行不增长。
- **硬盘健康检测几乎不占本进程 CPU**：`smartctl` 是独立子进程，
  父进程只负责等待与解析，实测 10 毫秒。
- **体积**：可执行文件 744 KB（761,832 字节），安装目录 9.2 MB
  （含文档截图与系统服务脚本），当前状态目录 260 KB。
- **每秒历史额外占约 3.6 MB 磁盘**（24 小时、4 核；`history.bin` 是预分配的，
  所以一开始 `du` 看着才 260 KB），这是唯一随运行时间增长的磁盘开销。
- **命令记录有上限**：内存里最多 500 条（约 0.4 MB），
  `commands.log` 走追加写、涨到约 1 MB 时压缩一次，
  不会因为盘上有几万个进程就把日志撑爆。

---

## 快速开始

```bash
# 解压后进入目录
tar xzf slhbond-monitor-1.20.tar.gz
cd slhbond-monitor-1.20

# 一键部署（会显示菜单）
sudo ./deploy.sh
```

菜单只有三项：

```
    1) 部署项目
    2) 卸载项目
    3) 退出
```

选 `1` 之后脚本会自动完成：检查并安装编译依赖 → 本机原生编译 →
安装到 `/opt/slhbond-monitor` → 写配置 → 注册开机自启 → 启动并自检。

完成后终端会直接给出访问地址：

```
  访问地址  http://192.168.5.100:8090/
```

也可以用非交互方式（适合放进自动化脚本）：

```bash
sudo ./deploy.sh install      # 直接部署
sudo ./deploy.sh uninstall    # 卸载（会问是否保留配置）
sudo ./deploy.sh purge        # 卸载并删除全部配置与数据
sudo ./deploy.sh status       # 查看运行状态 + 接口自检
```

---

## 部署脚本

`deploy.sh` 是唯一的安装入口，几个要点：

**自动安装编译依赖。** 识别 apt / dnf / yum / pacman / zypper / apk，
缺少 gcc 或 make 时自动装（设置 `SLH_NO_DEPS=1` 可关闭）。
同时会尽量装上 `smartmontools`，硬盘健康检测需要它。

**自动适配开机自启。** 按 `systemd → OpenRC → SysV init → cron @reboot`
的顺序探测，用哪个就注册哪个，都不支持时会明确告诉你"能跑但重启后不会自动运行"。

> 已实测：`systemctl reboot` 后服务由开机流程自动拉起（boot_id 变更、
> uptime 归零、MainPID 为全新进程），10 个接口全部恢复，硬盘健康结论
> 也从持久化状态正确还原，启动日志无告警。

**卸载是幂等的。** 重复执行不会报错，也不会留下残渣。

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `SLH_PREFIX` | `/opt/slhbond-monitor` | 安装目录 |
| `SLH_PORT` | `8090` | 监听端口 |
| `SLH_CONF` | `/etc/slhbond-monitor.conf` | 配置文件路径 |
| `SLH_NO_DEPS` | `0` | 设为 `1` 时不自动安装依赖 |

例如换个端口和目录：

```bash
sudo SLH_PORT=9100 SLH_PREFIX=/usr/local/slhbond ./deploy.sh install
```

安装后的目录结构：

```
/opt/slhbond-monitor/
├── bin/slhbond-monitor      可执行文件
├── web/                     前端（index.html / style.css / app.js）
├── docs/                    截图
├── README.md
└── VERSION
/etc/slhbond-monitor.conf    配置
/var/lib/slhbond-monitor     状态（首次自动检测过的硬盘名单与结论）
```

> **关于状态目录**：用 systemd 时，单元里的 `StateDirectory=` 会把它建成
> 指向 `/var/lib/private/slhbond-monitor` 的符号链接（`DynamicUser` 的标准行为）。
> 部署脚本刻意**不**预先创建这个目录 —— 提前用 root 建一个普通目录会让 systemd
> 以 `EXIT_STATE_DIRECTORY(238)` 拒绝启动。

---

## 支持平台与识别范围

所有数据都来自标准的 `/proc` 与 `/sys` 接口，因此不挑发行版。
下表是各项信息在两类架构上的实际来源：

| 信息 | ARM (aarch64 / armv7) | x86_64 | 缺失时 |
| --- | --- | --- | --- |
| 平台 / 架构 | `uname` | `uname` | — |
| 系统版本 | `/etc/os-release` | `/etc/os-release` | 回退到 `uname -r` |
| 主机名 / 运行时长 | `/proc/sys/kernel/hostname`、`/proc/uptime` | 同左 | 显示 `—` |
| IPv4 / IPv6 | `getifaddrs(3)` | 同左 | 显示「未获取」 |
| CPU 型号 | `/proc/cpuinfo` + `/proc/device-tree` | `/proc/cpuinfo` 的 `model name` | 显示 `—` |
| SoC | `/proc/device-tree/compatible` | 无（x86 本就没有） | 留空，不报错 |
| 核心 / 线程 | `/sys/devices/system/cpu/cpu*/topology` | 同左（能正确区分超线程） | 回退到 `/proc/cpuinfo` |
| 逐核负载 | `/proc/stat` 增量 | 同左 | — |
| 频率 | `cpufreq` sysfs | 同左 | 留空 |
| 温度 | `/sys/class/thermal` | 同左（`x86_pkg_temp` 等） | 留空并说明 |
| GPU | `/sys/class/devfreq` + device-tree | 无 devfreq 时回退到 `/dev/dri` | `present: false` |
| NPU | `rknpu` debugfs / devfreq / device-tree 三路探测 | 通常不存在 | 明确显示「未检测到」 |
| 网络吞吐 | `/proc/net/dev` 增量 + 历史环形缓冲 | 同左 | — |
| 硬盘 | `/sys/block` + `/proc/mounts` | 同左 | — |
| 硬盘健康 | `smartctl` 多路径查找 | 同左 | 回退到文件系统检查 |

**已经处理过的跨平台细节**（都是实际踩到的）：

- `/proc/device-tree` 只有 ARM 有，所有读取都有失败兜底，x86 上只是留空。
- `smartctl` 的位置随发行版不同：`/usr/sbin`（Debian、RHEL）、`/usr/bin`
  （Arch、Alpine）、`/sbin`（老系统）、`/usr/local/*`（源码编译）。
  全部会尝试，最后还会搜 `$PATH`。
- 热区名字各厂商不同，匹配 `cpu` / `soc` / `core` / `cluster` / `package`
  等关键词，都不匹配时退回到最热的那个区。
- 设备过滤覆盖 `loop` / `ram` / `zram` / `dm-` / `sr` / `md` / `fd`
  以及 eMMC 的 `boot0` / `boot1` / `rpmb` 硬件分区。
- NVMe、virtio 这类设备没有 `/sys/block/*/device/vendor`，属性读不到就留空，
  之后由 `smartctl` 回填（序列号、固件版本）。
- `getifaddrs(3)` 依赖 netlink，systemd 单元里必须放行 `AF_NETLINK`，
  否则 IPv4/IPv6 会全部变成「未获取」。
- `/proc/net/dev` 每行是 **16** 个计数器（接收 8 个 + 发送 8 个），
  只解析前 8 个会把发送字段读成垃圾。

> **已在两种架构上实测**：aarch64（RK3568 真机，Armbian 26.8.3）与
> x86_64（Ubuntu 24.04）。两边都零告警编译通过、接口全部正常、缺失的
> sysfs 项优雅留空。
> 注意在 WSL1 里测不出真实的硬盘、温度和频率 —— WSL 本身不暴露这些接口，
> 这是测试环境限制，不是程序问题。

---

## 面板说明

### 系统信息

左栏是 Logo 与项目名，下面绿色在线指示灯、版本号；检测到新版本时出现蓝色
「更新可用」按钮。右栏顶栏依次为平台类型、系统版本、主机名、运行时长、
IPv4、IPv6；未取到 IPv6 时显示「未获取」。卡片右上角是刷新与复制按钮。

**第一行右侧是两个正方形方块，上下排布**，系统信息卡片为它们让出了一条宽度：

- **警告**（上）：负载超限记录条数。有指标正在超限时数字与边框变红；
  有历史记录但当前正常时变琥珀色；平时是绿色。点开可以查看每条超限记录，
  并**直接修改自动记录的阈值**。
- **溯源记录**（下）：已保存的每秒快照条数，副标题显示覆盖时长与磁盘占用。
  点开进入**溯源界面**。

### 警告：负载超限自动记录

面板上可以设置七个值（六个指标 + 一个持续时长）：

| 指标 | 默认阈值 | 说明 |
| --- | --- | --- |
| CPU 使用率 | 90% | 按**最忙的单个线程**判断，不是平均值 |
| 内存使用率 | 90% | |
| CPU 温度 | 80 °C | |
| GPU 活跃度 | 90% | panfrost 不报利用率，这里比的是上电占比 |
| NPU 负载 | 90% | 驱动没加载时不参与判断 |
| 硬盘温度 | 55 °C | 所有硬盘里**最热的那块** |
| 持续时长 | 10 秒 | 连续超过阈值这么久才记录一条 |

每条记录包含 **哪一天几点几秒、哪个指标、峰值多少、阈值多少、持续了多久**。

**测不到的指标会如实标出来。** 这台机器的 RKNPU 驱动没加载，面板里 NPU
那一行会标注「测不到，暂不参与判断」并置灰，而不是永远显示 0% 让人以为正常。
阈值仍然可以设置 —— 换内核或插上硬盘后立刻就用得上。

指标表由服务端下发（`/api/v1/alerts` 的 `metrics` 字段），前端照着渲染，
所以以后加指标不用改前端。

两个细节是为了让它真的可用：

- **需要持续达标才记录。** 瞬时尖峰不记，否则一次编译就能刷出几十条。
- **回落到阈值以下还要留出余量才算结束**（默认 5% 或 2 个单位）。
  没有这个滞回，一个正好压在阈值上的值会反复开关记录。

**「为什么这次没记录？」面板上直接给答案。** 每个指标输入框下面都有一行实时状态：

```
CPU 使用率 %  = 90     当前 5.9%
CPU 温度 °C   = 80     当前 43.1°C
NPU 负载 %    = 90     未检测到
```

某个指标一旦超过阈值，这行就变成 `已超限 8 / 10 秒`，达到时长后变成
`已记录中 12 秒`。不用再猜是没触发还是漏记了。

判定用的是**真实经过的秒数，不是采样条数** —— 采样重复或漏采都不会让
"持续 10 秒"变成别的东西；两次采样间隔超过 5 秒时按中断处理，
不会把没测到的空档算成持续超限。

另外两点：**峰值边涨边落盘**（最多每 5 秒写一次），所以服务中途重启也不会把峰值记小；
**正在进行的超限在服务停止时会被记为「服务停止而中断」**，而不是无声消失。

阈值改动会写到状态目录下的 `alerts.conf`，重启后仍然生效，并且**优先于**
配置文件里的值。记录落在 `alerts.log`，最多保留 200 条。

### 溯源：按时间轴回看历史

**每一秒**把硬件状态压成一条定长二进制记录写到本地磁盘。点「溯源记录」方块
进入回看界面：**布局与首页完全一样**，只是所有卡片显示的是时间轴所指那一刻的数据。

拖动底部时间轴（或按 ▶ 自动播放）时，卡片里的 CPU 使用率、逐核负载、温度、
频率、内存、网络速率与波形、GPU 数据都会**跟着变**，运行时长也按那一刻重新推算。
按 `Esc` 或「← 实时」返回。

**可以直接跳到指定的日期与时间。** 点时间轴上的**时间戳**会弹出一个日期/时间
选择器（精确到秒），选好即跳转；旁边还有「最早 / −1 时 / −1 分 / +1 分 /
+1 时 / 最新」六个快捷按钮。如果选的时间超出了已保存的范围，会自动跳到最近的
一条并说明原因。

也可以只用键盘：溯源模式下按 `←` `→` 逐秒微调。

> **哪些是历史值，哪些不是。** 记录里没有的东西不会假装有：存储设备区
> （SMART 结论、逐盘读写）、网卡 MAC、累计收发量不参与历史，界面里已淡出或
> 沿用当前值，顶部提示条会写明。网络波形是从历史记录**重新画出来**的，
> 不是把当前曲线摆在那里。

**占多少磁盘**：每条记录 40 字节 + 每核 1 字节。4 核机器上：

| 保留时长 | 磁盘占用 |
| --- | --- |
| 1 小时 | 约 158 KB |
| 24 小时（默认） | **约 3.6 MB** |
| 7 天 | 约 25 MB |

三处设计都是为了少写闪存：

- 记录是**定长二进制**而不是 JSON（一条 JSON 快照要几 KB，二进制只要 44 字节）；
- 环形文件**一次性预分配**，永不增长、不产生碎片；
- 采样先攒在内存里，**每 60 秒才写一次盘**，而不是每秒写一次。

代价是断电最多丢失最近 60 秒的样本 —— 为了不把 eMMC 写坏而做的取舍，
可以用 `history_flush` 调整。

### 开机 / 关机 / 登录记录

点溯源时间轴上的「事件」按钮查看。**这里的「开机」「关机」指的是整机**，
不是监控服务自己的启停 —— 这个区别很重要，否则「什么时候断过电」根本查不出来。

判据是 `/proc/sys/kernel/random/boot_id`：**内核每次启动都会换这个值**。

| 记录 | 何时产生 | 时间来源 |
| --- | --- | --- |
| **开机** | `boot_id` 与上次看到的不同 —— 整机确实重启过 | `/proc/uptime` 反推，是真实的内核启动时刻 |
| **关机** | 下次启动时发现 `boot_id` 变了，才回头确认上一次整机真的关了 | 系统关闭前服务收到的退出信号 |
| **断电关机** | 同上，但关机时**没有**留下任何退出记录 | 关机时刻无法确定，记录里如实标注，不编造 |
| **登录 / 登出** | 读取 `/var/log/wtmp`，含用户名、来源 IP、终端号 | 每 10 秒补齐一次，运行期间的登录立刻可见 |

**监控服务自己的启停单独记录**（服务启动 / 服务停止），默认**不显示**，
勾选面板上的「显示服务启停」才出现。这样一次部署重启不会被误认成整机关机。

面板顶部还会直接写明本次启动的判定：

- `本次启动检测到整机重启过（内核 boot_id 变了）。`
- `本次启动与上次是同一次开机，只是监控服务被重启 —— 不算开关机。`

最多保留 300 条，按时间倒序显示。

### 命令执行记录（含删除）

同一个窗口的第二块列出用户执行的命令，**删除类操作用红色单独标出**，
标题上有「只看删除」开关，「事件」按钮上会挂一个删除次数的角标。

两个来源互补，都不需要额外权限：

| 来源 | 覆盖 | 局限 |
| --- | --- | --- |
| **进程扫描** | 每秒扫 4 次 `/proc`，记录新进程的完整命令行、用户、PID | **存活不足约 0.25 秒的进程会漏掉** |
| **shell 历史** | 读 `~/.bash_history` / `~/.zsh_history`，从保存的偏移量往后读 | 需要文件对服务可读，命令要等 shell 落盘 |

判定为删除的命令名：`rm`、`rmdir`、`unlink`、`shred`、`truncate`、`srm`，
包括 `sh -c "rm -rf ..."` 这种包装形式；记录里会带上**目标路径**。

> **一个绕不过去的限制：`/root/.bash_history` 是 `0600 root:root`。**
> 服务刻意不以 root 运行，所以读不到它 —— 面板会如实写明，而不是假装历史可用。
> 单元里的 `ProtectHome` 用的是 `read-only` 而不是 `yes`，否则 `/home` 与 `/root`
> 会被整个隐藏，非 root 用户的历史文件也一并看不到。
>
> 如果需要完整记录 root 的交互式命令，可以二选一：接入 `auditd`
> （`auditctl -a always,exit -F arch=b64 -S execve`，内核级、无遗漏），
> 或由管理员显式放开该文件权限。项目本身不会去改用户主目录的权限。

记录落在状态目录下的 `commands.log`，最多保留 500 条。

### CPU 总览

左上角是随负载变色的 CPU 状态图标（空闲绿 → 正常蓝 → 偏高琥珀 → 接近满载红），
右侧为 CPU 型号；上半部分左边是平均使用率环形图、右边是核心 / 线程 /
单线程最高占用 / CPU 最高温度；底部是每个线程的实时负载柱状图。

### GPU

显示 GPU 型号、驱动、频率档位、温度和电源状态。

> **为什么没有负载百分比**：panfrost 这类开源驱动不导出负载计数器。
> 面板改用「频率档位」和「上电占比」（取自 `power/runtime_active_time` 的增量）
> 来表达活跃度，并写明原因，而不是编一个恒定值。

### 网络

显示主接口（按流量自动挑选物理网卡，跳过 lo / veth / docker / br- 等虚拟接口）、
链路速率、累计收发、错误与丢包，以及**接收 / 发送的实时波形**。
历史窗口是 48 个采样点，只存在内存里，进程重启后重新累积。

### NPU

显示加速器的硬件型号、驱动状态、实时负载与频率。驱动没加载时明确说明原因，
不会把「没检测到」和「负载为 0」混为一谈。

### 存储设备

**每识别到一块硬盘就渲染一个正方形卡片**（200×200），包含硬盘 Logo、名称、
总容量、可用容量，Logo 右侧是健康检测按钮，卡片内部下方是读写速率和一条
迷你历史曲线。

装操作系统的那块盘会带一个蓝色的**「系统盘」**徽章 —— 判据是它承载了 `/`、
`/usr`、`/etc`、`/var`、`/boot` 等系统挂载点之一，与"哪块盘容量最大"无关。

**健康等级与 Logo 颜色**一一对应：

| 等级 | 颜色 | 含义 |
| --- | --- | --- |
| `healthy` | 绿色 | 各项指标正常 |
| `warning` | 黄色 | 出现非致命异常（重映射扇区非零、可用空间偏低…） |
| `fault` | 红色 | 明确的故障信号（SMART FAILED、可用空间 < 2%…） |
| `unknown` | 灰色 | 读不到任何健康数据（**不会**当成健康） |

**检测时机**：设备首次被识别时自动执行一次，此后仅在你点击按钮时触发。
已检测名单与检测结果分开持久化 —— 只在检测真正跑完之后才写"已检测"名单，
中途重启会重试，而不是把设备永久留成未检测。

采集按可信度依次尝试，取最严重的一档：

1. **`smartctl`**（ATA / NVMe）—— 通过带硬超时的子进程调用。不少 SBC 的
   SATA 口在 SAT 层后面，smartctl 自己判断不出协议时会打印
   `Probable ATA device behind a SAT layer` 却仍然返回 0，因此会自动用
   `-d sat` / `-d ata` 重试一次。
2. **MMC 控制器错误计数** —— 只反映 CRC / 超时类错误（排线或焊接问题），
   需要 debugfs，见下方说明。**不涉及寿命或磨损指标。**
3. **文件系统兜底** —— 可用空间，以及 ext4 的 `errors_count`。

> **关于"只读挂载"**：早期版本把只读挂载当作故障信号，这是个误报来源。
> 服务跑在 `ProtectSystem=strict` 下时，systemd 会把整个 mount namespace
> 重挂为只读，于是**每块盘都永远显示只读**。现在改用与命名空间无关的
> `errors_count`，只读标志仅作信息展示。

> **关于读写速度为什么有时是 0**：磁盘 I/O 是突发式的，空闲的板子可能每
> 5 秒窗口里都是 0，但实际上每分钟集中写几百 KB。所以卡片内显示的是
> **最近 32 个采样点的窗口均值**并画出曲线，突发会以尖峰形式显现；
> 瞬时值和峰值在悬浮详情里。

---

## HTTP 接口

默认监听 `0.0.0.0:8090`。

| 方法与路径 | 说明 |
| --- | --- |
| `GET /` | 仪表盘页面 |
| `GET /api/v1/status` | 一次性返回全部数据，**前端就是用它轮询的** |
| `GET /api/v1/system` | 主机身份与地址 |
| `GET /api/v1/cpu` | CPU 型号、逐核负载、温度、频率 |
| `GET /api/v1/gpu` | GPU 型号、频率档位、温度 |
| `GET /api/v1/network` | 逐接口计数器与吞吐历史 |
| `GET /api/v1/npu` | NPU 硬件、驱动状态与负载 |
| `GET /api/v1/disks` | 每块硬盘的身份、容量与健康结论 |
| `POST /api/v1/disk/check?device=sda` | **写接口**：立即执行一次硬盘健康检测 |
| `GET /api/v1/history?from=&to=&limit=` | 每秒硬件历史（溯源界面用它） |
| `GET /api/v1/alerts?limit=` | 指标阈值表 + 超限记录 |
| `POST /api/v1/alerts/config?cpu=&mem=&temp=&gpu=&npu=&disk_temp=&hold=` | **写接口**：修改告警阈值 |
| `GET /api/v1/events?limit=&service=1` | 整机开关机 + 登录（`service=1` 附带服务启停） |
| `GET /api/v1/commands?limit=&deletes=1` | 命令执行记录（`deletes=1` 只看删除） |
| `GET /api/v1/metrics` | 负载均值、内存、磁盘 |
| `GET /api/v1/version` | 当前版本与更新可用性 |
| `GET /api/v1/update` | 自更新状态（下载进度 / 是否已就绪） |
| `POST /api/v1/update/check` | 立刻检查一次版本清单 |
| `POST /api/v1/update/apply` | 下载 + 校验 + 暂存（安装由 root 侧完成） |
| `GET /api/v1/health` | 存活探针 |

方法语义：`HEAD` 由 `GET` 处理器代答（响应体自动省略）；路径存在但方法不对
返回 `405` 并带 `Allow` 头；`OPTIONS` 返回 `204` 与 `Allow`。
写接口只有两个：触发硬盘检测、修改告警阈值。

**`/api/v1/status` 只带摘要**（记录条数、当前是否有超限、阈值），不含告警与
事件的完整列表 —— 那个端点每几秒被轮询一次，把几百条记录塞进去纯属浪费。
面板打开对应方块时才去取完整数据。

**历史记录是定长数组而不是对象**：一天有 86400 条，每条都重复一遍字段名会让
响应体积翻好几倍。字段顺序固定，`web/app.js` 里的 `HIST_FIELDS` 与
`src/api.c` 必须一致：

```
[ts, cpu, cpuMax, mem, tempC10, freqMHz, load1x100,
 netRx, netTx, diskR, diskW, gpu, npu, gpuTempC10, gpuFreqMhz, diskTempC10,
 逐核百分比 × ncores]
```

`-1`（底层为 `255` / `0xFFFF`）表示该指标不可用，而不是 0。

`/api/v1/alerts` 里的 `metrics` 数组是面板上那张阈值表的唯一来源，每项包含
`key` / `name` / `unit` / `threshold` / `min` / `max` / `available`。
新增一个告警指标只需要改 C 侧的那张表。

用 curl 看一眼：

```bash
curl -s http://127.0.0.1:8090/api/v1/status | python3 -m json.tool | head -40
curl -s -X POST 'http://127.0.0.1:8090/api/v1/disk/check?device=sda'
```

---

## 配置

`/etc/slhbond-monitor.conf`，格式是每行一条 `键 = 值`。
优先级：内置默认值 < 配置文件 < 命令行参数。改完重启服务生效。

常用的几项：

```ini
bind = 0.0.0.0          # 只想本机访问就改成 127.0.0.1
port = 8090
workers = 4             # 工作线程数
doc_root = /opt/slhbond-monitor/web
state_dir = /var/lib/slhbond-monitor
poll_ms = 5000          # 前端轮询间隔（毫秒），越小越"实时"
cache_ms = 900          # 服务端采集缓存，避免多客户端重复读 /proc
log_level = info        # error | warn | info | debug

# --- 每秒历史（溯源）---
history = true          # 关掉后「溯源记录」方块一直是 0
history_hours = 24      # 保留时长；4 核机器上 24 小时约 3.6 MB
history_interval = 1    # 采样间隔（秒）
history_flush = 60      # 攒多少条写一次盘，越大越省闪存
history_cores = 0       # 每条存几个核，0 = 跟随检测到的核数

# --- 告警阈值（面板写入的会覆盖这里）---
alert_cpu_percent = 90
alert_mem_percent = 90
alert_temp_c = 80        # CPU 温度
alert_gpu_percent = 90   # GPU 活跃度（上电占比）
alert_npu_percent = 90   # NPU 负载
alert_disk_temp_c = 55   # 硬盘温度（取最热的一块）
alert_hold_sec = 10      # 连续超限多久才记录

update_check = true     # 关掉后前端不出现「更新可用」按钮
update_manifest = https://github.com/Slhbond/slhbond-monitor/releases/latest/download/update.json
update_daily_hour = 0   # 每天 0 点自动检查一次（-1 = 关闭）
update_interval = 0     # 0 = 不按间隔轮询；想更及时可设 1800

# 更新加速节点（第三方 GitHub 代理）：下载前逐个测延迟，自动用最快的一个。
# 留空只用直连。实测这台设备直连 github.com 连接要 19～36 秒，这几个只要 1～2 秒。
update_mirrors = https://gh-proxy.com/,https://ghfast.top/,https://ghproxy.net/,https://hk.gh-proxy.com/
update_manifest_via_mirror = true   # 直连拉清单失败时允许退到加速节点
```

重启服务：

```bash
sudo systemctl restart slhbond-monitor
```

---

## 接入 GitHub 自动更新

> **第一次配置？** 有一份逐步到位的教程：
> [`docs/GITHUB-SETUP.md`](docs/GITHUB-SETUP.md) —— 从注册账号、创建个人访问令牌、
> 推送代码、到打 tag 自动发布，每一步都写了具体点哪里、填什么、出错怎么查。
>
> **已经配好，想知道按钮怎么用？** 看
> [`docs/UPDATE-USAGE.md`](docs/UPDATE-USAGE.md) —— 手动检查、每天 0 点自动检查、
> 点一下自动更新的完整说明，含状态速查表与排错。

面板左上角版本号下面有一个「检查版本」按钮，旁边的「更新可用」按钮不是去打开网页，
而是**自己下载、校验、安装并重启**：

- **手动检查**：点「检查版本」，立刻问一次更新服务器。
- **自动检查**：默认**每天 `00:00` 查一次**，不按间隔反复轮询。
  改 `update_daily_hour` 可以换时间，`-1` 关闭。
  想更及时的话把 `update_interval` 设成 `1800`，两者取更早的那个。
- **更新**：发现新版时出现蓝色「更新可用」，点它 → 下载 → 校验 → 暂存 →
  root 定时器在 5 分钟内自动安装并重启。

下面是从零把 GitHub 配成更新服务器的完整步骤。

### 它是怎么工作的

```
①  服务每秒采样 ──无关── 更新是独立线程，只在点击时才动
②  点击「更新可用」
      └─ 下载 → SHA-256 校验 → 解包 → 写 ready 标记（服务无权限，到此为止）
③  slhbond-monitor-update.timer（root，每 5 分钟）
      └─ 发现 ready → 编译 → 安装 → 重启服务 → 删除 ready
```

**为什么分成两半**：服务以 systemd `DynamicUser` 非 root 身份运行，
既写不了 `/opt` 也重启不了 systemd。所以它只能把**已校验的包**放进状态目录，
真正安装由 root 的 oneshot 单元完成 —— 服务本身永远不具备「安装任意东西」的能力。

### 第一步：建仓库

在 GitHub 建一个仓库（公开私有都行，私有的话下载链接需要带 token，不推荐），
把项目源码推上去：

```bash
cd slhbond-monitor-1.20
git init
git add -A
git commit -m "v1.20"
git branch -M main
git remote add origin https://github.com/你的用户名/slhbond-monitor.git
git push -u origin main
```

### 第二步：发布一个版本

在 GitHub 上 **Releases → Draft a new release**，
Tag 写 `v1.21`，把打包好的 `slhbond-monitor-1.21.tar.gz` 作为附件传上去。

本地打包用：

```bash
make dist          # 产出 dist/slhbond-monitor-1.21.tar.gz
```

### 第三步：生成清单，别手抄 sha256

```bash
./deploy/make-manifest.sh 1.21 dist/slhbond-monitor-1.21.tar.gz \
  https://github.com/你的用户名/slhbond-monitor/releases/download/v1.21/slhbond-monitor-1.21.tar.gz \
  "这一版改了什么"
```

输出就是 `update.json`。**摘要由脚本计算** —— 清单里的 `sha256` 与实际包不一致时，
更新会被拒绝，所以这一栏抄错就等于发布了个装不上的版本。

### 第四步：把清单挂到 raw 地址

两种放法，都可以：

| 放法 | 清单地址 |
| --- | --- |
| 提交到仓库根目录（推荐，改完即生效） | `https://raw.githubusercontent.com/你的用户名/slhbond-monitor/main/update.json` |
| 作为 Release 附件上传 | `https://github.com/.../releases/download/v1.21/update.json` |

### 第五步：让设备指向这个地址

编辑 `/etc/slhbond-monitor.conf`：

```ini
update_check = true
update_manifest = https://github.com/你的用户名/slhbond-monitor/releases/latest/download/update.json
update_daily_hour = 0       # 每天 0 点自动查；想更频繁就设 update_interval = 1800
```

```bash
sudo systemctl restart slhbond-monitor
```

**https 需要 `curl` 或 `wget`**：项目刻意不链接 OpenSSL，所以 https 下载是
委托给系统自带的下载器。装一个就行：

```bash
sudo apt install -y curl        # 或 wget
```

没装也能跑，只是 https 下载不可用 —— 面板上会明确提示，不会假装能更新。

### 第六步：验证

```bash
# 清单是否被正确读到
curl -s http://127.0.0.1:8090/api/v1/version | python3 -m json.tool

# 强制立刻检查一次，不用等 30 分钟
curl -s -X POST http://127.0.0.1:8090/api/v1/update/check | python3 -m json.tool
```

`update_available` 变成 `true` 后面板右上角就出现蓝色按钮，点它能看下载进度。

### 更新失败怎么查

```bash
tail -50 /var/log/slhbond-monitor/update.log     # root 侧的完整输出
journalctl -u slhbond-monitor -n 50              # 服务侧的下载/校验过程
systemctl list-timers slhbond-monitor-update.timer
```

失败时**旧版本会继续运行**：`deploy.sh` 先编译后安装，编译不过就不会动现有安装；
解包内容不像一个完整源码树（缺 `deploy.sh`/`Makefile`/`src/`）也会被拒绝。

### 安全上你该知道的

- **每个包都做 SHA-256 校验**，摘要来自清单。传输只依赖系统 CA 的信任链，
  所以摘要这一关是真正的防线：镜像被篡改会被拒绝（这条有专门的测试）。
- 清单里**没写 `sha256` 时只警告不阻止** —— 这是为了让手工搭的测试源能用，
  但生产环境务必填上。
- 服务本身**不能安装任何东西**，它只能往状态目录放一个已校验的包。
- root 侧的更新单元执行的每一步都来自那个已校验的包，不读取任何网络输入。

---

## 代码地图

```
src/
  main.c          启动、参数解析、信号处理、线程编排
  config.[ch]     配置文件与命令行解析
  log.[ch]        分级日志
  util.[ch]       内存 / 字符串 / strbuf / 时间 / 子进程执行
  json.[ch]       极简 JSON 输出
  http.[ch]       HTTP/1.1 解析与响应、keep-alive
  router.[ch]     路由与方法语义（405 + Allow、OPTIONS）
  httpclient.[ch] HTTP GET，https 委托给 curl/wget（仅用于版本清单与更新包）
  sha256.[ch]     SHA-256（更新包完整性校验，不链接 OpenSSL）
  updater.[ch]    下载 / 校验 / 解包 / 暂存，交给 root 侧安装
  static_files.c  静态文件服务（含路径穿越防护）
  api.[ch]        各接口的装配与快照缓存
  sysinfo.[ch]    主机身份、运行时长、内存
  netinfo.[ch]    getifaddrs(3) 枚举 IPv4/IPv6 并挑选主地址
  cpustat.[ch]    CPU 型号、逐核负载、拓扑、频率、温度
  gpu.[ch]        GPU 型号、频率档位、上电占比、温度
  netstat.[ch]    逐接口计数器与吞吐历史（波形数据源）
  disk.[ch]       块设备枚举、健康检测、后台检测线程与结果持久化
  npu.[ch]        NPU 硬件/驱动探测与负载采样
  history.[ch]    每秒硬件快照的紧凑二进制环形存储 + 采样线程
  alerts.[ch]     阈值超限的记录与持久化（带滞回）
  events.[ch]     开机/关机时间与 wtmp 登录记录
  cmdlog.[ch]     命令执行记录（进程扫描 + shell 历史，标记删除）
  version.[ch]    版本比对与更新提示
web/
  index.html      页面结构
  style.css       暗色主题样式（CSS 自定义属性控制健康色）
  app.js          轮询、渲染、内联 SVG 图表
deploy/
  slhbond-monitor.service   systemd 单元模板（安装时替换 @PREFIX@）
  slhbond-monitor.conf      配置模板（安装时替换 @PREFIX@/@PORT@/@STATE_DIR@）
  update.json.example       版本清单示例
deploy.sh                   部署脚本（部署 / 卸载 / 状态）
```

---

## 安全说明

- 服务以 systemd `DynamicUser` 非 root 身份运行，并启用了
  `ProtectSystem=strict`、`ProtectHome=read-only`、`NoNewPrivileges` 等加固项。
  实测进程 `Uid` 是动态分配的高位 UID，`CapEff` 只有 `0x20000` 一位。
- **为读硬盘健康数据开的两道口子**，都是必需的且已压到最小：
  - `SupplementaryGroups=disk` —— 否则打不开 `/dev/sdX`；
  - `CapabilityBoundingSet` / `AmbientCapabilities=CAP_SYS_RAWIO` ——
    SMART 走 SG_IO 直通 ATA 命令，缺了会报 `Operation not permitted`。
  - 因此**不能**开 `PrivateDevices=yes`（它会隐藏块设备，SMART 直接失效）。
- **`ProtectHome` 用 `read-only` 而不是 `yes`**：命令记录要读
  `~/.bash_history` 才能补上进程扫描抓不到的短命令，而 `yes` 会把 `/home`
  与 `/root` 整个隐藏。`read-only` 只是允许读，仍然禁止写入。
  注意这**不会**让服务读到 root 的历史文件 —— 那个文件是 `0600`，属于属主权限，
  与单元加固无关（详见「命令执行记录」一节）。
- 小份持久状态写在 `/var/lib/slhbond-monitor`。注意 `/opt/slhbond-monitor`
  属主是 root，`DynamicUser` 写不进去，所以 `state_dir` 不要指到那里。
- 仪表盘只有一个写接口（触发硬盘检测），也不提供认证。请只监听在内网地址上；
  如无必要不要暴露到公网。需要跨网访问时，建议放在反向代理后面并加上认证。
- 接口只暴露主机名、系统版本、地址与磁盘型号这类基本信息，不涉及文件内容。

---

## 排错

**先看日志。**

```bash
sudo journalctl -u slhbond-monitor -n 50 --no-pager     # systemd
sudo tail -f /var/log/slhbond-monitor/out.log           # OpenRC / SysV
```

**改了日志级别后重启服务。**

```bash
sudo sed -i 's/^log_level = .*/log_level = debug/' /etc/slhbond-monitor.conf
sudo systemctl restart slhbond-monitor
```

**服务起不来，状态是 `activating (auto-restart)`。**

看退出码：

```bash
systemctl status slhbond-monitor --no-pager -n 20
```

- `status=238/STATE_DIRECTORY` —— 状态目录被提前用 root 建成了普通目录。
  systemd 在 `DynamicUser` 下需要把它做成符号链接。修复：

  ```bash
  sudo mv /var/lib/slhbond-monitor /var/lib/slhbond-monitor.bak
  sudo systemctl restart slhbond-monitor
  ```

- `status=203/EXEC` —— 可执行文件不在 `ExecStart` 指的位置，或没有执行权限。
- `status=226/NAMESPACE` —— 加固项与当前内核不兼容，可先把单元里的
  `ProtectKernelTunables` 等逐条注释掉定位。

**端口被占用。**

```bash
sudo ss -ltnp | grep 8090
```

换个端口重装：`sudo SLH_PORT=9100 ./deploy.sh install`，
或直接改 `/etc/slhbond-monitor.conf` 里的 `port` 后重启。

**局域网访问不到，但本机能打开。**

多半是防火墙：

```bash
sudo firewall-cmd --permanent --add-port=8090/tcp && sudo firewall-cmd --reload
# 或
sudo ufw allow 8090/tcp
```

**IPv4 / IPv6 显示「未获取」。**

确认服务能看到网络接口。如果是自己改过 systemd 单元，注意
`RestrictAddressFamilies` 里**必须包含 `AF_NETLINK`** ——
`getifaddrs(3)` 靠 netlink 枚举地址，漏掉它所有地址都会变空。

**温度 / 频率显示为空。**

说明这台机器（或这个容器 / WSL 环境）没有导出对应的 sysfs 节点：

```bash
ls /sys/class/thermal/            # 温度
ls /sys/devices/system/cpu/cpu0/cpufreq/   # 频率
```

虚拟机、容器、WSL 里通常是空的，这是正常现象，面板会如实留空。

**硬盘一直显示「未检测」（灰色）。**

```bash
# 1. 服务能不能看到块设备（需要 SupplementaryGroups=disk 且没有 PrivateDevices）
systemctl show slhbond-monitor -p PrivateDevices -p SupplementaryGroups --value
ls -l /dev/sda

# 2. SMART 是不是被权限挡住（需要 CAP_SYS_RAWIO）
systemctl show slhbond-monitor -p AmbientCapabilities --value
journalctl -u slhbond-monitor | grep -iE 'disk|smart'

# 3. 状态目录是否正常
ls -ld /var/lib/slhbond-monitor
```

**SMART 读不到，日志里是 `Operation not permitted`。**

服务缺少 `CAP_SYS_RAWIO`。确认单元里有这两行并 `daemon-reload`：

```ini
CapabilityBoundingSet=CAP_SYS_RAWIO
AmbientCapabilities=CAP_SYS_RAWIO
```

**SMART 输出 `Probable ATA device behind a SAT layer`。**

部分 SBC 的 SATA 口在 SAT 层后面，smartctl 需要显式设备类型。程序会自动用
`-d sat` / `-d ata` 重试，若仍失败可手动确认：`smartctl -H -d sat /dev/sda`。

**读写速度一直是 0 B/s。**

先判断是"真没 I/O"还是"读错了地方"：

```bash
# 原始计数器有没有在动（间隔 5 秒读两次，看第 3、7 列）
awk '{print $3, $7}' /sys/block/mmcblk1/stat; sleep 5
awk '{print $3, $7}' /sys/block/mmcblk1/stat
```

两次数值一样就说明这 5 秒里确实没有 I/O —— 面板没读错。想看到数字动起来，
注意 `/tmp`、`/run`、`/dev/shm` 多半是 tmpfs，写进去根本不碰磁盘：

```bash
df -hT /tmp                                    # 看 Type 是不是 tmpfs
dd if=/dev/zero of=/opt/t.bin bs=1M count=60 conv=fdatasync
```

**网络波形一直显示「正在采集波形…」。**

波形需要至少两个采样点，而采样点随请求产生。刷新几次页面、或过十几秒就会
画出来。历史窗口只存在内存里，进程重启后重新累积 —— 这是实时视图，
不是历史数据库。

**GPU 卡片没有百分比负载。**

这是如实反映：panfrost 这类驱动不导出负载计数器。卡片改用频率档位与上电
占比表达活跃度。换成带负载计数器的厂商驱动后，`load_percent` 会自动有值。

---

## 设计取舍

- **宁可留空也不编数据。** NPU 驱动不在、温度读不到时，接口返回 `null`
  并附上原因，前端显示占位文案，而不是给一个看起来正常实则恒定的假值。
- **同一个 Linux 接口只解析一次。** `/proc/stat`、`/proc/net/dev`、
  `/proc/mounts` 这些各自只有一个模块负责，多余的行解析函数会直接删掉，
  避免同一个数字在两个地方算出不同结果。
- **探测逻辑只依赖"能读到什么"，不依赖板子型号。** GPU 走 devfreq、
  NPU 走三条独立探针、硬盘走 sysfs + smartctl，因此同一份二进制在不同
  架构上都能给出各自能给出的答案，而不是靠型号白名单。
- **不展示寿命 / 磨损百分比。** 厂商对这类指标的暴露方式差异极大
  （JEDEC 只按 10% 一档上报、各家 SMART 属性编号和归一化方式都不统一），
  跨平台给出一个"百分比"必然是不可靠的。需要时请用 `smartctl -a` 直接查看。
