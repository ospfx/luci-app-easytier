#!/bin/bash
# 在云开发环境里用 OpenWrt SDK 交叉编译并打包 ipk
#
# 用法（本地执行，脚本自己同步 + 远端构建）:
#   scripts/cloud/build.sh              # 只编 easytierd 包
#   scripts/cloud/build.sh --luci       # 连带 luci-app-easytier 包（需要 feeds/luci）
#   scripts/cloud/build.sh --clean      # 先 clean
#
# 说明：远端脚本通过 stdin 灌给 bash -s 执行，避免多层引号把脚本内容吃掉。
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
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$CLOUD/known_hosts"
          -o ConnectTimeout=20 -o ServerAliveInterval=30 -o BatchMode=yes)
[ -f "$KEY" ] && SSH_OPTS+=(-i "$KEY")

REMOTE_REPO="/workspace/.build/pkg/src"

echo "[build] 预检云环境工具链..."
# 容器被平台回收后 /workspace 会保留但 apt 安装的依赖会丢失，两项都要检查
if ! ssh "${SSH_OPTS[@]}" "$ET_SSH" \
      "test -x /workspace/.toolchain/sdk/scripts/feeds && test -f /usr/include/ncurses.h && command -v gawk >/dev/null" 2>/dev/null; then
    echo "[build] 底座缺失，自动执行 setup.sh ..."
    bash "$ROOT/scripts/cloud/ssh.sh" -s < "$ROOT/scripts/cloud/setup.sh" 2>&1 | tail -5
fi

echo "[build] 同步工作树 -> $REMOTE_REPO"
# 注意：不要用 mktemp —— 在 Git Bash 下 TMPDIR 可能是 Windows 路径，tar 写不进去（0 字节）
TGZ="$CLOUD/build_src.tgz"
rm -f "$TGZ"
tar czf "$TGZ" -C "$ROOT" --exclude=.git --exclude=.cloud --exclude=build \
    --exclude='*.o' --owner=0 --group=0 --numeric-owner . 2>/dev/null
tsz=$(wc -c < "$TGZ")
echo "[build] 归档 ${tsz} 字节"
[ "$tsz" -gt 0 ] || { echo "[build] 归档失败" >&2; exit 1; }
ssh "${SSH_OPTS[@]}" "$ET_SSH" \
    "rm -rf '$REMOTE_REPO'; mkdir -p '$REMOTE_REPO'; tar xzf - --no-same-owner --no-same-permissions -C '$REMOTE_REPO' 2>/tmp/etsync.err; echo SYNC_RC=\$?; head -5 /tmp/etsync.err; ls '$REMOTE_REPO' | wc -l" \
    < "$TGZ" 2>&1 | grep -vE "post-quantum|store now|may need to be upgraded|^\*\*"

# 远端构建脚本（用 stdin 传送，$ 原样保留）
RSH="$CLOUD/remote_build.sh"
ROUT="$CLOUD/remote_build.log"
{
cat <<'EOS'
set -e
SDK=/workspace/.toolchain/sdk
REPO=__REPO__
cd "$SDK"

# SDK 的 staging_dir/target-* 是空的，所有依赖必须从 feeds 装进来现编
echo "[remote] 准备 feeds"
if [ ! -f feeds.conf ]; then
    cat > feeds.conf <<'FEOF'
src-git packages https://git.openwrt.org/feed/packages.git
src-git base https://git.openwrt.org/openwrt/openwrt.git;openwrt-24.10
src-git luci https://git.openwrt.org/project/luci.git;openwrt-24.10
FEOF
fi
for f in base packages luci; do
    [ -d "feeds/$f" ] || ./scripts/feeds update "$f" >/dev/null 2>&1
done
for p in libuci libjson-c; do
    ./scripts/feeds install "$p" >/dev/null 2>&1
done
if [ "__WITH_LUCI__" = "1" ]; then
    ./scripts/feeds install luci-base luci-compat >/dev/null 2>&1
    echo "[remote] luci feed: $(ls package/feeds/luci 2>/dev/null | wc -l) 个包"
fi
# feeds install 落在 package/feeds/<feed>/<源目录名>，libuci 的源目录叫 uci
echo "[remote] 已安装依赖: $(ls package/feeds/base 2>/dev/null | tr '\n' ' ')"
[ -d package/feeds/base/uci ] && [ -d package/feeds/base/libjson-c ] || {
    echo "[remote] 依赖安装失败" >&2; exit 1; }

echo "[remote] 链接包目录"
rm -f package/easytierd package/luci-app-easytier
ln -sfn "$REPO/easytierd" package/easytierd
if [ "__WITH_LUCI__" = "1" ]; then
    ln -sfn "$REPO/luci-app-easytier" package/luci-app-easytier
    if [ ! -f feeds/luci/luci.mk ]; then
        echo "[remote] 拉取 luci feed..."
        ./scripts/feeds update luci >/dev/null 2>&1 || echo "[remote] feeds update 失败（继续）"
    fi
fi

if [ ! -f .config ]; then
    echo "[remote] 生成 .config（defconfig）"
    make defconfig
fi

if [ "__DO_CLEAN__" = "1" ]; then
    echo "[remote] clean easytierd"
    make package/easytierd/clean >/dev/null 2>&1 || true
fi

echo "[remote] 开始编译 easytierd（首次会先构建 libubox/ubus/uci/json-c 等依赖，需数分钟）"
set +e
make package/easytierd/compile V=s -j"$(nproc)" > /tmp/etbuild.log 2>&1
mrc=$?
set -e
echo "[remote] --- make 输出尾部 ---"
tail -60 /tmp/etbuild.log
echo "[remote] MAKE_RC=$mrc"
[ "$mrc" -eq 0 ] || exit "$mrc"

if [ "__WITH_LUCI__" = "1" ]; then
    echo "[remote] 编译 luci-app-easytier"
    set +e
    make package/luci-app-easytier/compile V=s -j"$(nproc)" > /tmp/etbuild_luci.log 2>&1
    lrc=$?
    set -e
    tail -30 /tmp/etbuild_luci.log
    echo "[remote] LUCI_MAKE_RC=$lrc"
    [ "$lrc" -eq 0 ] || exit "$lrc"
fi

echo "[remote] 产物："
find bin -name 'easytierd*.ipk' -o -name 'luci-app-easytier*.ipk' 2>/dev/null | head
EOS
} | sed -e "s|__REPO__|$REMOTE_REPO|" \
      -e "s|__WITH_LUCI__|$WITH_LUCI|" \
      -e "s|__DO_CLEAN__|$DO_CLEAN|" > "$RSH"
wc -l < "$RSH" | xargs echo "[build] 远端脚本行数:"

ssh "${SSH_OPTS[@]}" "$ET_SSH" 'bash -s' < "$RSH" > "$ROUT" 2>&1
rc=$?
rm -f "$RSH"
grep -vE "post-quantum|store now|may need to be upgraded|^\*\*" "$ROUT"
rm -f "$ROUT"
[ $rc -eq 0 ] && echo "[build] OK" || { echo "[build] 失败 (rc=$rc)" >&2; }
exit $rc
