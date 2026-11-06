/*
 * etd_cli_peer.c - peer / route / node 三个子命令
 *
 * 数据全部来自 core 的自研 RPC（PeerManageRpc 1=ListPeer / 3=ListRoute /
 * 7=ShowNodeInfo），字段映射与排序规则逐条对照上游 easytier-cli 2.7.0
 * （handle_peer_list / handle_route_list / handle_node，以及
 *  easytier-proto/src/api.rs 里 PeerRoutePair 的 get_latency_ms 等）。
 */
#include "etd_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOSTNAME 128
#define PUBLIC_SERVER_PREFIX "PublicServer_"
#define NODE_LISTENERS 32

/* ===================== NodeInfo ===================== */

struct node_view {
	int have;
	uint32_t peer_id;
	char ipv4_addr[64];
	char hostname[MAX_HOSTNAME];
	char version[64];
	char inst_id[64];
	char config[4096];
	char proxy_join[512];
	char public_ip[8][64];
	int n_public;
	uint64_t nat;
	int have_nat;
	char listeners[NODE_LISTENERS][128];
	int listener_idx[NODE_LISTENERS];
	int n_listeners;
	char iface_v4[16][24];
	int n_iface_v4;
	char iface_v6[16][48];
	int n_iface_v6;
	/* NodeInfo 原始字节：-o json 时要按上游 serde 的字段全集输出 */
	et_pb raw;
};

struct str_join {
	char *out;
	size_t cap;
	size_t len;
	const char *sep;
};

static int str_join_cb(const char *s, size_t n, void *ud)
{
	struct str_join *c = ud;
	size_t sl = strlen(c->sep);
	size_t sn = strlen(s);

	(void)n;
	if (c->len && c->len + sl + sn + 1 < c->cap) {
		memcpy(c->out + c->len, c->sep, sl);
		c->len += sl;
	}
	if (c->len + sn + 1 >= c->cap)
		return 1;
	memcpy(c->out + c->len, s, sn);
	c->len += sn;
	c->out[c->len] = 0;
	return 0;
}

static void join_strings(const uint8_t *msg, size_t n, unsigned field,
			 const char *sep, char *out, size_t cap)
{
	struct str_join c;

	c.out = out;
	c.cap = cap;
	c.len = 0;
	c.sep = sep;
	out[0] = 0;
	(void)et_pb_foreach_str(msg, n, field, str_join_cb, &c);
}

/* RFC 5952：IPv6 文本格式（Rust Ipv6Addr Display 同款规则） */
static void fmt_ipv6_text(const unsigned char bytes[16], char *out, size_t cap)
{
	unsigned short g[8];
	int i, best = -1, bestlen = 0, cur = -1, curlen = 0;
	size_t o = 0;

	for (i = 0; i < 8; i++)
		g[i] = (unsigned short)((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
	for (i = 0; i < 8; i++) {
		if (g[i] == 0) {
			if (cur < 0) {
				cur = i;
				curlen = 1;
			} else {
				curlen++;
			}
		} else if (cur >= 0) {
			if (curlen > bestlen && curlen > 1) {
				best = cur;
				bestlen = curlen;
			}
			cur = -1;
			curlen = 0;
		}
	}
	if (cur >= 0 && curlen > bestlen && curlen > 1) {
		best = cur;
		bestlen = curlen;
	}
	for (i = 0; i < 8; i++) {
		if (i == best) {
			if (o + 2 < cap) {
				out[o++] = ':';
				out[o++] = ':';
			}
			i += bestlen - 1;
			continue;
		}
		if (i > 0 && o > 0 && out[o - 1] != ':' && o + 1 < cap)
			out[o++] = ':';
		o += (size_t)snprintf(out + o, cap - o, "%x", g[i]);
		if (o >= cap)
			break;
	}
	out[o < cap ? o : cap - 1] = 0;
}

static void fmt_ipv6_addr(const uint8_t *msg, size_t n, char *out, size_t cap)
{
	unsigned char bytes[16];

	for (unsigned i = 0; i < 4; i++) {
		uint64_t v = 0;

		(void)et_pb_get_u64(msg, n, i + 1, &v);
		bytes[i * 4] = (unsigned char)(v >> 24);
		bytes[i * 4 + 1] = (unsigned char)(v >> 16);
		bytes[i * 4 + 2] = (unsigned char)(v >> 8);
		bytes[i * 4 + 3] = (unsigned char)v;
	}
	fmt_ipv6_text(bytes, out, cap);
}

struct ipv4_list {
	char (*out)[24];
	int *n;
	int cap;
};

static int ipv4_list_cb(const uint8_t *b, size_t n, void *ud)
{
	struct ipv4_list *c = ud;
	uint64_t addr = 0;

	if (*c->n >= c->cap)
		return 1;
	(void)et_pb_get_u64(b, n, 1, &addr);
	et_fmt_ipv4((uint32_t)addr, c->out[*c->n], 24);
	(*c->n)++;
	return 0;
}

struct ipv6_list {
	char (*out)[48];
	int *n;
	int cap;
};

static int ipv6_list_cb(const uint8_t *b, size_t n, void *ud)
{
	struct ipv6_list *c = ud;

	if (*c->n >= c->cap)
		return 1;
	fmt_ipv6_addr(b, n, c->out[*c->n], 48);
	(*c->n)++;
	return 0;
}

struct public_ip_ctx {
	struct node_view *nv;
};

static int public_ip_cb(const char *s, size_t n, void *ud)
{
	struct public_ip_ctx *c = ud;

	(void)n;
	if (c->nv->n_public >= 8)
		return 1;
	snprintf(c->nv->public_ip[c->nv->n_public], 64, "%s", s);
	c->nv->n_public++;
	return 0;
}

struct listener_ctx {
	struct node_view *nv;
	int idx;
};

static int listener_cb(const char *s, size_t n, void *ud)
{
	struct listener_ctx *c = ud;
	size_t len = strlen(s);

	(void)n;
	if (c->nv->n_listeners < NODE_LISTENERS) {
		if (len > 127)
			len = 127;
		memcpy(c->nv->listeners[c->nv->n_listeners], s, len);
		c->nv->listeners[c->nv->n_listeners][len] = 0;
		c->nv->listener_idx[c->nv->n_listeners] = c->idx;
		c->nv->n_listeners++;
	}
	c->idx++;
	return 0;
}

static void node_view_free(struct node_view *nv)
{
	et_pb_free(&nv->raw);
	memset(nv, 0, sizeof(*nv));
}

static void node_view_parse(struct node_view *nv, const uint8_t *b, size_t n)
{
	memset(nv, 0, sizeof(*nv));
	et_pb_init(&nv->raw);
	if (b && n) {
		if (nv->raw.cap < n) {
			nv->raw.p = realloc(nv->raw.p, n);
			if (!nv->raw.p) {
				nv->raw.cap = 0;
			} else {
				nv->raw.cap = n;
			}
		}
		if (nv->raw.p) {
			memcpy(nv->raw.p, b, n);
			nv->raw.len = n;
		}
	}
	nv->have = 1;
	{
		uint64_t v = 0;

		if (!et_pb_get_u64(b, n, 1, &v))
			nv->peer_id = (uint32_t)v;
	}
	(void)et_pb_get_str(b, n, 2, nv->ipv4_addr, sizeof(nv->ipv4_addr));
	(void)et_pb_get_str(b, n, 4, nv->hostname, sizeof(nv->hostname));
	(void)et_pb_get_str(b, n, 6, nv->inst_id, sizeof(nv->inst_id));
	(void)et_pb_get_str(b, n, 8, nv->config, sizeof(nv->config));
	(void)et_pb_get_str(b, n, 9, nv->version, sizeof(nv->version));
	join_strings(b, n, 3, ", ", nv->proxy_join, sizeof(nv->proxy_join));
	{
		const uint8_t *st = NULL;
		size_t stl = 0;

		if (et_pb_find(b, n, 5, 2, NULL, &st, &stl)) {
			uint64_t nat = 0;

			if (!et_pb_get_u64(st, stl, 1, &nat)) {
				nv->nat = nat;
				nv->have_nat = 1;
			}
			{
				struct public_ip_ctx pc = { nv };

				(void)et_pb_foreach_str(st, stl, 4,
							public_ip_cb, &pc);
			}
		}
	}
	{
		struct listener_ctx lc = { nv, 0 };

		(void)et_pb_foreach_str(b, n, 7, listener_cb, &lc);
	}
	{
		const uint8_t *il = NULL;
		size_t ill = 0;

		if (et_pb_find(b, n, 11, 2, NULL, &il, &ill)) {
			struct ipv4_list c4;
			struct ipv6_list c6;

			c4.out = nv->iface_v4;
			c4.n = &nv->n_iface_v4;
			c4.cap = 16;
			c6.out = nv->iface_v6;
			c6.n = &nv->n_iface_v6;
			c6.cap = 16;
			(void)et_pb_foreach_msg(il, ill, 2, ipv4_list_cb, &c4);
			(void)et_pb_foreach_msg(il, ill, 4, ipv6_list_cb, &c6);
		}
	}
}

/* ===================== PeerInfo 统计 ===================== */

#define SCAN_LATENCY 0
#define SCAN_LOSS 1
#define SCAN_RX 2
#define SCAN_TX 3
#define SCAN_PROTOS 4

struct conn_scan {
	const char *def_id;
	int want;
	uint64_t best;
	int found;
	double loss;
	char protos[128];
};

static float decode_f32(const uint8_t *b)
{
	uint32_t u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	float f;

	memcpy(&f, &u, sizeof(f));
	return f;
}

static int scan_conn_cb(const uint8_t *b, size_t n, void *ud)
{
	struct conn_scan *s = ud;
	char conn_id[64] = "";
	const uint8_t *st = NULL;
	size_t stl = 0;

	(void)et_pb_get_str(b, n, 1, conn_id, sizeof(conn_id));
	if (s->want == SCAN_PROTOS) {
		const uint8_t *ti = NULL;
		size_t til = 0;
		char tt[32] = "";
		char needle[40];
		char hay[192];

		if (!et_pb_find(b, n, 5, 2, NULL, &ti, &til))
			return 0;
		(void)et_pb_get_str(ti, til, 1, tt, sizeof(tt));
		if (!tt[0])
			return 0;
		snprintf(needle, sizeof(needle), ",%s,", tt);
		snprintf(hay, sizeof(hay), ",%s,", s->protos);
		if (!strstr(hay, needle)) {
			size_t cur = strlen(s->protos);

			if (cur && cur + 2 < sizeof(s->protos))
				s->protos[cur++] = ',';
			snprintf(s->protos + cur, sizeof(s->protos) - cur, "%s",
				 tt);
		}
		return 0;
	}
	if (!et_pb_find(b, n, 6, 2, NULL, &st, &stl))
		return 0;
	if (s->want == SCAN_LATENCY) {
		uint64_t lat = 0;

		if (!et_pb_find(st, stl, 5, 0, &lat, NULL, NULL))
			return 0;
		if (s->def_id && s->def_id[0] && !strcmp(conn_id, s->def_id)) {
			s->best = lat;
			s->found = 2;
			return 1;
		}
		if (lat < s->best) {
			s->best = lat;
			s->found = 1;
		}
	} else if (s->want == SCAN_LOSS) {
		const uint8_t *lr = NULL;
		float lf;

		if (!et_pb_find(b, n, 7, 5, NULL, &lr, NULL) || !lr)
			return 0;
		lf = decode_f32(lr);
		if (s->def_id && s->def_id[0] && !strcmp(conn_id, s->def_id)) {
			s->loss = (double)lf;
			s->found = 2;
			return 1;
		}
		if (!s->found) {
			s->loss = (double)lf;
			s->found = 1;
		}
	} else {
		uint64_t v = 0;

		if (!et_pb_get_u64(st, stl, s->want == SCAN_RX ? 1 : 2, &v))
			s->best += v;
		s->found = 1;
	}
	return 0;
}

static void peer_def_conn(const uint8_t *pi, size_t n, char *out, size_t cap)
{
	const uint8_t *u = NULL;
	size_t ul = 0;

	out[0] = 0;
	if (pi && n && et_pb_find(pi, n, 3, 2, NULL, &u, &ul))
		et_fmt_uuid(u, ul, out, cap);
}

static void conn_scan_run(const uint8_t *pi, size_t n, struct conn_scan *s)
{
	char def[64];

	if (!pi || !n)
		return;
	peer_def_conn(pi, n, def, sizeof(def));
	s->def_id = def;
	(void)et_pb_foreach_msg(pi, n, 2, scan_conn_cb, s);
}

static int peer_latency(const uint8_t *pi, size_t n, double *out)
{
	struct conn_scan s;

	memset(&s, 0, sizeof(s));
	s.want = SCAN_LATENCY;
	s.best = UINT64_MAX;
	conn_scan_run(pi, n, &s);
	if (!s.found || s.best == UINT64_MAX)
		return 0;
	*out = (double)(uint32_t)s.best / 1000.0;
	return 1;
}

static int peer_loss(const uint8_t *pi, size_t n, double *out)
{
	struct conn_scan s;

	memset(&s, 0, sizeof(s));
	s.want = SCAN_LOSS;
	conn_scan_run(pi, n, &s);
	if (!s.found)
		return 0;
	*out = s.loss;
	return 1;
}

static unsigned long long peer_bytes(const uint8_t *pi, size_t n, int tx)
{
	struct conn_scan s;

	memset(&s, 0, sizeof(s));
	s.want = tx ? SCAN_TX : SCAN_RX;
	conn_scan_run(pi, n, &s);
	return s.best;
}

static void peer_protos(const uint8_t *pi, size_t n, char *out, size_t cap)
{
	struct conn_scan s;

	out[0] = 0;
	memset(&s, 0, sizeof(s));
	s.want = SCAN_PROTOS;
	conn_scan_run(pi, n, &s);
	snprintf(out, cap, "%s", s.protos);
}

/* ===================== 数据抓取 ===================== */

struct peer_data {
	struct node_view nv;
	et_pb peers;  /* ListPeerResponse */
	et_pb routes; /* ListRouteResponse */
	struct peer_row *prows;
	int nprows;
	struct route_row *rrows;
	int nrrows;
};

struct peer_row {
	char cidr[64];
	char ipv4[24];
	char hostname[MAX_HOSTNAME];
	char cost[24];
	char lat[24];
	char loss[24];
	char rx[32];
	char tx[32];
	char tunnel[96];
	char nat[24];
	char id[16];
	char version[64];
};

/* Join(',') 后的 proxy_cidrs -> 每行一个（上游 format_proxy_cidrs） */
static void proxy_multiline(const char *joined, char *out, size_t cap)
{
	size_t o = 0;
	const char *p;

	out[0] = 0;
	if (!joined)
		return;
	p = joined;
	while (*p && o + 2 < cap) {
		const char *comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);
		size_t a = 0, b = n;

		while (a < n && (p[a] == ' ' || p[a] == '\t'))
			a++;
		while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t'))
			b--;
		if (o && o + 1 < cap)
			out[o++] = '\n';
		for (size_t k = a; k < b && o + 1 < cap; k++)
			out[o++] = p[k];
		if (!comma)
			break;
		p = comma + 1;
	}
	out[o] = 0;
}

struct route_row {
	int skip;
	char ipv4[64];
	char hostname[MAX_HOSTNAME];
	char proxy[1024];
	char nh_ipv4[64];
	char nh_hostname[MAX_HOSTNAME];
	double nh_lat;
	long long path_len;
	long long path_latency;
	char nh_ipv4_lf[64];
	char nh_hostname_lf[MAX_HOSTNAME];
	long long path_len_lf;
	long long path_latency_lf;
	char version[64];
};

static void peer_data_free(struct peer_data *d)
{
	et_pb_free(&d->peers);
	et_pb_free(&d->routes);
	free(d->prows);
	free(d->rrows);
	memset(d, 0, sizeof(*d));
}

struct find_peer_ctx {
	uint32_t id;
	const uint8_t *msg;
	size_t len;
};

static int find_peer_cb(const uint8_t *b, size_t n, void *ud)
{
	struct find_peer_ctx *c = ud;
	uint64_t id = 0;

	if (et_pb_get_u64(b, n, 1, &id))
		return 0;
	if ((uint32_t)id != c->id)
		return 0;
	c->msg = b;
	c->len = n;
	return 1;
}

static int find_peer(const uint8_t *peers, size_t n, uint32_t peer_id,
		     const uint8_t **msg, size_t *len)
{
	struct find_peer_ctx c;

	c.id = peer_id;
	c.msg = NULL;
	c.len = 0;
	if (peers && n)
		(void)et_pb_foreach_msg(peers, n, 1, find_peer_cb, &c);
	if (!c.msg)
		return 0;
	*msg = c.msg;
	*len = c.len;
	return 1;
}

/* 在 ListRouteResponse 里按 peer_id 找 Route */
struct find_route_ctx {
	uint32_t id;
	const uint8_t *msg;
	size_t len;
};

static int find_route_cb(const uint8_t *b, size_t n, void *ud)
{
	struct find_route_ctx *c = ud;
	uint64_t id = 0;

	if (et_pb_get_u64(b, n, 1, &id))
		return 0;
	if ((uint32_t)id != c->id)
		return 0;
	c->msg = b;
	c->len = n;
	return 1;
}

static int find_route(const uint8_t *routes, size_t n, uint32_t peer_id,
		      const uint8_t **msg, size_t *len)
{
	struct find_route_ctx c;

	c.id = peer_id;
	c.msg = NULL;
	c.len = 0;
	if (routes && n)
		(void)et_pb_foreach_msg(routes, n, 1, find_route_cb, &c);
	if (!c.msg)
		return 0;
	*msg = c.msg;
	*len = c.len;
	return 1;
}

static int fetch_node_view(struct et_cli_ctx *ctx, struct node_view *nv)
{
	et_pb resp;
	int rc = -1;

	memset(nv, 0, sizeof(*nv));
	et_pb_init(&resp);
	if (et_cli_call_instance(ctx, "PeerManageRpc", 7, &resp))
		goto out;
	{
		const uint8_t *b = NULL;
		size_t n = 0;

		if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &b, &n))
			goto out;
		node_view_parse(nv, b, n);
	}
	rc = 0;
