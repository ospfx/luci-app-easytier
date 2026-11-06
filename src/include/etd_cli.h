/*
 * etd_cli.h - 与上游 easytier-cli 对齐的 CLI 公共设施
 *
 * 所有子命令实现都放在 src/lib/ 里（而不是 src/ctl/），原因有两个：
 *   1. 状态页（etd_api_conninfo.c）要在 easytierd 进程内直接渲染这些表，
 *      不能再 fork 一整个 easytier-cli 进程；
 *   2. 单测/探针程序可以只链接 lib/ 就能跑，不需要 libuci/json-c。
 *
 * 渲染一律写进 et_buf，由调用方决定打印到 stdout 还是塞进 JSON。
 */
#ifndef ETD_CLI_H
#define ETD_CLI_H

#include "etd_pb.h"
#include "etd_util.h"

#include <stdint.h>

/* ---------------- 全局选项 ---------------- */
struct et_cli_opts {
	const char *rpc;        /* -p/--rpc-portal，默认 127.0.0.1:15888 */
	const char *format;     /* -o/--output：table（默认）| json */
	int no_trunc;           /* --no-trunc：不按终端宽度裁列 */
	const char *instance_id;   /* -i/--instance-id：UUID */
	const char *instance_name; /* -n/--instance-name */
	int verbose;            /* -v/--verbose */
};

void et_cli_opts_init(struct et_cli_opts *o);
/* -o json 时返回 1 */
int et_cli_want_json(const struct et_cli_opts *o);

/* ---------------- Markdown 表格 ----------------
 * 版式对齐 Rust tabled 的 Style::markdown()：
 *   | h1 | h2 |
 *   |----|----|
 *   | v1 | v2 |
 * 单元格可含 '\n'（tabled 会把它渲染成同一行的多行），列宽按最长一行计算。
 */
typedef struct {
	int ncol;
	int nrow;
	int cap;
	const char **hdr; /* 指向外部字符串，不接管所有权 */
	char ***rows;     /* nrow × ncol，单元格是 strdup 出来的 */
} et_table;

/* hdr 生命周期必须覆盖 et_table 的使用期（通常传静态字面量） */
void et_table_init(et_table *t, int ncol, const char *const *hdr);
/* 拷贝一行（cells 里的字符串会被 strdup，NULL 视为 ""） */
int et_table_add_row(et_table *t, const char *const *cells);
/* 添加一行，但字符串所有权直接接管（用于 malloc 出来的值） */
int et_table_add_row_owned(et_table *t, char **cells);
void et_table_free(et_table *t);
/* 渲染；opts->no_trunc=0 且 stdout 是终端时按终端宽度裁列（对齐上游） */
int et_table_render(et_table *t, const struct et_cli_opts *o, et_buf *out);
/* 渲染并写到 stdout（末尾带换行） */
int et_table_print(et_table *t, const struct et_cli_opts *o);

/* 单列表键值对表格（node 用）：行 = {label, value} */
int et_table_kv_print(const char *const *labels, const char *const *values,
		      int n, const struct et_cli_opts *o, et_buf *out);

/* ---------------- JSON 输出 ---------------- */
/* 与 serde_json::to_string_pretty 同版式：2 空格缩进、"k": v */
typedef struct {
	et_buf *out;
	int depth;
	int first[24]; /* 每层是否已输出过元素 */
} et_json;

void et_json_init(et_json *j, et_buf *out);
void et_json_obj_begin(et_json *j, const char *key);
void et_json_obj_end(et_json *j);
void et_json_arr_begin(et_json *j, const char *key);
void et_json_arr_end(et_json *j);
void et_json_str(et_json *j, const char *key, const char *v);
void et_json_int(et_json *j, const char *key, long long v);
void et_json_uint(et_json *j, const char *key, unsigned long long v);
void et_json_double(et_json *j, const char *key, double v);
void et_json_bool(et_json *j, const char *key, int v);
/* 原样写入（调用方保证合法 JSON，如 {\"a\":1} 或 [1,2]） */
void et_json_raw(et_json *j, const char *key, const char *raw);
void et_json_null(et_json *j, const char *key);

/* 静默 RPC 错误（proxy 多域探测等场景按上游语义忽略错误） */
extern int et_cli_quiet;
/* 最近一次 RPC 错误详情（空串表示没有） */
const char *et_cli_last_error(void);

/* ---------------- RPC 会话 ---------------- */
#include "etd_rpc.h"

struct et_cli_target { /* 一个待查询的网络实例 */
	uint8_t uuid[32]; /* instance UUID 消息字节（长度可变，proto 里是 4 个 uint32） */
	int uuid_len;
	int have_uuid;
	char id_str[40];  /* 规范 UUID 字符串 */
	char name[128];   /* 实例名，可能为空 */
};

