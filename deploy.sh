#!/usr/bin/env bash
#
#  Slhbond-Monitor 部署脚本
#  ------------------------------------------
#  在一个脚本里完成：安装、卸载、查看状态。
#  支持 systemd / OpenRC / SysV init / cron 四种开机自启方式。
#  支持 x86_64、aarch64、armv7 等任意 Linux 架构（本机原生编译）。
#
#  用法：
#     ./deploy.sh              交互菜单
#     ./deploy.sh install      直接部署
#     ./deploy.sh uninstall    直接卸载
#     ./deploy.sh status       查看运行状态
#     ./deploy.sh help         查看帮助
#
#  可用环境变量覆盖默认值：
#     SLH_PREFIX   安装目录      默认 /opt/slhbond-monitor
#     SLH_PORT     监听端口      默认 8090
#     SLH_CONF     配置文件路径  默认 /etc/slhbond-monitor.conf
#     SLH_NO_DEPS  =1 时不自动安装编译依赖
#
set -uo pipefail

APP="slhbond-monitor"
PREFIX="${SLH_PREFIX:-/opt/$APP}"
BIN_DIR="$PREFIX/bin"
WEB_DIR="$PREFIX/web"
DOC_DIR="$PREFIX/docs"
CONF="${SLH_CONF:-/etc/$APP.conf}"
PORT="${SLH_PORT:-8090}"
STATE_DIR="/var/lib/$APP"
LOG_DIR="/var/log/$APP"
UNIT="/etc/systemd/system/$APP.service"
INITD="/etc/init.d/$APP"

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NO_DEPS="${SLH_NO_DEPS:-0}"

# ------------------------------------------------------------------ 输出

if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[36m'
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""
fi

info()  { printf '  %s\n' "$*"; }
ok()    { printf '  %s✓%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn()  { printf '  %s!%s %s\n' "$C_YELLOW" "$C_RESET" "$*"; }
err()   { printf '  %s✗%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
step()  { printf '\n%s==>%s %s%s%s\n' "$C_BLUE" "$C_RESET" "$C_BOLD" "$*" "$C_RESET"; }
die()   { err "$*"; exit 1; }

banner() {
    printf '%s' "$C_BOLD"
    cat <<'EOF'
   ____  _ _     _                     _   __  __             _ _
  / ___|| | |__ | |__   ___  _ __   __| | |  \/  | ___  _ __ (_) |_ ___  _ __
  \___ \| | '_ \| '_ \ / _ \| '_ \ / _` | | |\/| |/ _ \| '_ \| | __/ _ \| '__|
   ___) | | |_) | | | | (_) | | | | (_| | | |  | | (_) | | | | | || (_) | |
  |____/|_|_.__/|_| |_|\___/|_| |_|\__,_| |_|  |_|\___/|_| |_|_|\__\___/|_|
EOF
    printf '%s' "$C_RESET"
    printf '  %s轻量级 Linux 系统监控 · 纯 C · 零第三方依赖%s\n' "$C_DIM" "$C_RESET"
    printf '  %s架构 %s · 系统 %s%s\n' "$C_DIM" "$(uname -m)" "$(uname -sr)" "$C_RESET"
}

# ------------------------------------------------------------------ 环境探测

have() { command -v "$1" >/dev/null 2>&1; }

require_root() {
    [ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行：sudo $0"
}

detect_pm() {
    for pm in apt-get dnf yum pacman zypper apk; do
        if have "$pm"; then echo "$pm"; return 0; fi
    done
    echo ""
}

install_packages() {
    local pm; pm="$(detect_pm)"
    [ -n "$pm" ] || { warn "没识别到包管理器，请手动安装：gcc make smartmontools"; return 1; }

    step "安装依赖（$pm）"
    case "$pm" in
        apt-get) apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential smartmontools ;;
        dnf)     dnf install -y gcc make glibc-devel smartmontools ;;
        yum)     yum install -y gcc make glibc-devel smartmontools ;;
        pacman)  pacman -Sy --noconfirm gcc make smartmontools ;;
        zypper)  zypper --non-interactive install gcc make glibc-devel smartmontools ;;
        apk)     apk add --no-cache build-base smartmontools ;;
    esac
}

