#!/bin/sh
# tests/build_cli_probe.sh - 交叉编译开发探针 cli_probe（aarch64-linux-musl 静态）
#
# 用法：
#   OUT=build/cli_probe_a bash tests/build_cli_probe.sh
#   ZIG=/path/to/zig OUT=/tmp/cli_probe bash tests/build_cli_probe.sh
set -eu
cd "$(dirname "$0")/.."
[ -n "$ZIG" ] || ZIG=zig
[ -n "$OUT" ] || OUT=build/cli_probe
mkdir -p build
mkdir -p "$(dirname "$OUT")"
"$ZIG" cc -target aarch64-linux-musl -static -O2 -g \
	-std=gnu11 -D_GNU_SOURCE -Isrc/include \
	-Wall -Wextra -Werror -Wno-unused-parameter \
	tests/cli_probe.c \
	src/lib/etd_pb.c src/lib/etd_rpc.c src/lib/etd_util.c \
	src/lib/etd_proc.c src/lib/etd_detect.c \
	src/lib/etd_cli_common.c src/lib/etd_cli_peer.c \
	src/lib/etd_cli_extra_a.c src/lib/etd_cli_extra_b.c \
	src/lib/etd_cli_extra_c.c src/lib/etd_cli_acl_toml.c \
	-o "$OUT"
echo "built $OUT"
