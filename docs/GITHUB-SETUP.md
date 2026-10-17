# 把 GitHub 配成更新服务器（详细教程）

从零开始，一步一步做完，设备就能自动发现新版本、点一下更新。

---

## 最快的路径（三分钟）

项目根目录带了一个 `publish.ps1`，把命令行部分压缩成一条命令。
**但有三件事必须你先在浏览器里做完**，脚本代替不了：

1. **建仓库**：<https://github.com/new> → 名字 `slhbond-monitor` → 选 **Public**
   → README / .gitignore / license **都不要勾**
2. **建令牌**：<https://github.com/settings/tokens> → **Tokens (classic)** →
   **Generate new token (classic)** → 勾 **`repo`** → 复制 `ghp_...`（只显示一次）
3. **开写权限**：仓库 → Settings → Actions → General → 最下面
   **Workflow permissions** → **Read and write** → Save

然后在这个项目目录里：

```powershell
.\publish.ps1 -User 你的GitHub用户名
```

照着弹窗提示输入用户名和**令牌**（不是密码）。推完脚本会把接下来要点的链接都打出来。

以后每次发新版：

```powershell
.\publish.ps1 -User 你的GitHub用户名 -Release 1.21
```

它会改 `VERSION`、提交、打 tag、推送，GitHub Actions 自动打包发布。

> 下面是从零开始的完全手动版，遇到问题可以对照着看。
> 不想用脚本也可以全程手动，两种方式结果一样。

---

## 0. 全景图：要搭的是什么

```
GitHub 仓库
 ├── 源码（git push 上去）
 └── Releases
      ├── slhbond-monitor-1.21.tar.gz     ← 发布包
      └── update.json                     ← 版本清单（版本号 / 下载地址 / SHA-256）
                    │
                    │  设备定期读这个固定地址：
                    │  https://github.com/用户名/仓库/releases/latest/download/update.json
                    ▼
              你的设备 192.168.5.100
                    ├── 版本比清单旧 → 面板出现蓝色「更新可用」
                    └── 点击 → 下载 → 校验 → 定时器以 root 安装
```

**关键点：`releases/latest/download/update.json` 这个地址永远指向"最新一次发布"。**
所以设备上只配一次，以后每次发新版都不用再改设备。

---

## 1. 准备工作

| 需要什么 | 怎么确认 |
| --- | --- |
| 一个 GitHub 账号 | 没有就注册，见第 2 步 |
| 本机装了 git | PowerShell 里执行 `git --version`，有版本号就行 |
| 设备能装上 `curl` 或 `wget` | 设备上执行 `command -v curl wget` |

> **设备侧只需要 curl 或 wget 中的一个**。项目本身不链接 OpenSSL，
> 所以 https 下载是交给系统自带下载器做的。装一个：
> `sudo apt install -y curl`

---

## 2. 注册 GitHub 账号（已有可跳过）

1. 打开 <https://github.com/signup>
2. 填邮箱 → 设密码 → 取用户名
3. 验证邮箱（GitHub 会发一封邮件，点里面的链接）

**用户名会被写进下载地址**，所以记一下。下面教程里用 `你的用户名` 代替。

---

## 3. 建仓库

1. 打开 <https://github.com/new>
2. 填写：

   | 字段 | 填什么 |
   | --- | --- |
   | **Repository name** | `slhbond-monitor`（记下来，下面用得到） |
   | **Description** | 随便写，可不填 |
   | **Public / Private** | **选 Public**（私有仓库的下载地址需要带 token，很麻烦） |
   | **Add a README file** | **不要勾** |
   | **Add .gitignore** | **不要勾**（项目里已经有 `.gitignore` 了） |
   | **Choose a license** | **不要勾**（项目里已经有 `LICENSE`） |

3. 点 **Create repository**

建完会看到一个空仓库页面，上面有 `https://github.com/你的用户名/slhbond-monitor.git` ——
这就是**远程地址**，第 5 步要用。

---

## 4. 创建访问令牌（PAT）—— 这一步不能跳过

> **GitHub 从 2021 年起不接受账号密码推送代码。**
> 直接用密码会报 `Support for password authentication was removed`。
> 必须用 **Personal Access Token** 代替密码。

