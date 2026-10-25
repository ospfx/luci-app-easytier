#!/bin/bash
# 同步本地工作树到 CNB 云开发环境并编译（本地绝不编译）
#
# 用法:
#   scripts/cloud/cc.sh                 # SDK 交叉编译全部对象（make objs）
#   scripts/cloud/cc.sh --link          # 交叉编译并链接两个二进制
#   scripts/cloud/cc.sh --host          # 用宿主 gcc（+ 宿主 libubox/ubus/uci）编译
#   scripts/cloud/cc.sh --host --link   # 宿主编译并链接（用于本地单测/联调）
#   ET_BUILD_ID=foo scripts/cloud/cc.sh # 指定独立构建目录（多 agent 并行防冲突）
#
# 退出码 = 远端 make 的退出码。
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLOUD="$ROOT/.cloud"
MODE="cross"
DO_LINK=0

for a in "$@"; do
    case "$a" in
        --host) MODE="host" ;;
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
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts" -o ConnectTimeout=20 -o ServerAliveInterval=30)
[ -f "$KEY" ] && SSH_OPTS+=(-i "$KEY")

# 交叉编译器路径（由 setup.sh 安装）
CROSS_CC="/workspace/.toolchain/sdk/staging_dir/toolchain-x86_64_gcc-13.3.0_musl/bin/x86_64-openwrt-linux-musl-gcc"
CROSS_SYSROOT="/workspace/.toolchain/sdk/staging_dir/target-x86_64_musl"
HOST_PREFIX="/workspace/.toolchain/host"

echo "[cc] 同步工作树 -> $REMOTE_SRC (build-id=$BUILD_ID)"
tar czf - -C "$ROOT" \
    --exclude=.git --exclude=.cloud --exclude=build --exclude=.toolchain \
    --exclude='*.o' . 2>/dev/null \
  | ssh "${SSH_OPTS[@]}" "$ET_SSH" "mkdir -p '$REMOTE_SRC' && rm -rf '$REMOTE_SRC'/* && tar xzf - -C '$REMOTE_SRC'" || {
    echo "[cc] 同步失败（云环境可能已回收，见 scripts/cloud/start.sh）" >&2
    exit 1
}

if [ "$MODE" = "cross" ]; then
    REMOTE_CMD="cd '$REMOTE_SRC/src' && make -j8 $([ $DO_LINK -eq 1 ] && echo link || echo objs) \
        CC='$CROSS_CC' \
        CFLAGS='-O2 -g -std=gnu11 -D_GNU_SOURCE -Iinclude -I$CROSS_SYSROOT/usr/include -Wall -Wextra -Werror -Wno-unused-parameter' \
        LDFLAGS='-L$CROSS_SYSROOT/usr/lib' \
        STAGING='$CROSS_SYSROOT/usr'"
else
    REMOTE_CMD="cd '$REMOTE_SRC/src' && PATH='$HOST_PREFIX/bin:$PATH' PKG_CONFIG_PATH='$HOST_PREFIX/lib/pkgconfig' \
        make -j8 $([ $DO_LINK -eq 1 ] && echo link || echo objs) \
        CC='gcc' \
        CFLAGS='-O1 -g -std=gnu11 -D_GNU_SOURCE -Iinclude -I$HOST_PREFIX/include -Wall -Wextra -Werror -Wno-unused-parameter' \
        LDFLAGS='-L$HOST_PREFIX/lib -Wl,-rpath,$HOST_PREFIX/lib' \
        STAGING='$HOST_PREFIX'"
fi

ssh "${SSH_OPTS[@]}" "$ET_SSH" "$REMOTE_CMD"
rc=$?
if [ $rc -eq 0 ]; then
    echo "[cc] OK ($MODE$([ $DO_LINK -eq 1 ] && echo ', linked' || echo ''))"
else
    echo "[cc] 编译失败 (rc=$rc)" >&2
fi
exit $rc