struct et_cli_ctx {
	struct et_cli_opts opts;
	et_rpc rpc;
	int opened;
	const struct et_cli_target *target; /* 当前正在渲染的实例；NULL=不指定 */
	et_buf out; /* 所有渲染结果都写这里，由调用方决定去哪 */
};

/* 把格式化文本追加到 ctx 输出缓冲 */
int et_cli_puts(struct et_cli_ctx *ctx, const char *s);
int et_cli_printf(struct et_cli_ctx *ctx, const char *fmt, ...);
/* 输出当前累积内容到 stdout（一次 write，减少管道碎片） */
int et_cli_flush(struct et_cli_ctx *ctx);

/* 建连（幂等）；失败时打印与上游一致的错误 */
int et_cli_ctx_open(struct et_cli_ctx *ctx);
void et_cli_ctx_close(struct et_cli_ctx *ctx);

/* 解析目标实例列表（-i/-n、或 ListNetworkInstance 扇出）。
 * 返回个数；*out 由调用方 free。失败返回 -1。 */
int et_cli_resolve_targets(struct et_cli_ctx *ctx,
			   struct et_cli_target **out);
void et_cli_free_targets(struct et_cli_target *t, int n);
/* 单个实例时不需要打印 "== name == " 表头 */
void et_cli_print_target_header(const struct et_cli_target *t);

/* 在 req 里写入 field 1 = InstanceIdentifier（按当前 target 选择器） */
int et_cli_req_instance(struct et_cli_ctx *ctx, et_pb *req);
/* 取当前实例选择器的 InstanceIdentifier 消息；返回 0=有选择器 1=无 -1=错误 */
int et_cli_instance_message(struct et_cli_ctx *ctx, et_pb *out);
/* ConfigRpc.PatchConfig（method 1）：patch 是 api.config.InstanceConfigPatch */
int et_cli_patch_config(struct et_cli_ctx *ctx, const et_pb *patch);
/* 调一次 RPC：req 是请求消息本体（不含 {2:…,3:…} 包装），
 * resp 拿到响应消息本体（已剥掉 {1:…}）。 */
int et_cli_call(struct et_cli_ctx *ctx, const char *service, unsigned method,
		const et_pb *req, et_pb *resp);
/* 带 domain_name 的调用（TcpProxyRpc 等按域注册的服务要用） */
int et_cli_call_domain(struct et_cli_ctx *ctx, const char *domain,
		       const char *service, unsigned method, const et_pb *req,
		       et_pb *resp);
/* 简易：服务 + 方法 + 可选实例项，一次调用 */
int et_cli_call_instance(struct et_cli_ctx *ctx, const char *service,
			 unsigned method, et_pb *resp);

/* ---------------- protobuf 便捷读取 ---------------- */
/* 取 wire=2 的字符串字段，拷贝进 out（NUL 结尾）。找到返回长度，否则 -1 */
int et_pb_get_str(const uint8_t *d, size_t n, unsigned field, char *out,
		  size_t cap);
/* 取 wire=0 的整数字段 */
int et_pb_get_u64(const uint8_t *d, size_t n, unsigned field, uint64_t *out);
/* 遍历 repeated 子消息字段，cb 返回非 0 时提前结束 */
typedef int (*et_pb_msg_cb)(const uint8_t *b, size_t n, void *ud);
int et_pb_foreach_msg(const uint8_t *d, size_t n, unsigned field,
		      et_pb_msg_cb cb, void *ud);
/* 取 repeated 字符串字段的第 idx 个元素 */
int et_pb_get_str_n(const uint8_t *d, size_t n, unsigned field, int idx,
		    char *out, size_t cap);
/* 遍历 repeated 字符串字段（wire=2） */
typedef int (*et_pb_str_cb)(const char *s, size_t n, void *ud);
int et_pb_foreach_str(const uint8_t *d, size_t n, unsigned field,
		      et_pb_str_cb cb, void *ud);

