# CLI 移植档案：把 `easytierctl` 合并成上游兼容的 `easytier-cli`

> 目标（2026-11-05 定）：C 重写版对外只提供一个 CLI，名字与上游一致 —— `easytier-cli`。
> 它要覆盖上游 `easytier-cli` 的子命令与人类可读输出（表格），同时保留本项目的管理命令
> （`api` / `service` / `doctor` / `version`）。Lua 转发层与 `init.d` 跟随改名。

## 1. 现状

- `/usr/sbin/easytierctl`：C 实现的管理 CLI，只有 `api` / `service` / `version` / `doctor` 四个命令
  （[src/ctl/easytierctl.c](../src/ctl/easytierctl.c)）。
- `/usr/bin/easytier-cli`：**上游 EasyTier 的 CLI**，属于上游 `easytier` / `easytier-noweb` 包
  （[easytier/Makefile](../easytier/Makefile)），本仓库不提供；只装 C 版三个包时它不存在。
- C 版状态页的连接信息分区（node/peer/route/…）就是 fork 上游 `easytier-cli` 拿到文本再交给前端渲染
  （[src/api/etd_api_conninfo.c](../src/api/etd_api_conninfo.c)），前端按 `| a | b |` 的 Markdown 表格解析
  （[easytier_cli.htm](../luci-app-easytier/luasrc/view/easytier/easytier_cli.htm)）。
- 二进制自报版本已对齐：`ETD_VERSION` 可由 `-DETD_VERSION` 覆盖，打包时取 `PKG_VERSION`（=2.7.0）。

## 2. 上游 CLI → core 的通信协议（已查证）

上游 `easytier-cli` **不是** HTTP/JSON，是自研 RPC，跑在 core 的 rpc-portal（默认 `127.0.0.1:15888`）上：

- 传输：裸 TCP。帧 = `[u32 LE 帧长][16B PeerManagerHeader][RpcPacket protobuf]`；
  帧长的值 = 16 + payload 长度（上游 `ZCPacketType::TCP`，源码 `easytier-core/src/tunnel/framed.rs`）。
- PeerManagerHeader（16B，packed，小端）：`u32 from_peer_id, u32 to_peer_id, u8 packet_type,
  u8 flags, u8 forward_counter, u8 reserved, u32 len`；`packet_type` 用 `PacketType::RpcReq(4?)/RpcResp`，
  `flags` 位 0=ENCRYPTED、位 4=COMPRESSED（`easytier-core/src/packet/mod.rs`）。
- RpcPacket（`easytier-proto/proto/common.proto`）：
  `uint32 from_peer=1, uint32 to_peer=2, int64 transaction_id=3, RpcDescriptor descriptor=4,
  bytes body=5, bool is_request=6, uint32 total_pieces=7, uint32 piece_idx=8, int32 trace_id=9,
  RpcCompressionInfo compression_info=10`。
- RpcDescriptor：`string domain_name=1, string proto_name=2, string service_name=3, uint32 method_index=4`。
- 客户端身份：上游 CLI 用 `scoped_client::<F>(1, 1, "")`，即 `from_peer=1, to_peer=1, domain_name=""`
  （`easytier-core/src/rpc/standalone.rs`）。
- 压缩：`RpcCompressionInfo{algo, accepted_algo}`，`None=1, Zstd=2`；协商不上就该字段省略/填 None。

## 3. 还差的协议细节（下一步用"假 core"现场抓）

1. 每个子命令的 `proto_name` / `service_name` / `method_index`；
2. 请求/响应消息的字段编号（PeerInfo、NodeInfo、Route、Stats、ACL 等）；
3. 表格列名、列序、空结果与错误的文案（上游用 Rust `tabled` 的 Markdown 样式，前端的解析依赖它）。

抓法（不需要真 core）：在测试路由器上跑一个"假 core"监听 15888，然后把上游
`easytier-cli -p 127.0.0.1:15888 peer` 指过去，直接 dump 请求字节；再按猜测回包，观察 CLI 的渲染结果。
这样请求和响应两个方向都能定死。

## 4. 验证环境

- 测试路由器：`root@10.0.15.254`（ImmortalWrt 25.12.1，aarch64_cortex-a53，已装本仓库三个 apk）。
- 快速迭代：本地用 zig 交叉编译 `aarch64-linux-musl` 的小探针（不依赖 libuci/json-c），scp 上去跑；
  正式包仍走 CI（`scripts/ci/build-immortalwrt-apk.sh`）。
