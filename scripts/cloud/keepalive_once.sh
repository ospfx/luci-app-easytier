#!/bin/bash
# 云开发环境保活（单次执行，供循环或外部调度调用）
#
# 手段：维持一条常驻的 SSH 主连接（ssh -M -N -f）。相比每次发一条命令就断开，
# 长会话更能让平台判定"环境在用"，同时把后续命令复用同一连接（快得多）。
# 每次执行还会校验工具链，缺失则自动补装（容器被重置后 /workspace 会被清空）。
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
LOG="$CLOUD/keepalive.log"
CM="$CLOUD/cm.sock"
mkdir -p "$CLOUD"

log() { printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$LOG"; }

[ -f "$CLOUD/ssh.env" ] || { log "缺少 ssh.env，先 start.sh"; exit 1; }
# shellcheck disable=SC1090
. "$CLOUD/ssh.env"

KEY="${ET_SSH_KEY:-$HOME/.ssh/id_ed25519}"
BASE=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts"
      -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=20
      -o ServerAliveCountMax=3 -o ControlPath="$CM")
[ -f "$KEY" ] && BASE+=(-i "$KEY")

restart_env() {
    log "环境不可用，重新拉起 (branch=${ET_BRANCH:-main})"
    CNB_TOKEN="$(cat "$CLOUD/token" 2>/dev/null)" \
        bash "$ROOT/scripts/cloud/start.sh" "${ET_BRANCH:-main}" >>"$LOG" 2>&1 || return 1
    # shellcheck disable=SC1090
    . "$CLOUD/ssh.env"
    log "已重新拉起 sn=$ET_SN"
}

ensure_master() {
    # 已有可用主连接？
    if ssh "${BASE[@]}" -O check "$ET_SSH" >/dev/null 2>&1; then
        return 0
    fi
    rm -f "$CM" 2>/dev/null
    ssh "${BASE[@]}" -M -N -f "$ET_SSH" >/dev/null 2>&1
}

ensure_toolchain() {
    if ! ssh "${BASE[@]}" "$ET_SSH" "test -x /workspace/.toolchain/host/sbin/ubusd && command -v gcc >/dev/null" >/dev/null 2>&1; then
        log "工具链缺失（容器被重置），重跑 setup.sh"
        bash "$ROOT/scripts/cloud/ssh.sh" -s < "$ROOT/scripts/cloud/setup.sh" >>"$LOG" 2>&1 \
            && log "setup 完成" || log "setup 失败"
    fi
}

# 一次完整检查：主连接 → 心跳 → 必要时拉起环境 → 必要时补工具链
if ensure_master && ssh "${BASE[@]}" "$ET_SSH" "date +%s" >/dev/null 2>&1; then
    log "心跳 OK（主连接存活）"
else
    if restart_env && ensure_master && ssh "${BASE[@]}" "$ET_SSH" "date +%s" >/dev/null 2>&1; then
        log "心跳 OK（重连后）"
    else
        log "心跳失败"
        exit 1
    fi
fi

ensure_toolchain
exit 0
