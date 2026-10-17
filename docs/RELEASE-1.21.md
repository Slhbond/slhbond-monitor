# Slhbond-Monitor v1.21

两件事：**自动检查收敛到每天 0 点一次**，以及**新增浅色主题**。

---

## 新增：深色 / 浅色主题切换

面板右上角（刷新、复制按钮右边）多了一个太阳 / 月亮图标：

- 深色下显示太阳，点一下切到**浅色**
- 浅色下显示月亮，点一下切回深色
- 选择**保存在浏览器本地**，下次打开还是这套配色
- 首屏绘制前就已套用主题，**不会先闪一下黑底再变白**
- 有 `prefers-reduced-motion` 时自动关掉切换动画

两套配色都单独调过，不是简单反色：

- 整个样式表里 40 处「淡白色叠加」和 12 处阴影改成引用 `--overlay` / `--shade` 基色，
  浅色主题下基色反过来（深色低透明度），层次感一致
- 进度条 / 环形图 / 频谱柱的**空槽单独用 `--track` 变量**：白底上 6% 的叠加太淡，
  浅色主题用了 11%，保证看得见
- 蓝底上的浅蓝文字、绿/黄/红的状态文字都换成了在浅色背景下有对比度的深色变体
- 卡片阴影在浅色主题下减弱，避免整页发灰

## 变更：自动检查只在每天 0 点

之前是「每 30 分钟一次 + 每天 0 点一次，取更早的那个」。现在**默认只在每天 0 点查一次**，
不再反复轮询上游。

```ini
update_check = true          # 总开关
update_daily_hour = 0        # 每天 00:00 查一次
update_interval = 0          # 0 = 不按间隔轮询（默认）
```

- 想立刻知道有没有新版，面板上的「检查版本」按钮随时可点
- 需要更及时（例如局域网里几十台设备）就把 `update_interval` 设成 `1800`
  打开间隔轮询，两种检查取更早的那个
- `update_daily_hour = -1` 且 `update_interval = 0` 时，定时检查全部关闭，
  面板上那行会显示「仅手动检查」

面板上的说明也跟着变了：`上次 12:01 · 下次 12.0 小时后` + `每天 00:00 自动检查`，
一眼能看出下次什么时候查。

## 默认清单地址

全新安装的默认清单地址指向本项目的「最新 Release」：

```ini
update_manifest = https://github.com/Slhbond/slhbond-monitor/releases/latest/download/update.json
```

装完不用改任何配置就能收到后续更新。想换成自己的服务器、或先自己审一遍再更新，改这一行即可。

## 升级方式

已经装了 v1.20 的设备，什么都不用做：

- 面板上点「检查版本」→ 出现蓝色「更新可用」→ 点它
- 或者在 v1.20 里 0 点自动检查过后，按钮自己会出现

也可以手动升级：

```bash
tar xzf slhbond-monitor-1.21.tar.gz
cd slhbond-monitor-1.21
sudo ./deploy.sh install
```

配置文件会被保留，不会覆盖你改过的阈值和清单地址。

> ⚠️ **v1.20 的默认清单地址指向本地占位文件**，所以装 v1.20 的设备需要手动改一行才能收到更新：
>
> ```bash
> sed -i 's|^update_manifest.*|update_manifest = https://github.com/Slhbond/slhbond-monitor/releases/latest/download/update.json|' /etc/slhbond-monitor.conf
> systemctl restart slhbond-monitor
> ```
>
> 从 v1.21 起不再需要这一步。

## 验证

- 真机接口与逻辑验收 **157 项全通过**
- 主题切换实测：背景色 `rgb(13,15,19)` → `rgb(238,242,247)`，文字同步反转，
  刷新后仍是浅色，图标随主题切换
- `make check`（`-Werror` + 严格转换告警）**零告警**
- ASan + UBSan 零报告
