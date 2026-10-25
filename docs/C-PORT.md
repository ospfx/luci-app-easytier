# 移植档案：从 Lua + Shell 到 C

本文写给两类人：

- 想知道"`main` 分支和 `legacy/lua` 到底差在哪"的老用户；
- 接手 C 版、需要快速定位"旧代码的某段逻辑现在在哪"的贡献者。

所有结论都来自仓库代码。旧实现指分支 `legacy/lua`，新实现指 `main`。

```
git show legacy/lua:luci-app-easytier/luasrc/controller/easytier.lua   # 旧 controller
git show legacy/lua:luci-app-easytier/root/etc/init.d/easytier         # 旧 init.d
```

---

## 1. 移植范围与边界

### 搬进 C 的部分（后端全部）

| 旧位置 | 职责 | 现在 |
| --- | --- | --- |
| `luasrc/controller/easytier.lua`（1349 行） | 24 个 HTTP 端点、状态采集、连接信息缓存、上传/下载、Web 控制台配置 | `src/api/*.c` + `src/ctl/easytierctl.c` |
| `root/etc/init.d/easytier`（1014 行） | 启动/停止/重载、二进制自检与下载、argv 拼装、防火墙与接口编排、procd 参数、看门狗脚本生成 | `src/daemon/*.c` + `src/lib/etd_argv.c` |
| `root/usr/share/easytier/download.sh`（151 行） | GitHub 版本查询、镜像回退、下载解压 | `src/api/etd_api_ops.c` |
| `root/usr/share/easytier/firewall.sh`（206 行） | firewall/network 的 UCI 增删、延迟 reload | `src/daemon/etd_net.c` |
| `root/usr/share/easytier/utils.sh`（89 行） | TZ、架构、日志、空间、进程工具函数 | `src/lib/etd_*.c` |
| `luasrc/model/cbi/easytier.lua`（580 行） | 配置表单（84 个 `taboption`，分 general/privacy 两页） | `src/lib/etd_schema.c` + `src/api/etd_api_config.c` |

### 留在 Lua 的部分（只有壳）

`luci-app-easytier/luasrc/controller/easytier.lua` 现在是 129 行，只做三件事：

1. `index()` 注册 5 个页面与 28 个端点；
2. 把 query/form 拼成 `key=value`、把 JSON body 落盘为 `/tmp/et_body_*`，调用
   `/usr/sbin/easytierctl api <method> ...`，把 stdout 原样写回并设置正确的 `Content-Type`；
3. `upload_binary` 用 `http.setfilehandler` 收 multipart 落盘到 `/tmp/upload_<name>`，再把路径交给 C。

### 为什么还保留 Lua

- **LuCI Lua1 框架强制要求**：18.06 没有 `menu.d` JSON，菜单必须由 `entry()` 注册，页面必须由 `template()` 渲染；
- **模板仍是 Lua 模板**：6 个 `.htm` 里大量 `<% %>` / `<%: %>`（i18n）/ `<%=url(...)%>`，这些只能在 LuCI 的 Lua 运行时里求值；
- **把转发壳做薄，风险就小**：129 行里没有 UCI 读写、没有业务判断、没有文件操作（除 multipart 落盘），
  出问题的面从"整个后端"缩到"一次 exec 的参数拼装"。

反过来，**C 侧不实现 HTTP**：`easytierd` 不知道 uhttpd 的存在，也不实现 ubus 对象（见第 5 节）。
这让"同一套 handler 三条调用路径"成立：`easytierctl`、未来的 ubus、daemon 内部调用都走 `et_api_dispatch()`。

---

## 2. 行数对比

