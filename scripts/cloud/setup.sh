#!/bin/bash
# 在 CNB 云原生开发环境内执行（幂等）：准备交叉编译 SDK 与宿主联调依赖
# 缓存目录 /workspace/.toolchain（随 workspace 持久化，可跨环境重建复用）
#
# 用法：scripts/cloud/ssh.sh -s < scripts/cloud/setup.sh
set -eu

TC=/workspace/.toolchain
SDK="$TC/sdk"
HOST="$TC/host"
SRC="$TC/src"
SDK_VER=24.10.0
SDK_ARCH=x86-64
# 实测文件名：openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64.tar.zst
# 注意：文件名中段含多个 '.'，正则必须用 [^"<>]* 吞掉，否则 .tar.zst 里的 tar 会变成字面量匹配失败
SDK_GLOB="openwrt-sdk-${SDK_VER}-${SDK_ARCH}_"'[^"<>]*\.tar\.zst'
# 官方源 downloads.openwrt.org 在云端会超时（国内网络），镜像优先
MIRRORS="
https://mirror.nju.edu.cn/openwrt
https://mirrors.ustc.edu.cn/openwrt
https://mirrors.tuna.tsinghua.edu.cn/openwrt
https://mirrors.aliyun.com/openwrt
https://mirrors.cloud.tencent.com/openwrt
https://downloads.openwrt.org
"

mkdir -p "$TC"
log() { echo "[setup] $*"; }

# ---------- 1. 基础编译工具 ----------
missing=""
for c in gcc g++ make cmake zstd pkg-config git curl tar; do
    command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
done
[ -f /usr/include/json-c/json.h ] || missing="$missing libjson-c-dev"
[ -f /usr/include/openssl/ssl.h ] || missing="$missing libssl-dev"
# OpenWrt SDK 的 prerequisites 检查项（容器被回收后 apt 包会丢，必须一并检测）
[ -f /usr/include/ncurses.h ] || missing="$missing libncurses-dev"
command -v gawk >/dev/null 2>&1 || missing="$missing gawk"
command -v python3 >/dev/null 2>&1 || missing="$missing python3"
command -v lua5.1 >/dev/null 2>&1 || missing="$missing lua5.1"

if [ -n "$missing" ]; then
    log "安装基础工具链（缺失:$missing）..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq 2>&1 | tail -2 || true
    apt-get install -y -qq --no-install-recommends \
        build-essential cmake pkg-config git curl wget ca-certificates \
        zstd xz-utils unzip zip rsync file gawk \
        libjson-c-dev libssl-dev zlib1g-dev libncurses-dev \
        python3 lua5.1 2>&1 | tail -3
else
    log "基础工具链已存在"
fi

# ---------- 2. OpenWrt SDK ----------
sdk_gcc() { ls "$SDK"/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc 2>/dev/null | head -1; }

if [ -n "$(sdk_gcc)" ] && [ -d "$SDK/staging_dir/target-x86_64_musl" ]; then
    log "SDK 已存在: $SDK"
