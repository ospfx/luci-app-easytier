# 交接文档：把 easytierctl 重写为上游兼容的 easytier-cli

> 最后更新：2026-11-06（+08:00）
> 关联：docs/CLI-PORT.md（协议档案，含抓包实证）、docs/C-PORT.md（C 重写总档案）

## 0. 一句话现状

C 重写版已经**不带任何上游二进制**直连 core 的 RPC，实现了上游 easytier-cli 2.7.0 的
**全部 17 个子命令**（peer / connector / mapped-listener / stun / route / peer-center /
vpn-portal / node / service / proxy / acl / port-forward / whitelist / stats / logger /
credential / gen-autocomplete），并保留本项目的 api / doctor / version。

在 8 节点真实组网（测试路由 root@10.0.15.254）上与上游 easytier-cli 2.7.0-8b7f1f01 对拍：
peer / route / node / connector / mapped-listener / vpn-portal / proxy / acl stats /
port-forward list / whitelist show / stats show / logger get / credential list / stun 的
人类可读输出**逐字节一致**（动态字段如延迟/流量除外）。

## 1. 用户已确认的决策（不要再改方向）

| 议题 | 决定 |
| --- | --- |
| CLI 形态 | **合并成单一 easytier-cli**，api / service / doctor / version 作为它的子命令 |
| 实现方式 | **全量 C 复刻**，不依赖上游 easytier-cli 二进制 |
| 命名 | tag/标题 v2.7.0-c，版本号跟随 version.mk（2.7.0），标注 C 重写 / ospfx 作者版 |
| Release | 直接在 v2.7.0-c 上**删旧资产换新**，不新增 tag |
| 范围 | 上游自己没写完的不管；但现有对齐不能破坏现有功能 |
| 改动范围 | 只改工作流 + 为对齐版本/CLI 所需的代码；不碰无关业务代码 |

## 2. 架构（重要，别再往 ctl/ 里堆代码）

```
src/include/etd_cli.h       CLI 公共 API：选项、Markdown 表格、JSON、多实例扇出、
                            实例选择器、RPC 调用（含分片重组）、格式化、子命令声明
src/lib/etd_pb.c            最小 protobuf 编解码
src/lib/etd_rpc.c           自研 RPC 客户端：帧/头/RpcPacket、txid、压缩协商、
                            **total_pieces/piece_idx 分片重组**、domain_name 支持
src/lib/etd_cli_common.c    表格/JSON/RPC/格式化/扇出/状态页分区渲染的公共实现
src/lib/etd_cli_peer.c      peer / route / node
src/lib/etd_cli_extra_a.c   connector / mapped-listener / vpn-portal / proxy
src/lib/etd_cli_extra_b.c   stun / peer-center / stats / logger / credential /
                            gen-autocomplete
src/lib/etd_cli_extra_c.c   acl / port-forward / whitelist
src/ctl/easytierctl.c       只做 main + 全局选项 + api/service/doctor/version 分发
                            （安装名是 /usr/bin/easytier-cli）
src/api/etd_api_conninfo.c  状态页连接信息：**进程内**调用 et_cli_render_part()，
                            不再 fork easytier-cli
```

为什么子命令都在 lib/ 而不是 ctl/：状态页要在 **easytierd 进程内**直接渲染这些表
（旧实现每个分区 fork 一次 easytier-cli，路由器上约 200ms/次；现在是毫秒级 RPC）。
测试探针 tests/cli_probe.c 只链接 lib/ 即可交叉编译，不需要 libuci/json-c。

## 3. 已实现（对照上游 2.7.0 源码）