| 部分 | 旧（`legacy/lua`） | 新（`main`） | 说明 |
| --- | --- | ---: | --- |
| LuCI controller | 1349 | **129** | 只剩菜单注册 + 转发 |
| CBI 配置页 | 599 | **0** | `easytier.lua` 580 + `easytier_status.lua` 19；CBI 页面整体移除 |
| `init.d/easytier` | 1014 | **24** | start/stop/reload/status 各一行转发 |
| Shell 辅助脚本 | 446 | **0（仍随包安装但无人调用）** | `download.sh` 151 + `firewall.sh` 206 + `utils.sh` 89 |
| C 后端 | 0 | **10 721** | `src/` 下全部 `.c` + `.h`：`include` 816 / `lib` 3 803 / `daemon` 2 163 / `api` 3 767 / `ctl` 172 |
| 前端模板 | 6 643 | 6 643 | 6 个 `.htm` 未改（ES5 + Lua1 模板） |
| 测试 | 0 | **1 566** | `argv_test.c` 794、`web_bridge_test.c` 272、`bridge_lua_test.lua` 290、两个 `run_*.sh` 共 210 |

合计：旧后端（controller + CBI + init.d + shell）约 **3 408 行**脚本，新后端约 **10 721 行** C。
行数变多换来的是：单一数据源（schema）、一次编译期检查、没有字符串拼 shell、没有"每个选项在四处重复"。

---

## 3. 模块映射表

### 3.1 controller 端点

| 旧函数（controller） | 新位置 |
| --- | --- |
| `index()` | `luci-app-easytier/luasrc/controller/easytier.lua:index()` |
| `act_status()` | `src/api/etd_api_status.c:et_api_status()` |
| `get_tun_info()` | `src/api/etd_api_status.c:et_api_tun_info()` |
| `get_disk_space()` | `src/api/etd_api_status.c:et_api_disk_space()` |
| `get_log()` / `get_log_size()` / `clear_log()` | `src/api/etd_api_log.c:et_api_log_get/_size/_clear()` |
| `get_wlog()` / `get_wlog_size()` / `clear_wlog()` | `src/api/etd_api_log.c:et_api_wlog_get/_size/_clear()` |
| `clear_version_cache()` | `src/api/etd_api_log.c:et_api_version_cache_clear()` |
| `act_conninfo()` + `read_conninfo_cache()` + `write_conninfo_cache()` | `src/api/etd_api_conninfo.c:et_api_conninfo()`（`cache_read` / `cache_write`） |
| `upload_binary()` | `src/api/etd_api_ops.c:et_api_upload_binary()`（`upload_single` / `upload_archive`） |
| `test_binary(path)` | `src/lib/etd_detect.c:et_test_binary()` |
| `cleanup_files(...)` | `src/daemon/etd_service.c:et_remove_temp_files()` |
| `get_upload_config()` / `save_upload_config()` | `src/api/etd_api_ops.c:et_api_upload_config_get/_save()` |
| `download_easytier()` + `download_file()` + `check_dependencies()` + `get_github_proxies()` | `src/api/etd_api_ops.c:et_api_download_start()` + `dl_run()` / `dl_fetch()` / `dl_proxies()` |
| `download_progress()` | `src/api/etd_api_ops.c:et_api_download_progress()` |
| `cancel_download()` | `src/api/etd_api_ops.c:et_api_download_cancel()` |
| `get_web_config()` / `save_web_config()` | `src/api/etd_api_web.c:et_api_web_config_get/_save()` |
| `check_web_status()` | `src/api/etd_api_web.c:et_api_web_status()` |
| `reset_database()` | `src/api/etd_api_web.c:et_api_web_db_reset()` |
| `restart_service()` | `src/api/etd_api_service.c:et_api_service_restart()` |
| `toggle_core()` / `toggle_web()` | `src/api/etd_api_service.c:et_api_toggle_core/_web()`（共用 `et_api_toggle()`） |
| `calc_uptime()` | `src/api/etd_api_priv.h:et_api_uptime_text()` + `src/lib/etd_util.c:et_format_uptime()` |
| `detect_arch()` | `src/lib/etd_detect.c:et_detect_arch()` |
| `safe_exec()` / `safe_read_file()` | `src/lib/etd_util.c:et_run()` / `et_read_file()` |
| 新增（旧版没有） | `src/api/etd_api_config.c` 的 `config_get` / `config_save` / `config_render` / `schema` |

### 3.2 `init.d/easytier`