check_build_tools() {
    local missing=()
    have cc || have gcc || missing+=("gcc")
    have make || missing+=("make")
    if [ ${#missing[@]} -gt 0 ]; then
        warn "缺少编译工具：${missing[*]}"
        if [ "$NO_DEPS" = "1" ]; then
            die "已设置 SLH_NO_DEPS=1，请先手动安装：${missing[*]}"
        fi
        install_packages || die "依赖安装失败，请手动安装后重试"
    fi
    have cc || have gcc || die "仍然找不到 C 编译器"
    have make || die "仍然找不到 make"
}

init_system() {
    if have systemctl && [ -d /run/systemd/system ]; then echo "systemd"; return; fi
    if have rc-service && [ -d /etc/init.d ]; then echo "openrc"; return; fi
    if have update-rc.d || have chkconfig; then echo "sysv"; return; fi
    if [ -d /etc/init.d ]; then echo "sysv"; return; fi
    if have crontab; then echo "cron"; return; fi
    echo "none"
}

# ------------------------------------------------------------------ 构建

do_build() {
    step "编译（本机 $(uname -m) 原生构建）"
    check_build_tools
    cd "$SRC_DIR" || die "无法进入源码目录 $SRC_DIR"
    make clean >/dev/null 2>&1
    if ! make -j"$(nproc 2>/dev/null || echo 2)" 2>&1 | tail -3; then
        die "编译失败，请检查上面的错误输出"
    fi
    [ -x "$SRC_DIR/build/$APP" ] || die "编译产物不存在：$SRC_DIR/build/$APP"
    ok "编译完成：build/$APP（$(du -h "$SRC_DIR/build/$APP" | cut -f1)）"
}

# ------------------------------------------------------------------ 安装

write_config() {
    if [ -f "$CONF" ]; then
        ok "配置文件已存在，保留：$CONF"
        return
    fi
    step "写入配置 $CONF"
    # 从随包发布的模板生成，而不是在这里重写一份 —— 键名只有一处定义，
    # 不会出现脚本和 config.c 对不上的情况。
    sed -e "s|@PREFIX@|$PREFIX|g" \
        -e "s|@PORT@|$PORT|g" \
        -e "s|@STATE_DIR@|$STATE_DIR|g" \
        "$SRC_DIR/deploy/$APP.conf" > "$CONF" || die "写入配置失败"
    chmod 644 "$CONF"
    ok "已写入 $CONF（端口 $PORT）"
}

install_files() {
    step "安装到 $PREFIX"
    mkdir -p "$BIN_DIR" "$WEB_DIR" "$DOC_DIR" || die "无法创建 $PREFIX"

    # 先停掉可能在跑的老进程，避免覆盖正在执行的二进制
    stop_service >/dev/null 2>&1

    install -m 0755 "$SRC_DIR/build/$APP" "$BIN_DIR/$APP" || die "拷贝可执行文件失败"
    ok "$BIN_DIR/$APP"

    for f in "$SRC_DIR"/web/*; do
        [ -f "$f" ] || continue
        install -m 0644 "$f" "$WEB_DIR/$(basename "$f")"
    done
    ok "$WEB_DIR/（$(ls -1 "$WEB_DIR" | wc -l) 个文件）"

    [ -f "$SRC_DIR/README.md" ] && install -m 0644 "$SRC_DIR/README.md" "$PREFIX/README.md"
    [ -f "$SRC_DIR/VERSION" ]   && install -m 0644 "$SRC_DIR/VERSION"   "$PREFIX/VERSION"
    if [ -d "$SRC_DIR/docs" ]; then
        for f in "$SRC_DIR"/docs/*; do
            [ -f "$f" ] || continue
            install -m 0644 "$f" "$DOC_DIR/$(basename "$f")"
        done
        ok "$DOC_DIR/"
    fi
}

# ------------------------------------------------------------------ 开机自启

install_service_systemd() {
    step "注册 systemd 服务（开机自启）"
    sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@CONF@|$CONF|g" \
        "$SRC_DIR/deploy/$APP.service" > "$UNIT" || die "写入 unit 失败"
    chmod 644 "$UNIT"
    systemctl daemon-reload || die "systemctl daemon-reload 失败"
    systemctl enable "$APP" >/dev/null 2>&1 || warn "systemctl enable 失败，请手动检查"
    systemctl restart "$APP" || die "服务启动失败，用 journalctl -u $APP -n 40 查看原因"
    ok "systemd 服务已注册并设为开机自启"
}

install_service_openrc() {
    step "注册 OpenRC 服务（开机自启）"
    cat > "$INITD" <<EOF
#!/sbin/openrc-run
name="$APP"
description="Slhbond-Monitor 轻量级系统监控"
command="$BIN_DIR/$APP"
command_args="--foreground --config $CONF"
command_background=true
pidfile="/run/$APP.pid"
output_log="$LOG_DIR/out.log"
error_log="$LOG_DIR/err.log"
depend() { need net; after firewall; }
EOF
    chmod 0755 "$INITD"
    mkdir -p "$LOG_DIR"
    rc-update add "$APP" default >/dev/null 2>&1 || warn "rc-update 失败"
    rc-service "$APP" restart >/dev/null 2>&1 || warn "rc-service 启动失败"
    ok "OpenRC 服务已注册并设为开机自启"
}

install_service_sysv() {
    step "注册 SysV init 服务（开机自启）"
    cat > "$INITD" <<EOF
#!/bin/sh
### BEGIN INIT INFO
# Provides:          $APP
# Required-Start:    \$network \$remote_fs
# Required-Stop:     \$network \$remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Slhbond-Monitor 轻量级系统监控
### END INIT INFO
DAEMON="$BIN_DIR/$APP"
ARGS="--foreground --config $CONF"
PIDFILE="/run/$APP.pid"
NAME="$APP"

start() {
    [ -x "\$DAEMON" ] || exit 5
    printf 'Starting %s: ' "\$NAME"
    start-stop-daemon --start --quiet --background --make-pidfile \\
        --pidfile "\$PIDFILE" --exec "\$DAEMON" -- \$ARGS && echo ok || echo failed
}
stop() {
    printf 'Stopping %s: ' "\$NAME"
    start-stop-daemon --stop --quiet --retry=TERM/10/KILL/5 \\
        --pidfile "\$PIDFILE" && echo ok || echo failed
    rm -f "\$PIDFILE"
}
case "\$1" in
    start) start ;;
    stop) stop ;;
    restart|force-reload) stop; sleep 1; start ;;
    status) [ -f "\$PIDFILE" ] && kill -0 "\$(cat \$PIDFILE)" 2>/dev/null \\
             && echo "\$NAME is running" || echo "\$NAME is not running" ;;
    *) echo "Usage: \$0 {start|stop|restart|status}"; exit 2 ;;
esac
EOF
    chmod 0755 "$INITD"
    if have update-rc.d; then
        update-rc.d "$APP" defaults >/dev/null 2>&1 || warn "update-rc.d 失败"
    elif have chkconfig; then
        chkconfig --add "$APP" >/dev/null 2>&1 || warn "chkconfig 失败"
    fi
    "$INITD" restart >/dev/null 2>&1 || warn "启动失败"
    ok "SysV 服务已注册并设为开机自启"
}

install_service_cron() {
    step "注册 cron @reboot 自启（无 init 系统时的兜底）"
    mkdir -p "$LOG_DIR"
    local line="@reboot $BIN_DIR/$APP --foreground --config $CONF >> $LOG_DIR/out.log 2>&1"
    ( crontab -l 2>/dev/null | grep -vF "$APP" ; echo "$line" ) | crontab - \
        || die "写入 crontab 失败"
    nohup "$BIN_DIR/$APP" --foreground --config "$CONF" >> "$LOG_DIR/out.log" 2>&1 &
    ok "已加入 cron @reboot，并已启动"
}

start_service() {
    case "$(init_system)" in
        systemd) systemctl restart "$APP" ;;
        openrc)  rc-service "$APP" restart ;;
        sysv)    "$INITD" restart ;;
        cron)    pkill -f "$BIN_DIR/$APP" 2>/dev/null; sleep 1
                 nohup "$BIN_DIR/$APP" --foreground --config "$CONF" >> "$LOG_DIR/out.log" 2>&1 & ;;
        *)       nohup "$BIN_DIR/$APP" --foreground --config "$CONF" >/dev/null 2>&1 & ;;
    esac
}

stop_service() {
    case "$(init_system)" in
        systemd) systemctl stop "$APP" 2>/dev/null ;;
        openrc)  rc-service "$APP" stop 2>/dev/null ;;
        sysv)    [ -x "$INITD" ] && "$INITD" stop 2>/dev/null ;;
        *)       pkill -f "$BIN_DIR/$APP" 2>/dev/null ;;
    esac
    return 0
}

setup_state_dir() {
    if [ "$(init_system)" = "systemd" ]; then
        # 这里刻意 *不* 创建目录，交给单元里的 StateDirectory= 自己建。
        # DynamicUser=yes 下 systemd 会把它做成指向 /var/lib/private/ 的符号
        # 链接；如果提前用 root 建好一个普通目录，systemd 会以
        # EXIT_STATE_DIRECTORY(238) 拒绝启动，服务永远卡在 activating。
        if [ -L "$STATE_DIR" ]; then
            return 0                       # 已经是 systemd 管理的符号链接
        fi
        if [ -d "$STATE_DIR" ]; then
            if [ -z "$(ls -A "$STATE_DIR" 2>/dev/null)" ]; then
                rmdir "$STATE_DIR" 2>/dev/null && ok "移除空的 $STATE_DIR，交给 systemd 创建"
            else
                warn "$STATE_DIR 已存在且非空，systemd 可能拒绝启动（错误码 238）"
                info "修复：mv $STATE_DIR ${STATE_DIR}.bak && systemctl restart $APP"
            fi
        fi
        return 0
    fi

    # 非 systemd：服务以 root 运行，自己建好即可。
    mkdir -p "$STATE_DIR" 2>/dev/null || return 0
    chmod 0750 "$STATE_DIR" 2>/dev/null
    info "状态目录：$STATE_DIR"
}

install_service() {
    case "$(init_system)" in
        systemd) install_service_systemd ;;
        openrc)  install_service_openrc ;;
        sysv)    install_service_sysv ;;
        cron)    install_service_cron ;;
        *)       warn "未识别到 init 系统，无法设置开机自启"
                 nohup "$BIN_DIR/$APP" --foreground --config "$CONF" >/dev/null 2>&1 &
                 warn "已临时启动，但重启后不会自动运行" ;;
    esac
}

# ------------------------------------------------------------------ 验证

local_ip() {
    local ip=""
    if have ip; then
        ip="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}' | head -1)"
    fi
    [ -n "$ip" ] || ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    [ -n "$ip" ] || ip="127.0.0.1"
    echo "$ip"
}

verify() {
    step "验证服务"
    local url="http://127.0.0.1:$PORT/api/v1/health"
    local i code=""
    for i in $(seq 1 20); do
        if have curl; then
            code="$(curl -s -o /dev/null -w '%{http_code}' -m 3 "$url" 2>/dev/null)"
        elif have wget; then
            wget -q -O /dev/null -T 3 "$url" 2>/dev/null && code="200"
        fi
        [ "$code" = "200" ] && break
        sleep 0.5
    done

    if [ "$code" = "200" ]; then
        ok "服务响应正常（$url）"
    else
        warn "服务未在预期时间内响应，用下面的命令查看日志："
        info "journalctl -u $APP -n 40 --no-pager"
        return 1
    fi

    # 顺手检查一下防火墙是否放行了端口
    if have firewall-cmd && firewall-cmd --state >/dev/null 2>&1; then
        if ! firewall-cmd --list-ports 2>/dev/null | grep -q "$PORT/tcp"; then
            warn "firewalld 似乎未放行 $PORT/tcp，外部可能访问不到："
            info "firewall-cmd --permanent --add-port=$PORT/tcp && firewall-cmd --reload"
        fi
    elif have ufw && ufw status 2>/dev/null | grep -q "Status: active"; then
        if ! ufw status 2>/dev/null | grep -q "$PORT"; then
            warn "ufw 已启用但似乎未放行 $PORT，外部可能访问不到："
            info "ufw allow $PORT/tcp"
        fi
    fi
    return 0
}

print_access() {
    printf '\n  %s访问地址%s  http://%s:%s/\n' "$C_BOLD" "$C_RESET" "$(local_ip)" "$PORT"
    printf '  %s本机地址%s  http://127.0.0.1:%s/\n' "$C_DIM" "$C_RESET" "$PORT"
    printf '  %s安装目录%s  %s\n' "$C_DIM" "$C_RESET" "$PREFIX"
    printf '  %s配置文件%s  %s\n' "$C_DIM" "$C_RESET" "$CONF"
}

# ------------------------------------------------------------------ 安装 / 卸载

do_install() {
    banner
    require_root
    step "环境"
    info "架构：$(uname -m)"
    info "内核：$(uname -sr)"
    info "init：$(init_system)"
    info "源码：$SRC_DIR"

    do_build
    install_files
    write_config
    setup_state_dir
    install_service

    if verify; then
        print_access
        printf '\n  %s部署完成%s\n' "$C_GREEN$C_BOLD" "$C_RESET"
    else
        printf '\n  %s部署了，但服务没起来，请按上面的提示查日志%s\n' "$C_YELLOW" "$C_RESET"
        return 1
    fi
}

do_uninstall() {
    banner
    require_root
    local purge="${1:-ask}"

    step "停止并注销服务"
    case "$(init_system)" in
        systemd)
            systemctl disable --now "$APP" >/dev/null 2>&1
            rm -f "$UNIT"
            systemctl daemon-reload >/dev/null 2>&1
            ok "systemd 服务已移除"
            ;;
        openrc)
            rc-service "$APP" stop >/dev/null 2>&1
            rc-update del "$APP" default >/dev/null 2>&1
            rm -f "$INITD"; ok "OpenRC 服务已移除"
            ;;
        sysv)
            [ -x "$INITD" ] && "$INITD" stop >/dev/null 2>&1
            if have update-rc.d; then update-rc.d -f "$APP" remove >/dev/null 2>&1
            elif have chkconfig; then chkconfig --del "$APP" >/dev/null 2>&1; fi
            rm -f "$INITD"; ok "SysV 服务已移除"
            ;;
        *)
            pkill -f "$BIN_DIR/$APP" 2>/dev/null
            ok "已停止运行中的进程"
            ;;
    esac
    if have crontab; then
        crontab -l 2>/dev/null | grep -vF "$APP" | crontab - 2>/dev/null
    fi

    step "删除程序文件"
    rm -rf "$PREFIX" && ok "已删除 $PREFIX"

    if [ "$purge" = "ask" ]; then
        printf '\n  是否同时删除配置与状态数据？\n'
        printf '    %s1)%s 全部删除（配置 + 已检测的硬盘记录）\n' "$C_BOLD" "$C_RESET"
        printf '    %s2)%s 保留（下次安装还能用）\n' "$C_BOLD" "$C_RESET"
        printf '  请选择 [1/2，默认 2]: '
        read -r ans
        [ "$ans" = "1" ] && purge="yes" || purge="no"
    fi

    if [ "$purge" = "yes" ]; then
        rm -f "$CONF"
        # DynamicUser + StateDirectory 下，$STATE_DIR 只是指向
        # /var/lib/private/<name> 的符号链接，真正数据在那个目录里，
        # 只删链接会留下孤儿数据。
        rm -rf "$STATE_DIR" "/var/lib/private/$APP" "$LOG_DIR"
        ok "已删除 $CONF、状态数据、$LOG_DIR"
    else
        info "已保留 $CONF 与状态数据"
    fi
    printf '\n  %s卸载完成%s\n' "$C_GREEN$C_BOLD" "$C_RESET"
}

do_status() {
    banner
    step "运行状态"
    case "$(init_system)" in
        systemd) systemctl status "$APP" --no-pager -n 8 2>/dev/null || warn "服务未安装或未运行" ;;
        openrc)  rc-service "$APP" status 2>/dev/null || warn "服务未安装或未运行" ;;
        sysv)    [ -x "$INITD" ] && "$INITD" status || warn "服务未安装" ;;
        *)       pgrep -f "$BIN_DIR/$APP" >/dev/null && ok "进程在运行" || warn "进程未运行" ;;
    esac

    step "开机自启"
    case "$(init_system)" in
        systemd) systemctl is-enabled "$APP" 2>/dev/null || info "未启用" ;;
        openrc)  rc-update show default 2>/dev/null | grep -F "$APP" || info "未启用" ;;
        sysv)    ls /etc/rc*.d/ 2>/dev/null | grep -F "$APP" || info "未启用" ;;
        *)       crontab -l 2>/dev/null | grep -F "$APP" || info "未启用" ;;
    esac

    step "接口自检"
    local code
    code="$(curl -s -o /dev/null -w '%{http_code}' -m 3 "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null)"
    if [ "$code" = "200" ]; then ok "HTTP $code"; else warn "无响应（$code）"; fi
    print_access
}

# ------------------------------------------------------------------ 菜单

menu() {
    while true; do
        banner
        printf '\n  %s请选择操作：%s\n\n' "$C_BOLD" "$C_RESET"
        printf '    %s1)%s 部署项目\n' "$C_BOLD" "$C_RESET"
        printf '    %s2)%s 卸载项目\n' "$C_BOLD" "$C_RESET"
        printf '    %s3)%s 退出\n' "$C_BOLD" "$C_RESET"
        printf '\n  %s（另有：./deploy.sh status 查看运行状态）%s\n' "$C_DIM" "$C_RESET"
        printf '\n  请输入 [1-3]: '
        read -r choice
        case "$choice" in
            1) do_install; printf '\n  按回车返回菜单...'; read -r _ ;;
            2) do_uninstall ask; printf '\n  按回车返回菜单...'; read -r _ ;;
            3|q|quit|exit) printf '\n  再见。\n\n'; exit 0 ;;
            *) printf '\n  %s无效选择，请输入 1、2 或 3%s\n' "$C_YELLOW" "$C_RESET"; sleep 1 ;;
        esac
    done
}

usage() {
    banner
    cat <<EOF

  ${C_BOLD}用法${C_RESET}
    ./deploy.sh              交互菜单（部署 / 卸载 / 退出）
    ./deploy.sh install      直接部署
    ./deploy.sh uninstall    直接卸载（会询问是否保留配置）
    ./deploy.sh purge        直接卸载并删除全部配置与数据
    ./deploy.sh status       查看运行状态与自检
    ./deploy.sh help         显示本帮助

  ${C_BOLD}环境变量${C_RESET}
    SLH_PREFIX   安装目录      默认 /opt/$APP
    SLH_PORT     监听端口      默认 8090
    SLH_CONF     配置文件      默认 /etc/$APP.conf
    SLH_NO_DEPS  =1 不自动安装编译依赖

EOF
}

# ------------------------------------------------------------------ 入口

case "${1:-}" in
    install)   do_install ;;
    uninstall) do_uninstall ask ;;
    purge)     do_uninstall yes ;;
    status)    do_status ;;
    help|-h|--help) usage ;;
    "")        menu ;;
    *)         err "未知参数：$1"; usage; exit 2 ;;
esac
