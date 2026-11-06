/*
 * etd_cli_extra_c.c - acl / port-forward / whitelist
 *
 * 上游规格：E:\dsh\up_easytier-cli.rs（v2.7.0）
 *   - acl stats   ：AclManageRpc.GetAclStats(1)，文本是 easytier-proto/src/acl.rs 里
 *                    AclStats 的 Display 实现（不是表格）
 *   - acl set     ：整段 TOML 反序列化为 acl.Acl 后 PatchConfig
 *   - port-forward：PortForwardManageRpc.ListPortForward(1) → markdown 表
 *   - whitelist   ：AclManageRpc.GetWhitelist(2) → 两行文本
 * 写入统一走 ConfigRpc.PatchConfig（et_cli_patch_config）。
 */
#include "etd_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============ 通用：Rust Debug 打印 repeated string ============ */

struct strvec {
	char **v;
	int n;
	int cap;
};

static void strvec_add(struct strvec *sv, const char *s)
{
	if (sv->n >= sv->cap) {
		int cap = sv->cap ? sv->cap * 2 : 4;
		char **p = realloc(sv->v, (size_t)cap * sizeof(*p));

		if (!p)
			return;
		sv->v = p;
		sv->cap = cap;
	}
	sv->v[sv->n++] = strdup(s ? s : "");
}

static void strvec_free(struct strvec *sv)
{
	for (int i = 0; i < sv->n; i++)
		free(sv->v[i]);
	free(sv->v);
	memset(sv, 0, sizeof(*sv));
}

struct strvec_ctx {
	struct strvec *sv;
};

static int strvec_cb(const char *s, size_t n, void *ud)
{
	struct strvec_ctx *c = ud;

	(void)n;
	strvec_add(c->sv, s);
	return 0;
}

static void strvec_debug(const struct strvec *sv, et_buf *out)
{
	et_buf_add(out, "[");
	for (int i = 0; i < sv->n; i++)
		et_buf_addf(out, "%s\"%s\"", i ? ", " : "", sv->v[i]);
	et_buf_add(out, "]");
}

/* ============ chrono 的 Local Display ============ */

static void fmt_local_time(uint64_t secs, char *out, size_t cap)
{
	time_t t = (time_t)secs;
	struct tm tmv;
	char base[32];
	char tz[16] = "+00:00";

	localtime_r(&t, &tmv);
	strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tmv);
	{
		long off = tmv.tm_gmtoff;

		snprintf(tz, sizeof(tz), "%c%02ld:%02ld", off < 0 ? '-' : '+',
			 labs(off) / 3600, (labs(off) % 3600) / 60);
	}
	snprintf(out, cap, "%s %s", base, tz);
}

/* ============ acl ============ */

static const char *acl_proto_name(uint64_t v)
{
	static const char *names[] = { "Unspecified", "TCP",   "UDP",
				       "ICMP",	      "ICMPv6", "Any" };

	return v < 6 ? names[v] : "Unspecified";
}

static const char *acl_action_name(uint64_t v)
{
	static const char *names[] = { "Noop", "Allow", "Drop" };

	return v < 3 ? names[v] : "Noop";
}

static const char *acl_conn_state_name(uint64_t v)
{
	static const char *names[] = { "New", "Established", "Related",
				       "Invalid" };

	return v < 4 ? names[v] : "Invalid";
}