/* base64（标准字母表，带 '=' 填充） */
void et_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* ---------------- 格式化（与上游逐字一致） ---------------- */
/* humansize::DECIMAL：5510 -> "5.51 kB"，0 -> "0 B" */
void et_fmt_size(unsigned long long bytes, char *out, size_t cap);
/* NatType 枚举名（Rust Debug 名）：0 Unknown / 1 OpenInternet / … */
const char *et_nat_name(unsigned long long v);
/* f64 的 Rust Display 风格（能短则短）：11.554 / 178 / 0.005 */
void et_fmt_f64(double v, char *out, size_t cap);
/* common.Ipv4Addr{1:u32 big-endian} -> "1.2.3.4" */
void et_fmt_ipv4(uint32_t addr, char *out, size_t cap);
/* common.Ipv4Inet -> "1.2.3.4/24"；msg 为空时输出 "" */
void et_fmt_ipv4inet(const uint8_t *msg, size_t n, char *out, size_t cap);
/* common.SocketAddr -> "1.2.3.4:11010" */
void et_fmt_socket_addr(const uint8_t *msg, size_t n, char *out, size_t cap);
/* common.Url{1:string} */
void et_fmt_url(const uint8_t *msg, size_t n, char *out, size_t cap);
/* common.UUID(4×u32) -> 规范 UUID 字符串 */
void et_fmt_uuid(const uint8_t *msg, size_t n, char *out, size_t cap);
/* 解析规范 UUID 字符串 -> 24 字节 common.UUID 编码 */
/* 解析 UUID 字符串，写入序列化后的 UUID 消息；返回字节数，失败 -1 */
int et_parse_uuid(const char *s, uint8_t *out, size_t cap);

/* ---------------- 子命令 ----------------
 * 每个实现文件只依赖本头文件；新增子命令时在这里登记。
 * 返回值：0 成功；ET_ENOENT 表示不认识该子命令（由调用方给 usage）。
 */
int et_cli_peer(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_route(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_node(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_connector(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_mapped_listener(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_stun(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_peer_center(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_vpn_portal(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_proxy(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_acl(struct et_cli_ctx *ctx, int argc, char **argv);
/* acl set：解析 ACL TOML（或 @file）→ acl.Acl → ConfigRpc patch；返回 0 成功 */
int et_cli_acl_set_apply(struct et_cli_ctx *ctx, const char *raw);
int et_cli_port_forward(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_whitelist(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_stats(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_logger(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_credential(struct et_cli_ctx *ctx, int argc, char **argv);
int et_cli_gen_autocomplete(struct et_cli_ctx *ctx, int argc, char **argv);

/* ---------------- 多实例扇出 ----------------
 * 与上游 collect_instance_results/print_results 等价：
 *   - 没有 -i/-n 时向 core 查实例列表，逐个渲染；多于一个实例时每段前打印
 *     "== 名字 (uuid) =="；
 *   - -o json 且多实例时，每个结果包成 {instance_id, instance_name, result}。
 * 用法：
 *   struct et_cli_fanout f;
 *   if (et_cli_fanout_begin(ctx, &f)) return 1;
 *   for (f.index = 0; f.index < f.nt; f.index++) {
 *       ctx->target = &f.tg[f.index];
 *       ...取数据...
 *       et_cli_fanout_header(ctx, &f);          // 表格模式的多实例表头
 *       if (f.json) { et_cli_fanout_json_open(&f);
 *                     et_json_arr_begin(&f.j, f.nt > 1 ? "result" : NULL);
 *                     ...写数组元素/对象...
 *                     et_json_arr_end(&f.j);
 *                     et_cli_fanout_json_close(&f); }
 *       else { ...渲染表格... }
 *   }
 *   et_cli_fanout_end(ctx, &f);
 */
struct et_cli_fanout {
	struct et_cli_target *tg;
	int nt;
	int json;
	int index;
	et_json j;
};

int et_cli_fanout_begin(struct et_cli_ctx *ctx, struct et_cli_fanout *f);
void et_cli_fanout_header(struct et_cli_ctx *ctx, struct et_cli_fanout *f);
void et_cli_fanout_json_open(struct et_cli_fanout *f);
void et_cli_fanout_json_close(struct et_cli_fanout *f);
void et_cli_fanout_end(struct et_cli_ctx *ctx, struct et_cli_fanout *f);

/* 定义一个子命令的 opts 友好入口：字符串命令名 -> 分发。从未知命令返回
 * ET_ENOENT。 */
int et_cli_dispatch(struct et_cli_ctx *ctx, const char *cmd, int argc,
		    char **argv);

/* 状态页（etd_api_conninfo.c）用：把某个分区渲染成文本，
 * part 名与旧 CLI_PARTS 一致：node/peer/route/connector/stun/peer_center/
 * vpn_portal/proxy/acl/mapped_listener/stats。 */
int et_cli_render_part(const char *part, et_buf *out);
/* 同上，但指定 RPC 地址（状态页要用 UCI 里配置的 rpc_portal 端口） */
int et_cli_render_part_ex(const char *part, const char *rpc_addr, et_buf *out);

#endif /* ETD_CLI_H */
