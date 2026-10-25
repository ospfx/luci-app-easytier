#!/bin/bash
# tests/run_host.sh - golden 测试一键跑
#
# 流程：同步本地工作树到 CNB 云开发环境 → 宿主编译（gcc）→ 跑 tests/argv_test
#       并与 tests/golden/*.args 逐行比对，失败时打印期望/实际差异。
#
# 用法：
#   bash tests/run_host.sh                 # 默认 build-id=cfg
#   ET_BUILD_ID=foo bash tests/run_host.sh # 独立构建目录（多 agent 并行）
#
# 前置：云环境已被 scripts/cloud/start.sh 拉起且跑过 scripts/cloud/setup.sh
#       （setup 会装 gcc 以及 /workspace/.toolchain 下的 libuci 等宿主依赖）。
# 注意：本地（Windows）绝不编译，一切编译都在云端完成。
#
# 关于行号：tests/golden/*.uci 头部注释里引用的 init.d 行号，均指重写前那份
# 1014 行的 luci-app-easytier/root/etc/init.d/easytier（即 git HEAD 版本，
# 可用 `git show HEAD:luci-app-easytier/root/etc/init.d/easytier` 查看）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ID="${ET_BUILD_ID:-cfg}"
REMOTE_SRC="/workspace/.build/$BUILD_ID/src"
TC="/workspace/.toolchain/host"

cd "$ROOT" || exit 1

echo "[test] 1/3 同步到云环境 + 全量编译（scripts/cloud/cc.sh）"
if ! ET_BUILD_ID="$BUILD_ID" bash scripts/cloud/cc.sh; then
	echo "[test] 提示：全量编译未通过（可能是其它模块尚未完成的文件），继续单独编译本模块"
fi

echo "[test] 2/3 编译 golden 测试程序"
REMOTE_SCRIPT=$(cat <<EOF
set -e
cd '$REMOTE_SRC'
gcc -O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter \\
	-I src/include -I '$TC/include' \\
	-DET_FILE_MACHINE_ID='"/tmp/et_test_machine_id"' \\
	tests/argv_test.c \\
	src/lib/etd_util.c src/lib/etd_uci.c src/lib/etd_schema.c src/lib/etd_cfg.c \\
	src/lib/etd_argv.c src/lib/etd_toml.c src/lib/etd_detect.c \\
	-L'$TC/lib' -Wl,-rpath,'$TC/lib' -luci -lubox -o /tmp/et_argv_test
echo "[test] 3/3 运行 golden 测试（tests/golden）"
set +e
/tmp/et_argv_test tests/golden
echo "ET_TEST_RC=\$?"
exit 0
EOF
)

OUT="$(printf '%s\n' "$REMOTE_SCRIPT" | bash scripts/cloud/ssh.sh -s 2>&1)"
printf '%s\n' "$OUT" | grep -vE "post-quantum|store now|may need to be upgraded|^\*\*"
rc="$(printf '%s\n' "$OUT" | sed -n 's/^ET_TEST_RC=//p' | tail -1)"

if [ -z "$rc" ]; then
	echo "[test] 无法获取测试退出码（同步或编译失败？见上方输出）" >&2
	exit 2
fi
if [ "$rc" -ne 0 ]; then
	echo "[test] 存在失败用例，上方为逐行 diff" >&2
	exit "$rc"
fi
echo "[test] 全部通过"
exit 0