| 旧函数 | 新位置 |
| --- | --- |
| `check()` | `src/daemon/etd_service.c:et_bin_ok()` / `et_bin_version()`；`etd_net.c:et_net_ip_forward_enable()` |
| `get_tz()` | `src/daemon/etd_service.c:et_get_tz()` |
| `check_bin()` | `etd_service.c:et_resolve_core_bin()`（空间自愈 + 补执行位 + 触发下载） |
| `check_config()` | `etd_service.c:et_check_config()` + `et_ports_from_toml()` |
| `get_etconfig()` | `src/lib/etd_cfg.c:et_cfg_load()` |
| `start_et()` 的 **argv 拼装块**（`etcmd=etcmd` 分支，init.d 472~653） | `src/lib/etd_argv.c:et_argv_core()`；特殊规则在 `emit_core_custom()` / `emit_port()` / `emit_relay_whitelist()` / `emit_vpn_portal()` |
| `start_et()` 其余部分 | `src/daemon/etd_service.c:et_start_et()`（清理残留 → config 校验 → 启动 → 防火墙 → 时间戳） |
| `start_web()` | `etd_service.c:et_start_web()` + `etd_net.c:et_net_apply_web_firewall()` |
| `set_firewall()` | `src/daemon/etd_net.c:et_net_apply_firewall()`（`fw_rule` / `fw_zone_ensure` / `fw_forward`） |
| `start_service()` | `etd_service.c:et_service_start(true)`（前台监督）/ `et_service_start(false)`（后台拉起） |
| `stop_service()` | `et_service_stop()` + `et_teardown()` + `etd_net.c:et_net_cleanup_all/_web()` |
| `reload_service()` | `etd_service.c:et_service_restart()` |
| `service_triggers()`（procd 触发器） | 取消；改为 `et_net_schedule_reload()`：commit 后延迟 5 秒 reload network/firewall（锁文件防重复，残留锁 60 秒可接管） |
| procd `respawn threshold/timeout/retry` | `src/daemon/etd_super.c:et_slot_died()`（3600s / 5s / 20 次） |
| procd 的日志轮转（60 秒 / 5120KB / 500 行） | `etd_super.c:et_rotate_inplace()` |
| 生成 `/tmp/easytier_check` 的看门狗 shell | `src/daemon/etd_watchdog.c:et_watchdog_main()` |
| procd 的 `term_timeout` / 信号级联 | `etd_super.c:et_sup_shutdown()`（TERM → 5 秒 → KILL） |

### 3.3 Shell 辅助脚本

| 旧文件:函数 | 新位置 |
| --- | --- |
| `download.sh:get_proxy_list` | `src/api/etd_api_ops.c:dl_proxies()` |
| `download.sh:get_latest_version` | `src/api/etd_api_status.c:github_latest_tag()` / `latest_tag()` |
| `download.sh:download_binary` / `check_and_download` / `handle_uploaded_binary` | `etd_api_ops.c:dl_run()` / `dl_fetch()` / `et_api_upload_binary()` |
| `firewall.sh:add_firewall_rule` | `src/daemon/etd_net.c:fw_rule()` |
| `firewall.sh:set_firewall_rules` | `etd_net.c:et_net_apply_firewall()` |
| `firewall.sh:setup_network_interface` | `etd_net.c:et_net_apply_firewall()` 的 `network.EasyTier` 段 |
| `firewall.sh:setup_firewall_zone` | `etd_net.c:fw_zone_ensure()` |
| `firewall.sh:setup_forwarding_rules` | `etd_net.c:fw_forward()` |
| `firewall.sh:setup_web_firewall` | `etd_net.c:et_net_apply_web_firewall()` |
| `firewall.sh:clean_firewall_rules` | `etd_net.c:et_net_cleanup_all()` / `et_net_cleanup_web()` |
| `firewall.sh:apply_network_changes` | `etd_net.c:et_net_schedule_reload()` |
| `utils.sh:get_tz` | `etd_service.c:et_get_tz()` |
| `utils.sh:get_cpu_arch` | `etd_detect.c:et_detect_arch()` |
| `utils.sh:log_message` | `src/lib/etd_util.c:et_log_to()` |
| `utils.sh:manage_log_size` | `etd_super.c:et_rotate_inplace()`（原地截断） |
| `utils.sh:get_available_space` | `etd_util.c:et_df_available_kb()` |
| `utils.sh:safe_exec` | `etd_util.c:et_run()` |
| `utils.sh:is_process_running` | `src/lib/etd_proc.c:et_proc_running()` |
| `utils.sh:stop_process` | `etd_proc.c:et_proc_terminate()` |