1. 打开 <https://github.com/settings/tokens>
2. 点 **Tokens (classic)** → 右上角 **Generate new token** → **Generate new token (classic)**
3. 填：

   | 字段 | 填什么 |
   | --- | --- |
   | **Note** | `slhbond-push`（随便起，能认出来就行） |
   | **Expiration** | 选 `90 days` 或 `No expiration`（自己权衡） |
   | **Select scopes** | **只勾 `repo`**（整个大项勾上即可） |

4. 拉到最下面点 **Generate token**
5. **立刻复制那串 `ghp_...` 开头的字符**

> ⚠️ **这串字符只显示一次**，关掉页面就再也看不到了。
> 先粘到记事本里，丢了只能重新生成。

---

## 5. 把项目推上 GitHub

打开 PowerShell，进项目目录：

```powershell
cd C:\Users\Administrator\Desktop\slh
```

### 5.1 初始化仓库

```powershell
git init
git add -A
git commit -m "Slhbond-Monitor v1.20"
```

### 5.2 改分支名为 main 并关联远程仓库

把 URL 里的 `你的用户名` 换成你自己的：

```powershell
git branch -M main
git remote add origin https://github.com/你的用户名/slhbond-monitor.git
```

### 5.3 推送

```powershell
git push -u origin main
```

**会弹出一个登录窗口**，要求填：

| 字段 | 填什么 |
| --- | --- |
| **Username** | 你的 GitHub 用户名 |
| **Password** | **粘贴第 4 步那个 `ghp_...` 令牌**（不是账号密码！） |

推成功后，刷新 GitHub 页面就能看到所有文件了。

> **Windows 会把这个令牌存进「凭据管理器」，以后 push 不用再输。**
> 如果输了还是报错，去 控制面板 → 凭据管理器 → Windows 凭据，
> 删掉 `git:https://github.com` 那条重试。

---

## 6. 打开 Actions 的写权限（第一次必做）

发布流程要用 `GITHUB_TOKEN` 创建 Release，默认权限可能只有只读，会失败。

1. 仓库页面 → **Settings**
2. 左侧 **Actions** → **General**
3. 拉到最下面 **Workflow permissions**
4. 选 **Read and write permissions**
5. 点 **Save**

---

## 7. 发布第一个版本

项目里已经带了 `.github/workflows/release.yml`，**打个 tag 就自动发布**。

### 7.1 打 tag 并推送

```powershell
cd C:\Users\Administrator\Desktop\slh
git push origin main          # 确保代码是最新的
git tag v1.20
git push origin v1.20
```

### 7.2 看它自动干活

打开 `https://github.com/你的用户名/slhbond-monitor/actions`，
能看到一个叫 **Release** 的任务在跑，大约 1 分钟。

它做了这些事：

1. 检查 tag（`v1.20`）和 `VERSION` 文件（`1.20`）是否一致 —— **不一致直接失败**，
   防止发出一个版本号对不上的包
2. `make check` 编译（有告警就失败）并 `make dist` 打包
3. 算 SHA-256，生成 `update.json`
4. 发布 Release，把**发布包**和 `update.json` 一起挂上去

跑完后打开 `https://github.com/你的用户名/slhbond-monitor/releases`，
应该能看到 `Slhbond-Monitor 1.20`，下面有两个附件。

### 7.3 确认附件都在

点开这次 Release，确认这两个文件存在：

- `slhbond-monitor-1.20.tar.gz`
- `update.json`

**少任何一个，设备都更新不了。**

---

## 8. 拿到清单地址（配一次，永久有效）

```
https://github.com/你的用户名/slhbond-monitor/releases/latest/download/update.json
```

**这个地址不用改。** `latest` 会自动指向最新一次 Release，
以后你发 1.21、1.22，设备读的还是这个地址，自动拿到新清单。

先在浏览器里打开它验证一下 —— 应该直接显示 JSON 内容，像这样：

```json
{
  "version": "1.20",
  "url": "https://github.com/你的用户名/slhbond-monitor/releases/download/v1.20/slhbond-monitor-1.20.tar.gz",
  "sha256": "a1b2c3...",
  "size": 234567,
  "notes": "见本次 Release 的说明",
  "published": "2026-10-17T10:00:00Z"
}
```

> **打开是 404？** 见第 12 节排错。

---

## 9. 配置设备

SSH 登录设备（`ssh root@192.168.5.100`），编辑配置：

```bash
nano /etc/slhbond-monitor.conf
```

找到这两行，改成：

