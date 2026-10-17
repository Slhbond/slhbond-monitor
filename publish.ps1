<#
.SYNOPSIS
    把 Slhbond-Monitor 推送到 GitHub，并可选地发布一个新版本。

.DESCRIPTION
    只做「命令行能做的部分」：设置提交身份、配置远程地址、推送。
    建仓库和创建访问令牌必须在浏览器里做，脚本没法代你完成。

.EXAMPLE
    # 第一次推送
    .\publish.ps1 -User 你的GitHub用户名

.EXAMPLE
    # 发布 1.21（改 VERSION、提交、打 tag、推送，触发 GitHub Actions）
    .\publish.ps1 -User 你的GitHub用户名 -Release 1.21

.NOTES
    推送时会要求输入凭据：
      用户名 = 你的 GitHub 用户名
      密码   = 个人访问令牌（ghp_... 开头），不是账号密码
#>
param(
    [Parameter(Mandatory = $true)][string]$User,
    [string]$Repo = "slhbond-monitor",
    [string]$Release,
    [string]$Email
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  OK   $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  !    $msg" -ForegroundColor Yellow }
function Info($msg) { Write-Host "       $msg" -ForegroundColor Gray }

# ---------- 0. 前置检查 ----------
Step "检查环境"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "找不到 git。先装 Git for Windows: https://git-scm.com/download/win"
}
Ok "git $(git --version)"

if (-not (Test-Path ".git")) {
    throw "当前目录不是 git 仓库。publish.ps1 必须放在项目根目录（与 Makefile 同级）。"
}
Ok "仓库 $PWD"

# ---------- 1. 提交身份 ----------
Step "设置提交身份（仅本仓库）"
if (-not (git config user.name)) {
    git config user.name $User
    Ok "user.name = $User"
} else {
    Ok "user.name = $(git config user.name)（已有，跳过）"
}
if (-not (git config user.email)) {
    $e = if ($Email) { $Email } else { "$User@users.noreply.github.com" }
    git config user.email $e
    Ok "user.email = $e"
} else {
    Ok "user.email = $(git config user.email)（已有，跳过）"
}

# ---------- 2. 远程地址 ----------
Step "配置远程地址"
$url = "https://github.com/$User/$Repo.git"
# 不要用 `git remote get-url origin 2>$null` 来判断：origin 不存在时 git 会往
# stderr 写字，配合 ErrorActionPreference=Stop 会被当成致命错误抛出来。
$remotes = @(git remote)
if ($remotes -contains "origin") {
    $existing = (git remote get-url origin)
    if ($existing -ne $url) {
        Warn "原来指向 $existing"
        git remote set-url origin $url
        Ok "改为 $url"
    } else {
        Ok "$url（已有，跳过）"
    }
} else {
    git remote add origin $url
    Ok $url
}

# ---------- 3. 发布新版本（可选） ----------
if ($Release) {
    Step "准备发布 $Release"
    $old = (Get-Content VERSION -Raw).Trim()
    if ($old -eq $Release) {
        Ok "VERSION 已经是 $Release，跳过修改"
    } else {
        Set-Content -Path VERSION -Value "$Release`n" -NoNewline -Encoding ascii
        Ok "VERSION $old -> $Release"
    }

    git add -A
    # 没有改动时 git commit 会非 0 退出并往 stderr 写字，先判断
    $dirty = @(git status --porcelain)
    if ($dirty.Count -gt 0) {
        git commit -q -m "v$Release"
        Ok "已提交"
    } else {
        Ok "没有新改动，跳过提交"
    }

    $tags = @(git tag -l "v$Release")
    if ($tags -contains "v$Release") {
        Warn "tag v$Release 已存在，删掉重打"
        git tag -d "v$Release" | Out-Null
    }
    git tag "v$Release"
    Ok "已打 tag v$Release"
}

# ---------- 4. 推送 ----------
Step "推送到 GitHub"
Info "如果弹出登录窗口："
Info "  用户名 = $User"
Info "  密码   = 个人访问令牌（ghp_ 开头），不是账号密码"
Info "  令牌生成： https://github.com/settings/tokens   （只勾 repo）"

git push -u origin main
if ($LASTEXITCODE -ne 0) {
    throw "推送 main 失败。检查：仓库建了吗？令牌勾了 repo 吗？仓库名是 $Repo 吗？"
}
Ok "main 已推送"

if ($Release) {
    git push origin "v$Release"
    if ($LASTEXITCODE -ne 0) { throw "推送 tag 失败" }
    Ok "tag v$Release 已推送"
}

# ---------- 5. 收尾 ----------
Write-Host ""
Write-Host "完成。接下来还有两步要在网页上做：" -ForegroundColor Cyan
Write-Host ""
Write-Host "  1) 打开写权限（第一次必做，否则发 Release 会失败）"
Write-Host "     https://github.com/$User/$Repo/settings/actions"
Write-Host "     General 拉到最下面 -> Workflow permissions -> Read and write -> Save"
Write-Host ""
if ($Release) {
    Write-Host "  2) 看自动发布跑完"
    Write-Host "     https://github.com/$User/$Repo/actions"
    Write-Host "     然后设备上配这个清单地址："
} else {
    Write-Host "  2) 仓库首页"
    Write-Host "     https://github.com/$User/$Repo"
    Write-Host "     设备上配这个清单地址："
}
Write-Host "     https://github.com/$User/$Repo/releases/latest/download/update.json"
Write-Host ""
