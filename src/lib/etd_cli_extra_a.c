/*
 * etd_cli_extra_a.c - connector / mapped-listener / vpn-portal / proxy
 *
 * 上游这三个命令的人类可读输出是 Rust 的 {:#?}（Debug pretty），见
 * E:\dsh\up_easytier-cli.rs 的 handle_connector_list / handle_mapped_listener_list /
 * handle_proxy。这里按同样的缩进与尾逗号逐字节复刻。
 */
#include "etd_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IND "    "

static void dbg_indent(et_buf *b, int level)
{
	for (int i = 0; i < level; i++)
		et_buf_add(b, IND);
}

/* ============ connector ============ */

struct conn_row {
	char url[512];
	int have_url;
	uint64_t status;
};

struct conn_list {
	struct conn_row *v;
	int n;
	int cap;
};

struct conn_scan {
	struct conn_list *l;
};

static int connector_cb(const uint8_t *b, size_t n, void *ud)
{
	struct conn_scan *s = ud;
	struct conn_list *l = s->l;
	struct conn_row row;
	const uint8_t *u = NULL;
	size_t ul = 0;

	memset(&row, 0, sizeof(row));
	if (et_pb_find(b, n, 1, 2, NULL, &u, &ul)) {
		row.have_url = 1;
		(void)et_pb_get_str(u, ul, 1, row.url, sizeof(row.url));
	}
	(void)et_pb_get_u64(b, n, 2, &row.status);
	if (l->n >= l->cap) {
		int cap = l->cap ? l->cap * 2 : 8;
		struct conn_row *p = realloc(l->v, (size_t)cap * sizeof(*p));

		if (!p)
			return 1;
		l->v = p;
		l->cap = cap;
	}
	l->v[l->n++] = row;
	return 0;
}

static const char *connector_status_name(uint64_t v)
{
	static const char *names[] = { "Connected", "Disconnected",
				       "Connecting" };

	return v < 3 ? names[v] : "Connected";
}

static int fetch_connectors(struct et_cli_ctx *ctx, struct conn_list *l)
{
	et_pb resp;
	struct conn_scan s;

	memset(l, 0, sizeof(*l));
	s.l = l;
	et_pb_init(&resp);
	if (et_cli_call_instance(ctx, "ConnectorManageRpc", 1, &resp)) {
		et_pb_free(&resp);
		return -1;
	}
	(void)et_pb_foreach_msg(resp.p, resp.len, 1, connector_cb, &s);
	et_pb_free(&resp);
	return 0;
}

static void connector_debug(struct et_cli_ctx *ctx, const struct conn_list *l)
{
	if (!l->n) {
		et_cli_puts(ctx, "response: []\n");
		return;
	}
	et_cli_puts(ctx, "response: [\n");
	for (int i = 0; i < l->n; i++) {
		et_buf *b = &ctx->out;

		dbg_indent(b, 1);
		et_buf_add(b, "Connector {\n");
		dbg_indent(b, 2);
		if (l->v[i].have_url) {
			et_buf_add(b, "url: Some(\n");
			dbg_indent(b, 3);
			et_buf_add(b, "Url {\n");
			dbg_indent(b, 4);
			et_buf_addf(b, "url: \"%s\",\n", l->v[i].url);
			dbg_indent(b, 3);
			et_buf_add(b, "},\n");
			dbg_indent(b, 2);
			et_buf_add(b, "),\n");
		} else {
			et_buf_add(b, "url: None,\n");
		}
		dbg_indent(b, 2);
		et_buf_addf(b, "status: %s,\n",
			    connector_status_name(l->v[i].status));
		dbg_indent(b, 1);
		et_buf_add(b, "},\n");
	}
	et_cli_puts(ctx, "]\n");
}

static void connector_json(struct et_cli_ctx *ctx, const struct conn_list *l)
{
	et_json j;

	et_json_init(&j, &ctx->out);
	et_json_arr_begin(&j, NULL);
	for (int i = 0; i < l->n; i++) {
		et_json_obj_begin(&j, NULL);
		if (l->v[i].have_url) {
			et_json_obj_begin(&j, "url");
			et_json_str(&j, "url", l->v[i].url);
			et_json_obj_end(&j);
		} else {
			et_json_null(&j, "url");
		}
		et_json_uint(&j, "status", l->v[i].status);
		et_json_obj_end(&j);
	}
	et_json_arr_end(&j);
	et_cli_puts(ctx, "\n");
}