```ini
update_check = true
update_manifest = https://github.com/你的用户名/slhbond-monitor/releases/latest/download/update.json
update_interval = 1800
```

- `update_interval = 1800` 是每 30 分钟检查一次，想更灵敏就改小
- **注意不要把地址写错**，特别是别丢了 `latest/download` 这一段

确认设备能访问：

```bash
curl -sS -m 10 https://github.com/你的用户名/slhbond-monitor/releases/latest/download/update.json
```

能打印出 JSON 就对了。打不出来先解决网络（见第 12 节）。

重启服务：

```bash
systemctl restart slhbond-monitor
```

---

## 10. 验证整条链路

### 10.1 清单读到了吗

```bash
curl -s http://127.0.0.1:8090/api/v1/version | python3 -m json.tool
```

看这几个字段：

```json
{
  "current": "1.20",
  "latest": "1.20",
  "update_available": false,      ← 版本相同，所以是 false
  "release_sha256": "a1b2c3...",  ← 说明 sha256 被正确解析了
  "release_size": 234567,
  "last_check_ok": true,
  "last_error": null
}
```

`last_error` 有内容就照着改。

### 10.2 强制检查一次（不用等 30 分钟）

```bash
curl -s -X POST http://127.0.0.1:8090/api/v1/update/check | python3 -m json.tool
```

### 10.3 看更新能力

```bash
curl -s http://127.0.0.1:8090/api/v1/update | python3 -m json.tool
```

`download_ready` 必须是 `true`。是 `false` 就是设备上没装 curl/wget。

### 10.4 真正测一次更新

改代码 → 把 `VERSION` 改成 `1.21` → 提交 → `git tag v1.21 && git push origin v1.21` →
等 Actions 跑完 → 回设备上强制检查：

```bash
curl -s -X POST http://127.0.0.1:8090/api/v1/update/check | python3 -m json.tool
```

这时 `update_available` 应该是 `true`，面板右上角出现蓝色「更新可用」按钮。

**点它**，然后什么都不用做。会在几分钟内自动完成：下载 → 校验 → 等待 → 安装 → 重启。
想看过程：

```bash
tail -f /var/log/slhbond-monitor/update.log
```

---

## 11. 以后每次发新版（就三步）

```powershell
cd C:\Users\Administrator\Desktop\slh

# 1. 改版本号
"1.21" | Out-File -Encoding ascii VERSION

# 2. 提交
git add -A
git commit -m "v1.21: 改了什么"
git push origin main

# 3. 打 tag 触发自动发布
git tag v1.21
git push origin v1.21
```

等 Actions 跑完，设备上什么都不用改 —— 下次检查（最多 30 分钟）就会看到新版本。

> ⚠️ **`VERSION` 文件和 tag 必须一致**（`1.21` ↔ `v1.21`）。
> 不一致时 Actions 会直接失败并给出明确提示，不会发出错误的包。

---

## 12. 不想用命令行？纯网页也能做完

如果你不想碰 git，可以全部在网页上操作。缺点是要手动算摘要。

### 12.1 上传源码（可选）

仓库页面 → **Add file** → **Upload files** → 把项目文件夹拖进去 → **Commit changes**。

> 网页上传整个目录有时会漏掉 `.github` 这类隐藏目录。
> 源码可以后续再传，**发版只需要附件，不影响更新功能**。

### 12.2 手动打一个包

在**能编译的环境**里：

```bash
make dist
```

Windows 上如果没有 Linux 环境，也可以用 WSL：`wsl make dist`。

### 12.3 算 SHA-256

PowerShell：

```powershell
Get-FileHash .\dist\slhbond-monitor-1.21.tar.gz -Algorithm SHA256
```

输出里 `Hash` 那一列就是摘要，**全部小写复制**。

Linux/macOS：

```bash
sha256sum dist/slhbond-monitor-1.21.tar.gz
```

### 12.4 手动写 update.json

新建一个文本文件，内容如下（把三处占位换掉）：

```json
{
  "version": "1.21",
  "url": "https://github.com/你的用户名/slhbond-monitor/releases/download/v1.21/slhbond-monitor-1.21.tar.gz",
  "sha256": "这里填上一步算出来的摘要",
  "size": 2345678,
  "notes": "这一版改了什么",
  "published": "2026-10-17T10:00:00Z"
}
```

- `url` 里的 tag 段（`v1.21`）和文件名必须和实际上传的一模一样
- `size` 是字节数，PowerShell 用 `(Get-Item 文件).Length`