out:
	et_pb_free(&resp);
	return rc;
}

static int fetch_peer_route_data(struct et_cli_ctx *ctx, struct peer_data *d)
{
	memset(d, 0, sizeof(*d));
	et_pb_init(&d->peers);
	et_pb_init(&d->routes);
	if (fetch_node_view(ctx, &d->nv))
		return -1;
	if (et_cli_call_instance(ctx, "PeerManageRpc", 1, &d->peers))
		return -1;
	if (et_cli_call_instance(ctx, "PeerManageRpc", 3, &d->routes))
		return -1;
	return 0;
}

/* ===================== 行构造 ===================== */

static void peer_row_local(struct peer_row *r, const struct node_view *nv)
{
	char tmp[64];
	char *slash;

	memset(r, 0, sizeof(*r));
	snprintf(r->cidr, sizeof(r->cidr), "%s", nv->ipv4_addr);
	snprintf(tmp, sizeof(tmp), "%s", nv->ipv4_addr);
	slash = strchr(tmp, '/');
	if (slash)
		*slash = 0;
	snprintf(r->ipv4, sizeof(r->ipv4), "%s", tmp);
	snprintf(r->hostname, sizeof(r->hostname), "%s", nv->hostname);
	snprintf(r->cost, sizeof(r->cost), "Local");
	snprintf(r->lat, sizeof(r->lat), "-");
	snprintf(r->loss, sizeof(r->loss), "-");
	snprintf(r->rx, sizeof(r->rx), "-");
	snprintf(r->tx, sizeof(r->tx), "-");
	snprintf(r->tunnel, sizeof(r->tunnel), "-");
	snprintf(r->nat, sizeof(r->nat), "%s",
		 nv->have_nat ? et_nat_name(nv->nat) : "Unknown");
	snprintf(r->id, sizeof(r->id), "%u", nv->peer_id);
	snprintf(r->version, sizeof(r->version), "%s", nv->version);
}

static void route_row_local(struct route_row *r, const struct node_view *nv)
{
	memset(r, 0, sizeof(*r));
	snprintf(r->ipv4, sizeof(r->ipv4), "%s", nv->ipv4_addr);
	snprintf(r->hostname, sizeof(r->hostname), "%s", nv->hostname);
	snprintf(r->proxy, sizeof(r->proxy), "%s", nv->proxy_join);
	snprintf(r->nh_ipv4, sizeof(r->nh_ipv4), "-");
	snprintf(r->nh_hostname, sizeof(r->nh_hostname), "Local");
	r->nh_lat = 0.0;
	snprintf(r->nh_ipv4_lf, sizeof(r->nh_ipv4_lf), "-");
	snprintf(r->nh_hostname_lf, sizeof(r->nh_hostname_lf), "Local");
	snprintf(r->version, sizeof(r->version), "%s", nv->version);
}