### 3.4 配置模型与表单

| 旧 | 新 |
| --- | --- |
| CBI `easytier.lua` 的 84 个 `s:taboption(Value/Flag/ListValue/DynamicList/TextValue/MultiValue)` | `src/lib/etd_schema.c:et_schema[]`（120 条，含表单/daemon 专用项） |
| CBI 的 `depends` | `src/api/etd_api_config.c:et_deps[]`（`data-dep` 属性 + 页面 JS） |
| CBI 的 `.default` | `et_form_def[]`（仅表单初值）；argv 用 `.def` |
| CBI 的 `write`/`cfgvalue`（config.toml、machine-id） | `et_extras[]` + `et_api_config_save()` 的 `et_config` / `uuid` 分支 |
| CBI 的 `_()` 翻译 | `src/lib/etd_i18n.c`（po→JSON 字典） |
| `model/cbi/easytier_status.lua`（19 行） | 删除；状态页是纯 `template` |

---

## 4. 行为差异清单

只列真实存在、且在代码里能指到位置的差异。

### 4.1 状态采集不再 fork 一堆外部命令

旧版一屏状态要起 `pgrep`×2、`top`×2、`pidof`×4、`awk`×4、`df`×2、`ls -lh`×2、`ip`×4、`ifconfig`×1。
新版：

- CPU：两次采样 `/proc/<pid>/stat` 的 `utime+stime` 求差分（`etd_api_status.c:cpu_pair()`）。
  注释里写明了原因：容器启用 time namespace 后 `/proc/uptime` 与 `starttime` 不同源，用"总时长"会算出负数；
- 内存：`/proc/<pid>/status` 的 `VmRSS`（`et_proc_rss_kb()`），输出格式仍是 `"%.2f MB"`；
- 流量：`/sys/class/net/<if>/statistics/{rx,tx}_bytes`；
- MTU：`/sys/class/net/<if>/mtu`；
- IPv6：`/proc/net/if_inet6`（不再 `ip -6 addr`）；
- 磁盘：`statvfs`（`et_df_available_kb()`），不再 `df`；
- 进程存在性：扫 `/proc/*/cmdline` 与 `/proc/*/comm`（`etd_proc.c`），不再 `pgrep`/`pidof`/`ps|grep`。

仍然会起子进程的只有：`easytier-core -V`、`easytier-cli <part>`、GitHub latest tag（`curl`）、
以及没有 `ifconfig` 时的 `ip addr show dev <if>` 回退。字段名与格式（`"3天 04小时05分06秒"`、`"1.2 MB"`）保持不变。

### 4.2 下载：后台异步 + 可取消 + 原子安装

- 旧版 `download_easytier` 在 HTTP 请求里同步跑完整个下载（每个镜像 300 秒超时，最多 5 个），
  用户看不到进度，取消只在"切换镜像"时生效；
- 新版 handler 只做参数与依赖校验（版本号、架构、`unzip`），然后 `dl_spawn()` 双 fork + `setsid()`，
  立刻返回 `{"success":true,"progress":1,...}`；
- 进度写 `/tmp/easytier_download_progress`（JSON：`progress/message/url/error`），前端轮询 `download_progress`；
- 取消写 `/tmp/easytier_download_cancel`；`run_cancellable()` 每 200ms 检查一次，命中就 `SIGKILL` 掉 curl/wget；
- 安装用 `copy_stream()`：先写 `<目标>.etnew`，`fsync` 后 `rename`，因此目标要么是旧文件、要么是完整新文件。
  旧实现里上传走 `mv`（同文件系统是 rename，跨文件系统退化为拷贝 + 删除，非原子），
  在线下载安装则直接 `cp src dest` 覆盖目标——中途失败会把旧二进制写坏，被 rc 拉起时 exec 失败。