| 上游命令 | RPC（服务/方法） | 输出 |
| --- | --- | --- |
| peer [list] | PeerManageRpc 1 + 3 + 7 | markdown 表，本机行 + 多 peer 行；排序 Local 优先 → PublicServer_ 前缀 → IPv4 → hostname |
| peer ipv6 | PeerManageRpc 7 + 3 + 2 | 6 列 markdown 表（本机行在首）+ 本机 provider 段（prefix / 租约表） |
| peer list-foreign [--trusted-keys] | PeerManageRpc 5 | "N Network Name: xxx" + 每个 peer 一行 conns；--trusted-keys 追加 trusted_keys 段 |
| peer list-global-foreign | PeerManageRpc 6 | "Peer ID: N" + 每个外网一行（含 PeerIds: [a, b]） |
| route [list] | PeerManageRpc 3 + 1 + 7 | 13 列 markdown 表；next_hop 关联不上时跳过该行（与上游一致） |
| route dump | PeerManageRpc 4 | `response: ` + core 生成的 ~50 KB Debug 文本，整段输出（不要用定长缓冲） |
| node [info｜config] | PeerManageRpc 7 | 键值表（Virtual IP 作表头、Interface IPv4/IPv6、Listener 0 基） |
| connector [list｜add｜remove] | ConnectorManageRpc 1 + ConfigRpc 1 | Rust {:#?} 版式 |
| mapped-listener [list｜add｜remove] | MappedListenerManageRpc 1 + ConfigRpc 1 | Rust {:#?} 版式 |
| vpn-portal [add-client｜remove-client｜clear-clients] | VpnPortalRpc 1 + ConfigRpc 1 | portal_name / listener / 每个 client 一段 |
| proxy | TcpProxyRpc 1（domain=tcp/kcp_src/kcp_dst/quic_src/quic_dst） | markdown 表 |
| acl stats | AclManageRpc 1 | AclStats 的 Display 文本（Global/ConnTrack/Rules） |
| acl set <TOML\|@file> | ConfigRpc 1（InstanceConfigPatch.acl→AclPatch） | TOML 子集解析→acl.Acl；逐实例 "== label ==" + "ACL updated successfully (N chain(s))"；-o json 回显整份 ACL（group_secret 打码） |
| port-forward [list｜add｜remove] | PortForwardManageRpc 1 + ConfigRpc 1 | markdown 表 |
| whitelist [show｜set-tcp｜set-udp｜clear-tcp｜clear-udp] | AclManageRpc 2 + ConfigRpc 1 | 两行文本 |
| stats [show｜prometheus] | StatsRpc 1 / 2 | Value 用 BINARY humanize、Labels 用 "k=v, k=v" |
| logger [get｜set] | LoggerRpc 2 / 1 | Current Log Level: xxx |
| credential [generate｜revoke｜list] | CredentialManageRpc 1 / 2 / 3 | rounded 风格表格 |
| stun | PeerManageRpc 7 的 NodeInfo.stun_info | Rust {:#?} 版式（见 §5 差异） |
| gen-autocomplete <shell> | — | bash/zsh/fish/powershell/elvish/nu 静态补全脚本 |
| -o json / --no-trunc / -i / -n / -p / -v | — | 与上游 clap 语义一致；多实例时输出 {instance_id, instance_name, result} |

**`-o json` 的统一规则（很容易踩）**：
- 单实例直接输出值本身；多实例才包 `{instance_id, instance_name, result}`；
- 首字符就是 `{`/`[`，根节点前**不能**有换行；
- 走 pbjson（proto3 JSON）语义：默认值（空串/空数组/0/false/枚举 0）**不输出**，
  枚举输出变体名（`"PortRestricted"`），int64/uint64 输出**字符串**，重复字段是真数组；
- f64 一定带小数点（0 输出 `0.0`），其余用最短往返表示。

## 4. 关键协议坑（这次踩出来的）

1. **RPC 响应会被分片**：core 按 UDP 载荷预算把大 RpcPacket 切成 total_pieces 片
   （easytier-core/src/rpc/packet.rs:252-278）。只读第一帧只能拿到碎片，
   表现为"peer 列表有行但 stats 全是 0"。必须按 transaction_id + piece_idx 重组。
2. **响应外层是 RpcResponse{response=1, error=2}**：错误帧不能当成空结果，
   否则服务名/方法序号写错时表现为"表格空行"。现在会显式打印错误。
3. **map 字段是 repeated entry{1:key,2:value}**：et_pb_find 只给第一条，
   必须 et_pb_foreach_msg 逐条遍历（peer-center 的 global_peer_map、stats 的 labels）。
4. **TcpProxyRpc 需要 descriptor.domain_name**：按 tcp/kcp_src/… 注册。
5. **实例选择器**：请求体是 {2: 请求, 3: 5000}；请求里的 InstanceIdentifier 是
   {1: UUID} 或 {2: {1: name}}；不指定时先查 WebClientService/5 做多实例扇出。
6. **数值格式**：humansize::DECIMAL 固定两位小数（8.50 kB），BINARY 用于 stats
   （2.58 MiB）；peer 表 lat 是 %.2f，route 表 next_hop_lat 是 Rust f64 Display（11.554）。

## 5. 有意的实现差异

| 项 | 差异 | 原因 |
| --- | --- | --- |
| stun | 直接读 core 已采集的 NodeInfo.stun_info，不自己发 STUN 包（不等 25s） | 避免 C 里再实现一套 STUN 采集；核心结果同源 |
| acl set 输入 | 只支持 TOML / @file；上游还接受 3 种 JSON 形态 | JSON 形态需要 json-c 依赖与额外映射，收益低；TOML 是文档主推写法 |
| peer-center 行序 | 按 RPC 返回的 map 顺序（确定性），上游是 HashMap 随机序 | 上游每次顺序不同，无法也不该对齐 |
| -v（verbose） | 只打调试信息到 stderr，不输出上游那种原始 protobuf JSON | 原始消息 JSON 需要完整 serde 映射，收益低 |
| service | 保持本项目语义（转发给 easytierd 的 start/stop/...），且必须补 `service` 关键字 | OpenWrt 包，服务由 procd 管理 |
| 子命令 --help | 手写三段式帮助，不是 clap 逐字文本 | 只影响可读性，命令/选项含义一致 |
| 终端宽度裁剪 | 统一从最右列往左丢；上游每个命令各有 drop 列清单 | 只在交互式终端窄于表宽时可见，管道/重定向一致 |
| gen-autocomplete | 手写静态脚本 | clap_complete 生成产物不稳定，静态脚本够用 |

## 6. 验证环境与对拍方法

- 测试路由：root@10.0.15.254（ImmortalWrt 25.12.1，aarch64_cortex-a53）。
  上游参照二进制：/root/et/easytier-core、/root/et/easytier-cli（2.7.0-8b7f1f01）。
- 组网（用户提供，只能用这一份）：

  ```sh
  /root/et/easytier-core --network-name ldeus-net --network-secret 'bjl385248CV?' \
    -i 10.0.200.247/24 --hostname bj-router-2 -p wss://152.136.26.248:2096 \
    --rpc-portal 127.0.0.1:15888 &
  ```

- 本地交叉编译探针（开发机无 gcc，用 zig）：

  ```sh
  OUT=build/cli_probe bash tests/build_cli_probe.sh
  scp build/cli_probe root@10.0.15.254:/root/et/cli_probe
  ssh root@10.0.15.254 '/root/et/cli_probe -p 127.0.0.1:15888 peer'
  ```

- 逐命令对拍（busybox 没有 diff，用 md5sum）：

  ```sh
  U=/root/et/easytier-cli; M=/root/et/cli_probe
  for c in peer route node connector mapped-listener vpn-portal proxy \
           "acl stats" "port-forward list" "whitelist show" "stats show" \
           "logger get" "credential list" stun; do
    a=$($U -p 127.0.0.1:15888 $c | md5sum); b=$($M -p 127.0.0.1:15888 $c | md5sum)
    [ "$a" = "$b" ] && echo "OK   $c" || echo "DIFF $c"
  done
  ```

- **2026-11-06 对拍结果**（上游 2.7.0-8b7f1f01 vs 本实现，同一 8 节点组网）：
  - 文本模式逐字节一致：`route`、`node`、`node config`、`connector`、`mapped-listener`、
    `vpn-portal`、`proxy`、`acl stats`、`port-forward list`、`whitelist show`、
    `stats show`、`logger get`、`credential list`、`peer ipv6`、`peer list-foreign`、
    `peer list-global-foreign`、`route dump`（2151 行全等）；
  - `peer`：去掉动态数值（lat/loss/rx/tx）后逐字节一致；
  - `-o json`：`peer/route/node/connector/mapped-listener/vpn-portal/proxy/acl stats/
    port-forward list/whitelist show/logger get/credential list/peer ipv6/peer list-foreign/
    peer list-global-foreign` 全部逐字节一致（`peer` 需忽略动态数值）；
  - `-i <uuid>`、`-n <name>` 与上游行为一致（`-n default` 命中，错误名同样报
    `No instance matches the selector`）；
  - 退出码：成功 0，失败 1（与上游一致）。

- 写操作验证要另起临时 core（不要动 15888 上的用户网络）：

  ```sh
  /root/et/easytier-core --no-tun -i 10.99.0.1/24 --network-name ettest \
    --network-secret ettest123 -l tcp://0.0.0.0:21010 -l udp://0.0.0.0:21010 \
    --rpc-portal 127.0.0.1:15889 &
  /root/et/cli_probe -p 127.0.0.1:15889 connector add tcp://1.2.3.4:11010
  ```

## 7. 构建 / 发布流程

1. push 到 gh main → workflow sr1010-immortalwrt-apk.yml 自动构建（约 20–45 分钟）；
2. workflow 自己用 softprops/action-gh-release（overwrite_files: true）把三个包
   覆盖到 v2.7.0-c，不需要手工删资产；
3. 失败时构建日志会推到 ci-logs 分支的 ci-fail-<run_id>.log（git fetch gh ci-logs）；
4. 装机：apk add --allow-untrusted --force-overwrite /tmp/<pkg>.apk。

## 8. 踩过的坑（别再踩）

| 坑 | 现象 / 解法 |
| --- | --- |
| PowerShell -replace + Set-Content 改带中文的源文件 | UTF-8 被破坏成乱码；一律用 edit/write 工具改文件 |
| git commit -m 多行中文经 pwsh | 参数被拆散提交失败；用 -F 文件 |
| GitHub Actions 日志 | 无 admin 权限时 API 403；靠 ci-logs 分支回传 |
| GitHub API 频率限制 | 未认证 60 次/小时 |
| git push 走 ssh:22 | 直连常超时；~/.ssh/config 配 SOCKS 代理 |
| 路由器 tcpdump -w | 写不出文件；tcpdump -i any -x -n port 15888 打屏 |
| tcpdump 过滤器引号 | pwsh→ssh→ash 会把引号吃掉 |
| busybox 没有 nohup/timeout/diff | 用 & + sleep；比较用 md5sum |
| init.d 里 USE_PROCD=1 但没注册 procd 实例 | rc.common 会覆盖脚本里的 start/stop/reload：stop 无效、reload 永不重启；别设 USE_PROCD |
| 按进程名 kill easytier-core | 会误杀用户手工起的其他 core；按 children pidfile + /proc/pid/exe 精确匹配 |
| char *rows[N][2] 强转 char *** | 指针步长错位 → SIGSEGV；先摆成 char **rows[N] |
| et_pb_find 复用 p/n 游标 | 后续字段会在上一个字段内容里找 → 解析结果为空 |
| 只读第一片 RPC 帧 | 大响应被分片，stats 全 0；必须按 piece_idx 重组 |
| 实例 UUID 长度写死 24 | proto 的 common.UUID 是 4 个 uint32，序列化长度 16–24 字节可变；长度不符时 ListNetworkInstance 解析为空 → 多实例扇出静默退化成单实例 |
| 把 RpcResponse.error 当空结果 | 服务/方法写错时静默空表；要显式报错 |
| /tmp noexec | 上游二进制要放 /root/et/ |
| zig cross-compile | 需要 ZIG_GLOBAL_CACHE_DIR / ZIG_LOCAL_CACHE_DIR 指到可写目录，否则 AccessDenied |

## 9. 剩余可做（非阻塞）

- 终端宽度裁剪改成上游的 per-command drop/optional 列清单（只影响交互式终端）
- `-v` 输出原始 protobuf JSON（需要完整 serde 映射）
- 平台：2.6.4 内核不认识 2.7.0 新增旗标（`--tcp-stun-servers`、`--socket-mark`、
  `--vpn-portal-client(-group)`、`--vpn-portal-private-key`）。这些只有用户显式配置时才发射，
  默认不会带；若以后要同时支持 2.6.4，需要按 core 版本过滤 argv。
- -v 输出上游那种原始 protobuf JSON（需要完整 serde 映射）
