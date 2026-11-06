/*
 * etd_cli_extra_b.c - stun / peer-center / stats / logger / credential / gen-autocomplete
 *
 * 规格来源（输出格式的唯一权威）：
 *   - 上游 CLI：E:\dsh\up_easytier-cli.rs（v2.7.0，对应测试路由上的
 *     easytier-cli 2.7.0-8b7f1f01）
 *   - proto：api_logger.proto / api_instance.proto / peer_rpc.proto / common.proto
 *
 * 有意的实现差异：
 *   stun - 上游在本地起 STUN 采集器、最多等 25 秒；我们改为直接读 core 已经采集好的
 *          NodeInfo.stun_info 并按同样的 Rust Debug 版式打印（不额外发 STUN 包，
 *          也不会因为 TCP NAT 测不出来而卡住）。
 */
#include "etd_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define IND "    " /* Rust {:#?} 的缩进单位 */

/* humansize::BINARY（1024 进制，两位小数，单位 KiB/MiB/…） */
static void fmt_size_bin(unsigned long long bytes, char *out, size_t cap)
{
	static const char *units[] = { "B",   "KiB", "MiB", "GiB",
				       "TiB", "PiB", "EiB" };
	double v = (double)bytes;
	int u = 0;

	while (v >= 1024.0 && u < 6) {
		v /= 1024.0;
		u++;
	}
	if (u == 0) {
		snprintf(out, cap, "%llu B", bytes);
		return;
	}
	snprintf(out, cap, "%.2f %s", v, units[u]);
}

/* ============ stun ============ */

struct stun_view {
	int have;
	uint64_t udp_nat;
	uint64_t tcp_nat;
	int64_t last_update;
	char public_ip[8][64];
	int n_public;
	uint64_t min_port;
	uint64_t max_port;
};

static int stun_ip_cb(const char *s, size_t n, void *ud)
{
	struct stun_view *v = ud;

	(void)n;
	if (v->n_public >= 8)
		return 1;
	snprintf(v->public_ip[v->n_public], 64, "%s", s);
	v->n_public++;
	return 0;
}

static int fetch_stun_view(struct et_cli_ctx *ctx, struct stun_view *v)
{
	et_pb resp;
	const uint8_t *node = NULL, *st = NULL;
	size_t nodelen = 0, stlen = 0;
	int rc = -1;

	memset(v, 0, sizeof(*v));
	et_pb_init(&resp);
	if (et_cli_call_instance(ctx, "PeerManageRpc", 7, &resp))
		goto out;
	if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &node, &nodelen))
		goto out;
	if (!et_pb_find(node, nodelen, 5, 2, NULL, &st, &stlen))
		goto out;
	v->have = 1;
	(void)et_pb_get_u64(st, stlen, 1, &v->udp_nat);
	(void)et_pb_get_u64(st, stlen, 2, &v->tcp_nat);
	{
		uint64_t t = 0;

		if (!et_pb_get_u64(st, stlen, 3, &t))
			v->last_update = (int64_t)t;
	}
	(void)et_pb_get_u64(st, stlen, 5, &v->min_port);
	(void)et_pb_get_u64(st, stlen, 6, &v->max_port);
	(void)et_pb_foreach_str(st, stlen, 4, stun_ip_cb, v);
	rc = 0;
out:
	et_pb_free(&resp);
	return rc;
}

static int cmd_stun(struct et_cli_ctx *ctx)
{
	struct stun_view v;
	et_json j;

	if (fetch_stun_view(ctx, &v) || !v.have) {
		fprintf(stderr, "easytier-cli: failed to get stun info\n");
		return 1;
	}
	if (et_cli_want_json(&ctx->opts)) {
		et_json_init(&j, &ctx->out);
		et_json_obj_begin(&j, NULL);
		et_json_uint(&j, "udp_nat_type", v.udp_nat);
		et_json_uint(&j, "tcp_nat_type", v.tcp_nat);
		et_json_int(&j, "last_update_time", v.last_update);
		et_json_arr_begin(&j, "public_ip");
		for (int i = 0; i < v.n_public; i++)
			et_json_str(&j, NULL, v.public_ip[i]);
		et_json_arr_end(&j);
		et_json_uint(&j, "min_port", v.min_port);
		et_json_uint(&j, "max_port", v.max_port);
		et_json_obj_end(&j);
		et_cli_puts(ctx, "\n");
		return 0;
	}
	/* 上游: println!("stun info: {:#?}", ret)，枚举字段打变体名 */
	et_cli_puts(ctx, "stun info: StunInfo {\n");
	et_cli_printf(ctx, "%sudp_nat_type: %s,\n", IND,
		      et_nat_name(v.udp_nat));
	et_cli_printf(ctx, "%stcp_nat_type: %s,\n", IND,
		      et_nat_name(v.tcp_nat));
	et_cli_printf(ctx, "%slast_update_time: %lld,\n", IND,
		      (long long)v.last_update);
	if (!v.n_public) {
		et_cli_printf(ctx, "%spublic_ip: [],\n", IND);
	} else {
		et_cli_printf(ctx, "%spublic_ip: [\n", IND);
		for (int i = 0; i < v.n_public; i++)
			et_cli_printf(ctx, "%s%s\"%s\",\n", IND, IND,
				      v.public_ip[i]);
		et_cli_printf(ctx, "%s],\n", IND);
	}
	et_cli_printf(ctx, "%smin_port: %llu,\n", IND,
		      (unsigned long long)v.min_port);
	et_cli_printf(ctx, "%smax_port: %llu,\n", IND,
		      (unsigned long long)v.max_port);
	et_cli_puts(ctx, "}\n");
	return 0;
}

/* ============ peer-center ============ */

struct peer_meta {
	uint32_t peer_id;
	char hostname[128];
	char ipv4[64];
};

struct peer_meta_list {
	struct peer_meta *v;
	int n;
	int cap;
};