/* Rule Display（acl.rs:60-79） */
static void render_rule(const uint8_t *b, size_t n, et_buf *out)
{
	char name[128] = "";
	struct strvec ports, src_ports, src_ips, dst_ips;
	struct strvec_ctx c;
	uint64_t prio = 0, action = 0, protocol = 0, rate = 0, burst = 0;
	uint64_t enabled = 0, stateful = 0;

	memset(&ports, 0, sizeof(ports));
	memset(&src_ports, 0, sizeof(src_ports));
	memset(&src_ips, 0, sizeof(src_ips));
	memset(&dst_ips, 0, sizeof(dst_ips));
	(void)et_pb_get_str(b, n, 1, name, sizeof(name));
	(void)et_pb_get_u64(b, n, 3, &prio);
	(void)et_pb_get_u64(b, n, 4, &enabled);
	(void)et_pb_get_u64(b, n, 5, &protocol);
	(void)et_pb_get_u64(b, n, 10, &action);
	(void)et_pb_get_u64(b, n, 11, &rate);
	(void)et_pb_get_u64(b, n, 12, &burst);
	(void)et_pb_get_u64(b, n, 13, &stateful);
	c.sv = &ports;
	(void)et_pb_foreach_str(b, n, 6, strvec_cb, &c);
	c.sv = &src_ports;
	(void)et_pb_foreach_str(b, n, 9, strvec_cb, &c);
	c.sv = &src_ips;
	(void)et_pb_foreach_str(b, n, 7, strvec_cb, &c);
	c.sv = &dst_ips;
	(void)et_pb_foreach_str(b, n, 8, strvec_cb, &c);

	et_buf_addf(out,
		    "[name: '%s', prio: %llu, action: %s, enabled: %s, proto: %s, ports: ",
		    name, (unsigned long long)prio,
		    acl_action_name(action),
		    enabled ? "true" : "false", acl_proto_name(protocol));
	strvec_debug(&ports, out);
	et_buf_add(out, ", src_ports: ");
	strvec_debug(&src_ports, out);
	et_buf_add(out, ", src_ips: ");
	strvec_debug(&src_ips, out);
	et_buf_add(out, ", dst_ips: ");
	strvec_debug(&dst_ips, out);
	et_buf_addf(out, ", stateful: %s, rate: %llu, burst: %llu]",
		    stateful ? "true" : "false", (unsigned long long)rate,
		    (unsigned long long)burst);
	strvec_free(&ports);
	strvec_free(&src_ports);
	strvec_free(&src_ips);
	strvec_free(&dst_ips);
}

static int acl_global_cb(const uint8_t *b, size_t n, void *ud)
{
	et_buf *out = ud;
	char k[128] = "";
	uint64_t v = 0;

	(void)et_pb_get_str(b, n, 1, k, sizeof(k));
	(void)et_pb_get_u64(b, n, 2, &v);
	et_buf_addf(out, "    %s: %llu\n", k, (unsigned long long)v);
	return 0;
}

static int acl_conn_track_cb(const uint8_t *b, size_t n, void *ud)
{
	et_buf *out = ud;
	const uint8_t *sa = NULL;
	size_t sal = 0;
	char src[64] = "-", dst[64] = "-";
	uint64_t proto = 0, state = 0, pkts = 0, bytes = 0, created = 0,
		 last = 0;
	char created_s[64] = "", last_s[64] = "";

	if (et_pb_find(b, n, 1, 2, NULL, &sa, &sal))
		et_fmt_socket_addr(sa, sal, src, sizeof(src));
	if (et_pb_find(b, n, 2, 2, NULL, &sa, &sal))
		et_fmt_socket_addr(sa, sal, dst, sizeof(dst));
	(void)et_pb_get_u64(b, n, 3, &proto);
	(void)et_pb_get_u64(b, n, 4, &state);
	(void)et_pb_get_u64(b, n, 5, &created);
	(void)et_pb_get_u64(b, n, 6, &last);
	(void)et_pb_get_u64(b, n, 7, &pkts);
	(void)et_pb_get_u64(b, n, 8, &bytes);
	fmt_local_time(created, created_s, sizeof(created_s));
	fmt_local_time(last, last_s, sizeof(last_s));
	et_buf_addf(
		out,
		"    [src: %s, dst: %s, proto: %s, state: %s, pkts: %llu, bytes: %llu, created: %s, last_seen: %s]\n",
		src, dst, acl_proto_name(proto), acl_conn_state_name(state),
		(unsigned long long)pkts, (unsigned long long)bytes, created_s,
		last_s);
	return 0;
}

static int acl_rule_cb(const uint8_t *b, size_t n, void *ud)
{
	et_buf *out = ud;
	const uint8_t *rule = NULL, *stat = NULL;
	size_t rulelen = 0, statlen = 0;

	et_buf_add(out, "    ");
	if (et_pb_find(b, n, 1, 2, NULL, &rule, &rulelen))
		render_rule(rule, rulelen, out);
	else
		et_buf_add(out, "<default/none>");
	et_buf_add(out, " ");
	if (et_pb_find(b, n, 2, 2, NULL, &stat, &statlen)) {
		uint64_t pkts = 0, bytes = 0;

		(void)et_pb_get_u64(stat, statlen, 1, &pkts);
		(void)et_pb_get_u64(stat, statlen, 2, &bytes);
		et_buf_addf(out, "[pkts: %llu, bytes: %llu]",
			    (unsigned long long)pkts,
			    (unsigned long long)bytes);
	}
	et_buf_add(out, "\n");
	return 0;
}

