#!/bin/bash
# 启动（或复用）CNB 云原生开发环境，并把 SSH 目标写入 .cloud/ssh.env
# 用法: scripts/cloud/start.sh [branch]   （默认 main）
set -u

REPO="${ET_REPO:-ospfx/luci-app-easytier}"
BRANCH="${1:-main}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
mkdir -p "$CLOUD"

TOKEN="${CNB_TOKEN:-}"
if [ -z "$TOKEN" ] && [ -f "$CLOUD/token" ]; then
    TOKEN="$(tr -d ' \r\n' < "$CLOUD/token")"
fi
if [ -z "$TOKEN" ]; then
    echo "[start] ERROR: 缺少 CNB_TOKEN（环境变量或 .cloud/token）" >&2
    exit 1
fi

API="https://api.cnb.cool"
HDR=(-H "Authorization: Bearer $TOKEN" -H "Accept: application/vnd.cnb.api+json" -H "Content-Type: application/json")

echo "[start] repo=$REPO branch=$BRANCH"
RESP="$(curl -s -m 60 -X POST "$API/$REPO/-/workspace/start" "${HDR[@]}" -d "{\"branch\":\"$BRANCH\"}")"
SN="$(printf '%s' "$RESP" | sed -n 's/.*"sn":"\([^"]*\)".*/\1/p')"
if [ -z "$SN" ]; then
    echo "[start] 启动失败: $RESP" >&2
    exit 1
fi
echo "[start] sn=$SN"

for i in $(seq 1 60); do
    DETAIL="$(curl -s -m 30 "$API/$REPO/-/workspace/detail/$SN" "${HDR[@]}")"
    SSH_TARGET="$(printf '%s' "$DETAIL" | sed -n 's/.*"remoteSsh":"\([^"]*\)".*/\1/p')"
    if [ -n "$SSH_TARGET" ]; then
        cat > "$CLOUD/ssh.env" <<EOF
ET_SN=$SN
ET_SSH=$SSH_TARGET
ET_BRANCH=$BRANCH
EOF
        echo "[start] OK"
        echo "ET_SN=$SN"
        echo "ET_SSH=$SSH_TARGET"
        exit 0
    fi
    sleep 5
done

echo "[start] 超时：环境未就绪，sn=$SN" >&2
exit 1