static void peer_meta_add(struct peer_meta_list *l, uint32_t id,
			  const char *host, const char *ip)
{
	struct peer_meta *p;

	if (l->n >= l->cap) {
		int cap = l->cap ? l->cap * 2 : 16;

		p = realloc(l->v, (size_t)cap * sizeof(*p));
		if (!p)
			return;
		l->v = p;
		l->cap = cap;
	}
	p = &l->v[l->n];
	memset(p, 0, sizeof(*p));
	p->peer_id = id;
	snprintf(p->hostname, sizeof(p->hostname), "%s", host ? host : "");
	snprintf(p->ipv4, sizeof(p->ipv4), "%s", ip ? ip : "");
	l->n++;
}

static const struct peer_meta *peer_meta_find(const struct peer_meta_list *l,
					      uint32_t id)
{
	for (int i = 0; i < l->n; i++)
		if (l->v[i].peer_id == id)
			return &l->v[i];
	return NULL;
}

static int route_meta_cb(const uint8_t *b, size_t n, void *ud)
{
	struct peer_meta_list *l = ud;
	uint64_t id = 0;
	char host[128] = "";
	char ip[64] = "";
	const uint8_t *ia = NULL;
	size_t ial = 0;

	(void)et_pb_get_u64(b, n, 1, &id);
	(void)et_pb_get_str(b, n, 6, host, sizeof(host));
	if (et_pb_find(b, n, 2, 2, NULL, &ia, &ial))
		et_fmt_ipv4inet(ia, ial, ip, sizeof(ip));
	peer_meta_add(l, (uint32_t)id, host, ip);
	return 0;
}

/* ShowNodeInfo + ListRoute -> peer_id → {hostname, ipv4} */
static int build_peer_meta(struct et_cli_ctx *ctx, struct peer_meta_list *l)
{
	et_pb resp, routes;
	int rc = -1;

	et_pb_init(&resp);
	et_pb_init(&routes);
	memset(l, 0, sizeof(*l));
	if (et_cli_call_instance(ctx, "PeerManageRpc", 7, &resp))
		goto out;
	{
		const uint8_t *node = NULL;
		size_t nodelen = 0;
		uint64_t id = 0;
		char host[128] = "";
		char ip[64] = "";

		if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &node, &nodelen))
			goto out;
		(void)et_pb_get_u64(node, nodelen, 1, &id);
		(void)et_pb_get_str(node, nodelen, 4, host, sizeof(host));
		(void)et_pb_get_str(node, nodelen, 2, ip, sizeof(ip));
		peer_meta_add(l, (uint32_t)id, host, ip);
	}
	if (et_cli_call_instance(ctx, "PeerManageRpc", 3, &routes))
		goto out;
	(void)et_pb_foreach_msg(routes.p, routes.len, 1, route_meta_cb, l);
	rc = 0;
out:
	et_pb_free(&resp);
	et_pb_free(&routes);
	return rc;
}

struct pc_direct {
	uint32_t peer_id;
	int32_t latency_ms;
};

struct pc_row {
	uint32_t node_id;
	struct pc_direct *d;
	int nd;
	int cap;
};

struct pc_rows {
	struct pc_row *v;
	int n;
	int cap;
};

struct map_val_ctx {
	void (*on_entry)(uint64_t key, const uint8_t *val, size_t len, void *ud);
	void *ud;
};

/* 遍历 repeated map-entry {1: key, 2: value}：把 key 和 value 一起交出去 */
static int map_entry_walk(const uint8_t *b, size_t n, void *ud)
{
	struct map_val_ctx *c = ud;
	const uint8_t *val = NULL;
	size_t vlen = 0;
	uint64_t key = 0;

	(void)et_pb_get_u64(b, n, 1, &key);
	if (et_pb_find(b, n, 2, 2, NULL, &val, &vlen))
		c->on_entry(key, val, vlen, c->ud);
	return 0;
}

static void pc_direct_on_entry(uint64_t key, const uint8_t *val, size_t len,
			       void *ud)
{
	struct pc_row *row = ud;
	uint64_t lat = 0;

	if (row->nd >= row->cap) {
		int cap = row->cap ? row->cap * 2 : 8;
		struct pc_direct *p = realloc(row->d, (size_t)cap * sizeof(*p));

		if (!p)
			return;
		row->d = p;
		row->cap = cap;
	}
	/* DirectConnectedPeerInfo { int32 latency_ms = 1 } */
	(void)et_pb_get_u64(val, len, 1, &lat);
	row->d[row->nd].peer_id = (uint32_t)key;
	row->d[row->nd].latency_ms = (int32_t)lat;
	row->nd++;
}

/* value = PeerInfoForGlobalMap { 1: map<uint32, DirectConnectedPeerInfo> } */
static void pc_row_on_entry(uint64_t key, const uint8_t *val, size_t len,
			    void *ud)
{
	struct pc_rows *rows = ud;
	struct pc_row row;
	const uint8_t *directs = NULL;
	size_t dl = 0;

	memset(&row, 0, sizeof(row));
	row.node_id = (uint32_t)key;
	if (rows->n >= rows->cap) {
		int cap = rows->cap ? rows->cap * 2 : 16;
		struct pc_row *p = realloc(rows->v, (size_t)cap * sizeof(*p));

		if (!p)
			return;
		rows->v = p;
		rows->cap = cap;
	}
	{
		struct map_val_ctx mc;

		(void)directs;
		(void)dl;
		mc.on_entry = pc_direct_on_entry;
		mc.ud = &row;
		/* PeerInfoForGlobalMap { map<uint32, DirectConnectedPeerInfo> direct_peers = 1 } */
		(void)et_pb_foreach_msg(val, len, 1, map_entry_walk, &mc);
	}
	rows->v[rows->n++] = row;
}