static void peer_row_from_route(struct peer_row *r, const uint8_t *route,
				size_t rn, const uint8_t *pi, size_t pin)
{
	const uint8_t *ia = NULL;
	size_t ial = 0;
	uint64_t peer_id = 0, cost = 0;
	double lat = 0.0, loss = 0.0;
	char ver[64] = "";

	memset(r, 0, sizeof(*r));
	if (et_pb_find(route, rn, 2, 2, NULL, &ia, &ial))
		et_fmt_ipv4inet(ia, ial, r->cidr, sizeof(r->cidr));
	{
		const uint8_t *addr = NULL;
		size_t al = 0;
		uint64_t a = 0;

		if (ia && et_pb_find(ia, ial, 1, 2, NULL, &addr, &al))
			(void)et_pb_get_u64(addr, al, 1, &a);
		et_fmt_ipv4((uint32_t)a, r->ipv4, sizeof(r->ipv4));
	}
	(void)et_pb_get_str(route, rn, 6, r->hostname, sizeof(r->hostname));
	(void)et_pb_get_u64(route, rn, 1, &peer_id);
	(void)et_pb_get_u64(route, rn, 4, &cost);
	if ((int32_t)cost == 1)
		snprintf(r->cost, sizeof(r->cost), "p2p");
	else
		snprintf(r->cost, sizeof(r->cost), "relay(%d)", (int32_t)cost);
	if ((int32_t)cost == 1) {
		if (!peer_latency(pi, pin, &lat))
			lat = 0.0;
	} else {
		uint64_t v = 0;

		if (!et_pb_get_u64(route, rn, 14, &v))
			lat = (double)(int32_t)v;
	}
	snprintf(r->lat, sizeof(r->lat), "%.2f", lat);
	snprintf(r->loss, sizeof(r->loss), "%.1f%%",
		 (peer_loss(pi, pin, &loss) ? loss : 0.0) * 100.0);
	et_fmt_size(peer_bytes(pi, pin, 0), r->rx, sizeof(r->rx));
	et_fmt_size(peer_bytes(pi, pin, 1), r->tx, sizeof(r->tx));
	peer_protos(pi, pin, r->tunnel, sizeof(r->tunnel));
	{
		const uint8_t *st = NULL;
		size_t stl = 0;
		uint64_t nat = 0;

		if (et_pb_find(route, rn, 7, 2, NULL, &st, &stl) &&
		    !et_pb_get_u64(st, stl, 1, &nat))
			snprintf(r->nat, sizeof(r->nat), "%s",
				 et_nat_name(nat));
		else
			snprintf(r->nat, sizeof(r->nat), "Unknown");
	}
	snprintf(r->id, sizeof(r->id), "%u", (unsigned)peer_id);
	(void)et_pb_get_str(route, rn, 9, ver, sizeof(ver));
	snprintf(r->version, sizeof(r->version), "%s",
		 ver[0] ? ver : "unknown");
}

static void route_row_from(struct route_row *r, const uint8_t *route,
			   size_t rn, const struct peer_data *d)
{
	uint64_t peer_id = 0, next_hop = 0, v = 0;
	int32_t cost = 0;
	const uint8_t *nh = NULL;
	size_t nhn = 0;

	memset(r, 0, sizeof(*r));
	{
		const uint8_t *ia = NULL;
		size_t ial = 0;

		if (et_pb_find(route, rn, 2, 2, NULL, &ia, &ial))
			et_fmt_ipv4inet(ia, ial, r->ipv4, sizeof(r->ipv4));
	}
	(void)et_pb_get_str(route, rn, 6, r->hostname, sizeof(r->hostname));
	join_strings(route, rn, 5, ",", r->proxy, sizeof(r->proxy));
	(void)et_pb_get_u64(route, rn, 1, &peer_id);
	(void)et_pb_get_u64(route, rn, 3, &next_hop);
	if (!et_pb_get_u64(route, rn, 4, &v))
		cost = (int32_t)v;
	r->path_len = cost;
	if (!et_pb_get_u64(route, rn, 11, &v))
		r->path_latency = (long long)(int32_t)v;
	if (!et_pb_get_u64(route, rn, 13, &v))
		r->path_len_lf = (long long)(int32_t)v;
	if (!et_pb_get_u64(route, rn, 14, &v))
		r->path_latency_lf = (long long)(int32_t)v;

	/* next_hop 必须能在同一张路由表里找到，否则上游直接跳过该行 */
	if (!find_route(d->routes.p, d->routes.len, (uint32_t)next_hop, &nh,
			&nhn)) {
		r->skip = 1;
		return;
	}
	{
		const uint8_t *pi = NULL;
		size_t pin = 0;
		double lat = 0.0;

		(void)find_peer(d->peers.p, d->peers.len, (uint32_t)next_hop,
				&pi, &pin);
		if (!peer_latency(pi, pin, &lat))
			lat = 0.0;
		r->nh_lat = lat;
	}
	if (cost == 1) {
		snprintf(r->nh_ipv4, sizeof(r->nh_ipv4), "DIRECT");
		snprintf(r->nh_hostname, sizeof(r->nh_hostname), "DIRECT");
	} else {
		const uint8_t *ia = NULL;
		size_t ial = 0;

		if (et_pb_find(nh, nhn, 2, 2, NULL, &ia, &ial))
			et_fmt_ipv4inet(ia, ial, r->nh_ipv4,
					sizeof(r->nh_ipv4));
		(void)et_pb_get_str(nh, nhn, 6, r->nh_hostname,
				    sizeof(r->nh_hostname));
	}
	/* latency_first 版本 */
	{
		uint64_t nh_lf = 0;
		const uint8_t *nhl = NULL;
		size_t nhln = 0;

		(void)et_pb_get_u64(route, rn, 12, &nh_lf);
		if (r->path_len_lf == 1) {
			snprintf(r->nh_ipv4_lf, sizeof(r->nh_ipv4_lf), "DIRECT");
			snprintf(r->nh_hostname_lf, sizeof(r->nh_hostname_lf),
				 "DIRECT");
		} else if (find_route(d->routes.p, d->routes.len,
				      (uint32_t)nh_lf, &nhl, &nhln)) {
			const uint8_t *ia = NULL;
			size_t ial = 0;

			if (et_pb_find(nhl, nhln, 2, 2, NULL, &ia, &ial))
				et_fmt_ipv4inet(ia, ial, r->nh_ipv4_lf,
						sizeof(r->nh_ipv4_lf));
			(void)et_pb_get_str(nhl, nhln, 6, r->nh_hostname_lf,
					    sizeof(r->nh_hostname_lf));
		}
	}
	{
		char ver[64] = "";

		(void)et_pb_get_str(route, rn, 9, ver, sizeof(ver));
		snprintf(r->version, sizeof(r->version), "%s",
			 ver[0] ? ver : "unknown");
	}
}

static int peer_row_cmp(const void *p1, const void *p2)
{
	const struct peer_row *a = p1, *b = p2;
	int a_local = !strcmp(a->cost, "Local");
	int b_local = !strcmp(b->cost, "Local");
	int a_pub, b_pub;
	uint32_t a_ip = 0, b_ip = 0;
	unsigned o1, o2, o3, o4;

	if (a_local != b_local)
		return a_local ? -1 : 1;
	a_pub = !strncmp(a->hostname, PUBLIC_SERVER_PREFIX,
			 strlen(PUBLIC_SERVER_PREFIX));
	b_pub = !strncmp(b->hostname, PUBLIC_SERVER_PREFIX,
			 strlen(PUBLIC_SERVER_PREFIX));
	if (a_pub != b_pub)
		return a_pub ? -1 : 1;
	if (sscanf(a->ipv4, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) == 4)
		a_ip = (o1 << 24) | (o2 << 16) | (o3 << 8) | o4;
	if (sscanf(b->ipv4, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) == 4)
		b_ip = (o1 << 24) | (o2 << 16) | (o3 << 8) | o4;
	if (a_ip != b_ip)
		return a_ip < b_ip ? -1 : 1;
	return strcmp(a->hostname, b->hostname);
}

static int route_row_cmp(const void *p1, const void *p2)
{
	const struct route_row *a = p1, *b = p2;
	uint32_t a_ip = 0, b_ip = 0;
	unsigned o1, o2, o3, o4;

	if (sscanf(a->ipv4, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) == 4)
		a_ip = (o1 << 24) | (o2 << 16) | (o3 << 8) | o4;
	if (sscanf(b->ipv4, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) == 4)
		b_ip = (o1 << 24) | (o2 << 16) | (o3 << 8) | o4;
	if (a_ip != b_ip)
		return a_ip < b_ip ? -1 : 1;
	return 0;
}

static int build_peer_rows(struct peer_data *d)
{
	int nroute = 0;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t l = 0;

	et_pb_rd_init(&r, d->routes.p, d->routes.len);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0)
		if (f == 1 && w == 2)
			nroute++;
	d->prows = calloc((size_t)nroute + 1, sizeof(*d->prows));
	if (!d->prows)
		return -1;
	peer_row_local(&d->prows[0], &d->nv);
	d->nprows = 1;
	et_pb_rd_init(&r, d->routes.p, d->routes.len);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
		const uint8_t *pi = NULL;
		size_t pin = 0;
		uint64_t peer_id = 0;

		if (f != 1 || w != 2)
			continue;
		(void)et_pb_get_u64(b, l, 1, &peer_id);
		(void)find_peer(d->peers.p, d->peers.len, (uint32_t)peer_id,
				&pi, &pin);
		peer_row_from_route(&d->prows[d->nprows], b, l, pi, pin);
		d->nprows++;
	}
	qsort(d->prows, (size_t)d->nprows, sizeof(*d->prows), peer_row_cmp);
	return 0;
}

