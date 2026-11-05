# 交接文档：把 easytierctl 重写为上游兼容的 easytier-cli

> 最后更新：2026-11-06 04:20（+08:00）
> 关联：`docs/CLI-PORT.md`（协议档案，含抓包实证）、`docs/C-PORT.md`（C 重写总档案）

## 0. 一句话现状

C 重写版已经能**不带任何上游二进制**直连 core 的 RPC 并渲染 `peer` / `route` / `node` 三张表，
单机场景与上游 `easytier-cli` **逐字节一致**；多 peer 行、其余 13 个子命令、conninfo 进程内改造仍未做。

## 1. 用户已确认的决策（不要再改方向）

| 议题 | 决定 |
| --- | --- |
| CLI 形态 | **合并成单一 `easytier-cli`**，`api` / `service` / `doctor` / `version` 作为它的子命令（方案 b） |
| 实现方式 | **全量 C 复刻**，不依赖上游 `easytier-cli` 二进制 |
| 命名 | tag/标题 `v2.7.0-c`，版本号跟随 `version.mk`（2.7.0），标注 C 重写 / ospfx 作者版 |
| Release | 直接在 `v2.7.0-c` 上**删旧资产换新**，不新增 tag |
| 范围 | 上游自己没写完的不管；但现有对齐不能破坏现有功能 |
| 改动范围 | 只改工作流 + 为对齐版本/CLI 所需的代码；不碰无关业务代码 |

## 2. 仓库状态

本地 `E:\dsh\luci-app-easytier`，远端 `gh` = `git@github.com:ospfx/luci-app-easytier.git`（main）。

关键提交（按时间）：

| commit | 内容 |
| --- | --- |
| `4ad40ed` | CI：只发 C 重写版三包，剔除上游 easytier/easytier-noweb；tag/标题 `v2.7.0-c` |
| `af01492` | `ETD_VERSION` 跟随 `PKG_VERSION` |
| `900a445` | 版本宏改 `-DETD_VERSION_NUM` + 字符串化；CI 失败日志回传 `ci-logs` 分支 |
| `f951264` | **新增 C 实现**：`etd_pb.c` / `etd_rpc.c` / `etd_cli_upstream.c`，`peer|route|node` |
| `63931b5` | 修 NodeInfo 嵌套层级（body→{1:…}→{1:NodeInfo}） |
| `59100e2` | 修 `node` 段错误（`char*[N][2]` 强转 `char***` 指针算术错位）+ listener 行 |
| `dacddf7` | 三个包 Makefile 的 `version.mk` 读取时机修正 |
| `72347bb` | node：过滤 `ring://`、补 `Interface IPv4` |
| `4942f2a` | 合并改名为 `easytier-cli`，装 `/usr/bin`，Lua 与 ACL 同步 |
| `67fd7fb` | 修 install 缺 `/usr/bin` 目录（上一次 CI 失败原因） |
| `16d5873` | 补 `ipv4_addr`（NodeInfo field 2） |
| `7e90bc0` | `help` 版面按上游重写（Commands/Options 清单 + `help` 子命令） |
| `0656c47` | 修 cidr 解析复用游标导致 hostname/peer_id 清空 |

## 3. 已实现（代码）

| 文件 | 作用 |
| --- | --- |
| `src/include/etd_pb.h` + `src/lib/etd_pb.c` | 最小 protobuf 编解码（varint / length-delimited / fixed / 跳过未知字段） |
| `src/include/etd_rpc.h` + `src/lib/etd_rpc.c` | core 自研 RPC 客户端：TCP 连接、`[u32LE 帧长][16B PeerManagerHeader][RpcPacket]`、txid 匹配、`accepted_algo=None` 避免 zstd |
| `src/include/etd_cli.h` + `src/ctl/etd_cli_upstream.c` | `peer` / `route` / `node` 三张 Markdown 表 + 本机行渲染 |
| `src/ctl/easytierctl.c`（安装名 `easytier-cli`） | 子命令分发：`peer|route|node|api|service|version|doctor|help`，`-p/--rpc-portal` |

## 4. 已验证（真机 `root@10.0.15.254`，ImmortalWrt 25.12.1 / aarch64_cortex-a53）

- `easytier-cli version` → `easytier-cli 2.7.0`（原来是 1.0.0）
- `easytier-cli api api_status` → 正常返回 JSON（Lua 转发层走同一条路径）
- 单机场景 `peer` / `route` 与上游 `diff` **无差异**
- `node` 与上游一致（listener 12 条、过滤 `ring://`、`Interface IPv4`）
- `usock` 无残留：旧的 `/usr/sbin/easytierctl` 已随包删除

## 5. 未实现（按优先级）

