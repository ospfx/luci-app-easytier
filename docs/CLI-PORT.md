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