static void render_acl_stats(const uint8_t *b, size_t n, et_buf *out)
{
	et_buf_add(out, "AclStats:\n  Global:\n");
	(void)et_pb_foreach_msg(b, n, 3, acl_global_cb, out);
	et_buf_add(out, "  ConnTrack:\n");
	(void)et_pb_foreach_msg(b, n, 2, acl_conn_track_cb, out);
	et_buf_add(out, "  Rules:\n");
	(void)et_pb_foreach_msg(b, n, 1, acl_rule_cb, out);
}

static int cmd_acl_stats(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb resp;
		const uint8_t *st = NULL;
		size_t stlen = 0;

		ctx->target = &f.tg[f.index];
		et_pb_init(&resp);
		if (et_cli_call_instance(ctx, "AclManageRpc", 1, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to get acl stats\n");
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		if (et_pb_find(resp.p, resp.len, 1, 2, NULL, &st, &stlen)) {
			render_acl_stats(st, stlen, &ctx->out);
			/* 上游是 println!("{}", acl_stats)：Display 自带尾换行，
			 * println 再加一个，所以末尾会多一个空行 */
			et_cli_puts(ctx, "\n");
		} else {
			et_cli_puts(ctx, "No ACL statistics available\n");
		}
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

static int cmd_acl_set(struct et_cli_ctx *ctx, const char *raw)
{
	/* 上游把整段 TOML 反序列化成 acl.Acl 再 PatchConfig。C 里没有 TOML→protobuf
	 * 的通用映射，这里明确报错而不是静默成功，避免用户以为规则生效了。 */
	if (!raw || !*raw) {
		fprintf(stderr,
			"error: the following required arguments were not provided:\n  <ACL>\n");
		return 2;
	}
	if (raw[0] == '@')
		fprintf(stderr,
			"easytier-cli: acl set: 读取 %s 后需要做 TOML→protobuf 转换，"
			"当前 C 版未实现；请在 LuCI 界面保存 ACL，或直接改 "
			"/etc/easytier/config.toml 后重启 core。\n",
			raw + 1);
	else
		fprintf(stderr,
			"easytier-cli: acl set: 整段 ACL TOML 需要做 TOML→protobuf 转换，"
			"当前 C 版未实现；请在 LuCI 界面保存 ACL，或直接改 "
			"/etc/easytier/config.toml 后重启 core。\n");
	(void)ctx;
	return 1;
}

/* ============ port-forward ============ */

static int parse_socket_addr(const char *s, et_pb *out)
{
	char host[64];
	unsigned a, b_, c, d, port;
	const char *colon = strrchr(s, ':');

	if (!colon)
		return -1;
	{
		size_t hl = (size_t)(colon - s);

		if (hl >= sizeof(host))
			return -1;
		memcpy(host, s, hl);
		host[hl] = 0;
	}
	if (sscanf(colon + 1, "%u", &port) != 1)
		return -1;
	/* 支持 IPv6 [::]:port 之外的形态：这里只处理 IPv4 / 0.0.0.0 */
	if (sscanf(host, "%u.%u.%u.%u", &a, &b_, &c, &d) != 4)
		return -1;
	{
		et_pb ip;

		et_pb_init(&ip);
		if (et_pb_uint(&ip, 1,
			       ((uint64_t)a << 24) | ((uint64_t)b_ << 16) |
				       ((uint64_t)c << 8) | d)) {
			et_pb_free(&ip);
			return -1;
		}
		if (et_pb_msg(out, 1, &ip) || et_pb_uint(out, 3, port)) {
			et_pb_free(&ip);
			return -1;
		}
		et_pb_free(&ip);
	}
	return 0;
}

static int patch_port_forward(struct et_cli_ctx *ctx, int action,
			      const char *protocol, const char *bind_addr,
			      const char *dst_addr)
{
	et_pb patch, pf, cfg, bind, dst;
	int rc = -1;

	if (strcmp(protocol, "tcp") && strcmp(protocol, "udp")) {
		fprintf(stderr, "easytier-cli: Protocol must be 'tcp' or 'udp'\n");
		return -1;
	}
	et_pb_init(&patch);
	et_pb_init(&pf);
	et_pb_init(&cfg);
	et_pb_init(&bind);
	et_pb_init(&dst);
	if (parse_socket_addr(bind_addr, &bind)) {
		fprintf(stderr, "easytier-cli: Invalid bind address: %s\n",
			bind_addr);
		goto out;
	}
	(void)et_pb_msg(&cfg, 1, &bind);
	if (dst_addr && *dst_addr) {
		if (parse_socket_addr(dst_addr, &dst)) {
			fprintf(stderr,
				"easytier-cli: Invalid dst address: %s\n",
				dst_addr);
			goto out;
		}
		(void)et_pb_msg(&cfg, 2, &dst);
	}
	/* SocketType: TCP=0 / UDP=1 */
	(void)et_pb_uint(&cfg, 3, !strcmp(protocol, "udp") ? 1 : 0);
	(void)et_pb_uint(&pf, 1, (uint64_t)action);
	(void)et_pb_msg(&pf, 2, &cfg);
	(void)et_pb_msg(&patch, 4, &pf);
	if (et_cli_patch_config(ctx, &patch)) {
		fprintf(stderr,
			"easytier-cli: failed to patch port forward rule\n");
		goto out;
	}
	rc = 0;
out:
	et_pb_free(&patch);
	et_pb_free(&pf);
	et_pb_free(&cfg);
	et_pb_free(&bind);
	et_pb_free(&dst);
	return rc;
}

static int cmd_port_forward_list(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb resp;
		et_table t;
		static const char *hdr[] = { "protocol", "bind_addr",
					     "dst_addr" };

		ctx->target = &f.tg[f.index];
		et_pb_init(&resp);
		if (et_cli_call_instance(ctx, "PortForwardManageRpc", 1,
					 &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to list port forwards\n");
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		et_table_init(&t, 3, hdr);
		{
			et_pb_rd r;
			unsigned fld, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&r, resp.p, resp.len);
			while (et_pb_next(&r, &fld, &w, &v, &b, &l) > 0) {
				const uint8_t *sa = NULL;
				size_t sal = 0;
				char bind[64] = "";
				char dst[64] = "";
				char proto[8] = "Tcp";
				const char *cells[3];

				if (fld != 1 || w != 2)
					continue;
				if (et_pb_find(b, l, 1, 2, NULL, &sa, &sal))
					et_fmt_socket_addr(sa, sal, bind,
							   sizeof(bind));
				if (et_pb_find(b, l, 2, 2, NULL, &sa, &sal))
					et_fmt_socket_addr(sa, sal, dst,
							   sizeof(dst));
				{
					uint64_t st = 0;

					if (!et_pb_get_u64(b, l, 3, &st) &&
					    st == 1)
						snprintf(proto, sizeof(proto),
							 "Udp");
				}
				cells[0] = proto;
				cells[1] = bind;
				cells[2] = dst;
				et_table_add_row(&t, cells);
			}
		}
		(void)et_table_render(&t, &ctx->opts, &ctx->out);
		et_table_free(&t);
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* ============ whitelist ============ */

static int patch_whitelist(struct et_cli_ctx *ctx, int action,
			   const char *ports, int is_tcp)
{
	et_pb patch, acl;

	et_pb_init(&patch);
	et_pb_init(&acl);
	if (action == 2) {
		et_pb sp;

		et_pb_init(&sp);
		(void)et_pb_uint(&sp, 1, 2);
		(void)et_pb_str(&sp, 2, "");
		(void)et_pb_msg(&acl, is_tcp ? 2 : 3, &sp);
		et_pb_free(&sp);
	} else {
		/* 先清空再逐条添加（上游是 Clear + 若干个 Add 的 StringPatch 列表） */
		et_pb clear;

		et_pb_init(&clear);
		(void)et_pb_uint(&clear, 1, 2);
		(void)et_pb_str(&clear, 2, "");
		(void)et_pb_msg(&acl, is_tcp ? 2 : 3, &clear);
		et_pb_free(&clear);
		if (ports && *ports) {
			char *copy = strdup(ports);
			char *save = NULL, *tok;

			for (tok = strtok_r(copy, ",", &save); tok;
			     tok = strtok_r(NULL, ",", &save)) {
				et_pb sp;

				while (*tok == ' ')
					tok++;
				et_pb_init(&sp);
				(void)et_pb_uint(&sp, 1, 0);
				(void)et_pb_str(&sp, 2, tok);
				(void)et_pb_msg(&acl, is_tcp ? 2 : 3, &sp);
				et_pb_free(&sp);
			}
			free(copy);
		}
	}
	(void)et_pb_msg(&patch, 5, &acl);
	{
		int rc = et_cli_patch_config(ctx, &patch);

		et_pb_free(&patch);
		et_pb_free(&acl);
		return rc;
	}
}

static int cmd_whitelist_show(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb resp;
		et_buf tcp, udp;

		ctx->target = &f.tg[f.index];
		et_pb_init(&resp);
		if (et_cli_call_instance(ctx, "AclManageRpc", 2, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to get whitelist\n");
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		et_buf_init(&tcp);
		et_buf_init(&udp);
		{
			struct strvec_ctx c;
			struct strvec sv;

			memset(&sv, 0, sizeof(sv));
			c.sv = &sv;
			(void)et_pb_foreach_str(resp.p, resp.len, 1,
						strvec_cb, &c);
			for (int i = 0; i < sv.n; i++)
				et_buf_addf(&tcp, "%s%s", i ? ", " : "",
					    sv.v[i]);
			strvec_free(&sv);
			c.sv = &sv;
			(void)et_pb_foreach_str(resp.p, resp.len, 2,
						strvec_cb, &c);
			for (int i = 0; i < sv.n; i++)
				et_buf_addf(&udp, "%s%s", i ? ", " : "",
					    sv.v[i]);
			strvec_free(&sv);
		}
		et_cli_fanout_header(ctx, &f);
		et_cli_printf(ctx, "TCP Whitelist: %s\n",
			      tcp.len ? tcp.p : "None");
		et_cli_printf(ctx, "UDP Whitelist: %s\n",
			      udp.len ? udp.p : "None");
		et_buf_free(&tcp);
		et_buf_free(&udp);
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* ============ 入口 ============ */

int et_cli_acl(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "stats";

	if (!strcmp(sub, "stats") || !strcmp(sub, "show"))
		return cmd_acl_stats(ctx);
	if (!strcmp(sub, "set")) {
		if (argc < 2)
			return ET_ENOENT;
		return cmd_acl_set(ctx, argv[1]);
	}
	return ET_ENOENT;
}

int et_cli_port_forward(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "list";

	if (!strcmp(sub, "list") || argc == 0)
		return cmd_port_forward_list(ctx);
	if (!strcmp(sub, "add")) {
		if (argc < 4)
			return ET_ENOENT;
		if (patch_port_forward(ctx, 0, argv[1], argv[2], argv[3]))
			return 1;
		et_cli_printf(ctx, "Port forward rule add: %s %s\n", argv[1],
			      argv[2]);
		return 0;
	}
	if (!strcmp(sub, "remove")) {
		if (argc < 3)
			return ET_ENOENT;
		if (patch_port_forward(ctx, 1, argv[1], argv[2],
				       argc > 3 ? argv[3] : NULL))
			return 1;
		et_cli_printf(ctx, "Port forward rule remove: %s %s\n", argv[1],
			      argv[2]);
		return 0;
	}
	return ET_ENOENT;
}

int et_cli_whitelist(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "show";

	if (!strcmp(sub, "show") || argc == 0)
		return cmd_whitelist_show(ctx);
	if (!strcmp(sub, "set-tcp")) {
		if (argc < 2)
			return ET_ENOENT;
		if (patch_whitelist(ctx, 0, argv[1], 1)) {
			fprintf(stderr,
				"easytier-cli: failed to set TCP whitelist\n");
			return 1;
		}
		et_cli_printf(ctx, "TCP whitelist updated: %s\n", argv[1]);
		return 0;
	}
	if (!strcmp(sub, "set-udp")) {
		if (argc < 2)
			return ET_ENOENT;
		if (patch_whitelist(ctx, 0, argv[1], 0)) {
			fprintf(stderr,
				"easytier-cli: failed to set UDP whitelist\n");
			return 1;
		}
		et_cli_printf(ctx, "UDP whitelist updated: %s\n", argv[1]);
		return 0;
	}
	if (!strcmp(sub, "clear-tcp")) {
		if (patch_whitelist(ctx, 2, NULL, 1)) {
			fprintf(stderr,
				"easytier-cli: failed to clear TCP whitelist\n");
			return 1;
		}
		et_cli_puts(ctx, "TCP whitelist cleared\n");
		return 0;
	}
	if (!strcmp(sub, "clear-udp")) {
		if (patch_whitelist(ctx, 2, NULL, 0)) {
			fprintf(stderr,
				"easytier-cli: failed to clear UDP whitelist\n");
			return 1;
		}
		et_cli_puts(ctx, "UDP whitelist cleared\n");
		return 0;
	}
	return ET_ENOENT;
}