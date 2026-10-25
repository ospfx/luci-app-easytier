#!/bin/bash
# 在 CNB 云原生开发环境里执行命令：scripts/cloud/ssh.sh "<远端命令>"
# 或把 stdin 作为脚本灌进去：scripts/cloud/ssh.sh -s < local.sh
#
# 环境被回收（10 分钟闲置）时自动重新拉起一次再重试。
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"

if [ ! -f "$CLOUD/ssh.env" ]; then
    echo "[ssh] 没有 $CLOUD/ssh.env，先执行 scripts/cloud/start.sh" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$CLOUD/ssh.env"

KEY="${ET_SSH_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts"
          -o ConnectTimeout=20 -o ServerAliveInterval=30 -o BatchMode=yes)
[ -f "$KEY" ] && SSH_OPTS+=(-i "$KEY")

run_once() {
    if [ "${1:-}" = "-s" ]; then
        ssh "${SSH_OPTS[@]}" "$ET_SSH" 'bash -s'
    else
        ssh "${SSH_OPTS[@]}" "$ET_SSH" "$@"
    fi
}

if [ "${1:-}" = "-s" ]; then
    # stdin 只能消费一次，先落地到临时文件
    tmp="$(mktemp)"
    cat > "$tmp"
    if run_once -s < "$tmp"; then rc=0; else rc=$?; fi
    if [ $rc -ne 0 ]; then
        echo "[ssh] 失败（rc=$rc），尝试重新拉起环境后重试..." >&2
        if CNB_TOKEN="$(cat "$CLOUD/token" 2>/dev/null)" \
            bash "$ROOT/scripts/cloud/start.sh" "${ET_BRANCH:-main}" >/dev/null 2>&1; then
            # shellcheck disable=SC1090
            . "$CLOUD/ssh.env"
            SSH_OPTS[3]="-o UserKnownHostsFile=$CLOUD/known_hosts"
            if run_once -s < "$tmp"; then rc=0; else rc=$?; fi
        fi
    fi
    rm -f "$tmp"
    exit $rc
fi

if run_once "$@"; then
    exit 0
fi
rc=$?
echo "[ssh] 失败（rc=$rc），尝试重新拉起环境后重试..." >&2
if CNB_TOKEN="$(cat "$CLOUD/token" 2>/dev/null)" \
    bash "$ROOT/scripts/cloud/start.sh" "${ET_BRANCH:-main}" >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    . "$CLOUD/ssh.env"
    exec ssh "${SSH_OPTS[@]}" "$ET_SSH" "$@"
fi
exit $rc
