#!/usr/bin/env bash
#
# slhbond-update.sh - 应用暂存的更新（以 root 身份运行）
#
# 由 slhbond-update.service 定时调用。监控服务本身是 DynamicUser 无权限，
# 只能把「已校验的发布包」放进状态目录；真正解包安装、重启服务都在这里做。
#
# 设计上的两个要点：
#   1. 没有 ready 标记就直接退出 —— 这是常态，绝大多数时候什么都不做。
#   2. 先构建、后安装。deploy.sh 内部也是先 make（失败即中止），所以一个
#      编译不过的更新不会把正在运行的服务替换掉。

set -uo pipefail

STATE_DIR="${SLH_STATE_DIR:-/var/lib/slhbond-monitor}"
READY="$STATE_DIR/update/ready"
STAGED="$STATE_DIR/update/staged"
LOG_DIR="/var/log/slhbond-monitor"
LOG="$LOG_DIR/update.log"
APP="slhbond-monitor"

mkdir -p "$LOG_DIR" 2>/dev/null

log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"; }

# 没有待应用的更新：安静退出
[ -f "$READY" ] || exit 0

VERSION="$(head -n 1 "$READY" 2>/dev/null | tr -d '[:space:]')"
[ -n "$VERSION" ] || VERSION="未知"

log "=== 开始应用更新 v$VERSION ==="

if [ ! -f "$STAGED/deploy.sh" ]; then
    log "staged/deploy.sh 不存在，跳过（保留 ready 供人工排查）"
    exit 1
fi
if [ ! -f "$STAGED/Makefile" ] || [ ! -d "$STAGED/src" ]; then
    log "暂存目录不是完整源码树，跳过"
    exit 1
fi

# 构建阶段单独跑一次：编译不过就不要动正在运行的安装
if ! ( cd "$STAGED" && make clean >/dev/null 2>&1 && make -j"$(nproc 2>/dev/null || echo 2)" >>"$LOG" 2>&1 ); then
    log "编译失败，保持现有版本不变（详见上方输出）"
    systemctl start "$APP" >/dev/null 2>&1
    exit 1
fi

# 安装阶段：deploy.sh 会停止服务、拷贝文件、重新注册单元并启动
if ( cd "$STAGED" && bash deploy.sh install >>"$LOG" 2>&1 ); then
    log "v$VERSION 应用成功"
    cd /
    rm -rf "$STAGED" "$READY"
    exit 0
fi

log "v$VERSION 安装失败，已尝试恢复运行旧版本"
systemctl start "$APP" >/dev/null 2>&1
# 保留暂存目录与 ready，方便下次排查或重试
exit 1