- 参考实现：上游 `easytier/src/easytier-cli.rs`（clap 子命令与输出）、`easytier-core/src/rpc/*`（协议）。

## 5. 分阶段计划

1. **（本次）版本对齐**：`ETD_VERSION` 跟随 `PKG_VERSION`，`easytierctl version` 输出 2.7.0。
2. **协议探针**：假 core + 抓包，确定 descriptor 与消息字段，写成 `docs/CLI-PORT.md` 附录。
3. **C 实现**：`src/lib/etd_pb.c`（最小 protobuf 编解码）、`src/lib/etd_rpc.c`（TCP+帧+RPC）、
   `src/ctl/easytier_cli.c`（子命令与表格渲染）。
4. **合并与改名**：`easytierctl` → `easytier-cli`，`api/service/doctor` 作为子命令保留；
   Lua controller、`/etc/init.d/easytier`、i18n、文档同步。
5. **发版**：在现有 `v2.7.0-c` Release 上删除旧资产、上传新资产（不新增 tag）。

## 6. 现场实测记录（测试路由 root@10.0.15.254，aarch64）

上游 `easytier-cli` = `2.7.0-8b7f1f01`，放在 `/root/et/`（`/tmp` 是 noexec，不能直接跑）。
启动 core 做对照：

```sh
/root/et/easytier-core --no-tun --network-name ettest --network-secret ettest123 \
  --rpc-portal 127.0.0.1:15888 &
/root/et/easytier-cli -p 127.0.0.1:15888 peer
```

子命令全集（`easytier-cli -h`）：peer / connector / mapped-listener / stun / route / peer-center /
vpn-portal / node / service / proxy / acl / port-forward / whitelist / stats / logger / credential /
generate-completions。

输出是 Markdown 表格（`tabled` 的 markdown 样式，前端 `parseTable()` 依赖它），实测样本：

```text
# peer
| ipv4 | hostname    | cost  | lat(ms) | loss | rx | tx | tunnel | NAT            | version        |
|------|-------------|-------|---------|------|----|----|--------|----------------|----------------|
|      | ImmortalWrt | Local | -       | -    | -  | -  | -      | PortRestricted | 2.7.0-8b7f1f01 |

# route
| ipv4 | hostname | proxy_cidrs | next_hop_ipv4 | next_hop_hostname | next_hop_lat | path_len | path_latency | next_hop_ipv4_lat_first | next_hop_hostname_lat_first | path_len_lat_first | path_latency_lat_first | version |
| ...  |          |             | -             | Local             | 0            | 0        | 0            | -                       | Local                       | 0                  | 0                      | 2.7.0-… |

# node（单列键值对，第一行是表头）
| Virtual IP     |                         |
|----------------|-------------------------|
| Hostname       | ImmortalWrt             |
| Peer ID        | 611652664               |
| Public IPv4    | 120.245.26.127          |
| UDP Stun Type  | PortRestricted          |
| Interface IPv4 | 10.0.15.254             |
```

## 7. 线上字节（已抓包解码，2026-11-06）

抓法（`-w` 在这台机器写不出文件，直接打屏再解析）：

```sh
tcpdump -i any -x -n port 15888 > /root/cap.txt &
sleep 2; /root/et/easytier-cli -p 127.0.0.1:15888 peer
kill -INT %1
```

`peer` 一次调用的实际报文（已按 `[u32LE 帧长][16B PeerManagerHeader][RpcPacket]` 解开）：

```
帧长 85 | pm{from=1,to=1,ptype=8,flags=0,fwd=1,len=69}
        | RpcPacket{ 1:from=1, 2:to=1, 3:txid, 4:{2:"WebClientService",3:"WebClientService",4:5},
                    5:body(18 88 27), 6:is_request=1, 7:total_pieces=1,
                    10:{1:algo=None, 2:accepted_algo=Zstd} }
帧长 113| desc service=WebClientService method=8   body: 12 1a 0a 18 <uuid 24B> 18 88 27
帧长 109| desc service=PeerManageRpc    method=7   body: 12 1c 0a 1a 0a 18 <uuid> 18 88 27
帧长 109| desc service=PeerManageRpc    method=1   body: 同上
帧长 109| desc service=PeerManageRpc    method=3   body: 同上
响应     | pm.packet_type=9（RpcResp），descriptor 与请求一致，txid 不变
```

