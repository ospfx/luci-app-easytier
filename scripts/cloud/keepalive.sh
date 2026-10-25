#!/bin/bash
# 云开发环境保活循环
#
# 平台策略：CNB 云原生开发环境空闲约 10 分钟会自动回收，且回收后 /workspace
# 是全新容器（工具链、SDK、构建产物全部丢失）。因此保活必须做两件事：
#   1) 维持常驻 SSH 主连接，让环境始终处于"在用"状态（见 keepalive_once.sh）
#   2) 容器真的被重置时，自动补装工具链，使后续构建无需人工介入
#
# 用法（后台常驻）:
#   scripts/cloud/keepalive.sh &
#   ET_KEEPALIVE_INTERVAL=120 scripts/cloud/keepalive.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
INTERVAL="${ET_KEEPALIVE_INTERVAL:-120}"
LOG="$CLOUD/keepalive.log"
mkdir -p "$CLOUD"

printf '%s keepalive 循环启动，间隔 %ss\n' "$(date '+%F %T')" "$INTERVAL" | tee -a "$LOG"

while true; do
    bash "$ROOT/scripts/cloud/keepalive_once.sh" || \
        printf '%s 本轮保活失败\n' "$(date '+%F %T')" >> "$LOG"
    sleep "$INTERVAL"
done