static int build_route_rows(struct peer_data *d)
{
	int nroute = 0;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t l = 0;

	et_pb_rd_init(&r, d->routes.p, d->routes.len);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0)
		if (f == 1 && w == 2)
			nroute++;
	d->rrows = calloc((size_t)nroute + 1, sizeof(*d->rrows));
	if (!d->rrows)
		return -1;
	route_row_local(&d->rrows[0], &d->nv);
	d->nrrows = 1;
	et_pb_rd_init(&r, d->routes.p, d->routes.len);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
		struct route_row row;

		if (f != 1 || w != 2)
			continue;
		route_row_from(&row, b, l, d);
		if (row.skip)
			continue; /* next_hop 找不到：上游跳过 */
		d->rrows[d->nrrows] = row;
		d->nrrows++;
	}
	qsort(d->rrows + 1, (size_t)(d->nrrows - 1), sizeof(*d->rrows),
	      route_row_cmp);
	return 0;
}

/* ===================== 渲染 ===================== */

static void emit_peer_table(struct et_cli_ctx *ctx, const struct peer_data *d)
{
	static const char *hdr[] = { "ipv4", "hostname", "cost", "lat(ms)",
				     "loss", "rx",	   "tx",   "tunnel",
				     "NAT",  "version" };
	et_table t;

	et_table_init(&t, 10, hdr);
	for (int i = 0; i < d->nprows; i++) {
		const char *cells[10] = { d->prows[i].cidr,
					  d->prows[i].hostname,
					  d->prows[i].cost,
					  d->prows[i].lat,
					  d->prows[i].loss,
					  d->prows[i].rx,
					  d->prows[i].tx,
					  d->prows[i].tunnel,
					  d->prows[i].nat,
					  d->prows[i].version };

		et_table_add_row(&t, cells);
	}
	(void)et_table_render(&t, &ctx->opts, &ctx->out);
	et_table_free(&t);
}

static void emit_peer_json(et_json *j, const struct peer_data *d)
{
	et_json_arr_begin(j, NULL);
	for (int i = 0; i < d->nprows; i++) {
		et_json_obj_begin(j, NULL);
		et_json_str(j, "cidr", d->prows[i].cidr);
		et_json_str(j, "ipv4", d->prows[i].ipv4);
		et_json_str(j, "hostname", d->prows[i].hostname);
		et_json_str(j, "cost", d->prows[i].cost);
		et_json_str(j, "lat_ms", d->prows[i].lat);
		et_json_str(j, "loss_rate", d->prows[i].loss);
		et_json_str(j, "rx_bytes", d->prows[i].rx);
		et_json_str(j, "tx_bytes", d->prows[i].tx);
		et_json_str(j, "tunnel_proto", d->prows[i].tunnel);
		et_json_str(j, "nat_type", d->prows[i].nat);
		et_json_str(j, "id", d->prows[i].id);
		et_json_str(j, "version", d->prows[i].version);
		et_json_obj_end(j);
	}
	et_json_arr_end(j);
}

static void emit_route_table(struct et_cli_ctx *ctx, const struct peer_data *d)
{
	static const char *hdr[] = { "ipv4",
				     "hostname",
				     "proxy_cidrs",
				     "next_hop_ipv4",
				     "next_hop_hostname",
				     "next_hop_lat",
				     "path_len",
				     "path_latency",
				     "next_hop_ipv4_lat_first",
				     "next_hop_hostname_lat_first",
				     "path_len_lat_first",
				     "path_latency_lat_first",
				     "version" };
	et_table t;

	et_table_init(&t, 13, hdr);
	for (int i = 0; i < d->nrrows; i++) {
		char proxy[1024];
		char nh_lat[32];
		char path_len[24], path_lat[24], len_lf[24], lat_lf[24];
		const char *cells[13];

		proxy_multiline(d->rrows[i].proxy, proxy, sizeof(proxy));
		et_fmt_f64(d->rrows[i].nh_lat, nh_lat, sizeof(nh_lat));
		snprintf(path_len, sizeof(path_len), "%lld",
			 d->rrows[i].path_len);
		snprintf(path_lat, sizeof(path_lat), "%lld",
			 d->rrows[i].path_latency);
		snprintf(len_lf, sizeof(len_lf), "%lld",
			 d->rrows[i].path_len_lf);
		snprintf(lat_lf, sizeof(lat_lf), "%lld",
			 d->rrows[i].path_latency_lf);
		cells[0] = d->rrows[i].ipv4;
		cells[1] = d->rrows[i].hostname;
		cells[2] = proxy;
		cells[3] = d->rrows[i].nh_ipv4;
		cells[4] = d->rrows[i].nh_hostname;
		cells[5] = nh_lat;
		cells[6] = path_len;
		cells[7] = path_lat;
		cells[8] = d->rrows[i].nh_ipv4_lf;
		cells[9] = d->rrows[i].nh_hostname_lf;
		cells[10] = len_lf;
		cells[11] = lat_lf;
		cells[12] = d->rrows[i].version;
		et_table_add_row(&t, cells);
	}
	(void)et_table_render(&t, &ctx->opts, &ctx->out);
	et_table_free(&t);
}

static void emit_route_json(et_json *j, const struct peer_data *d)
{
	et_json_arr_begin(j, NULL);
	for (int i = 0; i < d->nrrows; i++) {
		et_json_obj_begin(j, NULL);
		et_json_str(j, "ipv4", d->rrows[i].ipv4);
		et_json_str(j, "hostname", d->rrows[i].hostname);
		et_json_str(j, "proxy_cidrs", d->rrows[i].proxy);
		et_json_str(j, "next_hop_ipv4", d->rrows[i].nh_ipv4);
		et_json_str(j, "next_hop_hostname", d->rrows[i].nh_hostname);
		et_json_double(j, "next_hop_lat", d->rrows[i].nh_lat);
		et_json_int(j, "path_len", d->rrows[i].path_len);
		et_json_int(j, "path_latency", d->rrows[i].path_latency);
		et_json_str(j, "next_hop_ipv4_lat_first",
			    d->rrows[i].nh_ipv4_lf);
		et_json_str(j, "next_hop_hostname_lat_first",
			    d->rrows[i].nh_hostname_lf);
		et_json_int(j, "path_len_lat_first", d->rrows[i].path_len_lf);
		et_json_int(j, "path_latency_lat_first",
			    d->rrows[i].path_latency_lf);
		et_json_str(j, "version", d->rrows[i].version);
		et_json_obj_end(j);
	}
	et_json_arr_end(j);
}

/* 多实例时统一包一层；单实例直接输出数组/对象 */
struct fanout {
	struct et_cli_target *tg;
	int nt;
	int json;
	int index;
	et_json j;
};

static int fanout_begin(struct et_cli_ctx *ctx, struct fanout *f)
{
	memset(f, 0, sizeof(*f));
	f->nt = et_cli_resolve_targets(ctx, &f->tg);
	if (f->nt < 0)
		return -1;
	f->json = et_cli_want_json(&ctx->opts);
	if (f->json) {
		et_json_init(&f->j, &ctx->out);
		if (f->nt > 1)
			et_json_arr_begin(&f->j, NULL);
	}
	return 0;
}

static void fanout_end(struct et_cli_ctx *ctx, struct fanout *f)
{
	if (f->json && f->nt > 1)
		et_json_arr_end(&f->j);
	if (ctx->out.len && ctx->out.p && ctx->out.p[ctx->out.len - 1] != '\n')
		et_cli_puts(ctx, "\n");
	et_cli_free_targets(f->tg, f->nt);
	ctx->target = NULL;
}

static void fanout_header(struct et_cli_ctx *ctx, struct fanout *f)
{
	if (f->json || f->nt <= 1)
		return;
	if (f->index)
		et_cli_puts(ctx, "\n");
	{
		char label[192];
		const struct et_cli_target *t = &f->tg[f->index];

		if (t->name[0] && t->id_str[0])
			snprintf(label, sizeof(label), "%s (%s)", t->name,
				 t->id_str);
		else if (t->name[0])
			snprintf(label, sizeof(label), "%s", t->name);
		else if (t->id_str[0])
			snprintf(label, sizeof(label), "%s", t->id_str);
		else
			snprintf(label, sizeof(label), "selected instance");
		et_cli_printf(ctx, "== %s ==\n", label);
	}
}

/* 多实例 JSON 包裹 */
static void fanout_json_open(struct fanout *f)
{
	if (!f->json || f->nt <= 1)
		return;
	et_json_obj_begin(&f->j, NULL);
	et_json_str(&f->j, "instance_id", f->tg[f->index].id_str);
	et_json_str(&f->j, "instance_name", f->tg[f->index].name);
}

static void fanout_json_close(struct fanout *f)
{
	if (!f->json || f->nt <= 1)
		return;
	et_json_obj_end(&f->j);
}

/* ===================== peer ===================== */

static int run_peer_list(struct et_cli_ctx *ctx)
{
	struct fanout f;
	int rc = 0;

	if (fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		struct peer_data d;

		ctx->target = &f.tg[f.index];
		if (fetch_peer_route_data(ctx, &d) || build_peer_rows(&d)) {
			fprintf(stderr,
				"easytier-cli: failed to get peer info\n");
			peer_data_free(&d);
			rc = 1;
			break;
		}
		fanout_header(ctx, &f);
		if (f.json) {
			fanout_json_open(&f);
			if (f.nt > 1) {
				et_json_arr_begin(&f.j, "result");
				emit_peer_json(&f.j, &d);
				et_json_arr_end(&f.j);
			} else {
				emit_peer_json(&f.j, &d);
			}
			fanout_json_close(&f);
		} else {
			emit_peer_table(ctx, &d);
		}
		peer_data_free(&d);
	}
	fanout_end(ctx, &f);
	return rc;
}

/* ===================== peer ipv6 / 外网列表 ===================== */

static uint32_t ipv6_local_id; /* 排序用：本机行永远排最前 */

/* common.Ipv6Inet -> "addr/len"；缺失时 "-"（上游 fmt_ipv6_inet） */
static void fmt_ipv6inet_or_dash(const uint8_t *b, size_t n, char *out,
				 size_t cap)
{
	const uint8_t *addr = NULL;
	size_t addrlen = 0;
	uint64_t plen = 0;
	char ip[64] = "";

	if (!b || !n || !et_pb_find(b, n, 1, 2, NULL, &addr, &addrlen)) {
		snprintf(out, cap, "-");
		return;
	}
	fmt_ipv6_addr(addr, addrlen, ip, sizeof(ip));
	(void)et_pb_get_u64(b, n, 2, &plen);
	snprintf(out, cap, "%s/%llu", ip, (unsigned long long)plen);
}

static void fmt_local_time_str(uint64_t secs, char *out, size_t cap)
{
	time_t t = (time_t)secs;
	struct tm tmv;
	char base[32];

	localtime_r(&t, &tmv);
	strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tmv);
	snprintf(out, cap, "%s", base);
}

