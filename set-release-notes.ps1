<#
.SYNOPSIS
    用 GitHub API 修改某个 Release 的说明，不需要安装 gh。

.DESCRIPTION
    gh 命令在 Windows 上默认没有安装。这个脚本直接用 REST API 改发布说明，
    只依赖 PowerShell 自带的 Invoke-RestMethod。

    令牌从键盘安全输入（不回显、不落盘、不进命令历史），
    需要有 repo 权限：https://github.com/settings/tokens

.EXAMPLE
    .\set-release-notes.ps1 -Tag v1.20 -File docs\RELEASE-1.20.md

.EXAMPLE
    # 不传 -File 就默认用 docs\RELEASE-<版本>.md
    .\set-release-notes.ps1 -Tag v1.20
#>
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$User = "Slhbond",
    [string]$Repo = "slhbond-monitor",
    [string]$File
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

$ver = $Tag.TrimStart("v")
if (-not $File) { $File = "docs\RELEASE-$ver.md" }

$path = Join-Path (Get-Location) $File
if (-not (Test-Path -LiteralPath $path)) {
    throw "找不到说明文件：$path"
}

# 必须显式按 UTF-8 读：Windows PowerShell 的 Get-Content 默认按 ANSI/GBK 解码，
# 中文会变成乱码并被原样写进 Release。
$body = [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false))
Write-Host "说明文件: $File  ($($body.Length) 字符)" -ForegroundColor Gray
Write-Host "目标    : $User/$Repo  标签 $Tag" -ForegroundColor Gray

$sec = Read-Host "粘贴访问令牌（输入时不显示）" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try {
    $token = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}
if (-not $token) { throw "没有输入令牌" }

$headers = @{
    Authorization          = "Bearer $token"
    Accept                 = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent"           = "slhbond-monitor-publish"
}

Write-Host "`n查找 Release $Tag ..." -ForegroundColor Cyan
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$User/$Repo/releases/tags/$Tag" -Headers $headers -TimeoutSec 30
} catch {
    throw "找不到 Release $Tag（或者令牌没有权限）。原始错误：$($_.Exception.Message)"
}
Write-Host "  找到：$($rel.name)  id=$($rel.id)" -ForegroundColor Green

Write-Host "`n更新说明 ..." -ForegroundColor Cyan
$payload = @{ body = $body } | ConvertTo-Json -Compress
try {
    $updated = Invoke-RestMethod -Method Patch `
        -Uri "https://api.github.com/repos/$User/$Repo/releases/$($rel.id)" `
        -Headers $headers -ContentType "application/json; charset=utf-8" `
        -Body ([System.Text.Encoding]::UTF8.GetBytes($payload)) -TimeoutSec 60
} catch {
    throw "更新失败。原始错误：$($_.Exception.Message)"
}

Write-Host "  完成" -ForegroundColor Green
Write-Host ""
Write-Host "  打开看看： $($updated.html_url)" -ForegroundColor Cyan
Write-Host "  说明长度： $($updated.body.Length) 字符" -ForegroundColor Gray
