# 测试手册

本文给出三层验证的**可复制命令**，以及上真机前的检查清单。

前置约定（很重要）：

- **本地（Windows）不编译、不运行任何被测程序**。下面所有标着"在本地执行"的命令，
  实际动作都发生在 CNB 云原生开发环境里（脚本内部 `tar` 同步 + `ssh` 远程执行）；
- 需要先有 `.cloud/ssh.env`：`scripts/cloud/start.sh` 会写它；
- 环境空闲约 10 分钟会被回收，且重拉后是**全新容器**（工具链丢失）。
  脚本会自愈（见 [DEVELOPMENT.md](../DEVELOPMENT.md#环境会被回收必须幂等自愈)），
  长时间测试建议开着 `scripts/cloud/keepalive.sh &`。

```
本地 Bash ──tar──> 云端 /workspace/.build/<id>/src ──gcc/make──> 测试二进制 ──> 结果回传
```

---

## 1. 三层验证

| 层 | 验证什么 | 入口 | 耗时 | 失败意味着 |
| --- | --- | --- | --- | --- |
| L1 编译 | 语法/类型/告警/链接；交叉目标能否为 musl 编译 | `scripts/cloud/cc.sh` | 十几秒（增量） | 代码本身有问题，先修这个，别急着跑测试 |
| L2 单元与 golden | argv 与旧实现逐字一致、UCI 读写语义、配置表单渲染与 i18n | `tests/run_host.sh`、`tests/run_web.sh` | 1~3 分钟（含同步） | 行为回归或契约漂移 |
| L3 云端集成自检 | ubusd/uci 运行时可用、服务生命周期、防火墙 UCI 落盘、下载安装 | `scripts/cloud/setup.sh` 的冒烟 + 本文第 4 节手工步骤 | 5~10 分钟 | 环境/依赖/编排层面的问题，代码可能没错 |

三者是"由便宜到贵"的关系：L1 不过不看 L2，L2 不过不上真机。

---

## 2. 命令清单

### 2.1 L1：编译验证

```bash
# 在本地执行，实际在云端跑：只编译不链接（增量最快，适合边写边看语法/告警）
scripts/cloud/cc.sh

# 在本地执行，实际在云端跑：编译并链接 easytierd / easytierctl
scripts/cloud/cc.sh --link

# 在本地执行，实际在云端跑：交叉 gcc（x86_64 musl），只做头文件/语法验证
scripts/cloud/cc.sh --cross

# 在本地执行，实际在云端跑：交叉编译并链接（要求 SDK staging 里已有依赖，通常没有）
scripts/cloud/cc.sh --cross --link
```

要点：

- 零告警才算通过（`-Wall -Wextra -Werror`）；
- `--cross` 常因 SDK 的 `target-*/usr/include` 是空的而报 `json-c/json.h: No such file`，
  **这不算代码错**，权威验证请用 2.2 的包构建；
- 多任务并行时必须加 `ET_BUILD_ID`，否则会互相清空远程目录：

```bash
ET_BUILD_ID=fix-argv scripts/cloud/cc.sh --link
```

### 2.2 包构建（权威）

```bash
# 在本地执行，实际在云端跑：OpenWrt SDK 构建 easytierd 的 ipk
scripts/cloud/build.sh

# 连带 luci-app-easytier（需要 feeds/luci，首次会自动 feeds update）
scripts/cloud/build.sh --luci

# 先 clean
scripts/cloud/build.sh --clean
```

成功后远程会打印 `--- 产物 ---` 与 `bin/.../easytierd*.ipk` 路径。要取回本地：

```bash
# 在本地执行
. .cloud/ssh.env
scp -o UserKnownHostsFile=.cloud/known_hosts "$ET_SSH":'/workspace/.toolchain/sdk/bin/packages/*/base/easytierd*.ipk' .
```

### 2.3 L2：golden 与前端桥接

```bash
# 在本地执行，实际在云端跑：单元 + UCI 语义 + 写回往返 + 11 组 golden（约 54 项）
bash tests/run_host.sh

# 在本地执行，实际在云端跑：controller 语法/转发、模板 Lua+JS 语法、i18n 同步、C 渲染
bash tests/run_web.sh
```

单独跑某一项（仍然在云端）：

```bash
# 只跑 i18n 一致性检查
scripts/cloud/ssh.sh "cd /workspace/.build/main/src && python3 tools/i18n_build.py --check"

# 只跑 controller 的转发单测（需要 lua5.1）
scripts/cloud/ssh.sh "cd /workspace/.build/main/src && lua5.1 tests/bridge_lua_test.lua"

# 只跑某个 golden 用例（argv_test 支持传目录；可用 -- 过滤前先用独立目录）
scripts/cloud/ssh.sh "cd /workspace/.build/main/src && /tmp/et_argv_test tests/golden"
```

> `/tmp/et_argv_test` 是 `tests/run_host.sh` 编译出来的产物，先跑一次该脚本再单独调用。

### 2.4 重新生成 i18n 字典

po 改过之后必须重跑并提交产物：

```bash
# 生成需要 python3（本地也可能有；不涉及编译）
python3 tools/i18n_build.py

# 仅校验（CI 与 run_web.sh 用的就是这条）
python3 tools/i18n_build.py --check
```

### 2.5 L3：云端集成自检（ubusd / uci）

`scripts/cloud/setup.sh` 末尾自带一段冒烟，会做四件事：

1. 宿主 `gcc` 编一个 hello 并运行；
2. 交叉 gcc 编 hello（静态失败则退化为 `-c`），用 `file` 确认目标架构；
3. 编译一个 ubus 客户端，拉起 `/workspace/.toolchain/host/sbin/ubusd`，注册 `smoke.test`，
   用 `ubus list` 验证能看到它；
4. 用 `host/bin/uci` 读一个临时配置，验证 `uci get` 返回值。

手动重跑（幂等，已装则跳过安装、只跑冒烟）：

```bash
# 在本地执行，实际在云端跑
scripts/cloud/ssh.sh -s < scripts/cloud/setup.sh
```

期望输出末尾：`[setup] setup 完成`，且中间有 `ubus obj : smoke.test 注册成功`、
`uci get  : test.sec.a=b`。

---

## 3. L2 到底测了什么

### 3.1 `tests/run_host.sh`

流程：`cc.sh` 全量编译（失败不致命，继续单独编本模块）→ 编译 `tests/argv_test`
（链接 `etd_util/uci/schema/cfg/argv/toml/detect`，并用 `-DET_FILE_MACHINE_ID="/tmp/et_test_machine_id"`
把 machine-id 指到临时文件）→ 跑 `tests/golden`。

断言分四组：

1. `unit_tests()`：工具函数、schema 查找、emit 规则等；
2. `uci_semantics()`：list 保序/保留元素内空格、默认值兜底、`@easytier[0]` 寻址；
3. `save_roundtrip()`：读 → 改 → 写回 → 再读，确认不物化默认值、list 先清后写；
4. `uci_section_tests()`：section 创建/删除/changed 判定。

golden 用例（11 组）覆盖：

| 用例 | 覆盖点 |
| --- | --- |
| `min` | 最小配置（大量空值），确认不输出多余的旗标 |
| `full` | 全字段，确认 argv 顺序与参数值 |
| `config_mode` | `etcmd=config` → `-c /etc/easytier/config.toml` |
| `web_mode` | `etcmd=web` → `--machine-id <uuid>` + `-w <url>` + `--hostname` |
| `webconsole` / `webconsole_min` | `easytier-web` 的 argv（含 OIDC / webhook 门控） |
| `web_badurl` | `web_config` 不是完整 URL → 跳过 `-w` 并写 notes |
| `relay_bare` | `relay_network=1` 且 whitelist 为空 → 输出**裸旗标** |
| `vpn_new` | 新写法 `wg://host:port` + 私钥 + 客户端列表 + toml 渲染 |
| `vpn_old` | 2.6.4 旧写法被跳过并写 notes |
| `vpn_nokey` | 有门户但无私钥 → 跳过并写 notes |

输出形如（数字以实际运行结果为准）：

```
== 结果: 54 项通过, 0 项失败 ==
```

失败时会打印 `期望/实际` 的逐行 diff，直接按行修 `etd_argv.c` / `etd_schema.c` 或更新期望文件
（**先确认是实现错了还是期望过时了**，不要习惯性改 `.args`）。

### 3.2 `tests/run_web.sh`

六步，任意一步失败都会让整体 rc≠0：

1. 同步 + `cc.sh` 编译 `src/`；
2. `luac -p luci-app-easytier/luasrc/controller/easytier.lua`（缺 `luac` 会尝试 `apt-get install lua5.1`）；
3. `lua5.1 tests/bridge_lua_test.lua`：用假的 `luci.http/util/sys/nixio.fs` 驱动真实 controller，校验
   - 菜单结构（5 个页面、顺序、标题、模板名）；
   - 28 个 `call` 端点全部注册（24 个旧端点一个不少）；
   - 每个端点的 `easytierctl api ...` 命令行拼装与 shell quote；
   - JSON body → `--body-file`（内容正确且事后删除）；
   - 表单 → `key=value`；`upload_binary` → `file=/tmp/upload_<name> name=<name>`；
   - 每个端点的 `Content-Type`；
4. 模板检查：抽出所有 `<% %>` 的 Lua 块交给 `luac -p`；把 `<script>` 里的模板标签替换成占位符后交给
   `node --check`（ES5 语法检查）；
5. `python3 tools/i18n_build.py --check`：po 与 `root/usr/share/easytier/i18n/*.json` 是否同步；
6. 编译并运行 `tests/web_bridge_test`：`config_get` / `config_save` / `config_render` / `schema` + i18n
   （用 `ET_UCI_CONFDIR` 指到临时目录、`ET_I18N_DIR` 指到构建出的字典）。

---

## 4. 手工集成自检（假 core）

云端还没有自动化的"假 easytier-core"用例，需要端到端确认生命周期时按下面做。
**全部在云端容器里执行**（`scripts/cloud/ssh.sh` 进交互 shell，或把整段作为 `-s` 脚本灌进去）。

### 4.1 准备一个假 core

```sh
mkdir -p /tmp/et-it/bin
cat > /tmp/et-it/bin/easytier-core <<'EOF'
#!/bin/sh
# 假 core：-h 输出 >=3 行且包含 easytier（et_test_binary 的判据），-V 给出可解析的版本
case "$1" in
  -h|--help)
    echo "easytier-core fake"
    echo "  --network-name <NAME>"
    echo "  --network-secret <SECRET>"
    echo "  -i <IP>"
    exit 0 ;;
  -V|--version)
    echo "easytier-core 2.7.0-fake"
    exit 0 ;;
  --check-config)
    echo "config ok"
    exit 0 ;;
esac
echo "fake core started: $*"
trap 'echo "fake core got TERM"; exit 0' TERM
while :; do sleep 1; done
EOF
chmod +x /tmp/et-it/bin/easytier-core
cp /tmp/et-it/bin/easytier-core /tmp/et-it/bin/easytier-cli
```

### 4.2 准备 UCI 与配置目录

```sh
mkdir -p /tmp/et-it/etc/config /tmp/et-it/etc/easytier
cat > /tmp/et-it/etc/config/easytier <<'EOF'
config easytier 'easytier'
	option enabled '1'
	option etcmd 'etcmd'
	option easytierbin '/tmp/et-it/bin/easytier-core'
	option network_name 'it-net'
	option network_secret 'it-secret'
	option ipaddr '10.144.0.1'
	option tunname 'tun9'
	option tcp_port '11010'
	option listenermode 'ON'
EOF
```

> 注意：`easytierd` 支持用环境变量 `ET_UCI_CONFDIR` 把 libuci 的配置目录指到别处（`etd_uci.c`、`etd_net.c`
> 都读它；生产环境不设置，行为与默认一致）。测试时 `firewall` / `network` 包也会落在同一个 confdir 里，
> 不会碰真机配置。

### 4.3 跑起来看行为

下面的 `BIN` 指向云端构建产物（真机上则直接用 `/usr/sbin`）：

```sh
export ET_UCI_CONFDIR=/tmp/et-it/etc/config
BIN=/workspace/.build/main/src/build

"$BIN/easytierctl" api config_get | head -c 400; echo
"$BIN/easytierd" service start
sleep 2
ps w | grep -E 'easytierd|easytier-core' | grep -v grep
tail -n 20 /tmp/easytier.log
```

期望：

- `config_get` 返回 120 个字段（含默认值，比如 `rpc_portal=15888`、`fallback_version=v2.7.0`）；
- `/tmp/easytier.log` 里有 `运行 '.../easytier-core' ...`（敏感参数显示为 `'***'`）；
- 有 `添加防火墙规则 easytier_tcp_udp 放行端口 11010`、`添加网络接口 EasyTier 绑定虚拟接口 tun9`；
- `firewall` 包里出现 `easytier_tcp_udp`、`easytierzone` 等 section，`network` 包里出现 `EasyTier`。

```sh
# 检查防火墙/接口配置是否落盘（5 秒延迟 reload 之后）
sleep 6
cat /tmp/et-it/etc/config/firewall 2>/dev/null | sed -n '/easytier/,+6p'
cat /tmp/et-it/etc/config/network 2>/dev/null | sed -n '/EasyTier/,+6p'
```

```sh
# 停止：确认进程被带走、规则被删、日志有记录
"$BIN/easytierd" service stop
sleep 2
ps w | grep easytier-core | grep -v grep || echo "core 已退出"
grep -c easytier /tmp/et-it/etc/config/firewall 2>/dev/null || echo "防火墙规则已清空"
tail -n 5 /tmp/easytier.log
```

### 4.4 假 core 覆盖不到的部分

假 core 不能验证：TUN 设备、真实端口监听、`--check-config` 的真实语义、`easytier-cli` 的分区输出、
下载 zip 的真实解包。这些必须上真机（见第 5 节）。

---

## 5. 上真机前的验证清单

每条都给出"怎么判成功"。建议在一台**可随时重刷**的设备上按顺序做。

### 5.1 安装与升级

- [ ] `opkg install easytierd_*.ipk luci-app-easytier_*.ipk` 成功，`ls -l /usr/sbin/easytierd /usr/sbin/easytierctl`
      存在且可执行
- [ ] 从旧版（`legacy/lua`）覆盖升级：`/etc/config/easytier` 与 `/etc/easytier/` 内容保留；
      升级后第一次启动日志里没有"配置解析失败"
- [ ] 用 `opkg install --nodeps` 强制只装界面（不装 `easytierd`）时，接口返回可读的错误而不是空白页——
      用来确认依赖声明确实必要
- [ ] `/etc/rc.d/S99easytier` 存在，重启设备后服务自动起来（`/etc/init.d/easytier status` 返回 0）

### 5.2 状态页字段

- [ ] 运行中/已停止状态与实际一致（`pgrep -f easytier-core` 对照）
- [ ] 版本：`ettag` 与 `easytier-core -V` 一致；`etnewtag` 能取到 GitHub 最新 tag（断网时为空但不报错）
- [ ] `etsta`（运行时长）与 `cat /tmp/easytier_time` 的差值一致，格式为 `"3天 04小时05分06秒"`
- [ ] CPU 占用非负、随负载变化（首次请求会多花约 120ms 做差分采样，属正常）
- [ ] 内存显示为 `"xx.xx MB"`
- [ ] 无 TUN 模式（`no_tun=1`）时页面提示正确，不显示流量

### 5.3 `ifconfig` 解析

- [ ] busybox `ifconfig <if>` 输出能被正确解析：`ip` / `netmask` / `state`（UP/DOWN）与实际一致
- [ ] 没有 `ifconfig` 时回退 `ip addr show dev <if>` 也能拿到地址
- [ ] 无 `ifconfig` 且无 `ip` 时，页面显示空地址而不是报错
- [ ] `ifname` 参数含非法字符（如 `tun0;reboot`）时返回 `exists:false`（`ifname_ok` 白名单）

### 5.4 真实下载与安装

- [ ] "上传程序"页点下载：接口立即返回（不卡页面），进度条从 1% 走到 100%
- [ ] 下载过程中点取消：`/tmp/easytier_download_cancel` 出现，进度很快变为"已取消下载"（取消检查间隔 200ms），
      没有残留的 `curl`/`wget` 进程
- [ ] 镜像全部失败时提示"所有下载地址均下载失败…"，`/tmp/easytier_download` 被清理
- [ ] 装完后 `easytier-core -V` 可执行、版本缓存 `/tmp/easytier.tag` 被删除并重新生成
- [ ] 安装目录只读/空间不足时，旧二进制仍可用（确认 `.etnew` + `rename` 的原子性：
      `ls /usr/bin/easytier-core*` 不应出现残留 `.etnew`）

### 5.5 防火墙与网络

- [ ] 启动后 `uci show firewall | grep easytier` 出现 `easytier_tcp_udp`、`easytierzone`、
      以及按配置出现的 `easytier_ws/wss/wg/quic`
- [ ] `uci show network | grep EasyTier` 出现 `proto/ipaddr/netmask/device/ifname`
- [ ] 5 秒后 `logread` 里能看到 network/firewall reload 的痕迹（或 `fw3` 规则已生效：
      `nft list ruleset | grep 11010` / `iptables -S | grep 11010`）
- [ ] 关闭"自动配置防火墙"（`auto_config_firewall=0`）后重启，不新增任何规则，日志有"已禁用，跳过配置"
- [ ] 关闭"自动配置网络接口"后，`network.EasyTier` 与 `firewall.easytierzone` 被删除
- [ ] 停止服务后规则与接口 section 全部消失；`/tmp/easytier-*`（带点的临时文件）被清理

### 5.6 overlay 空间自愈

- [ ] 把 `/overlay` 填到剩余 < 5000KB（或临时用一个小分区模拟），删除 `/usr/bin/easytier-core` 后启动：
      日志出现"可用空间不足…自动切换为内存/tmp/easytier-core"，`uci get easytier.@easytier[0].easytierbin`
      变成 `/tmp/easytier-core`
- [ ] 恢复空间后手动改回 `/usr/bin/easytier-core` 能正常工作（确认不是单向锁死）

### 5.7 日志

- [ ] 日志 >5120KB 后每 60 秒被截断到最后 500 行；**截断后 core 仍在写日志**（验证原地截断，
      不是 `tmp+rename`）
- [ ] 页面上"清空日志"后文件大小为 0，且 core 后续日志仍能写入（同上）
- [ ] 页面日志不带 ANSI 颜色码

### 5.8 其它易漏项

- [ ] `vpn_portal` 用 2.6.4 旧写法（`wg://0.0.0.0:11011/10.14.14.0/24`）时，日志明确提示已跳过，
      且 `ps` 里看不到 `--vpn-portal`
- [ ] 配置了门户私钥时，`ps w | grep easytier-core` **看不到私钥**（只在环境变量里）
- [ ] 上传一个非 EasyTier 的可执行文件：提示"不是有效的easytier程序"，原二进制不受影响
- [ ] 用带路径穿越的文件名上传（如 `../../etc/passwd`）：被 `upload_src_ok()` 拒绝
- [ ] `easytierctl doctor` 输出与本机实际一致（架构、工具、磁盘、进程）
- [ ] `easytierctl api schema | head -c 200` 返回 120 条字段，`count` 为 120

---

## 6. 常见失败与定位

| 现象 | 多半原因 | 处理 |
| --- | --- | --- |
| `cc.sh` 报 `json-c/json.h: No such file` | 用了 `--cross`，SDK staging 为空 | 用默认宿主模式；权威验证走 `build.sh` |
| `cc.sh` 报工具链缺失后仍失败 | 环境被回收且 `setup.sh` 也失败 | 看 `.cloud/keepalive.log`；手动 `scripts/cloud/ssh.sh -s < scripts/cloud/setup.sh` |
| `run_host.sh` 提示"无法获取测试退出码" | 同步或编译失败，或 SSH 断开 | 先单独跑 `cc.sh` 看编译输出 |
| golden diff 只差顺序 | 在 `et_schema[]` 中间插了一行 | 行顺序 = argv 顺序，插入位置要对，或同步更新期望文件并说明 |
| `run_web.sh` 第 5 步失败 | 改了 po 没重跑 `i18n_build.py` | `python3 tools/i18n_build.py` 后提交 JSON |
| 模板 JS 检查失败 | 用了 ES6（`let`/箭头函数/模板字符串） | 改回 ES5 |
| ubus 冒烟失败 | `/var/run/ubus` 不存在或 `ubusd` 已在跑 | `pkill -x ubusd` 后重跑 `setup.sh` |
| ipk 构建缺 `luci.mk` | 没跑 `feeds update` | `build.sh --luci` 会自动尝试；必要时在云端手动 `./scripts/feeds update -a && ./scripts/feeds install -a` |