else
    log "下载 OpenWrt $SDK_VER SDK ($SDK_ARCH)..."
    FILE=""
    for M in $MIRRORS; do
        BASE="$M/releases/$SDK_VER/targets/x86/64"
        CAND="$(curl -fsSL -m 30 "$BASE/" 2>/dev/null | grep -oE "$SDK_GLOB" | head -1 || true)"
        if [ -n "$CAND" ]; then
            FILE="$CAND"; SRCBASE="$BASE"; log "镜像可用: $M -> $FILE"; break
        fi
    done
    [ -n "$FILE" ] || { log "ERROR: 所有镜像都没列出 $SDK_GLOB"; exit 1; }

    PKG="$TC/$FILE"
    want_sha="$(curl -fsSL -m 30 "$SRCBASE/sha256sums" 2>/dev/null | awk -v f="$FILE" '($2==f)||($2=="*"f){print $1}' | head -1 || true)"
    if [ -s "$PKG" ] && [ -n "$want_sha" ] && [ "$(sha256sum "$PKG" | awk '{print $1}')" = "$want_sha" ]; then
        log "SDK 包已存在且校验通过: $(du -h "$PKG" | cut -f1)"
    else
        rm -f "$PKG"
        ok=0
        for M in $MIRRORS; do
            URL="$M/releases/$SDK_VER/targets/x86/64/$FILE"
            log "  下载 $URL"
            if curl -fL -m 1800 --retry 3 --retry-delay 2 -C - -o "$PKG" "$URL"; then
                if [ -z "$want_sha" ] || [ "$(sha256sum "$PKG" | awk '{print $1}')" = "$want_sha" ]; then
                    ok=1; break
                fi
                log "  校验失败，换下一个镜像"; rm -f "$PKG"
            fi
        done
        [ "$ok" = 1 ] || { log "ERROR: SDK 下载失败"; exit 1; }
    fi

    log "解压 SDK..."
    rm -rf "$SDK"; mkdir -p "$SDK"
    if ! tar --zstd -xf "$PKG" -C "$SDK" --strip-components=1 2>/tmp/sdk-tar.log; then
        log "  tar --zstd 失败（$(tail -1 /tmp/sdk-tar.log)），改用 zstd 管道"
        rm -rf "$SDK"; mkdir -p "$SDK"
        zstd -dc "$PKG" | tar -xf - -C "$SDK" --strip-components=1
    fi
    [ -n "$(sdk_gcc)" ] || { log "ERROR: SDK 解压后找不到交叉 gcc"; exit 1; }
    log "SDK 就绪: $SDK"
fi

# ---------- 3. 宿主联调依赖（libubox / ubus / uci），仅用于本机集成测试 ----------
if [ -f "$HOST/.setup-ok" ] && [ -x "$HOST/sbin/ubusd" ] && [ -x "$HOST/bin/ubus" ] && [ -x "$HOST/bin/uci" ]; then
    log "宿主依赖已存在: $HOST"
else
    log "从源码构建 libubox/ubus/uci（宿主，install=$HOST）..."
    mkdir -p "$SRC"; cd "$SRC"
    for r in libubox ubus uci; do
        if [ ! -d "$SRC/$r/.git" ]; then
            rm -rf "$SRC/$r"
            git clone --depth 1 "https://git.openwrt.org/project/$r.git" "$SRC/$r" 2>&1 | tail -1 \
                || git clone --depth 1 "https://github.com/openwrt/$r.git" "$SRC/$r" 2>&1 | tail -1
        fi
    done

    rm -rf "$HOST"; mkdir -p "$HOST"
    export PKG_CONFIG_PATH="$HOST/lib/pkgconfig:$HOST/lib64/pkgconfig"
    export CMAKE_PREFIX_PATH="$HOST"
    export LD_LIBRARY_PATH="$HOST/lib:$HOST/lib64"

    build_one() {
        r="$1"; shift
        log "  cmake build $r"
        rm -rf "/tmp/build-$r"
        {
            cmake -S "$SRC/$r" -B "/tmp/build-$r" \
                  -DCMAKE_INSTALL_PREFIX="$HOST" \
                  -DCMAKE_PREFIX_PATH="$HOST" \
                  -DCMAKE_BUILD_TYPE=Release \
                  -DCMAKE_INSTALL_RPATH="$HOST/lib;$HOST/lib64" \
                  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
                  -DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF -DBUILD_NL80211=OFF \
                  -DCMAKE_C_FLAGS="-O2 -g" "$@" \
              && cmake --build "/tmp/build-$r" -j"$(nproc)" \
              && cmake --install "/tmp/build-$r"
        } > "/tmp/build-$r.log" 2>&1 || {
            log "ERROR: $r 构建失败，日志尾部："; tail -40 "/tmp/build-$r.log"; exit 1;
        }
    }

    # 顺序：libubox（含 json-c）-> ubus / uci（依赖 ubox，靠 PKG_CONFIG_PATH）
    build_one libubox
    build_one ubus
    build_one uci

    touch "$HOST/.setup-ok"
    log "宿主依赖安装到 $HOST"
fi

