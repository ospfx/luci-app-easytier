/*
 * etd_rpc.c - EasyTier 自研 RPC 客户端
 *
 * 只依赖 POSIX socket 与 etd_pb，不引入任何第三方库。
 */
#include "etd_rpc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RPC_PTYPE_REQ 8
#define RPC_PTYPE_RESP 9
#define RPC_TIMEOUT_SEC 10
#define RPC_REQ_ID 1u

static int read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;
	ssize_t r;

	while (got < n) {
		r = read(fd, (uint8_t *)buf + got, n - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	size_t sent = 0;
	ssize_t r;

	while (sent < n) {
		r = write(fd, (const uint8_t *)buf + sent, n - sent);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)r;
	}
	return 0;
}

int et_rpc_open(et_rpc *c, const char *addr)
{
	char host[64];
	const char *colon;
	char *end = NULL;
	long port;
	struct addrinfo hints, *res = NULL;
	int fd = -1;

	if (!c || !addr)
		return -1;
	c->fd = -1;
	c->txid = 0;

	colon = strrchr(addr, ':');
	if (!colon || (size_t)(colon - addr) >= sizeof(host))
		return -1;
	memcpy(host, addr, (size_t)(colon - addr));
	host[colon - addr] = '\0';
	port = strtol(colon + 1, &end, 10);
	if (!end || *end != '\0' || port <= 0 || port > 65535)
		return -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, colon + 1, &hints, &res) != 0)
		return -1;

	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		return -1;

	struct timeval tv = { .tv_sec = RPC_TIMEOUT_SEC, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	c->fd = fd;
	return 0;
}