### 4.3 `default_protocol` 为空时不输出裸旗标

旧 init.d 的写法是 `[ "$default_protocol" = "-" ] || --default-protocol $x`。
当值为空时，展开结果只剩 `--default-protocol`，`easytier-core` 因缺少参数直接报错。
新版（`etd_argv.c:emit_core_custom()`）：

```c
if (!v || !*v) {
    note(notes, "default_protocol: 空值，已跳过 --default-protocol"
                "（旧实现会输出裸旗标，easytier-core 会因缺少参数报错）");
    return;
}
```

这属于**有意修正**，并在 golden 的 `.notes` 里有断言。

### 4.4 日志轮转：原地截断，不用 tmp + rename

旧版在 v2.6.4 **之前**用 `tmp + rename` 轮转，正在写日志的 core/web 进程仍持有被删 inode 的 fd，
结果是"日志文件不再增长、磁盘还被旧 inode 占着"。v2.6.4 改成原地截断（`tail → : > log → cat`），C 版沿用：

- `etd_super.c:et_rotate_inplace()`：`open(file, O_WRONLY|O_TRUNC)` 后回写最后 500 行，每 60 秒检查一次，
  超过 5120KB 才动手；
- `etd_api_log.c:log_clear()`：清空日志同样是 `O_TRUNC`，不是 `unlink`；
- 注意：`src/lib/etd_util.c:et_log_rotate()` 仍是 `tmp + rename` 实现，但**监督循环没有用它**
  （保留它只为通用工具完整性，见第 5 节）。

### 4.5 `upload_binary` 增加来源路径白名单

旧版直接对参数路径做 `chmod`/`unlink`/`mv`，而参数来自 multipart 文件名，恶意文件名可以把操作带出 `/tmp`。
新版 `etd_api_ops.c:upload_src_ok()` 要求：

- 必须是绝对路径；
- 必须是普通文件（不存在则放行，交给后续校验给出与旧版一致的错误文案）；
- 前缀不得命中 `/bin` `/sbin` `/usr` `/lib` `/lib64` `/boot` `/etc` `/dev` `/proc` `/sys` `/var/lib` `/var/run`。

另外 `web_db_reset` 也对 `db_path` 做了前缀白名单（`etd_api_web.c:db_path_safe()`）。

### 4.6 未知 `etcmd` 按默认模式处理

`et_argv_core()` 只识别 `config` 与 `web`，其余值（包括空值与拼写错误）一律走默认分支。
旧实现用 `if [ "$etcmd" = "etcmd" ]` / `if [ "$etcmd" = "web" ]` 两个独立判断，未知值两个分支都不进，
`core_cmd` 为空，最终只带着 `--console-log-level` 启动（表现为"起来了但什么都没配"）。

### 4.7 i18n：Lua 仍用 .lmo，C 渲染的表单改用 po→JSON

- Lua 模板与 controller 继续用 LuCI 的 `_()` / `<%: %>`（`.lmo`），这部分没变；
- 配置表单的 HTML 由 C 生成，拿不到 LuCI 的翻译表，因此构建期用 `tools/i18n_build.py` 把
  `po/*/easytier.po` 转成 `/usr/share/easytier/i18n/<lang>.json`（msgid → msgstr 扁平字典，`en.json` 是 identity），
  运行时 `etd_i18n.c` 按 `LC_ALL → LC_MESSAGES → LANG → uci luci.main.lang → HTTP_ACCEPT_LANGUAGE → en` 选语言；
- 影响范围仅限"C 渲染出来的文案"（表单标签/帮助/枚举/按钮）。如果某条文案只在模板里出现，
  它仍然只走 `.lmo`。

### 4.8 其它已核实的小差异