/* 取 GetGlobalPeerMapResponse（map key 是 node_id，外层 entry 的 field 1） */
static int fetch_peer_center(struct et_cli_ctx *ctx, struct pc_rows *rows)
{
	et_pb req, resp;
	const uint8_t *map = NULL;
	size_t maplen = 0;
	int rc = -1;

	memset(rows, 0, sizeof(*rows));
	et_pb_init(&req);
	et_pb_init(&resp);
	if (et_cli_call(ctx, "PeerCenterRpc", 2, &req, &resp))
		goto out;
	(void)map;
	(void)maplen;
	{
		struct map_val_ctx mc;

		mc.on_entry = pc_row_on_entry;
		mc.ud = rows;
		/* 每个 field 1 就是一个 map entry{1:key,2:value} */
		(void)et_pb_foreach_msg(resp.p, resp.len, 1, map_entry_walk,
					&mc);
	}
	rc = 0;
out:
	et_pb_free(&req);
	et_pb_free(&resp);
	return rc;
}

static void pc_rows_free(struct pc_rows *rows)
{
	for (int i = 0; i < rows->n; i++)
		free(rows->v[i].d);
	free(rows->v);
	memset(rows, 0, sizeof(*rows));
}

/* peer-center 表格：direct_peers 每行 "id(hostname[ipv4]): Nms" */
static void pc_render_table(struct et_cli_ctx *ctx, struct pc_rows *rows,
			    struct peer_meta_list *meta)
{
	static const char *hdr[] = { "node_id", "hostname", "ipv4",
				     "direct_peers" };
	et_table t;

	et_table_init(&t, 4, hdr);
	for (int i = 0; i < rows->n; i++) {
		const struct peer_meta *pm = peer_meta_find(meta, rows->v[i].node_id);
		char idbuf[16];
		et_buf dp;
		const char *cells[4];

		snprintf(idbuf, sizeof(idbuf), "%u", rows->v[i].node_id);
		et_buf_init(&dp);
		for (int k = 0; k < rows->v[i].nd; k++) {
			const struct peer_meta *dpm =
				peer_meta_find(meta, rows->v[i].d[k].peer_id);

			et_buf_addf(&dp, "%s%u(%s[%s]): %dms", k ? "\n" : "",
				    rows->v[i].d[k].peer_id,
				    dpm ? dpm->hostname : "",
				    dpm ? dpm->ipv4 : "",
				    rows->v[i].d[k].latency_ms);
		}
		cells[0] = idbuf;
		cells[1] = pm ? pm->hostname : "";
		cells[2] = pm ? pm->ipv4 : "";
		cells[3] = dp.p ? dp.p : "";
		et_table_add_row(&t, cells);
		et_buf_free(&dp);
	}
	(void)et_table_render(&t, &ctx->opts, &ctx->out);
	et_table_free(&t);
}

