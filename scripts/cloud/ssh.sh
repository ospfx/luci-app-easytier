#!/bin/bash
# 在 CNB 云原生开发环境里执行命令：scripts/cloud/ssh.sh "<远端命令>"
# 或把 stdin 作为脚本灌进去：scripts/cloud/ssh.sh -s < local.sh
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
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts" -o ConnectTimeout=20 -o ServerAliveInterval=30)
if [ -f "$KEY" ]; then
    SSH_OPTS+=(-i "$KEY")
fi

if [ "${1:-}" = "-s" ]; then
    ssh "${SSH_OPTS[@]}" "$ET_SSH" 'bash -s'
else
    ssh "${SSH_OPTS[@]}" "$ET_SSH" "$@"
fi