1. **多 peer 行**：`peer`/`route` 现在只渲染本机一行；8 节点组网下有 7 行缺失；
2. **其余 13 个子命令**：`connector` `mapped-listener` `stun` `peer-center` `vpn-portal`
   `proxy` `acl` `port-forward` `whitelist` `stats` `logger` `credential` `gen-autocomplete`
   （`help` 里已经列出，但敲下去会回落到 usage）；
3. **`-o json` / `--no-trunc` / `-i` / `-n`**：参数未实现；
4. **conninfo 进程内 RPC**：`src/api/etd_api_conninfo.c:106` 还在 fork 上游 `easytier-cli`，
   是 C 版最后的上游依赖 + 最大性能热点（11 分区 × ~200ms）；
5. `node` 的 `Proxy CIDRs` 恒为空（NodeInfo 里对应字段未接）。

## 6. 协议规格（实现多 peer 行所需，全部来自上游源码，不再需要抓包）

### 6.1 帧与 RPC

```
frame = u32LE(16 + len) | PeerManagerHeader(16B) | RpcPacket(len)
pm    = { u32 from=1, u32 to=1, u8 ptype(8=req,9=resp), u8 flags=0, u8 fwd=1, u8 rsv=0, u32 len }
RpcPacket = { 1:from, 2:to, 3:txid, 4:RpcDescriptor{2:proto_name, 3:service_name, 4:method_index},
              5:body, 6:is_request, 7:total_pieces=1, 10:{1:algo=None, 2:accepted_algo=None} }
```

### 6.2 PeerManageRpc method_index（`api_instance.proto` service 顺序，1-based）

| 方法 | index | 响应 |
| --- | --- | --- |
| `ListPeer` | 1 | `{ 1: repeated PeerInfo peer_infos, 2: NodeInfo my_info }` |
| `ListRoute` | 3 | `{ 1: repeated Route routes }` |
| `ShowNodeInfo` | 7 | `{ 1: NodeInfo }` |

请求体：`{ 2: { 1: { 1: <24B instance uuid> } }, 3: 5000 }`；
uuid 由 `WebClientService/5`（body `{3:5000}`）返回的 `{1:{1: uuid}}` 取得。

### 6.3 字段编号

```
PeerInfo      : 1 peer_id, 2 repeated PeerConnInfo conns, 3 default_conn_id, 4 directly_connected_conns
PeerConnInfo  : 1 conn_id, 2 my_peer_id, 3 peer_id, 4 features, 5 TunnelInfo, 6 PeerConnStats,
                7 float loss_rate, 8 is_client, 9 network_name, 10 is_closed
PeerConnStats : 1 rx_bytes, 2 tx_bytes, 3 rx_packets, 4 tx_packets, 5 latency_us
TunnelInfo    : 1 tunnel_type(string: tcp/udp/wg/ws/wss/quic/faketcp)
Route         : 1 peer_id, 2 Ipv4Inet, 3 next_hop_peer_id, 4 cost, 5 proxy_cidrs, 6 hostname,
                7 StunInfo, 8 inst_id, 9 version, 10 feature_flag, 11 path_latency,
                12 next_hop_peer_id_latency_first, 13 cost_latency_first, 14 path_latency_latency_first,
                15 ipv6_addr, 16 public_ipv6_addr, 17 ipv6_public_addr_prefix
Ipv4Inet      : 1 Ipv4Addr{1: uint32 addr}, 2 network_length
StunInfo      : 1 udp_nat_type(enum), 2 tcp_nat_type, 3 last_update_time, 4 repeated public_ip
NodeInfo      : 1 peer_id, 2 ipv4_addr(string CIDR), 4 hostname, 5 stun_info, 6 inst uuid,
                7 repeated listeners, 8 <实例配置文本>, 9 version, 10/11 <未解析>
```

### 6.4 表格映射（`easytier-cli.rs:1885-1916`）

| 列 | 取值 |
| --- | --- |
| ipv4 | `Route.ipv4_addr` → 点分 + `/network_length` |
| hostname | `Route.hostname` |
| cost | 1 → `p2p`；n>1 → `relay(n)`；本机 `Local` |
| lat(ms) | cost==1 → `PeerConnStats.latency_us / 1000`（`%.2f`）；否则 `Route.path_latency` |
| loss | `PeerConnInfo.loss_rate * 100` → `%.1f%%` |
| rx / tx | `PeerConnStats.rx_bytes / tx_bytes` → 十进制 humanize（`5.51 kB`） |
| tunnel | `PeerConnInfo.tunnel.tunnel_type`，多个用 `,` 连接 |
| NAT | `Route.stun_info.udp_nat_type` 枚举名（1 OpenInternet … 5 PortRestricted …） |
| version | `Route.version`，空则 `unknown` |

