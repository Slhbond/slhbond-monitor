# 检查版本与自动更新：使用说明

面板左上角版本号下面有三个东西，搞清楚它们就够用了：

```
v1.20                          ← 当前运行的版本
[ ⟳ 检查版本 ]                 ← 手动查一次有没有新版
上次 10:32 · 下次 30 分钟后
每天 00:00 自动检查
[ 更新可用 ] v1.21             ← 只有发现新版时才出现
```

---

## 一、手动检查：「检查版本」按钮

### 点一下会发生什么

1. 按钮变成 **`检查中…`**，图标开始旋转，按钮临时禁用（防重复点击）
2. 浏览器向本机服务发 `POST /api/v1/update/check`
3. 服务去读取配置里的版本清单（GitHub 上的 `update.json`）
4. 约 1～10 秒后出结果，右下角弹出提示：

| 提示内容 | 含义 | 下一步 |
| --- | --- | --- |
| **已是最新版本 v1.20** | 清单里的版本 ≤ 当前版本 | 什么都不用做 |
| **发现新版本 v1.21，点下面的「更新可用」即可自动更新** | 有新版本 | 点蓝色「更新可用」按钮 |
| **检查失败：…** | 网络不通 / 地址错 / 仓库不是公开的 | 见第六节排错 |

5. 同时按钮下面那行会更新成 `上次 10:33 · 下次 30 分钟后 · 每天 00:00 自动`

### 这行字是什么意思

```
上次 10:33 · 下次 30 分钟后      ← 上次检查成功的时刻 + 距下次自动检查多久
每天 00:00 自动检查              ← 每日固定检查的时间（可配置）
```

「下次」取的是**两种自动检查里更早的那个**（每 30 分钟的间隔检查，或每日检查）。

如果上一次检查**失败**了，这两行会变成：

```
上次 10:37 失败
无法读取 /opt/slhbond-monitor/up…
```

—— 失败原因直接写在面板上，不用去翻日志。**失败不写进告警记录**，
也不影响监控本身，只是那一刻没查到版本信息。再点一次「检查版本」即可重试。

---

## 二、自动检查：每天 0 点

### 默认行为

装好之后**默认就是开着的**，不用做任何配置：

```ini
update_check = true          # 总开关
update_daily_hour = 0        # 每天 00:00 查一次
update_interval = 0          # 不按间隔轮询（0 = 关闭）
```

**默认每天只在 0 点查一次**，不会反复去问上游：

```
00:00  查一次
（然后一整天不再自动查）
次日 00:00  再查一次
```

想立刻知道有没有新版，面板上随时可以点「检查版本」手动查。

### 也可以改成定时轮询

如果需要更及时（比如局域网里几十台设备，希望发布后很快都看到），
把间隔打开即可：

```ini
update_interval = 1800       # 每 30 分钟也查一次
```

两种检查**同时生效时取更早的那个**：

```
22:00  查（间隔）
22:30  查（间隔）
23:00  查（间隔）
23:30  查（间隔）
00:00  ← 每日检查在这里触发，不会被 00:30 顶掉
00:30  查（间隔）
```

### 改时间

```bash
sudo nano /etc/slhbond-monitor.conf
```

```ini
update_daily_hour = 3      # 改成每天凌晨 3 点
```

| 值 | 含义 |
| --- | --- |
| `0` | 每天 00:00（默认，就是"每天晚上 24 点"） |
| `1`～`23` | 每天该点整 |
| `-1` | 关闭每日检查（若 `update_interval` 也是 0，则只能手动查） |

改完重启：

```bash
sudo systemctl restart slhbond-monitor
```

确认排程生效（日志会直接告诉你要等多久）：

```bash
journalctl -u slhbond-monitor -n 20 | grep 'next daily check'
# update: next daily check in 1598 s (at 11:00 local)
```

### 时区说明

用的是**设备本地时间**，改时区后自动跟着变：