# ---------- 4. 自检 + 冒烟 ----------
log "自检:"
echo "  gcc      : $(gcc -dumpfullversion)"
echo "  cmake    : $(cmake --version | head -1)"
TGCC="$(sdk_gcc)"
echo "  SDK root : $SDK"
echo "  staging  : $(ls "$SDK/staging_dir" | tr '\n' ' ')"
echo "  cross gcc: $TGCC"
"$TGCC" -dumpversion >/dev/null || { log "ERROR: 交叉 gcc 无法执行"; exit 1; }

TMPD="$(mktemp -d)"
cat > "$TMPD/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("hello\n"); return 0; }
EOF

gcc -O2 "$TMPD/hello.c" -o "$TMPD/hello-host"
echo "  host out : $("$TMPD/hello-host") / $(file -b "$TMPD/hello-host" | cut -c1-55)"

if "$TGCC" -O2 -static "$TMPD/hello.c" -o "$TMPD/hello-target" 2>/dev/null; then
    TARGET_OUT="$TMPD/hello-target"
else
    "$TGCC" -O2 -c "$TMPD/hello.c" -o "$TMPD/hello-target.o"
    TARGET_OUT="$TMPD/hello-target.o"
fi
echo "  cross out: $(file -b "$TARGET_OUT" | cut -c1-70)"

# ubus 联调：ubusd 起来后注册一个对象，确认 ubus list 能看到
mkdir -p /var/run/ubus
cat > "$TMPD/smoke.c" <<'EOF'
#include <libubus.h>
#include <libubox/uloop.h>
static struct ubus_context *ctx;
static const struct ubus_method m[] = {};
static struct ubus_object_type t = UBUS_OBJECT_TYPE("smoke", m);
static struct ubus_object o = { .name = "smoke.test", .type = &t, .methods = m, .n_methods = 0 };
int main(void) {
    uloop_init();
    ctx = ubus_connect(NULL);
    if (!ctx) return 1;
    ubus_add_uloop(ctx);
    if (ubus_add_object(ctx, &o)) return 2;
    uloop_run();
    return 0;
}
EOF
gcc -O2 -I"$HOST/include" "$TMPD/smoke.c" -o "$TMPD/smoke" \
    -L"$HOST/lib" -Wl,-rpath,"$HOST/lib" -lubus -lubox -lblobmsg_json -ljson-c \
    || { log "ERROR: ubus 客户端冒烟编译失败"; exit 1; }

pkill -x ubusd >/dev/null 2>&1 || true
sleep 0.2
"$HOST/sbin/ubusd" >/tmp/ubusd.log 2>&1 &
UPID=$!
for _ in $(seq 1 40); do [ -S /var/run/ubus/ubus.sock ] && break; sleep 0.2; done
"$TMPD/smoke" >/tmp/smoke.log 2>&1 &
SPID=$!
sleep 0.5
if [ -S /var/run/ubus/ubus.sock ]; then
    echo "  ubusd    : pid=$UPID sock=/var/run/ubus/ubus.sock"
    echo "  ubus list: $("$HOST/bin/ubus" list | tr '\n' ' ')"
    "$HOST/bin/ubus" list | grep -qx "smoke.test" \
        && echo "  ubus obj : smoke.test 注册成功" \
        || { log "ERROR: ubus list 里看不到 smoke.test"; tail -5 /tmp/smoke.log; kill $UPID $SPID 2>/dev/null; exit 1; }
else
    log "ERROR: ubusd 未起来"; tail -5 /tmp/ubusd.log; exit 1
fi
kill "$SPID" "$UPID" >/dev/null 2>&1 || true
sleep 0.2

# uci 联调
mkdir -p "$TMPD/uci"
cat > "$TMPD/uci/test" <<'EOF'
config test "sec"
	option a "b"
EOF
UCI_OUT="$("$HOST/bin/uci" -c "$TMPD/uci" get test.sec.a)"
echo "  uci get  : test.sec.a=$UCI_OUT"
[ "$UCI_OUT" = "b" ] || { log "ERROR: uci 读取失败"; exit 1; }

rm -rf "$TMPD"
log "setup 完成"