合并：按 `peer_id` 把 `Route` 与 `PeerInfo` 关联，取第一个 `is_closed=false` 的 conn 读 stats/loss/tunnel。

## 7. 测试环境

- 测试路由：`root@10.0.15.254`（ImmortalWrt 25.12.1，aarch64_cortex-a53），SSH 用
  `C:\Users\admin\.ssh\id_ed25519_sign`（`/tmp` 是 **noexec**，二进制要放 `/root/et/`）。
- 上游参照二进制：`/root/et/easytier-core`、`/root/et/easytier-cli`（2.7.0-8b7f1f01）。
- 组网测试配置（用户提供，只能用这一份）：
  `ldeus-net` / `10.0.200.247/24` / `bj-router-2` / peer `wss://152.136.26.248:2096`，
  secret `bjl385248CV?`；`/root/et/test.toml` 是原始 TOML，但 **core 2.7.0 会因未知 flags 拒绝**，
  实测可用命令行代替：

  ```sh
  /root/et/easytier-core --network-name ldeus-net --network-secret 'bjl385248CV?' \
    -i 10.0.200.247/24 --hostname bj-router-2 -p wss://152.136.26.248:2096 \
    --rpc-portal 127.0.0.1:15888 &
  ```

- 参考输出（8 节点）：`/root/et/ref-peer.txt`、`/root/et/ref-route.txt`。

## 8. 构建 / 发布流程

1. push 到 `gh main` → workflow `sr1010-immortalwrt-apk.yml` 自动构建（约 20–45 分钟）；
2. 只挑 C 三包（`easytierd-*` / `luci-app-easytier-*` / `luci-i18n-easytier-*`）就地替换 `v2.7.0-c` 资产；
3. 失败时构建日志会推到 `ci-logs` 分支的 `ci-fail-<run_id>.log`（需要 `git fetch gh ci-logs`，无 GitHub API 权限也能看）；
4. 装机：`apk add --allow-untrusted --force-overwrite /tmp/<pkg>.apk`。

## 9. 踩过的坑（别再踩）

| 坑 | 现象 / 解法 |
| --- | --- |
| PowerShell `-replace` + `Set-Content` 改带中文的源文件 | **UTF-8 被破坏成乱码**；一律用 `edit`/`write` 工具改文件 |
| `git commit -m "多行中文"` 经 pwsh | 参数被拆散提交失败；用 `-F 文件` |
| GitHub Actions 日志 | 无 admin 权限时 API 403；靠 `ci-logs` 分支回传 |
| GitHub API 频率限制 | 未认证 60 次/小时，超了换 git 命令或等 reset |
| `git push` 走 ssh:22 | 直连常超时；配 SOCKS 代理 `~/.ssh/config` 里 `ProxyCommand connect.exe -S 127.0.0.1:10808` |
| 路由器 `tcpdump -w` | 写不出文件；用 `tcpdump -i any -x -n port 15888` 打屏 |
| tcpdump 过滤器加引号 | 经 pwsh→ssh→ash 会被吃掉，报 `syntax error`；写成不带引号的 `port 15888` |
| `node`/`nohup`/`timeout` | 路由器上是 busybox，没有；用 `&` + `sleep` |
| `char *rows[N][2]` 强转 `char ***` | 指针算术步长错位 → SIGSEGV；先摆成 `char **rows[N]` |
| `et_pb_find` 复用 `p/n` 游标 | 后续字段会在上一个字段内容里找 → 解析结果为空 |
| `/tmp` noexec | 上游二进制要放 `/root/et/` |

## 10. 下一步清单（照做即可）

- [ ] 装 `0656c47` 的包，验证 `help` 与本机行 cidr（上一版验证过 cidr 有、hostname 被游标 bug 清空）
- [ ] 实现 `ListPeer`(1) + `ListRoute`(3) 解析与多行合并渲染（规格见 §6）
- [ ] 用 `/root/et/ref-peer.txt` 逐行对拍（8 行）
- [ ] 逐个子命令实现：`connector`、`mapped-listener`、`stun`、`peer-center`、`vpn-portal`、
      `proxy`、`acl`、`port-forward`、`whitelist`、`stats`、`logger`、`credential`、`gen-autocomplete`
- [ ] `-o json` / `--no-trunc` / `-i` / `-n` 参数
- [ ] conninfo 11 分区改用 `etd_rpc.c`（消掉最后的上游 fork，状态页 ~2s → 数十 ms）
- [ ] README / README_EN / DEVELOPMENT.md 里 `easytierctl` 的说法同步为 `easytier-cli`