static int cmd_peer_center(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		struct peer_meta_list meta;
		struct pc_rows rows;

		ctx->target = &f.tg[f.index];
		if (build_peer_meta(ctx, &meta) || fetch_peer_center(ctx, &rows)) {
			fprintf(stderr,
				"easytier-cli: failed to get global peer map\n");
			free(meta.v);
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		if (f.json) {
			et_json_arr_begin(&f.j, f.nt > 1 ? "result" : NULL);
			et_cli_fanout_json_open(&f);
			for (int i = 0; i < rows.n; i++) {
				char idbuf[16];

				et_json_obj_begin(&f.j, NULL);
				snprintf(idbuf, sizeof(idbuf), "%u",
					 rows.v[i].node_id);
				et_json_str(&f.j, "node_id", idbuf);
				{
					const struct peer_meta *pm =
						peer_meta_find(&meta,
							       rows.v[i].node_id);

					et_json_str(&f.j, "hostname",
						    pm ? pm->hostname : "");
					et_json_str(&f.j, "ipv4",
						    pm ? pm->ipv4 : "");
				}
				et_json_arr_begin(&f.j, "direct_peers");
				for (int k = 0; k < rows.v[i].nd; k++) {
					const struct peer_meta *dpm =
						peer_meta_find(
							&meta,
							rows.v[i].d[k].peer_id);
					char kid[16];

					et_json_obj_begin(&f.j, NULL);
					snprintf(kid, sizeof(kid), "%u",
						 rows.v[i].d[k].peer_id);
					et_json_str(&f.j, "node_id", kid);
					et_json_str(&f.j, "hostname",
						    dpm ? dpm->hostname : "");
					et_json_str(&f.j, "ipv4",
						    dpm ? dpm->ipv4 : "");
					et_json_int(&f.j, "latency_ms",
						    rows.v[i].d[k].latency_ms);
					et_json_obj_end(&f.j);
				}
				et_json_arr_end(&f.j);
				et_json_obj_end(&f.j);
			}
			et_json_arr_end(&f.j);
			et_cli_fanout_json_close(&f);
		} else {
			pc_render_table(ctx, &rows, &meta);
		}
		free(meta.v);
		pc_rows_free(&rows);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* ============ stats ============ */

struct metric_lv {
	char k[64];
	char v[128];
};

struct metric_row {
	char name[128];
	uint64_t value;
	struct metric_lv lv[16];
	int nlv;
	char labels[512];
};

struct metric_list {
	struct metric_row *v;
	int n;
	int cap;
};

struct labels_ctx {
	struct metric_row *row;
	et_buf *buf;
};

static int labels_kv_cb(const uint8_t *b, size_t n, void *ud)
{
	struct labels_ctx *c = ud;
	char k[64] = "";
	char v[128] = "";

	(void)et_pb_get_str(b, n, 1, k, sizeof(k));
	(void)et_pb_get_str(b, n, 2, v, sizeof(v));
	if (c->buf->len)
		et_buf_add(c->buf, ", ");
	et_buf_addf(c->buf, "%s=%s", k, v);
	if (c->row->nlv < 16) {
		snprintf(c->row->lv[c->row->nlv].k, 64, "%s", k);
		snprintf(c->row->lv[c->row->nlv].v, 128, "%s", v);
		c->row->nlv++;
	}
	return 0;
}

static int metric_cb(const uint8_t *b, size_t n, void *ud)
{
	struct metric_list *l = ud;
	struct metric_row row;
	struct labels_ctx lc;
	et_buf labels;

	memset(&row, 0, sizeof(row));
	(void)et_pb_get_str(b, n, 1, row.name, sizeof(row.name));
	(void)et_pb_get_u64(b, n, 2, &row.value);
	et_buf_init(&labels);
	lc.row = &row;
	lc.buf = &labels;
	(void)et_pb_foreach_msg(b, n, 3, labels_kv_cb, &lc);
	snprintf(row.labels, sizeof(row.labels), "%s", labels.p ? labels.p : "");
	et_buf_free(&labels);
	if (l->n >= l->cap) {
		int cap = l->cap ? l->cap * 2 : 32;
		struct metric_row *q = realloc(l->v, (size_t)cap * sizeof(*q));

		if (!q)
			return 1;
		l->v = q;
		l->cap = cap;
	}
	l->v[l->n++] = row;
	return 0;
}

static void metric_list_free(struct metric_list *l)
{
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static int cmd_stats(struct et_cli_ctx *ctx, int prometheus)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb resp;

		ctx->target = &f.tg[f.index];
		et_pb_init(&resp);
		if (prometheus) {
			static char text[1 << 16];
			int n;

			if (et_cli_call_instance(ctx, "StatsRpc", 2, &resp)) {
				fprintf(stderr,
					"easytier-cli: failed to get prometheus stats\n");
				et_pb_free(&resp);
				rc = 1;
				break;
			}
			text[0] = 0;
			n = et_pb_get_str(resp.p, resp.len, 1, text,
					  sizeof(text));
			(void)n;
			et_cli_fanout_header(ctx, &f);
			if (f.json) {
				et_cli_fanout_json_open(&f);
				et_json_arr_begin(&f.j, f.nt > 1 ? "result" : NULL);
				et_json_obj_begin(&f.j, NULL);
				et_json_str(&f.j, "prometheus_text", text);
				et_json_obj_end(&f.j);
				et_json_arr_end(&f.j);
				et_cli_fanout_json_close(&f);
			} else {
				et_cli_puts(ctx, text);
				if (!text[0] ||
				    ctx->out.p[ctx->out.len - 1] != '\n')
					et_cli_puts(ctx, "\n");
			}
			et_pb_free(&resp);
			continue;
		}
		{
			struct metric_list ml;

			memset(&ml, 0, sizeof(ml));
			if (et_cli_call_instance(ctx, "StatsRpc", 1, &resp)) {
				fprintf(stderr,
					"easytier-cli: failed to get stats\n");
				et_pb_free(&resp);
				rc = 1;
				break;
			}
			(void)et_pb_foreach_msg(resp.p, resp.len, 1, metric_cb,
						&ml);
			et_cli_fanout_header(ctx, &f);
			if (f.json) {
				et_cli_fanout_json_open(&f);
				et_json_arr_begin(&f.j, f.nt > 1 ? "result" : NULL);
				for (int i = 0; i < ml.n; i++) {
					et_json_obj_begin(&f.j, NULL);
					et_json_str(&f.j, "name", ml.v[i].name);
					et_json_uint(&f.j, "value", ml.v[i].value);
					et_json_obj_begin(&f.j, "labels");
					for (int k = 0; k < ml.v[i].nlv; k++)
						et_json_str(&f.j,
							    ml.v[i].lv[k].k,
							    ml.v[i].lv[k].v);
					et_json_obj_end(&f.j);
					et_json_obj_end(&f.j);
				}
				et_json_arr_end(&f.j);
				et_cli_fanout_json_close(&f);
			} else {
				static const char *hdr[] = { "Metric Name",
							     "Value",
							     "Labels" };
				et_table t;

				et_table_init(&t, 3, hdr);
				for (int i = 0; i < ml.n; i++) {
					char val[64];
					const char *cells[3];

					if (strstr(ml.v[i].name, "bytes")) {
						fmt_size_bin(
							(unsigned long long)
								ml.v[i].value,
							val, sizeof(val));
					} else if (strstr(ml.v[i].name,
							  "duration")) {
						snprintf(val, sizeof(val),
							 "%llu ms",
							 (unsigned long long)
								 ml.v[i].value);
					} else {
						snprintf(val, sizeof(val),
							 "%llu",
							 (unsigned long long)
								 ml.v[i].value);
					}
					cells[0] = ml.v[i].name;
					cells[1] = val;
					cells[2] = ml.v[i].labels[0]
							   ? ml.v[i].labels
							   : "-";
					et_table_add_row(&t, cells);
				}
				(void)et_table_render(&t, &ctx->opts,
						      &ctx->out);
				et_table_free(&t);
			}
			metric_list_free(&ml);
		}
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* ============ logger ============ */

static const char *log_level_name(unsigned long long v)
{
	static const char *names[] = { "disabled", "error", "warning",
				       "info",     "debug", "trace" };

	return v < 6 ? names[v] : "unknown";
}

static int cmd_logger_get(struct et_cli_ctx *ctx)
{
	et_pb resp;
	uint64_t level = 0;

	et_pb_init(&resp);
	if (et_cli_call(ctx, "LoggerRpc", 2, NULL, &resp)) {
		fprintf(stderr, "easytier-cli: failed to get logger config\n");
		et_pb_free(&resp);
		return 1;
	}
	(void)et_pb_get_u64(resp.p, resp.len, 1, &level);
	if (et_cli_want_json(&ctx->opts)) {
		et_json j;

		et_json_init(&j, &ctx->out);
		et_json_obj_begin(&j, NULL);
		et_json_uint(&j, "level", level);
		et_json_obj_end(&j);
		et_cli_puts(ctx, "\n");
	} else {
		et_cli_printf(ctx, "Current Log Level: %s\n",
			      log_level_name(level));
	}
	et_pb_free(&resp);
	return 0;
}

static int cmd_logger_set(struct et_cli_ctx *ctx, const char *level)
{
	static const char *names[] = { "disabled", "error", "warning",
				       "info",     "debug", "trace" };
	et_pb req, resp;
	int found = -1;

	for (int i = 0; i < 6; i++) {
		if (!strcasecmp(level, names[i])) {
			found = i;
			break;
		}
	}
	if (found < 0) {
		fprintf(stderr,
			"Invalid log level: %s. Valid levels are: disabled, error, warning, info, debug, trace\n",
			level);
		return 1;
	}
	et_pb_init(&req);
	et_pb_init(&resp);
	if (et_pb_uint(&req, 1, (uint64_t)found) ||
	    et_cli_call(ctx, "LoggerRpc", 1, &req, &resp)) {
		fprintf(stderr, "easytier-cli: failed to set logger config\n");
		et_pb_free(&req);
		et_pb_free(&resp);
		return 1;
	}
	if (et_cli_want_json(&ctx->opts)) {
		et_json j;

		et_json_init(&j, &ctx->out);
		et_json_obj_begin(&j, NULL);
		et_json_obj_end(&j);
		et_cli_puts(ctx, "\n");
	} else {
		et_cli_printf(ctx, "Log level successfully set to: %s\n", level);
	}
	et_pb_free(&req);
	et_pb_free(&resp);
	return 0;
}

int et_cli_logger(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "get";

	if (!strcmp(sub, "get"))
		return cmd_logger_get(ctx);
	if (!strcmp(sub, "set")) {
		if (argc < 2)
			return ET_ENOENT;
		return cmd_logger_set(ctx, argv[1]);
	}
	return ET_ENOENT;
}

/* ============ credential ============ */

/* tabled Style::rounded()：╭─┬─╮ / │ │ │ / ├─┼─┤ / ╰─┴─╯ */
static void render_table_rounded(et_table *t, et_buf *out)
{
	int ncol = t->ncol;
	size_t *w = calloc((size_t)ncol, sizeof(size_t));

	if (!w)
		return;
	for (int j = 0; j < ncol; j++) {
		w[j] = strlen(t->hdr[j] ? t->hdr[j] : "");
		for (int i = 0; i < t->nrow; i++) {
			size_t cw = strlen(t->rows[i][j] ? t->rows[i][j] : "");

			if (cw > w[j])
				w[j] = cw;
		}
	}
	et_buf_add(out, "\u256d");
	for (int j = 0; j < ncol; j++) {
		for (size_t k = 0; k < w[j] + 2; k++)
			et_buf_add(out, "\u2500");
		et_buf_add(out, j + 1 < ncol ? "\u252c" : "\u256e");
	}
	et_buf_add(out, "\n");
	for (int j = 0; j < ncol; j++) {
		const char *h = t->hdr[j] ? t->hdr[j] : "";

		et_buf_add(out, "\u2502 ");
		et_buf_add(out, h);
		for (size_t k = strlen(h); k < w[j] + 1; k++)
			et_buf_addc(out, ' ');
	}
	et_buf_add(out, "\u2502\n");
	et_buf_add(out, "\u251c");
	for (int j = 0; j < ncol; j++) {
		for (size_t k = 0; k < w[j] + 2; k++)
			et_buf_add(out, "\u2500");
		et_buf_add(out, j + 1 < ncol ? "\u253c" : "\u2524");
	}
	et_buf_add(out, "\n");
	for (int i = 0; i < t->nrow; i++) {
		for (int j = 0; j < ncol; j++) {
			const char *cell = t->rows[i][j] ? t->rows[i][j] : "";

			et_buf_add(out, "\u2502 ");
			et_buf_add(out, cell);
			for (size_t k = strlen(cell); k < w[j] + 1; k++)
				et_buf_addc(out, ' ');
		}
		et_buf_add(out, "\u2502\n");
	}
	et_buf_add(out, "\u2570");
	for (int j = 0; j < ncol; j++) {
		for (size_t k = 0; k < w[j] + 2; k++)
			et_buf_add(out, "\u2500");
		et_buf_add(out, j + 1 < ncol ? "\u2534" : "\u256f");
	}
	et_buf_add(out, "\n");
	free(w);
}

struct cred_row {
	char id[64];
	char groups[256];
	int allow_relay;
	int reusable;
	long long expiry;
	char cidrs[256];
};

static void joined_repeated(const uint8_t *b, size_t n, unsigned field,
			    const char *sep, char *out, size_t cap)
{
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *p = NULL;
	size_t l = 0;
	size_t o = 0;

	out[0] = 0;
	et_pb_rd_init(&r, b, n);
	while (et_pb_next(&r, &f, &w, &v, &p, &l) > 0) {
		size_t c;

		if (f != field || w != 2)
			continue;
		c = l < cap - o - 1 ? l : cap - o - 1;
		if (o && o + 1 < cap) {
			size_t sl = strlen(sep);

			if (o + sl < cap) {
				memcpy(out + o, sep, sl);
				o += sl;
			}
		}
		if (!c)
			break;
		memcpy(out + o, p, c);
		o += c;
		out[o] = 0;
	}
}

static int cmd_credential_list(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb resp;
		struct cred_row *rows = NULL;
		int nrows = 0, cap = 0;

		ctx->target = &f.tg[f.index];
		et_pb_init(&resp);
		if (et_cli_call_instance(ctx, "CredentialManageRpc", 3, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to list credentials\n");
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		{
			et_pb_rd r;
			unsigned fld, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&r, resp.p, resp.len);
			while (et_pb_next(&r, &fld, &w, &v, &b, &l) > 0) {
				struct cred_row row;
				uint64_t x = 0;

				if (fld != 1 || w != 2)
					continue;
				memset(&row, 0, sizeof(row));
				(void)et_pb_get_str(b, l, 1, row.id,
						    sizeof(row.id));
				joined_repeated(b, l, 2, ",", row.groups,
						sizeof(row.groups));
				row.allow_relay =
					!et_pb_get_u64(b, l, 3, &x) && x != 0;
				x = 0;
				if (!et_pb_get_u64(b, l, 4, &x))
					row.expiry = (long long)x;
				joined_repeated(b, l, 5, ",", row.cidrs,
						sizeof(row.cidrs));
				x = 0;
				row.reusable = et_pb_get_u64(b, l, 6, &x) || x;
				if (nrows >= cap) {
					int nc = cap ? cap * 2 : 8;
					struct cred_row *p = realloc(
						rows,
						(size_t)nc * sizeof(*rows));

					if (!p)
						break;
					rows = p;
					cap = nc;
				}
				rows[nrows++] = row;
			}
		}
		et_cli_fanout_header(ctx, &f);
		if (f.json) {
			et_cli_fanout_json_open(&f);
			et_json_arr_begin(&f.j, f.nt > 1 ? "result" : NULL);
			for (int i = 0; i < nrows; i++) {
				et_json_obj_begin(&f.j, NULL);
				et_json_str(&f.j, "credential_id", rows[i].id);
				et_json_arr_begin(&f.j, "groups");
				if (rows[i].groups[0])
					et_json_str(&f.j, NULL, rows[i].groups);
				et_json_arr_end(&f.j);
				et_json_bool(&f.j, "allow_relay",
					     rows[i].allow_relay);
				et_json_int(&f.j, "expiry_unix", rows[i].expiry);
				et_json_arr_begin(&f.j, "allowed_proxy_cidrs");
				if (rows[i].cidrs[0])
					et_json_str(&f.j, NULL, rows[i].cidrs);
				et_json_arr_end(&f.j);
				et_json_bool(&f.j, "reusable",
					     rows[i].reusable);
				et_json_obj_end(&f.j);
			}
			et_json_arr_end(&f.j);
			et_cli_fanout_json_close(&f);
		} else if (nrows == 0) {
			et_cli_puts(ctx, "No active credentials\n");
		} else {
			static const char *hdr[] = { "ID",	  "Groups",
						     "Relay", "Reusable",
						     "Expiry", "Allowed CIDRs" };
			et_table t;
			long long now = (long long)time(NULL);

			et_table_init(&t, 6, hdr);
			for (int i = 0; i < nrows; i++) {
				char expiry[64];
				const char *cells[6];

				if (rows[i].expiry - now > 0)
					snprintf(expiry, sizeof(expiry),
						 "%llds remaining",
						 rows[i].expiry - now);
				else
					snprintf(expiry, sizeof(expiry),
						 "expired");
				cells[0] = rows[i].id;
				cells[1] = rows[i].groups;
				cells[2] = rows[i].allow_relay ? "yes" : "no";
				cells[3] = rows[i].reusable ? "yes" : "no";
				cells[4] = expiry;
				cells[5] = rows[i].cidrs;
				et_table_add_row(&t, cells);
			}
			render_table_rounded(&t, &ctx->out);
			et_table_free(&t);
		}
		free(rows);
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

static int cmd_credential_generate(struct et_cli_ctx *ctx, int argc,
				   char **argv)
{
	long long ttl = 0;
	const char *cred_id = NULL, *groups = NULL, *cidrs = NULL;
	int allow_relay = 0, reusable = 1;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--ttl") && i + 1 < argc)
			ttl = strtoll(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--credential-id") && i + 1 < argc)
			cred_id = argv[++i];
		else if (!strcmp(argv[i], "--groups") && i + 1 < argc)
			groups = argv[++i];
		else if (!strcmp(argv[i], "--allowed-proxy-cidrs") &&
			 i + 1 < argc)
			cidrs = argv[++i];
		else if (!strcmp(argv[i], "--allow-relay"))
			allow_relay = 1;
		else if (!strcmp(argv[i], "--reusable") && i + 1 < argc) {
			const char *v = argv[++i];

			reusable = !strcasecmp(v, "true") || !strcmp(v, "1");
		}
	}
	if (ttl <= 0) {
		fprintf(stderr,
			"error: invalid value '0' for '--ttl <TTL>': TTL must be greater than 0\n");
		return 2;
	}
	{
		et_pb req, ident, resp;
		char idbuf[64] = "", secret[256] = "";
		long long expiry = 0;

		et_pb_init(&req);
		et_pb_init(&ident);
		et_pb_init(&resp);
		if (groups && *groups) {
			char *copy = strdup(groups);
			char *save = NULL, *tok;

			for (tok = strtok_r(copy, ",", &save); tok;
			     tok = strtok_r(NULL, ",", &save))
				(void)et_pb_str(&req, 1, tok);
			free(copy);
		}
		(void)et_pb_uint(&req, 2, allow_relay ? 1 : 0);
		if (cidrs && *cidrs) {
			char *copy = strdup(cidrs);
			char *save = NULL, *tok;

			for (tok = strtok_r(copy, ",", &save); tok;
			     tok = strtok_r(NULL, ",", &save))
				(void)et_pb_str(&req, 3, tok);
			free(copy);
		}
		(void)et_pb_uint(&req, 4, (uint64_t)ttl);
		if (cred_id)
			(void)et_pb_str(&req, 5, cred_id);
		if (et_cli_instance_message(ctx, &ident) == 0)
			(void)et_pb_msg(&req, 6, &ident);
		(void)et_pb_uint(&req, 7, reusable ? 1 : 0);
		if (et_cli_call(ctx, "CredentialManageRpc", 1, &req, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to generate credential\n");
			et_pb_free(&req);
			et_pb_free(&ident);
			et_pb_free(&resp);
			return 1;
		}
		(void)et_pb_get_str(resp.p, resp.len, 1, idbuf, sizeof(idbuf));
		(void)et_pb_get_str(resp.p, resp.len, 2, secret,
				    sizeof(secret));
		{
			uint64_t v = 0;

			if (!et_pb_get_u64(resp.p, resp.len, 3, &v))
				expiry = (long long)v;
		}
		if (et_cli_want_json(&ctx->opts)) {
			et_json j;

			et_json_init(&j, &ctx->out);
			et_json_obj_begin(&j, NULL);
			et_json_str(&j, "credential_id", idbuf);
			et_json_str(&j, "credential_secret", secret);
			et_json_int(&j, "expiry_unix", expiry);
			et_json_obj_end(&j);
			et_cli_puts(ctx, "\n");
		} else {
			et_cli_puts(ctx,
				    "Credential generated successfully:\n");
			et_cli_printf(ctx, "  credential_id:     %s\n", idbuf);
			et_cli_printf(ctx, "  credential_secret: %s\n",
				      secret);
			et_cli_puts(ctx, "\n");
			et_cli_puts(
				ctx,
				"To use this credential on a new node:\n");
			et_cli_printf(
				ctx,
				"  easytier-core --network-name <name> --secure-mode --credential %s -p <node-url>\n",
				secret);
		}
		et_pb_free(&req);
		et_pb_free(&ident);
		et_pb_free(&resp);
	}
	return 0;
}

static int cmd_credential_revoke(struct et_cli_ctx *ctx, const char *id)
{
	et_pb req, ident, resp;
	uint64_t ok = 0;

	et_pb_init(&req);
	et_pb_init(&ident);
	et_pb_init(&resp);
	(void)et_pb_str(&req, 1, id ? id : "");
	if (et_cli_instance_message(ctx, &ident) == 0)
		(void)et_pb_msg(&req, 2, &ident);
	if (et_cli_call(ctx, "CredentialManageRpc", 2, &req, &resp)) {
		fprintf(stderr, "easytier-cli: failed to revoke credential\n");
		et_pb_free(&req);
		et_pb_free(&ident);
		et_pb_free(&resp);
		return 1;
	}
	(void)et_pb_get_u64(resp.p, resp.len, 1, &ok);
	if (et_cli_want_json(&ctx->opts)) {
		et_json j;

		et_json_init(&j, &ctx->out);
		et_json_obj_begin(&j, NULL);
		et_json_bool(&j, "success", ok != 0);
		et_json_obj_end(&j);
		et_cli_puts(ctx, "\n");
	} else if (ok) {
		et_cli_puts(ctx, "Credential revoked successfully\n");
	} else {
		et_cli_puts(ctx, "Credential not found\n");
	}
	et_pb_free(&req);
	et_pb_free(&ident);
	et_pb_free(&resp);
	return 0;
}

int et_cli_credential(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc < 1)
		return ET_ENOENT;
	if (!strcmp(argv[0], "generate"))
		return cmd_credential_generate(ctx, argc - 1, argv + 1);
	if (!strcmp(argv[0], "revoke")) {
		if (argc < 2)
			return ET_ENOENT;
		return cmd_credential_revoke(ctx, argv[1]);
	}
	if (!strcmp(argv[0], "list"))
		return cmd_credential_list(ctx);
	return ET_ENOENT;
}

/* ============ gen-autocomplete ============ */

static const char *const ALL_CMDS[] = {
	"peer",	     "connector", "mapped-listener", "stun",
	"route",     "peer-center", "vpn-portal",    "node",
	"service",   "proxy",	     "acl",	      "port-forward",
	"whitelist", "stats",	     "logger",	      "credential",
	"gen-autocomplete", "api",   "doctor",	      "version",
	"help"
};
#define ALL_CMDS_N (sizeof(ALL_CMDS) / sizeof(ALL_CMDS[0]))

static int cmd_gen_autocomplete(struct et_cli_ctx *ctx, const char *shell)
{
	if (!shell) {
		fprintf(stderr,
			"error: the following required arguments were not provided:\n  <SHELL>\n\nPossible values:\n  - bash\n  - elvish\n  - fish\n  - powershell\n  - zsh\n  - nu\n");
		return 2;
	}
	if (!strcmp(shell, "bash")) {
		et_cli_puts(ctx,
			    "_easytier-cli()\n{\n    local cur prev\n    COMPREPLY=()\n    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n    if [ \"$COMP_CWORD\" -eq 1 ]; then\n        COMPREPLY=( $(compgen -W \"");
		for (size_t i = 0; i < ALL_CMDS_N; i++)
			et_cli_printf(ctx, "%s%s", i ? " " : "", ALL_CMDS[i]);
		et_cli_puts(ctx,
			    "\" -- \"$cur\") )\n        return 0\n    fi\n    case \"${COMP_WORDS[1]}\" in\n"
			    "        node) COMPREPLY=( $(compgen -W \"info config\" -- \"$cur\") ) ;;\n"
			    "        route) COMPREPLY=( $(compgen -W \"list dump\" -- \"$cur\") ) ;;\n"
			    "        peer) COMPREPLY=( $(compgen -W \"list\" -- \"$cur\") ) ;;\n"
			    "        connector) COMPREPLY=( $(compgen -W \"list add remove\" -- \"$cur\") ) ;;\n"
			    "        mapped-listener) COMPREPLY=( $(compgen -W \"list add remove\" -- \"$cur\") ) ;;\n"
			    "        vpn-portal) COMPREPLY=( $(compgen -W \"add-client remove-client clear-clients\" -- \"$cur\") ) ;;\n"
			    "        acl) COMPREPLY=( $(compgen -W \"stats set\" -- \"$cur\") ) ;;\n"
			    "        port-forward) COMPREPLY=( $(compgen -W \"list add remove\" -- \"$cur\") ) ;;\n"
			    "        whitelist) COMPREPLY=( $(compgen -W \"show set-tcp set-udp clear-tcp clear-udp\" -- \"$cur\") ) ;;\n"
			    "        stats) COMPREPLY=( $(compgen -W \"show prometheus\" -- \"$cur\") ) ;;\n"
			    "        logger) COMPREPLY=( $(compgen -W \"get set\" -- \"$cur\") ) ;;\n"
			    "        credential) COMPREPLY=( $(compgen -W \"generate revoke list\" -- \"$cur\") ) ;;\n"
			    "        gen-autocomplete) COMPREPLY=( $(compgen -W \"bash elvish fish powershell zsh nu\" -- \"$cur\") ) ;;\n"
			    "        *) COMPREPLY=( $(compgen -W \"-p --rpc-portal -o --output --no-trunc -i --instance-id -n --instance-name -h --help -V --version\" -- \"$cur\") ) ;;\n"
			    "    esac\n    return 0\n}\n\ncomplete -F _easytier-cli easytier-cli\n");
		return 0;
	}
	if (!strcmp(shell, "zsh")) {
		et_cli_puts(ctx, "#compdef easytier-cli\n\n_easytier-cli() {\n    local -a commands\n    commands=(\n");
		for (size_t i = 0; i < ALL_CMDS_N; i++)
			et_cli_printf(ctx, "        '%s'\n", ALL_CMDS[i]);
		et_cli_puts(ctx,
			    "    )\n    _arguments -C \\\n"
			    "        '(-p --rpc-portal)'{-p,--rpc-portal}'[easytier-core rpc portal address]:addr:' \\\n"
			    "        '(-o --output)'{-o,--output}'[output format]:format:(table json)' \\\n"
			    "        '--no-trunc[disable column truncation]' \\\n"
			    "        '(-i --instance-id)'{-i,--instance-id}'[the instance id]:id:' \\\n"
			    "        '(-n --instance-name)'{-n,--instance-name}'[the instance name]:name:' \\\n"
			    "        '1: :->cmds' '*:: :->args'\n"
			    "    case $state in\n        cmds) _describe 'command' commands ;;\n    esac\n}\n\n"
			    "_easytier-cli \"$@\"\n");
		return 0;
	}
	if (!strcmp(shell, "fish")) {
		for (size_t i = 0; i < ALL_CMDS_N; i++)
			et_cli_printf(
				ctx,
				"complete -c easytier-cli -n '__fish_use_subcommand' -a '%s'\n",
				ALL_CMDS[i]);
		et_cli_puts(ctx,
			    "complete -c easytier-cli -s p -l rpc-portal -d 'easytier-core rpc portal address' -r\n"
			    "complete -c easytier-cli -s o -l output -d 'output format' -r -a 'table json'\n"
			    "complete -c easytier-cli -l no-trunc -d 'disable column truncation'\n"
			    "complete -c easytier-cli -s i -l instance-id -d 'the instance id' -r\n"
			    "complete -c easytier-cli -s n -l instance-name -d 'the instance name' -r\n"
			    "complete -c easytier-cli -s h -l help -d 'Print help'\n"
			    "complete -c easytier-cli -s V -l version -d 'Print version'\n");
		return 0;
	}
	if (!strcmp(shell, "powershell")) {
		et_cli_puts(ctx,
			    "using namespace System.Management.Automation\n"
			    "using namespace System.Management.Automation.Language\n\n"
			    "Register-ArgumentCompleter -Native -CommandName 'easytier-cli' -ScriptBlock {\n"
			    "    param($wordToComplete, $commandAst, $cursorPosition)\n    $commands = @(");
		for (size_t i = 0; i < ALL_CMDS_N; i++)
			et_cli_printf(ctx, "%s'%s'", i ? ", " : "",
				      ALL_CMDS[i]);
		et_cli_puts(ctx,
			    ")\n    $commands | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object {\n"
			    "        [CompletionResult]::new($_, $_, 'ParameterValue', $_)\n    }\n}\n");
		return 0;
	}
	if (!strcmp(shell, "elvish")) {
		et_cli_puts(ctx,
			    "edit:completion:arg-completer[easytier-cli] = [@words]{\n    var candidates = [");
		for (size_t i = 0; i < ALL_CMDS_N; i++)
			et_cli_printf(ctx, "%s'%s'", i ? " " : "",
				      ALL_CMDS[i]);
		et_cli_puts(ctx, "]\n    put $@candidates\n}\n");
		return 0;
	}
	if (!strcmp(shell, "nu")) {
		et_cli_puts(ctx,
			    "module completions {\n\n"
			    "  export extern easytier-cli [\n"
			    "    --rpc-portal(-p): string   # easytier-core rpc portal address\n"
			    "    --output(-o): string       # output format\n"
			    "    --no-trunc                 # disable column truncation\n"
			    "    --instance-id(-i): string  # the instance id\n"
			    "    --instance-name(-n): string # the instance name\n"
			    "    --help(-h)                 # Print help\n"
			    "    --version(-V)              # Print version\n"
			    "  ]\n}\n");
		return 0;
	}
	fprintf(stderr,
		"error: invalid value '%s' for '<SHELL>'\n  [possible values: bash, elvish, fish, powershell, zsh, nu]\n",
		shell);
	return 2;
}

/* ============ 入口 ============ */

int et_cli_stun(struct et_cli_ctx *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return cmd_stun(ctx);
}

int et_cli_peer_center(struct et_cli_ctx *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return cmd_peer_center(ctx);
}

int et_cli_stats(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc > 0) {
		if (!strcmp(argv[0], "show") || !strcmp(argv[0], "list"))
			return cmd_stats(ctx, 0);
		if (!strcmp(argv[0], "prometheus"))
			return cmd_stats(ctx, 1);
		return ET_ENOENT;
	}
	return cmd_stats(ctx, 0);
}

int et_cli_gen_autocomplete(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc < 1)
		return cmd_gen_autocomplete(ctx, NULL);
	return cmd_gen_autocomplete(ctx, argv[0]);
}