要点：
- `ptype` 8=请求、9=响应；`flags=0`；`forward_counter=1`；`reserved=0`；`pm.len` = RpcPacket 字节数。
- descriptor 的 `proto_name` 与 `service_name` 目前看到是同一个字符串（`WebClientService` / `PeerManageRpc`），`method_index` 是该 service 内 `rpc` 的声明序号。
- 请求体是 `{ 2: { 1: { instance_id, timeout_ms=5000 } } }` 这种包装；`18 88 27` = varint 5000。
- **响应里 PeerManageRpc 的大响应被 zstd 压缩**（`28 b5 2f fd` 魔数），因为客户端广播了 `accepted_algo=Zstd`。
  C 实现里我们广播 `accepted_algo=None` 就能拿到未压缩响应，从而**不必在 C 里引入 zstd**（待实测确认）。

### 7.1 `peer` 的响应消息（zstd 解压后逐字段读出）

```
WebClientService/5  -> { 1:{ 1:UUID{1..4}, 3:46 } }            # 列出网络实例，拿 instance uuid
WebClientService/8  -> { 1:{ 1:UUID, 2:"ettest", 3:3, 4:"default", 5:1 }, 3:48 }   # 实例配置
PeerManageRpc/7     -> { 1: PeerInfo, 3:4810 }                  # 列 peer（本机只有自己一条）
PeerManageRpc/1     -> { 3:56 }                                 # 小计数
PeerManageRpc/3     -> { 3:39 }                                 # 小计数
```

PeerInfo（本机自环那条的实测字段）：

```
1: peer_id      = 611652664
4: hostname     = "ImmortalWrt"
5: (34B)        6: (36B)                 # 未定，疑似 ipv4/ipv6 或地址结构
7: (repeated)   = tcp://0.0.0.0:11010 … wss://[::]:11012   # listeners
8: (397B)                                # 未定，疑似 route/统计子消息
9: version      = "2.7.0-8b7f1f01"
10/11: (小字节)                           # 未定，疑似 cost/nat 类型
```

表格列 `| ipv4 | hostname | cost | lat(ms) | loss | rx | tx | tunnel | NAT | version |` 里，
`hostname`/`version` 已确认；`ipv4`/`cost`/`lat`/`loss`/`rx`/`tx`/`tunnel`/`NAT` 还需要对照
上游 `easytier-cli.rs` 里 PeerInfo→表格的映射逐条定（下一步做，也可以用多节点网络造出真实 peer 行）。

### 7.2 PeerInfo 的实际字段（深度解码）

`PeerManageRpc/7` 的响应是 `{1: NodeInfo, 3: <计数>}`，NodeInfo 实测：

```
1  = peer_id            = 611652664
4  = hostname           = "ImmortalWrt"
5  = stun_info { 1: udp_nat_type=5, 2: 3, 3: unix_ts, 4: "120.245.26.127", 5: 34698, 6: 34698 }
6  = instance uuid 字符串 "197272bc-f32f-4c0e-a984-6e8515a91338"
7  = listeners (repeated string) + 一条嵌套的 field14
8  = { 13: f64, 13: f64, 6: msg, 7: msg, 4: 34 }      # 疑似连接/统计，列 lat/loss/rx/tx 可能来自这里
9  = version            = "2.7.0-8b7f1f01"
10 = { 3:1, 5:1, 6:1 }     11 = { 1: msg, 2: msg }
```

上游 `easytier-cli.rs:1859-1941` 的映射：
- 第一行永远是本机的 `NodeInfo`：`cost="Local"`、`lat/loss/rx/tx/tunnel="-"`、`NAT` 取自 `stun_info.udp_nat_type`、
  `version` 取 field 9、`hostname` 取 field 4、`cidr` 取 `ipv4_addr`（本机为空）；
- 其余行来自 `PeerRoutePair{route, stats}`：`cidr/ipv4` 取 `route.ipv4_addr`，`cost` 由 `route.cost` 映射，
  `lat(ms)` = cost==1 时用 stats 的延迟、否则用 `route.path_latency_latency_first`，
  `loss/rx/tx/tunnel` 全来自 stats（rx/tx 用十进制 humanize），`NAT` 取 `stats.udp_nat_type`。

**单机场景只需要 NodeInfo 这一行就能 100% 复刻**；多 peer 行要造一个两节点网络再抓一次。

待办：`accepted_algo=None` 是否让 core 不压缩（决定 C 里要不要带 zstd）。
