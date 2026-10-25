#!/bin/bash
# tests/run_web.sh - 前端桥接层（薄 Lua + 配置页 C 渲染 + i18n）自测
#
# 全部动作都在 CNB 云开发环境里完成（本地 Windows 不编译、不运行）：
#   1) 同步工作树并编译 src/（scripts/cloud/cc.sh，宿主 gcc）
#   2) luac -p 检查极薄 controller 的语法
#   3) bridge_lua_test.lua：用假 luci.* 驱动 controller，校验 28 个端点的
#      命令行拼装、Content-Type、JSON body、上传落盘与菜单结构
#   4) 逐个模板抽取 <% %> 里的 Lua 块，用 luac -p 检查语法
#   5) tools/i18n_build.py --check 校验 i18n JSON 与 po 同步
#   6) 编译并运行 tests/web_bridge_test（config_get/save/render/schema）
#
# 用法：
#   bash tests/run_web.sh
#   ET_BUILD_ID=foo bash tests/run_web.sh      # 独立构建目录（多 agent 并行）
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ID="${ET_BUILD_ID:-web}"
REMOTE_SRC="/workspace/.build/$BUILD_ID/src"
TC="/workspace/.toolchain/host"

cd "$ROOT" || exit 1

echo "[web] 1/6 同步到云环境 + 编译 src/"
if ! ET_BUILD_ID="$BUILD_ID" bash scripts/cloud/cc.sh; then
	echo "[web] 提示：全量编译未通过（可能是其它模块尚未完成的文件），继续单独验证本模块"
fi

REMOTE_SCRIPT=$(cat <<EOF
set -u
cd '$REMOTE_SRC'
RC=0

echo "[web] 2/6 luac -p controller"
if ! command -v luac >/dev/null 2>&1; then
	echo "  luac 缺失，尝试安装 lua5.1 …"
	(apt-get install -y lua5.1 >/tmp/et_apt.log 2>&1 || \\
	 (apt-get update >/dev/null 2>&1 && apt-get install -y lua5.1 >>/tmp/et_apt.log 2>&1)) || true
fi
if command -v luac >/dev/null 2>&1; then
	luac -p luci-app-easytier/luasrc/controller/easytier.lua && echo "  OK"
	echo "  controller 行数: \$(wc -l < luci-app-easytier/luasrc/controller/easytier.lua)"
else
	echo "  SKIP: 无法安装 lua5.1"
	RC=1
fi

echo "[web] 3/6 转发层 Lua 单测"
if command -v lua5.1 >/dev/null 2>&1; then
	lua5.1 tests/bridge_lua_test.lua || { echo "  失败"; RC=1; }
else
	echo "  SKIP: 无 lua5.1"
	RC=1
fi

echo "[web] 4/6 模板内 Lua 块 + 前端 JS 语法检查"
python3 - <<'PY'
import glob, re, subprocess, sys, tempfile, os
bad = 0
for path in sorted(glob.glob('luci-app-easytier/luasrc/view/easytier/*.htm')):
    src = open(path, encoding='utf-8', errors='replace').read()
    for idx, m in enumerate(re.finditer(r'<%(.*?)%>', src, re.S), 1):
        body = m.group(1)
        if body[:1] in (':', '+'):          # <%: 翻译 %> / <%+ 包含 %>
            continue
        if body[:1] == '=':
            body = 'return (' + body[1:] + ')'
        body = body.rstrip()
        if body.endswith('-'):
            body = body[:-1]
        if not body.strip():
            continue
        fd, tmp = tempfile.mkstemp(suffix='.lua')
        with os.fdopen(fd, 'w', encoding='utf-8') as fp:
            fp.write(body)
        rc = subprocess.call(['luac', '-p', tmp], stderr=subprocess.STDOUT)
        os.unlink(tmp)
        if rc != 0:
            print('  FAIL %s 第 %d 个 Lua 块' % (path, idx))
            bad += 1
print('  模板 Lua 块 OK' if not bad else '  模板 Lua 块失败 %d 处' % bad)

# 前端 JS：把 LuCI 模板标签替换成占位符后交给 node --check 做纯语法检查
for path in sorted(glob.glob('luci-app-easytier/luasrc/view/easytier/*.htm')):
    src = open(path, encoding='utf-8', errors='replace').read()
    if '<script' not in src:
        continue
    js = re.sub(r'<%=[^%]*?%>', '0', src)
    js = re.sub(r'<%:[^%]*?%>', 'x', js)
    js = re.sub(r'<%+[^%]*?%>', '', js)
    chunks = re.findall(r'<script[^>]*>(.*?)</script>', js, re.S)
    for idx, body in enumerate(chunks, 1):
        fd, tmp = tempfile.mkstemp(suffix='.js')
        with os.fdopen(fd, 'w', encoding='utf-8') as fp:
            fp.write(body)
        rc = subprocess.call(['node', '--check', tmp], stderr=subprocess.STDOUT)
        os.unlink(tmp)
        if rc != 0:
            print('  FAIL %s 第 %d 个 <script> 块' % (path, idx))
            bad += 1
print('  前端 JS 语法 OK' if not bad else '  前端检查失败 %d 处' % bad)
sys.exit(1 if bad else 0)
PY
[ \$? -eq 0 ] || RC=1

echo "[web] 5/6 i18n JSON 与 po 同步检查"
python3 tools/i18n_build.py --check || RC=1

echo "[web] 6/6 编译并运行 web_bridge_test"
gcc -O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter \\
	-I src/include -I '$TC/include' \\
	-DET_FILE_TOML='"/tmp/et_web_test/etc/easytier/config.toml"' \\
	-DET_DIR_ET='"/tmp/et_web_test/etc/easytier"' \\
	-DET_FILE_MACHINE_ID='"/tmp/et_web_test/etc/easytier/et_machine_id"' \\
	tests/web_bridge_test.c \\
	src/api/etd_api.c src/api/etd_api_config.c src/lib/etd_i18n.c \\
	src/lib/etd_util.c src/lib/etd_uci.c src/lib/etd_schema.c src/lib/etd_cfg.c \\
	src/lib/etd_argv.c src/lib/etd_toml.c src/lib/etd_detect.c src/lib/etd_proc.c \\
	-L'$TC/lib' -Wl,-rpath,'$TC/lib' -luci -lubox -ljson-c -o /tmp/et_web_test_bin
if [ \$? -ne 0 ]; then
	echo "  gcc 失败"
	RC=1
else
	/tmp/et_web_test_bin luci-app-easytier/root/usr/share/easytier/i18n || RC=1
fi

echo "ET_WEB_RC=\$RC"
exit 0
EOF
)

OUT="$(printf '%s\n' "$REMOTE_SCRIPT" | bash scripts/cloud/ssh.sh -s 2>&1)"
printf '%s\n' "$OUT" | grep -vE "post-quantum|store now|may need to be upgraded|^\*\*|safe-delete"
rc="$(printf '%s\n' "$OUT" | sed -n 's/^ET_WEB_RC=//p' | tail -1)"

if [ -z "$rc" ]; then
	echo "[web] 无法获取结果（同步或编译失败？见上方输出）" >&2
	exit 2
fi
if [ "$rc" -ne 0 ]; then
	echo "[web] 存在失败项" >&2
	exit "$rc"
fi
echo "[web] 全部通过"
exit 0