```bash
timedatectl set-timezone Asia/Shanghai
```

> ⚠️ 如果设备时钟不准（没有 RTC 的板子断电后会漂），0 点检查可能落在错误的时刻。
> 建议确认 `timedatectl` 显示 `System clock synchronized: yes`。

---

## 三、更新：点「更新可用」之后全自动

发现新版本时，蓝色按钮出现。**点它，然后什么都不用做。**

### 你会看到的过程

| 阶段 | 按钮显示 | 实际发生了什么 |
| --- | --- | --- |
| 下载 | `下载中 37%` | 服务从清单里的地址下载发布包 |
| 校验 | `下载中…` | 算 SHA-256 与清单比对，**不符立刻放弃** |
| 解包 | `下载中…` | 解压到状态目录，检查是不是完整的源码树 |
| 就绪 | **`等待应用`**（变灰） | 弹出「v1.21 已下载并校验完成，系统会在 5 分钟内自动应用并重启服务」 |
| 安装 | 页面短暂断开 | root 侧的定时器开始编译、安装、重启服务 |
| 完成 | 版本号变成 `v1.21` | 蓝色按钮消失 |

**大概要等多久**：下载 + 校验通常几秒；等 root 定时器最多 5 分钟；
编译安装在本机上约 30～60 秒。**总共 1～6 分钟。**

期间你可以继续看监控面板，只有最后重启时会出现几秒的「连接中…」。

### 为什么要点一下才更新

服务可以被局域网里任何人访问，如果发现新版就自动装，等于把「谁能改这台机器」
交给了更新服务器。所以设计成：**自动检查（不改变任何东西）+ 手动确认（才动手）**。

如果你想连这一步也省掉，见第五节。

### 想立刻应用，不等 5 分钟

```bash
sudo systemctl start slhbond-monitor-update.service
tail -f /var/log/slhbond-monitor/update.log
```

### 看更新日志

```bash
# 服务侧：下载、校验过程
journalctl -u slhbond-monitor -n 50 | grep -i update

# root 侧：安装过程（编译输出也在这里）
tail -50 /var/log/slhbond-monitor/update.log
```

---

## 四、状态速查表

| 按钮 | 含义 | 你能做什么 |
| --- | --- | --- |
| `检查版本` | 随时可查 | 点它 |
| `更新可用 v1.21` | 有新版本 | 点它开始更新 |
| `下载中 40%` | 正在下载 | 等 |
| `等待应用`（灰色） | 已就绪，等 root 定时器 | 等，或手动 `systemctl start slhbond-monitor-update.service` |
| `更新失败` | 下载/校验/解包出错 | 看 `/api/v1/update` 的 `error` 字段，或第六节 |
| 按钮消失 | 已是最新版 | — |

用命令行查当前状态：

```bash
curl -s http://127.0.0.1:8090/api/v1/update | python3 -m json.tool
```

```json
{
  "can_apply": true,          // 有新版本且清单里有下载地址
  "staged": false,            // 是否已下载完成等待应用
  "phase": "idle",            // idle / downloading / verifying / extracting / staged / failed
  "percent": -1,              // 下载进度，-1 表示未知
  "version": "1.21",
  "download_ready": true,     // 设备是否装了 curl 或 wget
  "error": null
}
```

---

## 五、进阶

### 完全无人值守（发现即自动装）

现在的设计要点一次确认。如果这台机器是无人值守的，希望发现新版就自动装，
可以把检查改成轮询 + 在服务侧直接触发：

```bash
# 一个 5 分钟跑一次的小定时器，发现新版就提交下载
sudo tee /etc/systemd/system/slhbond-autoupdate.service >/dev/null <<'EOF'
[Unit]
Description=自动提交 Slhbond-Monitor 更新
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'curl -s -X POST http://127.0.0.1:8090/api/v1/update/check >/dev/null; curl -s -X POST http://127.0.0.1:8090/api/v1/update/apply >/dev/null || true'
EOF
```