| 差异 | 位置 | 原因 |
| --- | --- | --- |
| machine-id 落盘失败也照常下发 `--machine-id` | `etd_argv.c:machine_uuid()` | 旧实现 `touch` 失败后 `cat` 回读也为空，Web 模式直接退化；落盘只是持久化 |
| 空间不足自愈时用 libuci 写回并 commit | `etd_service.c:et_resolve_core_bin()` | 旧实现用 `sed -i` 改配置文件；用 libuci 才能保证格式与并发安全 |
| 停止时的延迟 reload 放到 core/web 两个分支之后 | `etd_service.c:et_teardown()` | 旧实现把 reload 放在 core 分支里，此时 web 规则还没删，删除不会被 commit |
| 启动日志里的命令行会脱敏 | `etd_service.c` 用 `et_argv_visible()` | 旧实现把完整命令行（含 `--network-secret`）写进日志 |
| `easytier-web-em` 也能被识别 | `etd_api_status.c:et_api_pid_web()` | 内核把 `comm` 截断到 15 字节，`easytier-web-embed` 会变成 `easytier-web-em` |
| 看门狗不再落 `/tmp/easytier_check` | `etd_watchdog.c` | 直接 fork 子进程跑循环，用 `SIGUSR1` 请求重启；同时去掉旧脚本 `du -k = 1000` 才清日志的边角逻辑 |
| 保存时只写有变化的项 | `etd_cfg.c:et_cfg_save()` | 避免每次保存都把 61 条 schema 默认值写进 `/etc/config/easytier`；但表单提交的初值（`listenermode=ON` 等）与现状不同，仍会落库 |

---

## 5. 尚未完成与待验证

如实记录，不做美化。

### 未实现

1. **ubus 对象**。`src/include/etd_api.h` 的注释把 `ubus call easytier <method>` 列为调用路径之一，
   但 `src/` 里没有任何 `ubus_*` / `uloop_*` 调用（`grep -rn ubus src/` 只命中注释与 Makefile 的 `LIBS`）。
   当前所有调用都走 `easytierctl exec`。
   影响：`easytierd` 的包依赖里声明的 `libubox/libubus/libblobmsg-json` 实际未被使用，属于"为将来预留"。
2. **没有独立的集成测试脚本**。`setup.sh` 末尾带一段 ubusd/uci 冒烟（编译一个 ubus 客户端、注册对象、
   `ubus list` 验证、`uci get` 验证），但没有"假 easytier-core"的端到端用例：
   目前无法在云端自动验证"启动 → 写 firewall UCI → 延迟 reload → 停止清理"这条链路。
3. **真机验证清单未跑**。所有已知验证都停留在宿主 gcc + golden 与包构建层面，见
   [docs/TESTING.md](TESTING.md#5-上真机前的验证清单) 的 checklist。

### 待核对（代码里有疑点，未修改）

- `src/daemon/etd_net.c` 读取了 `udp_port`（`cfg_or(c, "udp_port", NULL)`），但 `et_schema[]` 里没有这个选项，
  所以单独的 `easytier_udp` 规则实际不会被创建；tcp 端口那条规则用的是 `proto="tcp udp"`，功能上覆盖了 UDP。
  需确认这是有意（只保留一条合并规则）还是历史遗漏。
- `src/lib/etd_util.c:et_log_rotate()` 仍是 `tmp + rename`，与"原地截断"的结论相反；当前无人调用它，
  但如果将来有人图省事改用它，会重新引入 fd 指向已删 inode 的问题。
- `luci-app-easytier/root/usr/share/easytier/{download,firewall,utils}.sh` 仍在包里（`luci.mk` 会整树安装
  `root/`），但已无任何引用。它们与 C 实现是两套逻辑，留着容易误导后来者。
- `src/ctl/easytierctl.c:do_service()` 用 `system()` 转发 `"/usr/sbin/easytierd service <sub>"`，
  而整串被 `et_shell_quote()` 包成了一个带空格的单参数；该文件也没有 `#include <sys/wait.h>` 却使用了
  `WIFEXITED()`。`easytierctl service ...` 是否真能工作需要在真机/云端确认（`/etc/init.d/easytier` 不依赖它，
  它直接调用 `easytierd`，所以主路径不受影响）。
- GitHub Actions 的 `PACKAGES` 列表没有 `easytierd`，而 `luci-app-easytier` 的 `LUCI_DEPENDS` 里有 `+easytierd`，
  CI 产物会缺包。见 `.github/workflows/build.yml`。
