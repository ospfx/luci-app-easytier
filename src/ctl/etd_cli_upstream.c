/*
 * etd_cli_upstream.c - peer / route / node 三个子命令
 *
 * 输出格式与上游 easytier-cli（tabled 的 Markdown 样式）逐字对齐：
 *   | h1 | h2 |
 *   |----|----|
 *   | v1 | v2 |
 *
 * 数据来源与字段映射见 docs/CLI-PORT.md（抓包实测）。
 */
#include "etd.h"
#include "etd_cli.h"
#include "etd_pb.h"
#include "etd_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAT_UNKNOWN "Unknown"

static const char *nat_type_str(uint64_t v)
{
	static const char *names[] = { "Unknown",   "OpenInternet", "NoPat",
				       "FullCone",  "Restricted",   "PortRestricted",
				       "Symmetric", "SymUdpFirewall",
				       "SymmetricEasyInc", "SymmetricEasyDec" };

	if (v < sizeof(names) / sizeof(names[0]))
		return names[v];
	return NAT_UNKNOWN;
}

static size_t utf8_len(const char *s)
{
	/* 表格宽度按字节算即可：这些字段都是 ASCII */
	return s ? strlen(s) : 0;
}

static void render_table(int ncol, const char *const *hdr, int nrow,
			 char ***rows)
{
	size_t width[16];
	char *pad;

	for (int j = 0; j < ncol && j < 16; j++) {
		width[j] = utf8_len(hdr[j]);
		for (int i = 0; i < nrow; i++) {
			size_t w = utf8_len(rows[i][j]);

			if (w > width[j])
				width[j] = w;
		}
	}

	for (int j = 0; j < ncol; j++) {
		printf("| ");
		pad = (char *)hdr[j];
		printf("%s", pad ? pad : "");
		for (size_t k = utf8_len(pad); k < width[j]; k++)
			putchar(' ');
		printf(" ");
	}
	printf("|\n");

	for (int j = 0; j < ncol; j++) {
		putchar('|');
		for (size_t k = 0; k < width[j] + 2; k++)
			putchar('-');
	}
	printf("|\n");

	for (int i = 0; i < nrow; i++) {
		for (int j = 0; j < ncol; j++) {
			const char *cell = rows[i][j] ? rows[i][j] : "";

			printf("| %s", cell);
			for (size_t k = utf8_len(cell); k < width[j]; k++)
				putchar(' ');
			printf(" ");
		}
		printf("|\n");
	}
}

/* ---------------- 取 NodeInfo ---------------- */

#define NI_MAX_LISTENERS 16
#define NI_LISTENER_LEN 64

struct node_info {
	uint64_t peer_id;
	char hostname[128];
	char public_ip[64];
	uint64_t nat_type;
	char version[64];
	char listeners[NI_MAX_LISTENERS][NI_LISTENER_LEN];
	int n_listeners;
	int have;
};

static int fetch_node_info(const char *addr, struct node_info *ni)
{
	et_rpc c;
	et_pb body, lvl1, lvl2, resp;
	uint8_t uuid[24];
	int rc = -1;
	const uint8_t *p = NULL;
	size_t n = 0;
	uint64_t v = 0;

	memset(ni, 0, sizeof(*ni));
	et_pb_init(&body);
	et_pb_init(&lvl1);
	et_pb_init(&lvl2);
	et_pb_init(&resp);

	if (et_rpc_open(&c, addr)) {
		fprintf(stderr,
			"easytier-cli: failed to connect to RPC server %s\n",
			addr);
		goto out;
	}
	if (et_rpc_instance_uuid(&c, uuid)) {
		fprintf(stderr, "easytier-cli: failed to get network instance\n");
		goto close;
	}
	if (et_pb_bytes(&lvl2, 1, uuid, sizeof(uuid)))
		goto close;
	if (et_pb_msg(&lvl1, 1, &lvl2) || et_pb_msg(&body, 2, &lvl1) ||
	    et_pb_uint(&body, 3, 5000))
		goto close;
	if (et_rpc_call(&c, "PeerManageRpc", 7, body.p, body.len, &resp)) {
		fprintf(stderr, "easytier-cli: failed to get node info\n");
		goto close;
	}
	if (et_debug) {
		fprintf(stderr, "[dbg] uuid:");
		for (size_t k = 0; k < sizeof(uuid); k++)
			fprintf(stderr, "%02x", uuid[k]);
		fprintf(stderr, "\n[dbg] req:");
		for (size_t k = 0; k < body.len; k++)
			fprintf(stderr, "%02x", body.p[k]);
		fprintf(stderr, "\n[dbg] resp(%zu):", resp.len);
		for (size_t k = 0; k < resp.len && k < 400; k++)
			fprintf(stderr, "%02x", resp.p[k]);
		fprintf(stderr, "\n");
	}
	if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &p, &n))
		goto close;
	/* 实际报文是 body -> {1: ...} -> {1: NodeInfo}，逐层下探到含 hostname(field 4)
	 * 的那一层，避免写死层数。 */
	for (int depth = 0; depth < 4; depth++) {
		const uint8_t *inner = NULL;
		size_t ilen = 0;

		if (et_pb_find(p, n, 4, 2, NULL, &inner, &ilen))
			break;
		if (!et_pb_find(p, n, 1, 2, NULL, &inner, &ilen))
			goto close;
		p = inner;
		n = ilen;
	}
	if (et_pb_find(p, n, 1, 0, &ni->peer_id, NULL, NULL) &&
	    et_pb_find(p, n, 4, 2, NULL, &p, &n)) {
		size_t l = n < sizeof(ni->hostname) - 1 ? n : sizeof(ni->hostname) - 1;

		memcpy(ni->hostname, p, l);
		ni->hostname[l] = 0;
	}
	{
		const uint8_t *q = NULL;
		size_t m = 0;

		/* 重新定位 NodeInfo（上面 p/n 已被 hostname 覆盖） */
		if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &q, &m))
			goto close;
		for (int depth = 0; depth < 4; depth++) {
			const uint8_t *inner = NULL;
			size_t ilen = 0;

			if (et_pb_find(q, m, 4, 2, NULL, &inner, &ilen))
				break;
			if (!et_pb_find(q, m, 1, 2, NULL, &inner, &ilen))
				break;
			q = inner;
			m = ilen;
		}
		p = q;
		n = m;
	}
	{
		const uint8_t *q = NULL;
		size_t m = 0;

		if (et_pb_find(p, n, 9, 2, NULL, &q, &m)) {
			size_t l = m < sizeof(ni->version) - 1 ? m : sizeof(ni->version) - 1;

			memcpy(ni->version, q, l);
			ni->version[l] = 0;
		}
		if (et_pb_find(p, n, 5, 2, NULL, &q, &m)) {
			const uint8_t *r2 = NULL;
			size_t k = 0;

			if (et_pb_find(q, m, 1, 0, &v, NULL, NULL))
				ni->nat_type = v;
			if (et_pb_find(q, m, 4, 2, NULL, &r2, &k)) {
				size_t l = k < sizeof(ni->public_ip) - 1 ? k : sizeof(ni->public_ip) - 1;

				memcpy(ni->public_ip, r2, l);
				ni->public_ip[l] = 0;
			}
		}
		{
			/* field 7 是 repeated string（listeners），逐个扫一遍 */
			et_pb_rd r;
			unsigned f, w;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&r, p, n);
			while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
				if (f != 7 || w != 2 || ni->n_listeners >= NI_MAX_LISTENERS)
					continue;
				if (l >= NI_LISTENER_LEN)
					l = NI_LISTENER_LEN - 1;
				memcpy(ni->listeners[ni->n_listeners], b, l);
				ni->listeners[ni->n_listeners][l] = 0;
				ni->n_listeners++;
			}
		}
	}
	ni->have = 1;
	rc = 0;