> 这样 root 侧仍然会做 SHA-256 校验，安全性不变；
> 失去的只是"人看一眼再决定"。要不要这么做自己想清楚。

### 关掉自动检查

```ini
update_check = false
```

面板上「检查版本」按钮仍然可用，但不会再自动查。

### 只保留每日检查（这就是默认）

```ini
update_interval = 0          # 0 = 不按间隔轮询
update_daily_hour = 0        # 每天 0 点查一次
```

### 完全关掉定时检查，只手动查

```ini
update_daily_hour = -1
update_interval = 0
```

这样只有面板上的「检查版本」按钮会去读清单，面板上那行会显示「仅手动检查」。

### 主题切换（浅色 / 深色）

面板右上角（刷新、复制按钮右边）有一个太阳/月亮图标，点一下就能在
**深色和浅色主题**之间切换：

- 深色主题下显示太阳 → 点它切到浅色
- 浅色主题下显示月亮 → 点它切回深色
- **选择保存在浏览器本地**（`localStorage`），下次打开还是这套配色
- 首屏绘制前就已套用主题，不会先闪一下黑底再变白

不同设备、不同浏览器各自独立，互不影响；服务端不保存这个偏好。

### 用命令行手动检查（不用浏览器）

```bash
curl -s -X POST http://127.0.0.1:8090/api/v1/update/check | python3 -m json.tool
curl -s http://127.0.0.1:8090/api/v1/version | python3 -m json.tool
```

---

## 六、排错

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| 点了提示 **检查失败：无法读取 …update.json** | `update_manifest` 指向了一个不存在的本地文件 | 按 `docs/GITHUB-SETUP.md` 配成 GitHub 地址 |
| 提示 **检查失败：HTTP 404** | 清单地址错 / 仓库不是公开的 / Release 没勾 latest | 浏览器里直接打开那个地址看 |
| 提示 **检查失败：https 需要 curl 或 wget** | 设备没装下载器 | `apt install -y curl` |
| 一直 **已是最新版本**，但明明发了新版 | 清单里的 `version` 没大于当前版本 | 检查 `curl /api/v1/version` 的 `current` 和 `latest` |
| 按钮一直不出现 | 同上；或 `update_check = false` | 同上 |
| 点更新后 **一直显示 等待应用** 超过 10 分钟 | root 定时器没跑 | `systemctl status slhbond-monitor-update.timer`，应为 `active` |
| 定时器 **`Refusing to start`** | 单元文件里 `Unit=` 指向不存在的服务 | 重新 `sudo ./deploy.sh install` |
| 提示 **更新失败：校验失败** | 清单里的 sha256 与实际包不符 | 用 `deploy/make-manifest.sh` 重新生成清单 |
| 提示 **更新失败：解包内容不像…发布包** | 传错文件了 | 确认是 `make dist` 产出、带顶层目录的 tar.gz |
| 0 点检查没生效 | 设备时钟不对 / 时区错 | `timedatectl` 确认 `synchronized: yes` |

**一条命令看全部状态：**

```bash
systemctl is-active slhbond-monitor slhbond-monitor-update.timer; \
curl -s http://127.0.0.1:8090/api/v1/version | python3 -m json.tool; \
curl -s http://127.0.0.1:8090/api/v1/update  | python3 -m json.tool
```

---

## 七、时序图：一天里都发生了什么

```
00:00  ──── 每日检查触发（无人工干预）
          └─ 读清单 → 有新版就把面板上的按钮点亮

00:30  ──── 按间隔再查一次（覆盖每日检查的间隔）

...      （每 30 分钟一次，只读清单，不下载、不安装、不改任何东西）

任意时刻 ── 用户点「检查版本」→ 立刻查一次

任意时刻 ── 用户点「更新可用」→ 下载 → 校验 → 解包 → 暂存
                                      ↓
           root 定时器（每 5 分钟）发现暂存 → 编译 → 安装 → 重启 → 清理
```
