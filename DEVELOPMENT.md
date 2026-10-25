![luci-app-easytier](https://socialify.git.ci/EasyTier/luci-app-easytier/image?description=1&font=JetBrains+Mono&forks=1&issues=1&logo=https%3A%2F%2Favatars.githubusercontent.com%2Fu%2F169161851%3Fs%3D200%26v%3D4&name=1&pulls=1&stargazers=1&theme=Auto)

# 开发指南

[English](DEVELOPMENT_EN.md) | 简体中文

面向贡献者的开发说明。读完这一篇，你应该能回答三个问题：代码放在哪、改一个选项要动几处、怎么在
**不碰本机编译器**的前提下验证。

移植背景与新旧对照见 [docs/C-PORT.md](docs/C-PORT.md)，验证清单见 [docs/TESTING.md](docs/TESTING.md)。

## 核心原则

**必须兼容 OpenWrt 18.06 ~ 26.x；业务逻辑只在 C 里写一份，LuCI 只做菜单与转发。**

- 18.06 没有 `menu.d` JSON，菜单与模板渲染只能用 Lua1 的 `entry()` / `template()`，
  所以 `luasrc/controller/easytier.lua` 必须保留，但**只允许**做两件事：注册菜单、把请求转发给 `easytierctl`；
- 后端语言不再是 Lua。旧的"Lua 写业务逻辑"约束已作废，**不要把逻辑搬回 Lua**；
- 前端仍然是 ES5 + LuCI Lua1 模板：禁止 ES6+、禁止 LuCI JS 框架（`view.js` / `form.js` / `rpc.js`）。

## 目录结构与模块职责

```
repo/
├── src/                          # C 后端（唯一的业务逻辑所在）
│   ├── include/                  # 契约头 + 私有头
│   │   ├── etd.h                 #   公共常量：路径、日志、超时、返回码
│   │   ├── etd_api.h             #   API 契约：et_req/et_res、28 个 handler 原型、路由表
│   │   ├── etd_cfg.h             #   配置模型 + argv 拼装 + toml
│   │   ├── etd_uci.h             #   libuci 封装
│   │   ├── etd_util.h            #   buf/list/file/exec/日志工具
│   │   ├── etd_proc.h            #   /proc 进程发现、spawn、pidfile
│   │   ├── etd_service.h         #   start/stop/restart/status/enable
│   │   ├── etd_api_priv.h        #   （私有）handler 共用小工具
│   │   ├── etd_net.h             #   （私有）防火墙与 network 编排
│   │   ├── etd_super.h           #   （私有）前台监督循环
│   │   ├── etd_watchdog.h        #   （私有）checkip 看门狗
│   │   └── etd_i18n.h            #   C 侧 po→JSON 字典
│   ├── lib/                      # 可复用层：schema、cfg、argv、uci、toml、proc、util、detect、i18n
│   ├── daemon/                   # 生命周期：etd_entry(main)、service、super、net、watchdog
│   ├── api/                      # HTTP/CLI 端点：1 个公共层 + 7 个 handler 文件 + 路由表
│   └── ctl/                      # easytierctl（薄 CLI）
├── luci-app-easytier/            # 前端桥接层（Lua1 + ES5 模板）
│   ├── luasrc/controller/easytier.lua   # 129 行：菜单 + 转发
│   ├── luasrc/view/easytier/*.htm       # 6 个页面模板（状态/Core/Web/日志/上传/控制台）
│   ├── root/etc/init.d/easytier         # 24 行：start/stop/reload/status 各转发一条命令
│   ├── root/etc/config/easytier         # 默认 UCI（enabled=0 等 7 项）
│   ├── root/etc/uci-defaults/luci-easytier  # 注册 ucitrack、enable 服务、清缓存
│   ├── root/usr/share/easytier/i18n/*.json  # C 渲染用字典（由 tools/i18n_build.py 生成）
│   ├── root/usr/share/rpcd/acl.d/*.json     # rpcd ACL（UCI easytier + easytierctl exec）
│   └── po/                              # 翻译源（zh_Hans + templates/*.pot）
├── easytierd/Makefile            # OpenWrt 包：安装 easytierd + easytierctl
├── easytier/  easytier-noweb/    # 官方二进制打包（与本插件解耦）
├── scripts/cloud/                # 云端构建体系（见下文）
├── tests/                        # golden + 前端桥接测试
├── tools/i18n_build.py           # po → JSON 字典
└── docs/                         # C-PORT.md、TESTING.md
```

各层职责的边界（改代码前先确认自己在哪一层）：

| 目录 | 只做 | 不要做 |
| --- | --- | --- |
| `src/lib` | 纯数据与系统能力：schema、UCI 读写、argv、toml、/proc、文件、执行 | 不写业务决策（"什么时候启动/是否放行"） |
| `src/api` | 一个端点一个 handler，输入 `et_req`、输出 `et_res` | 不直接 `fork` 长期进程、不写 `init.d` |
| `src/daemon` | 生命周期、监督、防火墙、看门狗 | 不解析 HTTP 参数 |
| `src/ctl` | 参数解析 + 调用 `et_api_dispatch()` | 不复制任何业务逻辑 |
| `luci-app-easytier` | 菜单、模板、UA/表单 → `key=value` | 不读 UCI、不读写业务文件、不拼业务命令 |

## 唯一事实来源：schema

`src/lib/etd_schema.c` 的 `et_schema[]` 有 **120 条**，一行就是"一个 UCI 选项"的全部元数据：

```c
{ .name = "network_name", .kind = ET_K_PASS, .def = NULL,
  .label = "Network Name", .tab = "general",
  .help  = "The network name used to identify this VPN network (--network-name parameter)",
  .core_flag = "--network-name", .core_emit = ET_E_VAL },
```

它同时驱动四件事：

1. **UCI 读写**：`et_cfg_load()` 按 schema 逐项读取，缺失项填 `.def`；`et_cfg_save()` 只写与 UCI 现有值不同的项
   （内存值恰好等于 `.def` 时不写回，避免一次保存就把 61 条 schema 默认值写进 `/etc/config/easytier`）；
2. **argv 拼装**：`et_argv_core()` / `et_argv_web()` **按表的行序遍历**，所以**行顺序就是命令行顺序**；
3. **表单渲染**：`config_render` 按 `.tab` 分组渲染，字段顺序 = 表的行顺序；
4. **文档与调试**：`easytierctl api schema` 直接输出 JSON 版 schema。

### 新增一个选项的标准动作

以新增 `--foo-bar <值>` 为例：

1. **在 `et_schema[]` 里加一条**，位置必须与 `easytier-core --help` 的顺序（或你希望的命令行顺序）一致：

   ```c
   { .name = "foo_bar", .kind = ET_K_STR, .def = NULL,
     .label = "Foo Bar", .tab = "privacy",
     .help  = "What foo bar does (--foo-bar parameter)",
     .core_flag = "--foo-bar", .core_emit = ET_E_VAL },
   ```

   `.kind` 决定控件与校验（`STR/INT/BOOL/ENUM/LIST/PASS`），`.core_emit` 决定发射规则
   （`ET_E_VAL` 有值才带参数、`ET_E_YES` 非 "0" 才输出裸旗标、`ET_E_REPEAT` 列表逐项、`ET_E_CUSTOM` 需在
   `etd_argv.c` 的 `emit_core_custom()` 里手写……）。**不确定就用 `ET_E_VAL`，不要发明新语义**；
2. **argv 自动生效**：不需要改 `etd_argv.c`（除非用 `ET_E_CUSTOM`），也不需要改 `etd_api_config.c`（表单会自动出现）；
3. **同步 po**：`.label` / `.help` 是 i18n 的 msgid，必须与 `po/templates/easytier.pot` 和 `po/zh_Hans/easytier.po` 对齐；
4. **重新生成字典**（po 改过就必须重跑，`tests/run_web.sh` 会 `--check`）：

   ```bash
   python3 tools/i18n_build.py
   ```

5. **补 golden 用例**（如果它影响命令行）：在 `tests/golden/<case>.uci` 里加配置，更新对应的 `.core.args`；
6. **验证**：`tests/run_host.sh`（argv 对比）+ `tests/run_web.sh`（渲染与 i18n）。

### 表单专属的补充表

有三类东西**不属于** schema，放在 `src/api/etd_api_config.c` 里，改动时不要动 `etd_schema.c`：

- `et_extras[]`：`et_config`（config.toml 文本域）与 `uuid`（machine-id 输入框），读写的是文件而不是 UCI；
- `et_form_def[]`：`.def` 为 NULL 但旧 CBI 有默认值的项（`listenermode=ON`、`tcp_port=11010`、
  `default_protocol=-`、`checktime=2` …）。**只影响表单初值，不影响 argv**；
- `et_deps[]` / `et_placeholders[]` / `et_enum_label[]` / `et_forward_opts[]`：显隐依赖、占位提示、枚举文案、多选项。

## 编码规范

- **语言**：C11 + POSIX（`-std=gnu11 -D_GNU_SOURCE`）；不要引入 C++、不要引入除 `libuci` / `json-c` 之外的新依赖；
- **缩进**：**Tab**（宽度 8），续行用 Tab 对齐；不要混空格；
- **告警**：`-Wall -Wextra -Werror`（`src/Makefile` 另加 `-Wno-unused-parameter`），**零告警**才算通过；
  不要在代码里用 `#pragma` 关告警来"解决"问题；
- **错误路径必须处理**：所有 `malloc` / `open` / `fork` / `exec` / `uci_*` 的返回值都要判断；
  失败要写日志（`et_log_to(ET_LOG_CORE, ...)`）或返回明确的 `ET_E*`，**不允许静默 `(void)` 掉可能失败的系统调用**；
- **资源**：`goto done` 或集中释放，`et_argv_free()` / `et_list_free()` / `et_buf_free()` 成对出现；
  `fork()` 的子进程里只允许 async-signal-safe 的调用，且必须 `_exit()` 而不是 `exit()`；
- **字符串与命令**：拼命令行**必须**用 argv 数组 + `execvp/execve`，**禁止** `system()` 拼接未转义的路径或用户输入；
  确需 shell 语义（管道）时用 `et_run_shell()` 并在调用处注释说明为什么必须走 shell；
- **敏感值**：密钥/密码（`network_secret`、`vpn_portal_private_key`、OIDC/Webhook secret、internal token）
  只能通过环境变量或 UCI 传递，不得进入 argv；展示命令行时用 `et_argv_visible()`（自动脱敏）；
- **文件写入**：用 `et_write_file()`（临时文件 + `rename`）保证原子性；需要跨文件系统搬移时用 `copy_stream()`；
- **日志文案**：面向用户的日志/错误文案保持中文原样（与旧版逐字一致，便于老用户对照），不要"顺手优化"措辞；
- **注释**：只在"为什么"不明显处写。凡是刻意保留旧实现怪行为的地方，必须写明出处（例如 `init.d:513-525`）。

## 云端构建纪律（必须遵守）

### 铁律：本地不编译、不测试

Windows 工作树上没有可用的交叉工具链，本地编译既不可信也会污染工作树。
**所有编译、链接、单元测试、ipk 打包都在 CNB 云原生开发环境里完成**，本地只跑 `scripts/cloud/*`。

### 三段式流程

```bash
# 1) 拉起（或复用）云环境：写入 .cloud/ssh.env（ET_SN / ET_SSH / ET_BRANCH）
scripts/cloud/start.sh

# 2) 装底座：基础工具 + OpenWrt SDK + 宿主 libubox/ubus/uci 源码构建
#    幂等：已装则跳过；冷启动约 60s（取决于镜像）
scripts/cloud/setup.sh

# 3) 日常编译（宿主 gcc，最快）
scripts/cloud/cc.sh                 # 只编译（objs）
scripts/cloud/cc.sh --link          # 编译并链接 easytierd / easytierctl
scripts/cloud/cc.sh --cross         # 交叉 gcc，仅头文件/语法验证
scripts/cloud/cc.sh --cross --link  # 交叉编译并链接（需 staging 已有依赖）

# 4) 权威构建：走 OpenWrt 包系统出 ipk（会自己构建依赖）
scripts/cloud/build.sh              # 只编 easytierd
scripts/cloud/build.sh --luci       # 连带 luci-app-easytier（需要 feeds/luci）
```

`setup.sh` 与 `build.sh` 也可以远程执行：`scripts/cloud/ssh.sh -s < scripts/cloud/setup.sh`。
多任务并行时用 `ET_BUILD_ID=xxx` 给每个任务独立的远程构建目录，避免互相清空：

```bash
ET_BUILD_ID=feat-argv scripts/cloud/cc.sh --link
```

### 环境会被回收：必须幂等自愈

- 平台在**空闲约 10 分钟后回收**云开发环境；
- 回收后重新拉起来的是**全新容器**：`/workspace` 下的工具链（`/workspace/.toolchain`：SDK、宿主 libubox/ubus/uci）、
  构建产物全部丢失；
- 因此所有脚本都必须**幂等自愈**，不能假设"上次装过就一直在"：

| 脚本 | 自愈行为 |
| --- | --- |
| `start.sh` | 调 CNB API 拉起 workspace，轮询 60×5s 拿到 `remoteSsh` 写入 `.cloud/ssh.env` |
| `ssh.sh` | 命令失败且 rc=255（连接层）时自动重新 `start.sh` 再重试；`-s` 模式会先把 stdin 落盘再重放 |
| `cc.sh` | 先探测 `gcc`/`make`，缺失时自动跑 `setup.sh`；同步失败时提示环境已回收 |
| `build.sh` | 先探测 `sdk/scripts/feeds`，缺失时自动跑 `setup.sh` |
| `keepalive.sh` | 每 `ET_KEEPALIVE_INTERVAL`（默认 120s）调一次 `keepalive_once.sh` |
| `keepalive_once.sh` | 维持常驻 SSH 主连接（`ssh -M -N -f` + ControlPath），心跳失败就重新拉起环境；再检查 `/workspace/.toolchain/host/sbin/ubusd` 与 `gcc`，缺失就重跑 `setup.sh` |

后台保活：

```bash
scripts/cloud/keepalive.sh &
# 或调整间隔
ET_KEEPALIVE_INTERVAL=120 scripts/cloud/keepalive.sh &
```

日志在 `.cloud/keepalive.log`。

### 常见坑（都踩过）

1. **SDK 的 `target-*/usr/include` 默认是空的**。OpenWrt SDK 不解包依赖库的头文件，
   所以交叉编译时经常报 `fatal error: json-c/json.h: No such file or directory`。
   结论：**日常用宿主 gcc（`cc.sh`）迭代，权威验证用包构建（`build.sh`）**；
   `cc.sh --cross` 只用来确认"代码本身没写错"，不要指望它链接成功。
2. **tar 同步必须带属主参数**。本地是 Windows/NTFS，直接打包会把 uid/gid、权限带进云端，
   导致 `make` 因权限或属主异常失败。正确写法（脚本里已经这么写）：

   ```bash
   tar czf - -C "$ROOT" --owner=0 --group=0 --numeric-owner \
       --exclude=.git --exclude=.cloud --exclude=build --exclude='*.o' . \
     | ssh ... "tar xzf - --no-same-owner --no-same-permissions -C '$REMOTE_SRC'"
   ```

3. **同步会先 `rm -rf` 远程目录**。所以不要在云端目录里放"只存在于云端"的文件；需要保留的产物请放到
   `/workspace/.toolchain` 或独立目录。
4. **`make -j8` 与 `ET_BUILD_ID`**：多人/多 agent 并行时必须给不同的 build id，否则互相删目录。
5. **SDK 下载走镜像**：`downloads.openwrt.org` 在云端经常超时，`setup.sh` 已按
   南大 → 中科大 → 清华 → 阿里 → 腾讯 → 官方 的顺序回退，并用 `sha256sums` 校验。
6. **`tar --zstd` 不一定可用**：脚本在失败时会退化为 `zstd -dc | tar -xf -`。
7. **不要在 `cc.sh` 里加 `-Wno-error`**。零告警是提交门槛，绕过它等于把问题推给包构建。

## 测试

两个入口，都在云端跑（本地只负责触发）：

```bash
bash tests/run_host.sh    # 宿主编译 + 单元/UCI 语义/写回往返 + golden argv 对比
bash tests/run_web.sh     # controller 语法与转发单测、模板 Lua/JS 语法、i18n 同步、C 渲染测试
```

- `tests/run_host.sh`：同步工作树 → `cc.sh` 全量编译（失败不致命，会继续单独编译本模块）→
  编译 `tests/argv_test`（覆盖 `etd_util/uci/schema/cfg/argv/toml/detect`）→ 跑 `tests/golden`。
  golden 目前 11 组样例，加上单元/UCI/写回断言共 **54 项**（准确数字以运行输出的
  `== 结果: N 项通过, M 项失败 ==` 为准），`*.notes` 用于断言"与旧版差异的说明文案"
  （例如 vpn_portal 旧写法被跳过）。
- `tests/run_web.sh`：六步——`luac -p` 检查 controller、`bridge_lua_test.lua` 用假 `luci.*` 校验 28 个端点
  的命令行/MIME/JSON body/上传落盘/菜单结构、模板内 `<% %>` 的 Lua 块与 `<script>` 的 ES5 语法、
  `tools/i18n_build.py --check`、编译运行 `tests/web_bridge_test`（config_get/save/render/schema + i18n）。

更细的命令、单步复现方式与真机前清单见 [docs/TESTING.md](docs/TESTING.md)。

## 提交流程

1. **先在云端过三关**：`scripts/cloud/cc.sh --link`（零告警）、`tests/run_host.sh`、`tests/run_web.sh`；
   改动涉及包结构时再加 `scripts/cloud/build.sh --luci`。
2. **路由名与旧端点保持一致**。`src/api/etd_routes.c` 里的名字（`api_status`、`get_log`、`upload_binary` …）
   就是 LuCI 的 URL，前端模板 `url("admin","vpn","easytier","<name>")` 直接依赖它。
   改名 = 改前端 = 破坏 18.06 上的兼容性，**除非同时改模板并说明理由**。
   新增路由时同步更新 `luci-app-easytier/root/usr/share/rpcd/acl.d/luci-app-easytier.json`（如有新的文件/权限需求）
   和 `luasrc/controller/easytier.lua` 的 `EP` 列表。
3. **改契约头只能追加**。`src/include/etd*.h` 是模块之间的边界（7 份契约头 + 5 份私有头）：
   - 可以新增函数、新增枚举值（加在末尾）、新增字段（加在结构体末尾）；
   - **不要删除/改名/改变已有函数签名或结构体字段顺序**，否则 `src/api`、`src/daemon`、`src/ctl` 与
     `tests/` 会一起崩，且测试桩（如 `web_bridge_test.c` 里的空路由表）也要跟着改；
   - 新增枚举值时，检查所有 `switch` 是否有 `default`，避免 `-Wswitch` 告警变成错误。
4. **行为差异必须留痕**：凡是与旧版不同的地方，要么在 `src` 里写注释说明"为什么"，要么更新
   [docs/C-PORT.md](docs/C-PORT.md) 的差异清单；不要出现"代码改了、文档还是旧说法"。
5. **不要顺手重构**。修一个 bug 就只改与它相关的行；格式化、重命名、拆文件放在单独 PR。
6. **不要提交编译产物**：`build/`、`*.o`、`.cloud/` 已在忽略列表里。

## 常见问题

**Q：为什么不把 LuCI 也换成纯 JS（LuCI2）？**
A：18.06 没有 `menu.d` JSON，菜单和模板渲染只能走 Lua1；而 JS 框架不适合低配路由器。保留 129 行 Lua 是
兼容性成本最低的方案，详见 [docs/C-PORT.md](docs/C-PORT.md#为什么还保留-lua)。

**Q：为什么状态页还有子进程？**
A：只剩三类必要场景：`easytier-core -V` / `easytier-cli <part>`（版本与连接信息）、GitHub latest tag 查询、
没有 `ifconfig` 时的 `ip addr` 回退。CPU/内存/流量/磁盘全部读 `/proc`、`sysfs`、`statvfs`。

**Q：为什么 `config_save` 里未知字段被静默忽略？**
A：为了让前端/后端版本不一致时平滑升级（旧模板多提交一个字段不应导致整次保存失败）。校验失败（类型/枚举非法）
才会返回错误。

**Q：本地能不能至少跑 `gcc -fsyntax-only`？**
A：不行。没有 libuci/json-c 头文件与正确的 include 路径，结论不可信；请用 `scripts/cloud/cc.sh`。

## 参考

- [LuCI Lua1 API](https://github.com/openwrt/luci/wiki)
- [OpenWrt UCI](https://openwrt.org/docs/guide-user/base-system/uci)
- [EasyTier 命令行参数](https://easytier.cn)
- [ES5 语法参考](https://www.ecma-international.org/ecma-262/5.1/)