static uint32_t pb_get_u32(const uint8_t *b, size_t n, unsigned field)
{
	uint64_t v = 0;

	(void)et_pb_get_u64(b, n, field, &v);
	return (uint32_t)v;
}

struct ipv6_row {
	uint32_t peer_id;
	char hostname[MAX_HOSTNAME];
	char inst_id[64];
	char ipv4[64];
	char pub6[64];
	char prefix[64];
};

struct lease_row {
	uint32_t peer_id;
	char inst_id[64];
	char addr[64];
	char valid_until[32];
	int reused;
};

static int ipv6_row_cmp(const void *p1, const void *p2)
{
	const struct ipv6_row *a = p1, *b = p2;
	int an = a->peer_id != ipv6_local_id;
	int bn = b->peer_id != ipv6_local_id;

	if (an != bn)
		return an - bn;
	if (a->peer_id != b->peer_id)
		return a->peer_id < b->peer_id ? -1 : 1;
	return strcmp(a->inst_id, b->inst_id);
}

static int lease_row_cmp(const void *p1, const void *p2)
{
	const struct lease_row *a = p1, *b = p2;
	int c;

	if (a->peer_id != b->peer_id)
		return a->peer_id < b->peer_id ? -1 : 1;
	c = strcmp(a->inst_id, b->inst_id);
	if (c)
		return c;
	return strcmp(a->addr, b->addr);
}

static void ipv6_row_push(struct ipv6_row **rows, int *n, int *cap,
			  const struct ipv6_row *r)
{
	if (*n >= *cap) {
		int nc = *cap ? *cap * 2 : 8;
		struct ipv6_row *p = realloc(*rows, (size_t)nc * sizeof(*p));

		if (!p)
			return;
		*rows = p;
		*cap = nc;
	}
	(*rows)[(*n)++] = *r;
}

static void lease_row_push(struct lease_row **rows, int *n, int *cap,
			   const struct lease_row *r)
{
	if (*n >= *cap) {
		int nc = *cap ? *cap * 2 : 4;
		struct lease_row *p = realloc(*rows, (size_t)nc * sizeof(*p));

		if (!p)
			return;
		*rows = p;
		*cap = nc;
	}
	(*rows)[(*n)++] = *r;
}

static int run_peer_ipv6(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb node, routes, prov;
		struct ipv6_row *rows = NULL;
		int nrows = 0, cap = 0;
		struct lease_row *leases = NULL;
		int nleases = 0, lcap = 0;
		char provider_prefix[64] = "";
		int have_provider = 0;

		ctx->target = &f.tg[f.index];
		et_pb_init(&node);
		et_pb_init(&routes);
		et_pb_init(&prov);
		if (et_cli_call_instance(ctx, "PeerManageRpc", 7, &node) ||
		    et_cli_call_instance(ctx, "PeerManageRpc", 3, &routes) ||
		    et_cli_call_instance(ctx, "PeerManageRpc", 2, &prov)) {
			fprintf(stderr,
				"easytier-cli: failed to get peer ipv6 info\n");
			et_pb_free(&node);
			et_pb_free(&routes);
			et_pb_free(&prov);
			rc = 1;
			break;
		}
		/* 本机行 */
		{
			const uint8_t *ni = NULL, *q = NULL;
			size_t nil = 0, ql = 0;
			struct ipv6_row r;

			memset(&r, 0, sizeof(r));
			if (et_pb_find(node.p, node.len, 1, 2, NULL, &ni, &nil)) {
				r.peer_id = pb_get_u32(ni, nil, 1);
				ipv6_local_id = r.peer_id;
				(void)et_pb_get_str(ni, nil, 4, r.hostname,
						    sizeof(r.hostname));
				(void)et_pb_get_str(ni, nil, 6, r.inst_id,
						    sizeof(r.inst_id));
				(void)et_pb_get_str(ni, nil, 2, r.ipv4,
						    sizeof(r.ipv4));
				if (et_pb_find(ni, nil, 12, 2, NULL, &q, &ql))
					fmt_ipv6inet_or_dash(q, ql, r.pub6,
							     sizeof(r.pub6));
				else
					snprintf(r.pub6, sizeof(r.pub6), "-");
				if (et_pb_find(ni, nil, 13, 2, NULL, &q, &ql))
					fmt_ipv6inet_or_dash(q, ql, r.prefix,
							     sizeof(r.prefix));
				else
					snprintf(r.prefix, sizeof(r.prefix),
						 "-");
				ipv6_row_push(&rows, &nrows, &cap, &r);
			}
		}
		/* 路由行 */
		{
			et_pb_rd rd;
			unsigned fld, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&rd, routes.p, routes.len);
			while (et_pb_next(&rd, &fld, &w, &v, &b, &l) > 0) {
				struct ipv6_row r;
				const uint8_t *ia = NULL, *q = NULL;
				size_t ial = 0, ql = 0;

				if (fld != 1 || w != 2)
					continue;
				memset(&r, 0, sizeof(r));
				r.peer_id = pb_get_u32(b, l, 1);
				(void)et_pb_get_str(b, l, 6, r.hostname,
						    sizeof(r.hostname));
				(void)et_pb_get_str(b, l, 8, r.inst_id,
						    sizeof(r.inst_id));
				if (et_pb_find(b, l, 2, 2, NULL, &ia, &ial))
					et_fmt_ipv4inet(ia, ial, r.ipv4,
							sizeof(r.ipv4));
				else
					snprintf(r.ipv4, sizeof(r.ipv4), "-");
				if (et_pb_find(b, l, 16, 2, NULL, &q, &ql))
					fmt_ipv6inet_or_dash(q, ql, r.pub6,
							     sizeof(r.pub6));
				else
					snprintf(r.pub6, sizeof(r.pub6), "-");
				if (et_pb_find(b, l, 17, 2, NULL, &q, &ql))
					fmt_ipv6inet_or_dash(q, ql, r.prefix,
							     sizeof(r.prefix));
				else
					snprintf(r.prefix, sizeof(r.prefix),
						 "-");
				ipv6_row_push(&rows, &nrows, &cap, &r);
			}
		}
		/* 本机作为 provider 的租约 */
		{
			const uint8_t *pfx = NULL;
			size_t pfxl = 0;

			if (et_pb_find(prov.p, prov.len, 1, 2, NULL, &pfx,
				       &pfxl)) {
				fmt_ipv6inet_or_dash(pfx, pfxl, provider_prefix,
						     sizeof(provider_prefix));
				have_provider =
					strcmp(provider_prefix, "-") != 0;
			}
			{
				et_pb_rd rd;
				unsigned fld, w;
				uint64_t v;
				const uint8_t *b = NULL;
				size_t l = 0;

				et_pb_rd_init(&rd, prov.p, prov.len);
				while (et_pb_next(&rd, &fld, &w, &v, &b, &l) > 0) {
					struct lease_row lr;
					const uint8_t *la = NULL;
					size_t lal = 0;
					uint64_t ts = 0, reused = 0;

					if (fld != 2 || w != 2)
						continue;
					memset(&lr, 0, sizeof(lr));
					lr.peer_id = pb_get_u32(b, l, 1);
					(void)et_pb_get_str(b, l, 2,
							    lr.inst_id,
							    sizeof(lr.inst_id));
					if (et_pb_find(b, l, 3, 2, NULL, &la,
						       &lal))
						fmt_ipv6inet_or_dash(
							la, lal, lr.addr,
							sizeof(lr.addr));
					else
						snprintf(lr.addr,
							 sizeof(lr.addr), "-");
					(void)et_pb_get_u64(b, l, 4, &ts);
					fmt_local_time_str(ts, lr.valid_until,
							   sizeof(lr.valid_until));
					(void)et_pb_get_u64(b, l, 5, &reused);
					lr.reused = reused != 0;
					lease_row_push(&leases, &nleases,
						       &lcap, &lr);
				}
			}
		}
		if (nrows > 1)
			qsort(rows, (size_t)nrows, sizeof(*rows), ipv6_row_cmp);
		if (nleases > 1)
			qsort(leases, (size_t)nleases, sizeof(*leases),
			      lease_row_cmp);
		et_cli_fanout_header(ctx, &f);
		if (f.json) {
			et_cli_fanout_json_open(&f);
			et_json_obj_begin(&f.j, f.nt > 1 ? "result" : NULL);
			et_json_arr_begin(&f.j, "nodes");
			for (int i = 0; i < nrows; i++) {
				et_json_obj_begin(&f.j, NULL);
				et_json_uint(&f.j, "peer_id", rows[i].peer_id);
				et_json_str(&f.j, "hostname", rows[i].hostname);
				et_json_str(&f.j, "inst_id", rows[i].inst_id);
				et_json_str(&f.j, "ipv4", rows[i].ipv4);
				et_json_str(&f.j, "public_ipv6_addr",
					    rows[i].pub6);
				et_json_str(&f.j, "provider_prefix",
					    rows[i].prefix);
				et_json_obj_end(&f.j);
			}
			et_json_arr_end(&f.j);
			if (have_provider) {
				et_json_obj_begin(&f.j, "local_provider");
				et_json_str(&f.j, "provider_prefix",
					    provider_prefix);
				et_json_arr_begin(&f.j, "leases");
				for (int i = 0; i < nleases; i++) {
					et_json_obj_begin(&f.j, NULL);
					et_json_uint(&f.j, "peer_id",
						     leases[i].peer_id);
					et_json_str(&f.j, "inst_id",
						    leases[i].inst_id);
					et_json_str(&f.j, "leased_addr",
						    leases[i].addr);
					et_json_str(&f.j, "valid_until",
						    leases[i].valid_until);
					et_json_bool(&f.j, "reused",
						     leases[i].reused);
					et_json_obj_end(&f.j);
				}
				et_json_arr_end(&f.j);
				et_json_obj_end(&f.j);
			} else {
				et_json_null(&f.j, "local_provider");
			}
			et_json_obj_end(&f.j);
			et_cli_fanout_json_close(&f);
		} else {
			static const char *hdr[] = {
				"peer_id", "hostname", "inst_id", "ipv4",
				"public_ipv6_addr", "provider_prefix"
			};
			et_table t;

			et_table_init(&t, 6, hdr);
			for (int i = 0; i < nrows; i++) {
				char id[16];
				const char *cells[6];

				snprintf(id, sizeof(id), "%u", rows[i].peer_id);
				cells[0] = id;
				cells[1] = rows[i].hostname;
				cells[2] = rows[i].inst_id;
				cells[3] = rows[i].ipv4;
				cells[4] = rows[i].pub6;
				cells[5] = rows[i].prefix;
				et_table_add_row(&t, cells);
			}
			(void)et_table_render(&t, &ctx->opts, &ctx->out);
			et_table_free(&t);
			if (have_provider) {
				et_cli_puts(ctx, "\n");
				et_cli_printf(ctx,
					      "Local provider prefix: %s\n",
					      provider_prefix);
				if (!nleases) {
					et_cli_puts(
						ctx,
						"No active provider leases\n");
				} else {
					static const char *hdr2[] = {
						"peer_id", "inst_id",
						"leased_addr", "valid_until",
						"reused"
					};
					et_table t2;

					et_table_init(&t2, 5, hdr2);
					for (int i = 0; i < nleases; i++) {
						char id[16];
						const char *c2[5];

						snprintf(id, sizeof(id), "%u",
							 leases[i].peer_id);
						c2[0] = id;
						c2[1] = leases[i].inst_id;
						c2[2] = leases[i].addr;
						c2[3] = leases[i].valid_until;
						c2[4] = leases[i].reused
								? "true"
								: "false";
						et_table_add_row(&t2, c2);
					}
					(void)et_table_render(&t2,
							      &ctx->opts,
							      &ctx->out);
					et_table_free(&t2);
				}
			}
		}
		free(rows);
		free(leases);
		et_pb_free(&node);
		et_pb_free(&routes);
		et_pb_free(&prov);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

