#!/usr/bin/env bash
#
# make-manifest.sh - 生成 update.json（版本清单）
#
# 用法：
#   ./make-manifest.sh <版本号> <发布包> <下载地址> [说明]
#
# 例子（GitHub Release 资产）：
#   ./make-manifest.sh 1.21 dist/slhbond-monitor-1.21.tar.gz \
#     https://github.com/你的用户名/slhbond-monitor/releases/download/v1.21/slhbond-monitor-1.21.tar.gz \
#     "修复了开关机记录"
#
# 输出直接写到标准输出，把它保存成 update.json 上传即可。
# sha256 由脚本计算，避免手抄出错 —— 清单里的摘要与实际包不一致时，
# 更新会被校验拒绝，这是有意的。
set -euo pipefail

if [ $# -lt 3 ]; then
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

VERSION="$1"
PKG="$2"
URL="$3"
NOTES="${4:-Slhbond-Monitor $VERSION}"

[ -f "$PKG" ] || { echo "找不到发布包：$PKG" >&2; exit 1; }

if command -v sha256sum >/dev/null 2>&1; then
    SHA=$(sha256sum "$PKG" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    SHA=$(shasum -a 256 "$PKG" | awk '{print $1}')
else
    echo "系统里没有 sha256sum / shasum" >&2
    exit 1
fi

SIZE=$(stat -c %s "$PKG" 2>/dev/null || stat -f %z "$PKG")

# 说明里如果带引号会破坏 JSON，简单转义一下
NOTES_ESC=$(printf '%s' "$NOTES" | sed 's/\\/\\\\/g; s/"/\\"/g')

cat <<EOF
{
  "version": "$VERSION",
  "url": "$URL",
  "sha256": "$SHA",
  "size": $SIZE,
  "notes": "$NOTES_ESC",
  "published": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
EOF
