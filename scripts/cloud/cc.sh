#!/bin/bash
# 同步本地工作树到 CNB 云开发环境并编译（本地绝不编译）
#
# 用法:
#   scripts/cloud/cc.sh                 # 宿主 gcc 编译（快，可链接可直接跑单测）—— 默认
#   scripts/cloud/cc.sh --link          # 宿主编译并链接 easytierd / easytierctl
#   scripts/cloud/cc.sh --cross         # OpenWrt 交叉 gcc 编译（仅语法/头文件验证）
#   scripts/cloud/cc.sh --cross --link  # 交叉编译并链接（需 SDK staging 已装依赖）
#   ET_BUILD_ID=foo scripts/cloud/cc.sh # 独立构建目录，多 agent 并行防冲突
#
# 注意：
#  - 宿主模式依赖 /workspace/.toolchain/host（libubox/ubus/uci 源码构建）+ Debian 的 json-c
#  - 交叉模式的 SDK staging 默认是空的（依赖包未构建），仅用于头文件/语法验证；
#    权威构建请用 scripts/cloud/build.sh（走 OpenWrt 包系统，自动构建依赖）
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
MODE="host"
DO_LINK=0

for a in "$@"; do
    case "$a" in
        --host) MODE="host" ;;
        --cross) MODE="cross" ;;
        --link) DO_LINK=1 ;;
        *) echo "[cc] 未知参数: $a" >&2; exit 2 ;;
    esac
done

if [ ! -f "$CLOUD/ssh.env" ]; then
    echo "[cc] 缺少 $CLOUD/ssh.env，先跑 scripts/cloud/start.sh" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$CLOUD/ssh.env"

BUILD_ID="${ET_BUILD_ID:-main}"
REMOTE_SRC="/workspace/.build/$BUILD_ID/src"
KEY="${ET_SSH_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts"
          -o ConnectTimeout=20 -o ServerAliveInterval=30 -o BatchMode=yes)
[ -f "$KEY" ] && SSH_OPTS+=(-i "$KEY")

TC=/workspace/.toolchain
CROSS_CC="$TC/sdk/staging_dir/toolchain-x86_64_gcc-13.3.0_musl/bin/x86_64-openwrt-linux-musl-gcc"
CROSS_ROOT="$TC/${ET_SDK_DIR:-sdk}/staging_dir/target-x86_64_musl"
HOST="$TC/host"
TARGET="$([ $DO_LINK -eq 1 ] && echo link || echo objs)"

echo "[cc] 预检云环境工具链..."
if ! ssh "${SSH_OPTS[@]}" "$ET_SSH" "command -v gcc >/dev/null && command -v make >/dev/null" 2>/dev/null; then
    echo "[cc] 工具链缺失（环境被回收后是全新容器），自动执行 setup.sh ..."
    bash "$ROOT/scripts/cloud/ssh.sh" -s < "$ROOT/scripts/cloud/setup.sh" 2>&1 | tail -6
fi

echo "[cc] 同步 -> $REMOTE_SRC (build-id=$BUILD_ID, mode=$MODE, target=$TARGET)"
tar czf - -C "$ROOT" \
    --exclude=.git --exclude=.cloud --exclude=build --exclude=.toolchain \
    --exclude='*.o' --owner=0 --group=0 --numeric-owner . 2>/dev/null \
  | ssh "${SSH_OPTS[@]}" "$ET_SSH" \
        "mkdir -p '$REMOTE_SRC' && rm -rf '$REMOTE_SRC'/* && tar xzf - --no-same-owner --no-same-permissions -C '$REMOTE_SRC'" \
  || { echo "[cc] 同步失败（云环境可能已回收，见 scripts/cloud/start.sh）" >&2; exit 1; }

if [ "$MODE" = "cross" ]; then
    # 头文件优先取 SDK staging，回退宿主构建的头（staging 常为空）
    REMOTE_CMD="cd '$REMOTE_SRC/src' && export STAGING_DIR='$TC/sdk/staging_dir' && \
make -j8 $TARGET CC='$CROSS_CC' \
  CFLAGS='-O2 -g -std=gnu11 -D_GNU_SOURCE -Iinclude -I$CROSS_ROOT/usr/include -I$HOST/include -Wall -Wextra -Werror -Wno-unused-parameter' \
  LDFLAGS='-L$CROSS_ROOT/usr/lib -L$HOST/lib' STAGING='$CROSS_ROOT/usr'"
else
    REMOTE_CMD="cd '$REMOTE_SRC/src' && PATH='$HOST/bin:$PATH' PKG_CONFIG_PATH='$HOST/lib/pkgconfig' \
make -j8 $TARGET CC='gcc' \
  CFLAGS='-O1 -g -std=gnu11 -D_GNU_SOURCE -Iinclude -I$HOST/include -Wall -Wextra -Werror -Wno-unused-parameter' \
  LDFLAGS='-L$HOST/lib -Wl,-rpath,$HOST/lib' STAGING='$HOST'"
fi

ssh "${SSH_OPTS[@]}" "$ET_SSH" "$REMOTE_CMD" 2>&1 | grep -vE "post-quantum|store now|may need to be upgraded|^\*\*"
rc=${PIPESTATUS[0]}
if [ "$rc" -eq 0 ]; then
    echo "[cc] OK ($MODE, $TARGET)"
else
    echo "[cc] 编译失败 (rc=$rc)" >&2
fi
exit $rc