static const char *trusted_source_name(uint64_t v)
{
	static const char *names[] = {
		"TRUSTED_KEY_SOURCE_PB_UNSPECIFIED",
		"TRUSTED_KEY_SOURCE_PB_OSPF_NODE",
		"TRUSTED_KEY_SOURCE_PB_OSPF_CREDENTIAL"
	};

	return v < 3 ? names[v] : names[0];
}

static void tunnel_remote_addr(const uint8_t *conn, size_t n, char *out,
			       size_t cap)
{
	const uint8_t *ti = NULL, *url = NULL;
	size_t til = 0, urll = 0;

	out[0] = 0;
	if (!et_pb_find(conn, n, 5, 2, NULL, &ti, &til))
		return;
	if (!et_pb_find(ti, til, 4, 2, NULL, &url, &urll) &&
	    !et_pb_find(ti, til, 3, 2, NULL, &url, &urll))
		return;
	(void)et_pb_get_str(url, urll, 1, out, cap);
}

/* peer list-foreign：PeerManageRpc/5，map<string, ForeignNetworkEntryPb> */
static int run_peer_list_foreign(struct et_cli_ctx *ctx, int trusted_keys)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb req, resp;

		ctx->target = &f.tg[f.index];
		et_pb_init(&req);
		et_pb_init(&resp);
		(void)et_cli_req_instance(ctx, &req);
		if (trusted_keys)
			(void)et_pb_uint(&req, 2, 1);
		if (et_cli_call(ctx, "PeerManageRpc", 5, &req, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to list foreign networks\n");
			et_pb_free(&req);
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		et_cli_fanout_json_open(&f);
		if (f.json)
			et_json_obj_begin(&f.j, f.nt > 1 ? "result" : NULL);
		{
			et_pb_rd rd;
			unsigned fld, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;
			int idx = 0;

			et_pb_rd_init(&rd, resp.p, resp.len);
			while (et_pb_next(&rd, &fld, &w, &v, &b, &l) > 0) {
				const uint8_t *key = NULL, *val = NULL;
				size_t keyl = 0, vall = 0;
				char name[128] = "";
				int nkeys = 0;
				et_pb_rd r2;
				unsigned f2, w2;
				uint64_t v2;
				const uint8_t *p2 = NULL;
				size_t l2 = 0;

				if (fld != 1 || w != 2)
					continue;
				if (!et_pb_find(b, l, 1, 2, NULL, &key, &keyl))
					continue;
				(void)et_pb_find(b, l, 2, 2, NULL, &val, &vall);
				if (keyl >= sizeof(name))
					keyl = sizeof(name) - 1;
				memcpy(name, key, keyl);
				name[keyl] = 0;
				idx++;
				/* 先数一下 peer/key 数量，JSON 里好开数组 */
				et_pb_rd_init(&r2, val, vall);
				while (et_pb_next(&r2, &f2, &w2, &v2, &p2,
						  &l2) > 0) {
					if (f2 == 4 && w2 == 2)
						nkeys++;
				}
				if (f.json) {
					et_json_arr_begin(&f.j, name);
				} else {
					et_cli_printf(ctx,
						      "%d Network Name: %s\n",
						      idx, name);
				}
				et_pb_rd_init(&r2, val, vall);
				while (et_pb_next(&r2, &f2, &w2, &v2, &p2,
						  &l2) > 0) {
					uint64_t pid = 0;
					int nconns = 0;
					et_buf conns;

					if (f2 != 1 || w2 != 2)
						continue;
					(void)et_pb_get_u64(p2, l2, 1, &pid);
					et_buf_init(&conns);
					{
						et_pb_rd r3;
						unsigned f3, w3;
						uint64_t v3;
						const uint8_t *p3 = NULL;
						size_t l3 = 0;

						et_pb_rd_init(&r3, p2, l2);
						while (et_pb_next(&r3, &f3, &w3,
								  &v3, &p3,
								  &l3) > 0) {
							char addr[256] = "";
							uint64_t rx = 0, tx = 0,
								 lat = 0;
							et_pb_rd r4;
							unsigned f4, w4;
							uint64_t v4;
							const uint8_t *p4 = NULL;
							size_t l4 = 0;

							if (f3 != 2 || w3 != 2)
								continue;
							tunnel_remote_addr(
								p3, l3, addr,
								sizeof(addr));
							et_pb_rd_init(&r4, p3, l3);
							while (et_pb_next(
								       &r4, &f4,
								       &w4, &v4,
								       &p4,
								       &l4) >
							       0) {
								if (f4 != 6 ||
								    w4 != 2)
									continue;
								(void)et_pb_get_u64(
									p4, l4, 1,
									&rx);
								(void)et_pb_get_u64(
									p4, l4, 2,
									&tx);
								(void)et_pb_get_u64(
									p4, l4, 5,
									&lat);
							}
							et_buf_addf(
								&conns,
								"%sremote_addr: %s, rx_bytes: %llu, tx_bytes: %llu, latency_us: %llu",
								nconns ? "; "
								       : "",
								addr,
								(unsigned long long)
									rx,
								(unsigned long long)
									tx,
								(unsigned long long)
									lat);
							nconns++;
						}
					}
					if (f.json) {
						et_json_obj_begin(&f.j, NULL);
						et_json_uint(&f.j, "peer_id",
							     pid);
						et_json_str(&f.j, "conns",
							    conns.p ? conns.p
								    : "");
						et_json_obj_end(&f.j);
					} else {
						et_cli_printf(
							ctx,
							"  peer_id: %llu, peer_conn_count: %d, conns: [ %s ]\n",
							(unsigned long long)pid,
							nconns,
							conns.p ? conns.p : "");
					}
					et_buf_free(&conns);
				}
				if (trusted_keys && nkeys) {
					if (!f.json)
						et_cli_puts(ctx,
							    "  trusted_keys:\n");
					et_pb_rd_init(&r2, val, vall);
					while (et_pb_next(&r2, &f2, &w2, &v2,
							  &p2, &l2) > 0) {
						const uint8_t *pk = NULL;
						size_t pkl = 0;
						uint64_t src = 0, exp = 0;
						int have_exp;
						char b64[512] = "";
						char expbuf[32] = "-";

						if (f2 != 4 || w2 != 2)
							continue;
						if (et_pb_find(p2, l2, 1, 2,
							       NULL, &pk,
							       &pkl))
							et_b64_encode(
								pk, pkl, b64,
								sizeof(b64));
						(void)et_pb_get_u64(p2, l2, 2,
								    &src);
						have_exp = !et_pb_get_u64(
							p2, l2, 3, &exp);
						if (have_exp)
							snprintf(
								expbuf,
								sizeof(expbuf),
								"%lld",
								(long long)(
									int64_t)
									exp);
						if (!f.json)
							et_cli_printf(
								ctx,
								"    source: %s, expiry_unix: %s, pubkey: %s\n",
								trusted_source_name(
									src),
								expbuf, b64);
					}
				}
				if (f.json)
					et_json_arr_end(&f.j);
			}
		}
		if (f.json)
			et_json_obj_end(&f.j);
		et_cli_fanout_json_close(&f);
		et_pb_free(&req);
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

/* peer list-global-foreign：PeerManageRpc/6，map<uint32, ForeignNetworks> */
static int run_peer_list_global_foreign(struct et_cli_ctx *ctx)
{
	struct et_cli_fanout f;
	int rc = 0;

	if (et_cli_fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		et_pb req, resp;

		ctx->target = &f.tg[f.index];
		et_pb_init(&req);
		et_pb_init(&resp);
		(void)et_cli_req_instance(ctx, &req);
		if (et_cli_call(ctx, "PeerManageRpc", 6, &req, &resp)) {
			fprintf(stderr,
				"easytier-cli: failed to list global foreign networks\n");
			et_pb_free(&req);
			et_pb_free(&resp);
			rc = 1;
			break;
		}
		et_cli_fanout_header(ctx, &f);
		et_cli_fanout_json_open(&f);
		if (f.json)
			et_json_obj_begin(&f.j, f.nt > 1 ? "result" : NULL);
		{
			et_pb_rd rd;
			unsigned fld, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&rd, resp.p, resp.len);
			while (et_pb_next(&rd, &fld, &w, &v, &b, &l) > 0) {
				const uint8_t *val = NULL;
				size_t vall = 0;
				uint64_t peer_id = 0;
				char keybuf[32];

				if (fld != 1 || w != 2)
					continue;
				/* map<uint32, V>：key 是 varint（wire 0） */
				(void)et_pb_get_u64(b, l, 1, &peer_id);
				if (!et_pb_find(b, l, 2, 2, NULL, &val, &vall))
					continue;
				snprintf(keybuf, sizeof(keybuf), "%u",
					 (unsigned)peer_id);
				if (f.json) {
					et_json_arr_begin(&f.j, keybuf);
				} else {
					et_cli_printf(ctx, "Peer ID: %u\n",
						      (unsigned)peer_id);
				}
				{
					et_pb_rd r2;
					unsigned f2, w2;
					uint64_t v2;
					const uint8_t *p2 = NULL;
					size_t l2 = 0;

					et_pb_rd_init(&r2, val, vall);
					while (et_pb_next(&r2, &f2, &w2, &v2,
							  &p2, &l2) > 0) {
						char netname[128] = "",
						     updated[64] = "";
						uint64_t version = 0;
						et_buf ids;
						et_pb_rd r3;
						unsigned f3, w3;
						uint64_t v3;
						const uint8_t *p3 = NULL;
						size_t l3 = 0;

						if (f2 != 1 || w2 != 2)
							continue;
						(void)et_pb_get_str(
							p2, l2, 1, netname,
							sizeof(netname));
						(void)et_pb_get_str(
							p2, l2, 3, updated,
							sizeof(updated));
						version = pb_get_u32(p2, l2, 4);
						et_buf_init(&ids);
						et_pb_rd_init(&r3, p2, l2);
						while (et_pb_next(&r3, &f3, &w3,
								  &v3, &p3, &l3) > 0)
							if (f3 == 2 && w3 == 0)
								et_buf_addf(
									&ids,
									"%s%u",
									ids.len
										? ", "
										: "",
									(unsigned)
										v3);
						if (f.json) {
							et_json_obj_begin(&f.j,
									  NULL);
							et_json_str(
								&f.j,
								"network_name",
								netname);
							et_json_str(
								&f.j,
								"last_updated",
								updated);
							et_json_uint(
								&f.j, "version",
								version);
							et_json_str(
								&f.j, "peer_ids",
								ids.p ? ids.p
								      : "");
							et_json_obj_end(&f.j);
						} else {
							et_cli_printf(
								ctx,
								"  Network Name: %s, Last Updated: %s, Version: %u, PeerIds: [%s]\n",
								netname, updated,
								(unsigned)version,
								ids.p ? ids.p
								      : "");
						}
						et_buf_free(&ids);
					}
				}
				if (f.json)
					et_json_arr_end(&f.j);
			}
		}
		if (f.json)
			et_json_obj_end(&f.j);
		et_cli_fanout_json_close(&f);
		et_pb_free(&req);
		et_pb_free(&resp);
	}
	et_cli_fanout_end(ctx, &f);
	return rc;
}

int et_cli_peer(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc > 0) {
		if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ls"))
			return run_peer_list(ctx);
		if (!strcmp(argv[0], "ipv6"))
			return run_peer_ipv6(ctx);
		if (!strcmp(argv[0], "list-foreign")) {
			int trusted = 0;

			for (int i = 1; i < argc; i++)
				if (!strcmp(argv[i], "--trusted-keys") ||
				    !strcmp(argv[i], "--include-trusted-keys"))
					trusted = 1;
			return run_peer_list_foreign(ctx, trusted);
		}
		if (!strcmp(argv[0], "list-global-foreign"))
			return run_peer_list_global_foreign(ctx);
		return ET_ENOENT;
	}
	return run_peer_list(ctx);
}