void et_rpc_close(et_rpc *c)
{
	if (c && c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

/* 组一个 RpcPacket 并连同 16 字节 PeerManagerHeader 一起发出去 */
static int send_packet(et_rpc *c, const char *service, unsigned method,
		       const uint8_t *body, size_t body_len)
{
	et_pb rpc, desc, comp;
	uint8_t hdr[16];
	uint8_t lenbuf[4];
	size_t flen;
	int rc = -1;

	et_pb_init(&rpc);
	et_pb_init(&desc);
	et_pb_init(&comp);

	if (et_pb_str(&desc, 2, service) || et_pb_str(&desc, 3, service) ||
	    et_pb_uint(&desc, 4, method))
		goto out;
	/* algo=1(None), accepted=1(None)：不触发 zstd 压缩 */
	if (et_pb_uint(&comp, 1, 1) || et_pb_uint(&comp, 2, 1))
		goto out;

	if (et_pb_uint(&rpc, 1, RPC_REQ_ID) ||
	    et_pb_uint(&rpc, 2, RPC_REQ_ID) ||
	    et_pb_varint(&rpc, (3u << 3) | 0) || et_pb_varint(&rpc, (uint64_t)++c->txid) ||
	    et_pb_msg(&rpc, 4, &desc) ||
	    et_pb_bytes(&rpc, 5, body, body_len) ||
	    et_pb_uint(&rpc, 6, 1) ||	/* is_request */
	    et_pb_uint(&rpc, 7, 1) ||	/* total_pieces */
	    et_pb_msg(&rpc, 10, &comp))
		goto out;

	flen = 16 + rpc.len;
	lenbuf[0] = (uint8_t)(flen & 0xff);
	lenbuf[1] = (uint8_t)((flen >> 8) & 0xff);
	lenbuf[2] = (uint8_t)((flen >> 16) & 0xff);
	lenbuf[3] = (uint8_t)((flen >> 24) & 0xff);

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = (uint8_t)(RPC_REQ_ID & 0xff);
	hdr[4] = (uint8_t)(RPC_REQ_ID & 0xff);
	hdr[8] = RPC_PTYPE_REQ;
	hdr[10] = 1; /* forward_counter */
	hdr[12] = (uint8_t)(rpc.len & 0xff);
	hdr[13] = (uint8_t)((rpc.len >> 8) & 0xff);
	hdr[14] = (uint8_t)((rpc.len >> 16) & 0xff);
	hdr[15] = (uint8_t)((rpc.len >> 24) & 0xff);

	if (write_full(c->fd, lenbuf, 4) || write_full(c->fd, hdr, 16) ||
	    write_full(c->fd, rpc.p, rpc.len))
		goto out;
	rc = 0;
out:
	et_pb_free(&rpc);
	et_pb_free(&desc);
	et_pb_free(&comp);
	return rc;
}

/* 读一帧，返回 payload（不含 16 字节头）到 out */
static int recv_packet(et_rpc *c, et_pb *out, int64_t *txid, unsigned *ptype)
{
	uint8_t lenbuf[4];
	uint8_t hdr[16];
	uint32_t flen;
	et_pb_rd r;
	unsigned f, w;
	uint64_t v;
	const uint8_t *b = NULL;
	size_t l = 0;

	if (read_full(c->fd, lenbuf, 4))
		return -1;
	flen = (uint32_t)lenbuf[0] | ((uint32_t)lenbuf[1] << 8) |
	       ((uint32_t)lenbuf[2] << 16) | ((uint32_t)lenbuf[3] << 24);
	if (flen < 16 || flen > (1u << 20))
		return -1;
	if (read_full(c->fd, hdr, sizeof(hdr)))
		return -1;
	*ptype = hdr[8];
	et_pb_reset(out);
	if (flen > 16) {
		size_t n = flen - 16;

		if (out->cap < n) {
			uint8_t *p = realloc(out->p, n);

			if (!p)
				return -1;
			out->p = p;
			out->cap = n;
		}
		if (read_full(c->fd, out->p, n))
			return -1;
		out->len = n;
	}

	*txid = 0;
	et_pb_rd_init(&r, out->p, out->len);
	while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
		if (f == 3 && w == 0)
			*txid = (int64_t)v;
	}
	return 0;
}

int et_rpc_call(et_rpc *c, const char *service, unsigned method,
		const uint8_t *body, size_t body_len, et_pb *out)
{
	et_pb frame;
	int64_t want, got;
	unsigned ptype;
	int rc = -1;

	if (send_packet(c, service, method, body, body_len))
		return -1;
	want = c->txid;

	et_pb_init(&frame);
	for (int i = 0; i < 16; i++) {
		if (recv_packet(c, &frame, &got, &ptype))
			goto out;
		if (ptype != RPC_PTYPE_RESP || got != want)
			continue; /* 跳过无关/迟到的帧 */
		{
			et_pb_rd r;
			unsigned f, w;
			uint64_t v;
			const uint8_t *b = NULL;
			size_t l = 0;

			et_pb_rd_init(&r, frame.p, frame.len);
			while (et_pb_next(&r, &f, &w, &v, &b, &l) > 0) {
				if (f == 5 && w == 2) {
					et_pb_reset(out);
					if (l && out->cap < l) {
						uint8_t *p = realloc(out->p, l);

						if (!p)
							goto out;
						out->p = p;
						out->cap = l;
					}
					if (l)
						memcpy(out->p, b, l);
					out->len = l;
					rc = 0;
				}
				if (f == 10 && w == 2) {
					uint64_t algo = 0;

					if (et_pb_find(b, l, 1, 0, &algo, NULL, NULL) &&
					    algo == 2) {
						fprintf(stderr, "easytier-cli: 响应被 zstd 压缩，当前实现不支持\n");
						goto out;
					}
				}
			}
		}
		if (rc == 0)
			break;
	}
out:
	et_pb_free(&frame);
	return rc;
}

int et_rpc_instance_uuid(et_rpc *c, uint8_t uuid[24])
{
	et_pb req, resp;
	const uint8_t *inst = NULL, *id = NULL;
	size_t inst_len = 0, id_len = 0;
	int rc = -1;

	et_pb_init(&req);
	et_pb_init(&resp);
	if (et_pb_uint(&req, 3, 5000))
		goto out;
	if (et_rpc_call(c, "WebClientService", 5, req.p, req.len, &resp))
		goto out;
	if (!et_pb_find(resp.p, resp.len, 1, 2, NULL, &inst, &inst_len))
		goto out;
	if (!et_pb_find(inst, inst_len, 1, 2, NULL, &id, &id_len))
		goto out;
	if (id_len != 24)
		goto out;
	memcpy(uuid, id, 24);
	rc = 0;
out:
	et_pb_free(&req);
	et_pb_free(&resp);
	return rc;
}