### 12.5 建 Release 并传两个附件

1. 仓库页面 → 右侧 **Releases** → **Draft a new release**
2. **Choose a tag** 输入 `v1.21` → 点 **Create new tag on publish**
3. **Title** 填 `Slhbond-Monitor 1.21`
4. 在 **Attach binaries** 区域，把 **两个文件** 都拖进去：
   - `slhbond-monitor-1.21.tar.gz`
   - `update.json`
5. 确认勾选了 **Set as the latest release**
6. 点 **Publish release**

**"Set as the latest release" 这一步很关键** —— 不勾，`releases/latest/download/update.json`
就指不到这个版本。

---

## 13. 排错对照表

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| 打开清单地址 **404** | ① 不是 Public 仓库<br>② 没有勾 "latest release"<br>③ 附件名不是 `update.json` | 逐条核对；私有仓库改成 Public，或改用带 token 的地址 |
| 设备 `last_error` 是 **HTTP 404** | 同上 | 在设备上 `curl -v` 那个地址看返回什么 |
| 设备报 **`https 需要 curl 或 wget`** | 设备没装下载器 | `apt install -y curl` |
| 设备报 **`下载失败（curl 退出码 6）`** | DNS 解析不了 github.com | 检查 `cat /etc/resolv.conf`、能不能 `ping github.com` |
| 设备报 **`下载失败（curl 退出码 7）`** | 连不上（防火墙/代理） | `curl -v` 直接试；有代理就设 `https_proxy` 环境变量 |
| 报 **`校验失败：SHA-256 与清单不符`** | `update.json` 里的摘要和包对不上（常见于改了包又忘了改清单） | 重新算摘要；用 `make-manifest.sh` 生成就不会错 |
| 报 **`解包内容不像一个 Slhbond-Monitor 发布包`** | 传错了文件（比如只传了源码子目录） | 确认传的是 `make dist` 产出、**带顶层目录**的 tar.gz |
| Actions 报 **`tag 与 VERSION 不一致`** | VERSION 里是 1.20，tag 是 v1.21 | 改 `VERSION` → 提交 → **删掉旧 tag 重新打**：<br>`git tag -d v1.21 && git push origin :refs/tags/v1.21` 然后重来 |
| Actions 报 **`Resource not accessible by integration`** | 没开写权限 | 做第 6 步 |
| 更新一直停在 **`staged`** | 定时器没跑 | `systemctl status slhbond-monitor-update.timer`，应为 `active` |
| 定时器 **`Refusing to start`** | 单元文件里 `Unit=` 指向不存在的服务 | 重新 `./deploy.sh install` |
| 面板一直没出现「更新可用」 | 清单版本不高于当前版本 | 检查 `curl /api/v1/version` 的 `current` 和 `latest` |

---

## 14. 安全上必须知道的三件事

1. **每个包都做 SHA-256 校验**，摘要来自清单。
   https 只保证传输，**摘要才是真正的防线** —— 镜像被篡改会被拒绝安装。

2. **仓库必须 Public**，否则下载地址需要 token。
   把 token 写进设备配置意味着**设备被拿下就等于仓库被拿下**，不划算。
   代码要保密的话，用自建静态服务器放清单和包更合适。

3. **服务本身装不了任何东西**。
   它以 `DynamicUser` 运行，只能把已校验的包放进状态目录；
   真正安装的是 root 的 `slhbond-monitor-update.service`，
   而且它执行的每一步都来自那个已校验的包，不读取任何网络输入。

---

## 15. 一页速查

```powershell
# ---- 首次 ----
git init && git add -A && git commit -m "v1.20"
git branch -M main
git remote add origin https://github.com/你的用户名/slhbond-monitor.git
git push -u origin main
# 网页上打开 Settings → Actions → Workflow permissions → Read and write

# ---- 发布 ----
git tag v1.20 && git push origin v1.20

# ---- 设备配置 ----
# update_manifest = https://github.com/你的用户名/slhbond-monitor/releases/latest/download/update.json
# systemctl restart slhbond-monitor

# ---- 验证 ----
curl -s http://127.0.0.1:8090/api/v1/version | python3 -m json.tool
curl -s -X POST http://127.0.0.1:8090/api/v1/update/check | python3 -m json.tool

# ---- 以后每次发版 ----
# 改 VERSION → commit → push → tag → push tag
```