static int cmd_connector_list(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		struct conn_list l;

		ctx->target = &f.tg[f.index];
		if (fetch_connectors(ctx, &l)) {
			fprintf(stderr,
				"easytier-cli: failed to list connectors\n");
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		if (f.json)
			connector_json(ctx, &l);
		else
			connector_debug(ctx, &l);
		free(l.v);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* action: 0=ADD 1=REMOVE 2=CLEAR */
static int patch_url(struct et_cli_ctx *ctx, unsigned field, int action,
		     const char *url)
{
	et_pb patch, sub, upatch;

	et_pb_init(&patch);
	et_pb_init(&sub);
	et_pb_init(&upatch);
	if (et_pb_uint(&upatch, 1, (uint64_t)action) ||
	    et_pb_str(&sub, 1, url) || et_pb_msg(&upatch, 2, &sub) ||
	    et_pb_msg(&patch, field, &upatch)) {
		et_pb_free(&patch);
		et_pb_free(&sub);
		et_pb_free(&upatch);
		return -1;
	}
	{
		int rc = et_cli_patch_config(ctx, &patch);

		et_pb_free(&patch);
		et_pb_free(&sub);
		et_pb_free(&upatch);
		return rc;
	}
}

static int cmd_connector_modify(struct et_cli_ctx *ctx, const char *url,
				int action)
{
	if (!url || !strstr(url, "://")) {
		fprintf(stderr, "easytier-cli: invalid url (%s)\n",
			url ? url : "");
		return 1;
	}
	if (patch_url(ctx, 10, action, url)) {
		fprintf(stderr, "easytier-cli: failed to patch connector\n");
		return 1;
	}
	et_cli_printf(ctx, "connector %s applied to selected instance(s): %s\n",
		      action == 1 ? "remove" : "add", url);
	return 0;
}

/* ============ mapped-listener ============ */

struct ml_row {
	char url[512];
	int have_url;
};

struct ml_list {
	struct ml_row *v;
	int n;
	int cap;
};

static int mapped_listener_cb(const uint8_t *b, size_t n, void *ud)
{
	struct ml_list *l = ud;
	struct ml_row row;
	const uint8_t *u = NULL;
	size_t ul = 0;

	memset(&row, 0, sizeof(row));
	if (et_pb_find(b, n, 1, 2, NULL, &u, &ul)) {
		row.have_url = 1;
		(void)et_pb_get_str(u, ul, 1, row.url, sizeof(row.url));
	}
	if (l->n >= l->cap) {
		int cap = l->cap ? l->cap * 2 : 8;
		struct ml_row *p = realloc(l->v, (size_t)cap * sizeof(*p));

		if (!p)
			return 1;
		l->v = p;
		l->cap = cap;
	}
	l->v[l->n++] = row;
	return 0;
}

static int cmd_mapped_listener(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "list";

	if (!strcmp(sub, "add") || !strcmp(sub, "remove")) {
		const char *url = argc > 1 ? argv[1] : "";

		if (strncmp(url, "tcp://", 6) && strncmp(url, "udp://", 6)) {
			fprintf(stderr,
				"easytier-cli: Url (%s) must start with tcp:// or udp://\n",
				url);
			return 1;
		}
		if (patch_url(ctx, 9, !strcmp(sub, "add") ? 0 : 1, url)) {
			fprintf(stderr,
				"easytier-cli: failed to patch mapped listener\n");
			return 1;
		}
		et_cli_printf(ctx, "%s mapped listener: %s\n",
			      !strcmp(sub, "add") ? "add" : "remove", url);
		return 0;
	}
	if (strcmp(sub, "list") && argc > 0)
		return ET_ENOENT;
	{
		struct et_cli_fanout f;
		int rc = 0;

		if (et_cli_fanout_begin(ctx, &f))
			return 1;
		for (f.index = 0; f.index < f.nt; f.index++) {
			et_pb resp;
			struct ml_list l;

			memset(&l, 0, sizeof(l));
			ctx->target = &f.tg[f.index];
			et_pb_init(&resp);
			if (et_cli_call_instance(ctx,
						 "MappedListenerManageRpc", 1,
						 &resp)) {
				fprintf(stderr,
					"easytier-cli: failed to list mapped listeners\n");
				et_pb_free(&resp);
				rc = 1;
				break;
			}
			(void)et_pb_foreach_msg(resp.p, resp.len, 1,
						mapped_listener_cb, &l);
			et_cli_fanout_header(ctx, &f);
			if (f.json) {
				et_json j;

				et_json_init(&j, &ctx->out);
				et_json_arr_begin(&j, NULL);
				for (int i = 0; i < l.n; i++) {
					et_json_obj_begin(&j, NULL);
					if (l.v[i].have_url) {
						et_json_obj_begin(&j, "url");
						et_json_str(&j, "url",
							    l.v[i].url);
						et_json_obj_end(&j);
					} else {
						et_json_null(&j, "url");
					}
					et_json_obj_end(&j);
				}
				et_json_arr_end(&j);
				et_cli_puts(ctx, "\n");
			} else if (!l.n) {
				et_cli_puts(ctx, "response: []\n");
			} else {
				et_cli_puts(ctx, "response: [\n");
				for (int i = 0; i < l.n; i++) {
					et_buf *b = &ctx->out;

					dbg_indent(b, 1);
					et_buf_add(b, "MappedListener {\n");
					dbg_indent(b, 2);
					if (l.v[i].have_url) {
						et_buf_add(b, "url: Some(\n");
						dbg_indent(b, 3);
						et_buf_add(b, "Url {\n");
						dbg_indent(b, 4);
						et_buf_addf(b, "url: \"%s\",\n",
							    l.v[i].url);
						dbg_indent(b, 3);
						et_buf_add(b, "},\n");
						dbg_indent(b, 2);
						et_buf_add(b, "),\n");
					} else {
						et_buf_add(b, "url: None,\n");
					}
					dbg_indent(b, 1);
					et_buf_add(b, "},\n");
				}
				et_cli_puts(ctx, "]\n");
			}
			free(l.v);
			et_pb_free(&resp);
		}
		et_cli_fanout_end(ctx, &f);
		return rc;
	}
}

/* ============ vpn-portal ============ */

struct vpn_client {
	char name[128];
	char virtual_ip[64];
	char groups[256];
	uint64_t state;
	int have_peer_id;
	uint64_t peer_id;
	char endpoint[128];
	int have_endpoint;
	char tunnel_ip[128];
	int have_tunnel_ip;
	char client_config[4096];
	char error[256];
	int have_error;
};

struct vpn_info {
	char vpn_type[32];
	int have_vpn_type;
	char listener[128];
	int have_listener;
	struct vpn_client *clients;
	int nclients;
	int cap;
};

static int vpn_client_cb(const uint8_t *b, size_t n, void *ud)
{
	struct vpn_info *vi = ud;
	struct vpn_client c;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *p = NULL;
	size_t l = 0;

	memset(&c, 0, sizeof(c));
	et_pb_rd_init(&r, b, n);
	while (et_pb_next(&r, &f, &w, &v, &p, &l) > 0) {
		if (f == 1 && w == 2) {
			size_t k = l < sizeof(c.name) - 1 ? l : sizeof(c.name) - 1;

			memcpy(c.name, p, k);
			c.name[k] = 0;
		} else if (f == 2 && w == 2) {
			size_t k = l < sizeof(c.virtual_ip) - 1
					   ? l
					   : sizeof(c.virtual_ip) - 1;

			memcpy(c.virtual_ip, p, k);
			c.virtual_ip[k] = 0;
		} else if (f == 3 && w == 2) {
			/* groups joined with ", " */
			if (c.groups[0])
				strncat(c.groups, ", ",
					sizeof(c.groups) - strlen(c.groups) - 1);
			{
				size_t k = strlen(c.groups);

				if (l > sizeof(c.groups) - k - 1)
					l = sizeof(c.groups) - k - 1;
				memcpy(c.groups + k, p, l);
				c.groups[k + l] = 0;
			}
		} else if (f == 4 && w == 0) {
			c.state = v;
		} else if (f == 5 && w == 0) {
			c.have_peer_id = 1;
			c.peer_id = v;
		} else if (f == 6 && w == 2) {
			size_t k = l < sizeof(c.endpoint) - 1
					   ? l
					   : sizeof(c.endpoint) - 1;

			memcpy(c.endpoint, p, k);
			c.endpoint[k] = 0;
			c.have_endpoint = 1;
		} else if (f == 7 && w == 2) {
			size_t k = l < sizeof(c.tunnel_ip) - 1
					   ? l
					   : sizeof(c.tunnel_ip) - 1;

			memcpy(c.tunnel_ip, p, k);
			c.tunnel_ip[k] = 0;
			c.have_tunnel_ip = 1;
		} else if (f == 8 && w == 2) {
			size_t k = l < sizeof(c.client_config) - 1
					   ? l
					   : sizeof(c.client_config) - 1;

			memcpy(c.client_config, p, k);
			c.client_config[k] = 0;
		} else if (f == 9 && w == 2) {
			size_t k = l < sizeof(c.error) - 1 ? l
							   : sizeof(c.error) - 1;

			memcpy(c.error, p, k);
			c.error[k] = 0;
			c.have_error = 1;
		}
	}
	if (vi->nclients >= vi->cap) {
		int cap = vi->cap ? vi->cap * 2 : 4;
		struct vpn_client *q = realloc(vi->clients,
					       (size_t)cap * sizeof(*q));

		if (!q)
			return 1;
		vi->clients = q;
		vi->cap = cap;
	}
	vi->clients[vi->nclients++] = c;
	return 0;
}

static int fetch_vpn_info(struct et_cli_ctx *ctx, struct vpn_info *vi)
{
	et_pb resp;
	const uint8_t *info = NULL;
	size_t infolen = 0;

	memset(vi, 0, sizeof(*vi));
	et_pb_init(&resp);
	if (et_cli_call_instance(ctx, "VpnPortalRpc", 1, &resp)) {
		et_pb_free(&resp);
		return -1;
	}
	if (et_pb_find(resp.p, resp.len, 1, 2, NULL, &info, &infolen)) {
		(void)et_pb_get_str(info, infolen, 1, vi->vpn_type,
				    sizeof(vi->vpn_type));
		vi->have_vpn_type = 1;
		(void)et_pb_get_str(info, infolen, 5, vi->listener,
				    sizeof(vi->listener));
		vi->have_listener = 1;
		(void)et_pb_foreach_msg(info, infolen, 4, vpn_client_cb, vi);
	}
	et_pb_free(&resp);
	return 0;
}

static const char *vpn_state_name(uint64_t v)
{
	static const char *names[] = {
		"VPN_PORTAL_CLIENT_STATE_UNSPECIFIED",
		"VPN_PORTAL_CLIENT_STATE_OFFLINE",
		"VPN_PORTAL_CLIENT_STATE_CONNECTING",
		"VPN_PORTAL_CLIENT_STATE_ONLINE",
		"VPN_PORTAL_CLIENT_STATE_ERROR"
	};

	return v < 5 ? names[v] : "VPN_PORTAL_CLIENT_STATE_UNSPECIFIED";
}

static int cmd_vpn_portal_show(struct et_cli_ctx *ctx)
{
	struct vpn_info vi;

	memset(&vi, 0, sizeof(vi));
	if (fetch_vpn_info(ctx, &vi)) {
		fprintf(stderr, "easytier-cli: failed to get vpn portal info\n");
		return 1;
	}
	et_cli_printf(ctx, "portal_name: %s\n",
		      vi.have_vpn_type ? vi.vpn_type : "null");
	if (vi.have_listener && vi.listener[0])
		et_cli_printf(ctx, "listener: %s\n", vi.listener);
	for (int i = 0; i < vi.nclients; i++) {
		const struct vpn_client *c = &vi.clients[i];
		char pid[32] = "-";

		if (c->have_peer_id)
			snprintf(pid, sizeof(pid), "%llu",
				 (unsigned long long)c->peer_id);
		et_cli_printf(
			ctx,
			"\nclient: %s\nvirtual_ip: %s\nstate: %s\npeer_id: %s\nendpoint: %s\ntunnel_ip: %s\ngroups: %s\n",
			c->name, c->virtual_ip, vpn_state_name(c->state), pid,
			c->have_endpoint ? c->endpoint : "-",
			c->have_tunnel_ip ? c->tunnel_ip : "-", c->groups);
		et_cli_printf(
			ctx,
			"############### client_config_start ###############\n%s############### client_config_end ###############\n",
			c->client_config);
		if (c->have_error)
			et_cli_printf(ctx, "error: %s\n", c->error);
	}
	free(vi.clients);
	return 0;
}

static int patch_vpn_client(struct et_cli_ctx *ctx, int action,
			    const char *name, const char *virtual_ip,
			    const char *groups)
{
	et_pb patch, vpatch, client;

	et_pb_init(&patch);
	et_pb_init(&vpatch);
	et_pb_init(&client);
	if (name && *name)
		(void)et_pb_str(&client, 1, name);
	if (virtual_ip && *virtual_ip)
		(void)et_pb_str(&client, 2, virtual_ip);
	if (groups && *groups) {
		char *copy = strdup(groups);
		char *save = NULL, *tok;

		for (tok = strtok_r(copy, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save))
			(void)et_pb_str(&client, 3, tok);
		free(copy);
	}
	(void)et_pb_uint(&vpatch, 1, (uint64_t)action);
	if (action != 2)
		(void)et_pb_msg(&vpatch, 2, &client);
	(void)et_pb_msg(&patch, 15, &vpatch);
	{
		int rc = et_cli_patch_config(ctx, &patch);

		et_pb_free(&patch);
		et_pb_free(&vpatch);
		et_pb_free(&client);
		return rc;
	}
}

static int cmd_vpn_portal(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "";

	if (!sub[0])
		return cmd_vpn_portal_show(ctx);
	if (!strcmp(sub, "add-client")) {
		const char *name = NULL, *vip = NULL, *groups = NULL;

		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--virtual-ip") && i + 1 < argc)
				vip = argv[++i];
			else if (!strcmp(argv[i], "--groups") && i + 1 < argc)
				groups = argv[++i];
			else if (argv[i][0] != '-')
				name = argv[i];
		}
		if (!name || !vip) {
			fprintf(stderr,
				"error: the following required arguments were not provided:\n  <NAME>\n  --virtual-ip <VIRTUAL_IP>\n");
			return 2;
		}
		if (!strchr(vip, '/')) {
			fprintf(stderr,
				"easytier-cli: client virtual IPv4 must include its network prefix\n");
			return 1;
		}
		if (patch_vpn_client(ctx, 0, name, vip, groups)) {
			fprintf(stderr,
				"easytier-cli: failed to add vpn portal client\n");
			return 1;
		}
		return 0;
	}
	if (!strcmp(sub, "remove-client")) {
		if (argc < 2)
			return ET_ENOENT;
		if (patch_vpn_client(ctx, 1, argv[1], NULL, NULL)) {
			fprintf(stderr,
				"easytier-cli: failed to remove vpn portal client\n");
			return 1;
		}
		return 0;
	}
	if (!strcmp(sub, "clear-clients")) {
		if (patch_vpn_client(ctx, 2, NULL, NULL, NULL)) {
			fprintf(stderr,
				"easytier-cli: failed to clear vpn portal clients\n");
			return 1;
		}
		return 0;
	}
	return ET_ENOENT;
}

/* ============ proxy ============ */

struct proxy_entry {
	char src[64];
	char dst[64];
	uint64_t start_time;
	uint64_t state;
	uint64_t transport;
};

struct proxy_list {
	struct proxy_entry *v;
	int n;
	int cap;
};

static int proxy_cb(const uint8_t *b, size_t n, void *ud)
{
	struct proxy_list *l = ud;
	struct proxy_entry e;
	const uint8_t *sa = NULL;
	size_t sal = 0;

	memset(&e, 0, sizeof(e));
	if (et_pb_find(b, n, 1, 2, NULL, &sa, &sal))
		et_fmt_socket_addr(sa, sal, e.src, sizeof(e.src));
	if (et_pb_find(b, n, 2, 2, NULL, &sa, &sal))
		et_fmt_socket_addr(sa, sal, e.dst, sizeof(e.dst));
	(void)et_pb_get_u64(b, n, 3, &e.start_time);
	(void)et_pb_get_u64(b, n, 4, &e.state);
	(void)et_pb_get_u64(b, n, 5, &e.transport);
	if (l->n >= l->cap) {
		int cap = l->cap ? l->cap * 2 : 8;
		struct proxy_entry *p = realloc(l->v, (size_t)cap * sizeof(*p));

		if (!p)
			return 1;
		l->v = p;
		l->cap = cap;
	}
	l->v[l->n++] = e;
	return 0;
}

static const char *proxy_state_name(uint64_t v)
{
	static const char *names[] = { "Unknown",     "SynReceived",
				       "ConnectingDst", "Connected",
				       "Closed",      "ClosingSrc",
				       "ClosingDst" };

	return v < 7 ? names[v] : "Unknown";
}

static const char *proxy_transport_name(uint64_t v)
{
	static const char *names[] = { "TCP", "KCP", "QUIC" };

	return v < 3 ? names[v] : "TCP";
}

static int cmd_proxy(struct et_cli_ctx *ctx)
{
	static const char *const types[] = { "tcp", "kcp_src", "kcp_dst",
					     "quic_src", "quic_dst" };
	struct proxy_list l;
	et_table t;
	static const char *hdr[] = { "src", "dst", "start_time", "state",
				     "transport_type" };

	memset(&l, 0, sizeof(l));
	/* 上游对每个域取一次，取不到就当空表（ret.unwrap_or_default()） */
	et_cli_quiet = 1;
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		et_pb req, resp;

		et_pb_init(&req);
		et_pb_init(&resp);
		if (et_cli_call_domain(ctx, types[i], "TcpProxyRpc", 1, &req,
				       &resp) == 0)
			(void)et_pb_foreach_msg(resp.p, resp.len, 1, proxy_cb,
						&l);
		et_pb_free(&req);
		et_pb_free(&resp);
	}
	et_cli_quiet = 0;
	if (et_cli_want_json(&ctx->opts)) {
		et_json j;

		et_json_init(&j, &ctx->out);
		et_json_arr_begin(&j, NULL);
		for (int i = 0; i < l.n; i++) {
			char ts[32];

			time_t secs = (time_t)l.v[i].start_time;
			struct tm tmv;

			localtime_r(&secs, &tmv);
			strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
			et_json_obj_begin(&j, NULL);
			et_json_str(&j, "src", l.v[i].src);
			et_json_str(&j, "dst", l.v[i].dst);
			et_json_str(&j, "start_time", ts);
			et_json_str(&j, "state",
				    proxy_state_name(l.v[i].state));
			et_json_str(&j, "transport_type",
				    proxy_transport_name(l.v[i].transport));
			et_json_obj_end(&j);
		}
		et_json_arr_end(&j);
		et_cli_puts(ctx, "\n");
		free(l.v);
		return 0;
	}
	et_table_init(&t, 5, hdr);
	for (int i = 0; i < l.n; i++) {
		char ts[32];
		time_t secs = (time_t)l.v[i].start_time;
		struct tm tmv;
		const char *cells[5];

		localtime_r(&secs, &tmv);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
		cells[0] = l.v[i].src;
		cells[1] = l.v[i].dst;
		cells[2] = ts;
		cells[3] = proxy_state_name(l.v[i].state);
		cells[4] = proxy_transport_name(l.v[i].transport);
		et_table_add_row(&t, cells);
	}
	(void)et_table_render(&t, &ctx->opts, &ctx->out);
	et_table_free(&t);
	free(l.v);
	return 0;
}

/* ============ 入口 ============ */

int et_cli_connector(struct et_cli_ctx *ctx, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "";

	if (!sub[0] || !strcmp(sub, "list"))
		return cmd_connector_list(ctx);
	if (!strcmp(sub, "add")) {
		if (argc < 2)
			return ET_ENOENT;
		return cmd_connector_modify(ctx, argv[1], 0);
	}
	if (!strcmp(sub, "remove")) {
		if (argc < 2)
			return ET_ENOENT;
		return cmd_connector_modify(ctx, argv[1], 1);
	}
	return ET_ENOENT;
}

int et_cli_mapped_listener(struct et_cli_ctx *ctx, int argc, char **argv)
{
	return cmd_mapped_listener(ctx, argc, argv);
}

int et_cli_vpn_portal(struct et_cli_ctx *ctx, int argc, char **argv)
{
	return cmd_vpn_portal(ctx, argc, argv);
}

int et_cli_proxy(struct et_cli_ctx *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return cmd_proxy(ctx);
}