close:
	et_rpc_close(&c);
out:
	et_pb_free(&body);
	et_pb_free(&lvl1);
	et_pb_free(&lvl2);
	et_pb_free(&resp);
	return rc;
}

static void u64_str(char *buf, size_t n, uint64_t v)
{
	snprintf(buf, n, "%llu", (unsigned long long)v);
}

int et_cli_peer(const char *addr)
{
	struct node_info ni;
	char id[32];
	const char *hdr[] = { "ipv4", "hostname", "cost",   "lat(ms)", "loss",
			      "rx",   "tx",       "tunnel", "NAT",     "version" };
	char *row[10];

	if (fetch_node_info(addr, &ni))
		return 1;
	u64_str(id, sizeof(id), ni.peer_id);
	row[0] = (char *)"";
	row[1] = ni.hostname;
	row[2] = (char *)"Local";
	row[3] = (char *)"-";
	row[4] = (char *)"-";
	row[5] = (char *)"-";
	row[6] = (char *)"-";
	row[7] = (char *)"-";
	row[8] = (char *)nat_type_str(ni.nat_type);
	row[9] = ni.version;
	{
		char **rows[] = { row };

		render_table(10, hdr, 1, rows);
	}
	(void)id;
	return 0;
}

int et_cli_route(const char *addr)
{
	struct node_info ni;
	const char *hdr[] = { "ipv4",
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
	char *row[13];

	if (fetch_node_info(addr, &ni))
		return 1;
	row[0] = (char *)"";
	row[1] = ni.hostname;
	row[2] = (char *)"";
	row[3] = (char *)"-";
	row[4] = (char *)"Local";
	row[5] = (char *)"0";
	row[6] = (char *)"0";
	row[7] = (char *)"0";
	row[8] = (char *)"-";
	row[9] = (char *)"Local";
	row[10] = (char *)"0";
	row[11] = (char *)"0";
	row[12] = ni.version;
	{
		char **rows[] = { row };

		render_table(13, hdr, 1, rows);
	}
	return 0;
}

int et_cli_node(const char *addr)
{
	struct node_info ni;
	char id[32];
	char label[NI_MAX_LISTENERS][16];
	const char *hdr[] = { "Virtual IP", "" };
	char *cells[6 + NI_MAX_LISTENERS][2];
	char **rows[6 + NI_MAX_LISTENERS];
	int nrow = 0;

	if (fetch_node_info(addr, &ni))
		return 1;
	u64_str(id, sizeof(id), ni.peer_id);

	cells[nrow][0] = (char *)"Hostname";
	cells[nrow][1] = ni.hostname;
	nrow++;
	cells[nrow][0] = (char *)"Proxy CIDRs";
	cells[nrow][1] = (char *)"";
	nrow++;
	cells[nrow][0] = (char *)"Peer ID";
	cells[nrow][1] = id;
	nrow++;
	cells[nrow][0] = (char *)"Public IPv4";
	cells[nrow][1] = ni.public_ip;
	nrow++;
	cells[nrow][0] = (char *)"UDP Stun Type";
	cells[nrow][1] = (char *)nat_type_str(ni.nat_type);
	nrow++;
	cells[nrow][0] = (char *)"Interface IPv4";
	cells[nrow][1] = (char *)"";
	nrow++;
	for (int i = 0; i < ni.n_listeners; i++) {
		snprintf(label[i], sizeof(label[i]), "Listener %d", i + 1);
		cells[nrow][0] = label[i];
		cells[nrow][1] = ni.listeners[i];
		nrow++;
	}
	for (int i = 0; i < nrow; i++)
		rows[i] = cells[i];
	render_table(2, hdr, nrow, rows);
	return 0;
}
