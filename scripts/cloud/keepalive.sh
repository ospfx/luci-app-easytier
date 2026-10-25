#!/bin/bash
# 云开发环境保活
#
# CNB 云原生开发环境 10 分钟无活动会自动停止。本脚本周期性发心跳，
# 并在检测到环境已停止时自动重新拉起 + 刷新 .cloud/ssh.env + 必要时补装底座。
#
# 用法（后台常驻）:
#   scripts/cloud/keepalive.sh &            # 默认 240s 心跳
#   ET_KEEPALIVE_INTERVAL=180 scripts/cloud/keepalive.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
INTERVAL="${ET_KEEPALIVE_INTERVAL:-240}"
LOG="$CLOUD/keepalive.log"
mkdir -p "$CLOUD"

log() { printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$LOG"; }

branch_of() {
    if [ -f "$CLOUD/ssh.env" ]; then
        # shellcheck disable=SC1090
        . "$CLOUD/ssh.env"
        printf '%s' "${ET_BRANCH:-main}"
    else
        printf 'main'
    fi
}

ping_env() {
    timeout 60 bash "$ROOT/scripts/cloud/ssh.sh" "date +%s" >/dev/null 2>&1
}

log "keepalive 启动，间隔 ${INTERVAL}s"
while true; do
    if ping_env; then
        log "心跳 OK"
    else
        log "环境无响应，尝试重新拉起 (branch=$(branch_of))"
        if CNB_TOKEN="$(cat "$CLOUD/token" 2>/dev/null)" \
            bash "$ROOT/scripts/cloud/start.sh" "$(branch_of)" >>"$LOG" 2>&1; then
            log "已重新拉起"
            # 底座丢失时补装（幂等，已存在则秒过）
            if ! timeout 120 bash "$ROOT/scripts/cloud/ssh.sh" "test -x /workspace/.toolchain/host/sbin/ubusd" >/dev/null 2>&1; then
                log "底座缺失，重跑 setup.sh"
                timeout 900 bash "$ROOT/scripts/cloud/ssh.sh" -s < "$ROOT/scripts/cloud/setup.sh" >>"$LOG" 2>&1
            fi
        else
            log "拉起失败，等待下一轮"
        fi
    fi
    sleep "$INTERVAL"
done