/* ===================== route ===================== */

static int run_route_list(struct et_cli_ctx *ctx)
{
	struct fanout f;
	int rc = 0;

	if (fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		struct peer_data d;

		ctx->target = &f.tg[f.index];
		if (fetch_peer_route_data(ctx, &d) || build_route_rows(&d)) {
			fprintf(stderr,
				"easytier-cli: failed to get route info\n");
			peer_data_free(&d);
			rc = 1;
			break;
		}
		fanout_header(ctx, &f);
		if (f.json) {
			fanout_json_open(&f);
			if (f.nt > 1) {
				et_json_arr_begin(&f.j, "result");
				emit_route_json(&f.j, &d);
				et_json_arr_end(&f.j);
			} else {
				emit_route_json(&f.j, &d);
			}
			fanout_json_close(&f);
		} else {
			emit_route_table(ctx, &d);
		}
		peer_data_free(&d);
	}
	fanout_end(ctx, &f);
	return rc;
}

static int run_route_dump(struct et_cli_ctx *ctx)
{
	et_pb resp;
	int rc = 1;

	et_pb_init(&resp);
	if (et_cli_call_instance(ctx, "PeerManageRpc", 4, &resp) == 0) {
		/* 上游直接 println 整段 Debug 文本（实测 ~50KB，不能用定长缓冲） */
		const uint8_t *s = NULL;
		size_t sl = 0;
		uint64_t fld = 0, w = 0, v = 0;
		et_pb_rd rd;

		et_cli_puts(ctx, "response: ");
		et_pb_rd_init(&rd, resp.p, resp.len);
		while (et_pb_next(&rd, (unsigned *)&fld, (unsigned *)&w, &v, &s,
				  &sl) > 0)
			if (fld == 1 && w == 2) {
				et_buf_addn(&ctx->out, (const char *)s, sl);
				break;
			}
		if (ctx->out.len && ctx->out.p[ctx->out.len - 1] != '\n')
			et_cli_puts(ctx, "\n");
		rc = 0;
	} else {
		fprintf(stderr, "easytier-cli: failed to dump route\n");
	}
	et_pb_free(&resp);
	return rc;
}

int et_cli_route(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc > 0) {
		if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ls"))
			return run_route_list(ctx);
		if (!strcmp(argv[0], "dump"))
			return run_route_dump(ctx);
		return ET_ENOENT;
	}
	return run_route_list(ctx);
}

/* ===================== node JSON（proto3 JSON / pbjson 语义） =====================
 * 上游 -o json node 输出 pbjson 的 proto3 JSON：
 *   - 默认值（空串/空数组/0/false/枚举 0）不输出；
 *   - 枚举输出变体名（"PortRestricted"），int64/uint64 输出字符串；
 *   - 消息字段存在就输出对象（内部再按默认值裁剪）。
 */

static void json_ipv4addr_obj(et_json *j, const char *key, const uint8_t *b,
			      size_t n)
{
	uint64_t v = 0;

	if (b && n)
		(void)et_pb_get_u64(b, n, 1, &v);
	et_json_obj_begin(j, key);
	if (v)
		et_json_uint(j, "addr", v);
	et_json_obj_end(j);
}

static void json_ipv6addr_obj(et_json *j, const char *key, const uint8_t *b,
			      size_t n)
{
	uint64_t part[4] = { 0, 0, 0, 0 };
	int any = 0;

	for (unsigned i = 0; i < 4; i++) {
		if (b && n)
			(void)et_pb_get_u64(b, n, i + 1, &part[i]);
		if (part[i])
			any = 1;
	}
	et_json_obj_begin(j, key);
	if (any)
		for (unsigned i = 0; i < 4; i++) {
			char k[12];

			if (!part[i])
				continue;
			snprintf(k, sizeof(k), "part%u", i + 1);
			et_json_uint(j, k, part[i]);
		}
	et_json_obj_end(j);
}

static void json_ipv6inet_obj(et_json *j, const char *key, const uint8_t *b,
			      size_t n)
{
	uint64_t plen = 0;
	const uint8_t *addr = NULL;
	size_t addrlen = 0;

	et_json_obj_begin(j, key);
	if (b && n) {
		(void)et_pb_get_u64(b, n, 2, &plen);
		if (et_pb_find(b, n, 1, 2, NULL, &addr, &addrlen))
			json_ipv6addr_obj(j, "address", addr, addrlen);
	}
	if (plen)
		et_json_uint(j, "network_length", plen);
	et_json_obj_end(j);
}

