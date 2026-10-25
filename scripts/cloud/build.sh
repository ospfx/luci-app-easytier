#!/bin/bash
# 在云开发环境里用 OpenWrt SDK 交叉编译并打包 ipk
#
# 用法（在本地执行，脚本自己同步+远程构建）:
#   scripts/cloud/build.sh              # 只编 easytierd 包
#   scripts/cloud/build.sh --luci       # 连带 luci-app-easytier 包（需要 feeds/luci）
#   scripts/cloud/build.sh --clean      # 先 clean
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
WITH_LUCI=0
DO_CLEAN=0
for a in "$@"; do
    case "$a" in
        --luci) WITH_LUCI=1 ;;
        --clean) DO_CLEAN=1 ;;
        *) echo "[build] 未知参数: $a" >&2; exit 2 ;;
    esac
done

[ -f "$CLOUD/ssh.env" ] || { echo "[build] 缺少 .cloud/ssh.env" >&2; exit 1; }
# shellcheck disable=SC1090
. "$CLOUD/ssh.env"
KEY="${ET_SSH_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts" -o ConnectTimeout=20 -o ServerAliveInterval=30)
[ -f "$KEY" ] && SSH_OPTS+=(-i "$KEY")

REMOTE_REPO="/workspace/.build/pkg/src"
echo "[build] 同步工作树 -> $REMOTE_REPO"
tar czf - -C "$ROOT" --exclude=.git --exclude=.cloud --exclude=build \
    --exclude='*.o' . 2>/dev/null \
  | ssh "${SSH_OPTS[@]}" "$ET_SSH" "mkdir -p '$REMOTE_REPO' && rm -rf '$REMOTE_REPO'/* && tar xzf - -C '$REMOTE_REPO'" || exit 1

REMOTE="set -e
SDK=/workspace/.toolchain/sdk
REPO=$REMOTE_REPO
cd \$SDK
# 包目录软链进 SDK（幂等）
rm -f package/easytierd package/luci-app-easytier
ln -sfn \$REPO/easytierd package/easytierd
$([ $WITH_LUCI -eq 1 ] && echo 'ln -sfn $REPO/luci-app-easytier package/luci-app-easytier')
$([ $WITH_LUCI -eq 1 ] && echo 'if [ ! -f feeds/luci/luci.mk ]; then ./scripts/feeds update -a >/dev/null 2>&1 || true; fi')
[ -f .config ] || make defconfig >/dev/null
$([ $DO_CLEAN -eq 1 ] && echo 'make package/easytierd/clean V=s >/dev/null 2>&1 || true')
make package/easytierd/compile V=s -j8 2>&1 | tail -40
$([ $WITH_LUCI -eq 1 ] && echo 'make package/luci-app-easytier/compile V=s -j8 2>&1 | tail -30')
echo '--- 产物 ---'
find bin -name 'easytierd*.ipk' -o -name 'luci-app-easytier*.ipk' | head
"

ssh "${SSH_OPTS[@]}" "$ET_SSH" "$REMOTE"
