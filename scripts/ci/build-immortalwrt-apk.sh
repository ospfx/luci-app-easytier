#!/bin/bash
# 构建 SR1010 / ImmortalWrt 25.12.1 / aarch64_cortex-a53 的 APK 包
# 用法: bash scripts/ci/build-immortalwrt-apk.sh
# 输出: out/*.apk
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/out"
WORK="$ROOT/.ci-work"
VER="25.12.1"
TARGET="mediatek/filogic"
ARCH="aarch64_cortex-a53"
SDK_NAME="immortalwrt-sdk-${VER}-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64"
SDK_TARBALL="${SDK_NAME}.tar.zst"
SDK_DIR="$WORK/sdk"

mkdir -p "$OUT" "$WORK"

log() { echo "[sr1010-apk] $*"; }

# ---------- 宿主依赖 ----------
need=""
for c in gcc g++ make cmake zstd rsync unzip python3 gawk git curl tar; do
  command -v "$c" >/dev/null 2>&1 || need="$need $c"
done
[ -f /usr/include/ncurses.h ] || need="$need libncurses-dev"
[ -f /usr/include/json-c/json.h ] || need="$need libjson-c-dev"
[ -f /usr/include/openssl/ssl.h ] || need="$need libssl-dev"
[ -f /usr/include/zlib.h ] || need="$need zlib1g-dev"
if [ -n "$need" ]; then
  log "安装依赖:$need"
  export DEBIAN_FRONTEND=noninteractive
  export TZ=UTC
  SUDO=""
  [ "$(id -u)" -eq 0 ] || SUDO="sudo"
  $SUDO apt-get update
  # CNB 精简镜像没有 tzdata；/etc/localtime 是 bind mount，postinst 的 mv 会失败。
  # 先下载 deb、替换 postinst 为 exit 0，再安装；GitHub runner 已有 tzdata 则跳过。
  if ! dpkg -s tzdata 2>/dev/null | grep -q '^Status: install ok installed'; then
    rm -rf /tmp/tzfix; mkdir -p /tmp/tzfix; cd /tmp/tzfix
    $SUDO apt-get download tzdata || true
    if ls tzdata_*.deb >/dev/null 2>&1; then
      dpkg-deb -R tzdata_*.deb tzdata
      printf '#!/bin/sh\nexit 0\n' > tzdata/DEBIAN/postinst
      chmod 755 tzdata/DEBIAN/postinst
      dpkg-deb -b tzdata tzdata-fixed.deb
      $SUDO dpkg -i tzdata-fixed.deb || true
    fi
    $SUDO apt-mark hold tzdata || true
    cd "$ROOT"
    $SUDO dpkg --configure -a || true
  fi
  $SUDO apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git curl wget ca-certificates \
    zstd xz-utils unzip zip rsync file gawk \
    libncurses-dev libjson-c-dev libssl-dev zlib1g-dev python3 || {
      echo "apt install failed, retrying with -f"; $SUDO apt-get -f install -y || true; $SUDO dpkg --configure -a || true;
      $SUDO apt-get install -y --no-install-recommends build-essential cmake pkg-config git curl wget ca-certificates zstd xz-utils unzip zip rsync file gawk libncurses-dev libjson-c-dev libssl-dev zlib1g-dev python3;
    }
fi

# ---------- SDK ----------
if [ ! -x "$SDK_DIR/scripts/feeds" ]; then
  rm -rf "$SDK_DIR"; mkdir -p "$SDK_DIR"
  BASES=(
    "https://downloads.immortalwrt.org/releases/$VER/targets/$TARGET"
    "https://mirror.nju.edu.cn/immortalwrt/releases/$VER/targets/$TARGET"
    "https://mirrors.ustc.edu.cn/immortalwrt/releases/$VER/targets/$TARGET"
    "https://mirrors.tuna.tsinghua.edu.cn/immortalwrt/releases/$VER/targets/$TARGET"
  )
  ok=0
  for base in "${BASES[@]}"; do
    url="$base/$SDK_TARBALL"
    log "下载 $url"
    if curl -fL --retry 3 --retry-delay 3 --connect-timeout 30 -o "$WORK/$SDK_TARBALL" "$url"; then
      ok=1; break
    fi
  done
  [ "$ok" = 1 ] || { log "SDK 下载失败"; exit 1; }
  log "解压 SDK"
  tar --zstd -xf "$WORK/$SDK_TARBALL" -C "$SDK_DIR" --strip-components=1
  rm -f "$WORK/$SDK_TARBALL"
fi
[ -x "$SDK_DIR/scripts/feeds" ] || { log "SDK 不完整"; exit 1; }

cd "$SDK_DIR"
[ -f feeds.conf ] || cp feeds.conf.default feeds.conf
log "feeds.conf:"; cat feeds.conf

log "更新 feeds"
./scripts/feeds update -a
log "安装依赖包"
./scripts/feeds install libuci libjson-c luci-base luci-compat

log "链接仓库包"
for p in easytierd easytier easytier-noweb luci-app-easytier; do
  rm -f "package/$p"
  ln -sfn "$ROOT/$p" "package/$p"
done

log "生成 .config"
make defconfig

log "编译 libuci/libjson-c"
make -j"$(nproc)" package/feeds/base/uci/compile package/feeds/base/libjson-c/compile V=s

log "编译 easytierd"
make -j"$(nproc)" package/easytierd/compile V=s

log "编译 luci-app-easytier"
make -j"$(nproc)" package/luci-app-easytier/compile V=s

log "编译官方核心包 easytier / easytier-noweb"
make -j"$(nproc)" package/easytier/compile package/easytier-noweb/compile V=s || log "核心包编译失败（可忽略，继续收集已有包）"

log "收集 APK"
find bin/packages -type f -name '*.apk' | sort | while read -r f; do
  cp -v "$f" "$OUT/"
done
log "产物:"
ls -l "$OUT"
[ -n "$(find "$OUT" -name '*.apk' -print -quit)" ] || { log "没有 APK 产物"; exit 1; }