static void node_json_emit(et_json *j, const struct node_view *nv)
{
	const uint8_t *b = nv->raw.p;
	size_t n = nv->raw.len;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *p = NULL;
	size_t l = 0;
	const uint8_t *stun = NULL, *ff = NULL, *ipl = NULL, *v6 = NULL,
		      *pfx = NULL;
	size_t stunlen = 0, fflen = 0, ipllen = 0, v6len = 0, pfxlen = 0;
	char ipv4_addr[64] = "", hostname[128] = "", inst_id[64] = "",
	     config[4096] = "", version[64] = "";
	uint64_t peer_id = 0;
	int n_proxy = 0, n_listeners = 0;

	et_pb_rd_init(&r, b, n);
	while (et_pb_next(&r, &f, &w, &v, &p, &l) > 0) {
		if (w != 2)
			continue;
		switch (f) {
		case 3:
			n_proxy++;
			break;
		case 5:
			stun = p;
			stunlen = l;
			break;
		case 7:
			n_listeners++;
			break;
		case 10:
			ff = p;
			fflen = l;
			break;
		case 11:
			ipl = p;
			ipllen = l;
			break;
		case 12:
			v6 = p;
			v6len = l;
			break;
		case 13:
			pfx = p;
			pfxlen = l;
			break;
		default:
			break;
		}
	}
	(void)et_pb_get_u64(b, n, 1, &peer_id);
	(void)et_pb_get_str(b, n, 2, ipv4_addr, sizeof(ipv4_addr));
	(void)et_pb_get_str(b, n, 4, hostname, sizeof(hostname));
	(void)et_pb_get_str(b, n, 6, inst_id, sizeof(inst_id));
	(void)et_pb_get_str(b, n, 8, config, sizeof(config));
	(void)et_pb_get_str(b, n, 9, version, sizeof(version));

	et_json_obj_begin(j, NULL);
	if (peer_id)
		et_json_uint(j, "peer_id", peer_id);
	if (ipv4_addr[0])
		et_json_str(j, "ipv4_addr", ipv4_addr);
	if (n_proxy) {
		et_json_arr_begin(j, "proxy_cidrs");
		{
			et_pb_rd r2;
			unsigned f2, w2;
			uint64_t v2;
			const uint8_t *p2 = NULL;
			size_t l2 = 0;

			et_pb_rd_init(&r2, b, n);
			while (et_pb_next(&r2, &f2, &w2, &v2, &p2, &l2) > 0) {
				char s[512];

				if (f2 != 3 || w2 != 2 || !l2 ||
				    l2 >= sizeof(s))
					continue;
				memcpy(s, p2, l2);
				s[l2] = 0;
				et_json_str(j, NULL, s);
			}
		}
		et_json_arr_end(j);
	}
	if (hostname[0])
		et_json_str(j, "hostname", hostname);
	if (stun) {
		uint64_t udp = 0, tcp = 0, t = 0, minp = 0, maxp = 0;
		int n_ip = 0;

		(void)et_pb_get_u64(stun, stunlen, 1, &udp);
		(void)et_pb_get_u64(stun, stunlen, 2, &tcp);
		(void)et_pb_get_u64(stun, stunlen, 3, &t);
		(void)et_pb_get_u64(stun, stunlen, 5, &minp);
		(void)et_pb_get_u64(stun, stunlen, 6, &maxp);
		{
			et_pb_rd r3;
			unsigned f3, w3;
			uint64_t v3;
			const uint8_t *p3 = NULL;
			size_t l3 = 0;

			et_pb_rd_init(&r3, stun, stunlen);
			while (et_pb_next(&r3, &f3, &w3, &v3, &p3, &l3) > 0)
				if (f3 == 4 && w3 == 2 && l3)
					n_ip++;
		}
		et_json_obj_begin(j, "stun_info");
		if (udp)
			et_json_str(j, "udp_nat_type", et_nat_name(udp));
		if (tcp)
			et_json_str(j, "tcp_nat_type", et_nat_name(tcp));
		if (t) {
			char buf[32];

			snprintf(buf, sizeof(buf), "%lld",
				 (long long)(int64_t)t);
			et_json_str(j, "last_update_time", buf);
		}
		if (n_ip) {
			et_json_arr_begin(j, "public_ip");
			{
				et_pb_rd r3;
				unsigned f3, w3;
				uint64_t v3;
				const uint8_t *p3 = NULL;
				size_t l3 = 0;

				et_pb_rd_init(&r3, stun, stunlen);
				while (et_pb_next(&r3, &f3, &w3, &v3, &p3,
						  &l3) > 0) {
					char s[128];

					if (f3 != 4 || w3 != 2 || !l3 ||
					    l3 >= sizeof(s))
						continue;
					memcpy(s, p3, l3);
					s[l3] = 0;
					et_json_str(j, NULL, s);
				}
			}
			et_json_arr_end(j);
		}
		if (minp)
			et_json_uint(j, "min_port", minp);
		if (maxp)
			et_json_uint(j, "max_port", maxp);
		et_json_obj_end(j);
	}
	if (inst_id[0])
		et_json_str(j, "inst_id", inst_id);
	if (n_listeners) {
		et_json_arr_begin(j, "listeners");
		{
			et_pb_rd r3;
			unsigned f3, w3;
			uint64_t v3;
			const uint8_t *p3 = NULL;
			size_t l3 = 0;

			et_pb_rd_init(&r3, b, n);
			while (et_pb_next(&r3, &f3, &w3, &v3, &p3, &l3) > 0) {
				char s[256];

				if (f3 != 7 || w3 != 2 || !l3 ||
				    l3 >= sizeof(s))
					continue;
				memcpy(s, p3, l3);
				s[l3] = 0;
				et_json_str(j, NULL, s);
			}
		}
		et_json_arr_end(j);
	}
	if (config[0])
		et_json_str(j, "config", config);
	if (version[0])
		et_json_str(j, "version", version);
	if (ff) {
		static const char *names[] = {
			"is_public_server", "avoid_relay_data", "kcp_input",
			"no_relay_kcp", "support_conn_list_sync", "quic_input",
			"no_relay_quic", "is_credential_peer", "need_p2p",
			"disable_p2p", "ipv6_public_addr_provider"
		};

		et_json_obj_begin(j, "feature_flag");
		for (unsigned i = 0; i < 11; i++) {
			uint64_t x = 0;

			(void)et_pb_get_u64(ff, fflen, i + 1, &x);
			if (x)
				et_json_bool(j, names[i], 1);
		}
		et_json_obj_end(j);
	}
	if (ipl) {
		const uint8_t *p4 = NULL;
		size_t l4 = 0;
		int n4 = 0, n6 = 0, n_ls = 0, pub6 = 0;
		et_pb_rd r4;
		unsigned f4, w4;
		uint64_t v4;
		const uint8_t *q = NULL;
		size_t ql = 0;

		et_pb_rd_init(&r4, ipl, ipllen);
		while (et_pb_next(&r4, &f4, &w4, &v4, &q, &ql) > 0) {
			if (w4 != 2)
				continue;
			if (f4 == 2)
				n4++;
			else if (f4 == 3)
				pub6 = 1;
			else if (f4 == 4)
				n6++;
			else if (f4 == 5)
				n_ls++;
		}
		et_json_obj_begin(j, "ip_list");
		if (et_pb_find(ipl, ipllen, 1, 2, NULL, &p4, &l4))
			json_ipv4addr_obj(j, "public_ipv4", p4, l4);
		if (n4) {
			et_json_arr_begin(j, "interface_ipv4s");
			{
				et_pb_rd r5;
				unsigned f5, w5;
				uint64_t v5;
				const uint8_t *p5 = NULL;
				size_t l5 = 0;

				et_pb_rd_init(&r5, ipl, ipllen);
				while (et_pb_next(&r5, &f5, &w5, &v5, &p5,
						  &l5) > 0)
					if (f5 == 2 && w5 == 2)
						json_ipv4addr_obj(j, NULL, p5,
								  l5);
			}
			et_json_arr_end(j);
		}
		if (pub6 && et_pb_find(ipl, ipllen, 3, 2, NULL, &p4, &l4))
			json_ipv6addr_obj(j, "public_ipv6", p4, l4);
		if (n6) {
			et_json_arr_begin(j, "interface_ipv6s");
			{
				et_pb_rd r5;
				unsigned f5, w5;
				uint64_t v5;
				const uint8_t *p5 = NULL;
				size_t l5 = 0;

				et_pb_rd_init(&r5, ipl, ipllen);
				while (et_pb_next(&r5, &f5, &w5, &v5, &p5,
						  &l5) > 0)
					if (f5 == 4 && w5 == 2)
						json_ipv6addr_obj(j, NULL, p5,
								  l5);
			}
			et_json_arr_end(j);
		}
		if (n_ls) {
			et_json_arr_begin(j, "listeners");
			{
				et_pb_rd r5;
				unsigned f5, w5;
				uint64_t v5;
				const uint8_t *p5 = NULL;
				size_t l5 = 0;

				et_pb_rd_init(&r5, ipl, ipllen);
				while (et_pb_next(&r5, &f5, &w5, &v5, &p5,
						  &l5) > 0) {
					char url[512] = "";

					if (f5 != 5 || w5 != 2)
						continue;
					(void)et_pb_get_str(p5, l5, 1, url,
							    sizeof(url));
					et_json_obj_begin(j, NULL);
					if (url[0])
						et_json_str(j, "url", url);
					et_json_obj_end(j);
				}
			}
			et_json_arr_end(j);
		}
		et_json_obj_end(j);
	}
	if (v6)
		json_ipv6inet_obj(j, "public_ipv6_addr", v6, v6len);
	if (pfx)
		json_ipv6inet_obj(j, "ipv6_public_addr_prefix", pfx, pfxlen);
	et_json_obj_end(j);
}

/* ===================== node ===================== */

static int run_node(struct et_cli_ctx *ctx, int want_config)
{
	struct fanout f;
	int rc = 0;

	if (fanout_begin(ctx, &f))
		return 1;
	for (f.index = 0; f.index < f.nt; f.index++) {
		struct node_view nv;

		ctx->target = &f.tg[f.index];
		if (fetch_node_view(ctx, &nv)) {
			fprintf(stderr,
				"easytier-cli: failed to get node info\n");
			node_view_free(&nv);
			rc = 1;
			break;
		}
		fanout_header(ctx, &f);
		if (f.json) {
			fanout_json_open(&f);
			if (f.nt > 1)
				et_json_arr_begin(&f.j, "result");
			if (want_config) {
				et_json_str(&f.j, NULL, nv.config);
			} else {
				node_json_emit(&f.j, &nv);
			}
			if (f.nt > 1)
				et_json_arr_end(&f.j);
			fanout_json_close(&f);
		} else if (want_config) {
			/* 上游 println!("{}", config)：config 自带的尾换行之外
			 * 还会再补一个，所以这里无条件追加换行 */
			et_cli_puts(ctx, nv.config);
			et_cli_puts(ctx, "\n");
		} else {
			/* kv 表：表头行是 Virtual IP，其余逐行 */
			const char *labels[96];
			const char *values[96];
			int n = 0;

			labels[n] = "Virtual IP";
			values[n] = nv.ipv4_addr;
			n++;
			labels[n] = "Hostname";
			values[n] = nv.hostname;
			n++;
			labels[n] = "Proxy CIDRs";
			values[n] = nv.proxy_join;
			n++;
			{
				static char idbuf[32];

				snprintf(idbuf, sizeof(idbuf), "%u",
					 nv.peer_id);
				labels[n] = "Peer ID";
				values[n] = idbuf;
				n++;
			}
			for (int i = 0; i < nv.n_public; i++) {
				labels[n] = strchr(nv.public_ip[i], ':')
						    ? "Public IPv6"
						    : "Public IPv4";
				values[n] = nv.public_ip[i];
				n++;
			}
			labels[n] = "UDP Stun Type";
			values[n] = nv.have_nat ? et_nat_name(nv.nat)
						: "Unknown";
			n++;
			for (int i = 0; i < nv.n_iface_v4; i++) {
				labels[n] = "Interface IPv4";
				values[n] = nv.iface_v4[i];
				n++;
			}
			for (int i = 0; i < nv.n_iface_v6; i++) {
				labels[n] = "Interface IPv6";
				values[n] = nv.iface_v6[i];
				n++;
			}
			for (int i = 0;
			     i < nv.n_listeners && n < 94; i++) {
				static char lb[NODE_LISTENERS][24];

				if (!strncmp(nv.listeners[i], "ring", 4))
					continue;
				snprintf(lb[i], sizeof(lb[i]), "Listener %d",
					 nv.listener_idx[i]);
				labels[n] = lb[i];
				values[n] = nv.listeners[i];
				n++;
			}
			(void)et_table_kv_print(labels, values, n, &ctx->opts,
						&ctx->out);
		}
		node_view_free(&nv);
	}
	fanout_end(ctx, &f);
	return rc;
}

int et_cli_node(struct et_cli_ctx *ctx, int argc, char **argv)
{
	if (argc > 0) {
		if (!strcmp(argv[0], "info"))
			return run_node(ctx, 0);
		if (!strcmp(argv[0], "config"))
			return run_node(ctx, 1);
		return ET_ENOENT;
	}
	return run_node(ctx, 0);
}
