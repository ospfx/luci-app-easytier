/*
 * etd_pb.c - 最小 protobuf 编解码
 */
#include "etd_pb.h"

#include <stdlib.h>
#include <string.h>

static int pb_grow(et_pb *b, size_t need)
{
	size_t cap;
	uint8_t *p;

	if (b->len + need <= b->cap)
		return 0;
	cap = b->cap ? b->cap : 128;
	while (cap < b->len + need)
		cap *= 2;
	p = realloc(b->p, cap);
	if (!p)
		return -1;
	b->p = p;
	b->cap = cap;
	return 0;
}

void et_pb_init(et_pb *b)
{
	memset(b, 0, sizeof(*b));
}

void et_pb_free(et_pb *b)
{
	free(b->p);
	memset(b, 0, sizeof(*b));
}

void et_pb_reset(et_pb *b)
{
	b->len = 0;
}

int et_pb_varint(et_pb *b, uint64_t v)
{
	uint8_t tmp[10];
	size_t n = 0;

	do {
		tmp[n] = (uint8_t)(v & 0x7f);
		v >>= 7;
		if (v)
			tmp[n] |= 0x80;
		n++;
	} while (v);

	if (pb_grow(b, n))
		return -1;
	memcpy(b->p + b->len, tmp, n);
	b->len += n;
	return 0;
}

int et_pb_tag(et_pb *b, unsigned field, unsigned wire)
{
	return et_pb_varint(b, ((uint64_t)field << 3) | (wire & 7));
}

int et_pb_uint(et_pb *b, unsigned field, uint64_t v)
{
	if (et_pb_tag(b, field, 0))
		return -1;
	return et_pb_varint(b, v);
}

int et_pb_bytes(et_pb *b, unsigned field, const void *data, size_t n)
{
	if (et_pb_tag(b, field, 2) || et_pb_varint(b, n))
		return -1;
	if (!n)
		return 0;
	if (pb_grow(b, n))
		return -1;
	memcpy(b->p + b->len, data, n);
	b->len += n;
	return 0;
}

int et_pb_str(et_pb *b, unsigned field, const char *s)
{
	return et_pb_bytes(b, field, s, s ? strlen(s) : 0);
}

int et_pb_msg(et_pb *b, unsigned field, const et_pb *sub)
{
	return et_pb_bytes(b, field, sub->p, sub->len);
}

void et_pb_rd_init(et_pb_rd *r, const void *data, size_t n)
{
	r->p = data;
	r->len = n;
	r->off = 0;
}

static int rd_varint(et_pb_rd *r, uint64_t *out)
{
	uint64_t v = 0;
	unsigned shift = 0;
	uint8_t c;

	while (r->off < r->len) {
		c = r->p[r->off++];
		v |= (uint64_t)(c & 0x7f) << shift;
		if (!(c & 0x80)) {
			*out = v;
			return 0;
		}
		shift += 7;
		if (shift > 63)
			return -1;
	}
	return -1;
}

int et_pb_next(et_pb_rd *r, unsigned *field, unsigned *wire, uint64_t *v,
	       const uint8_t **buf, size_t *blen)
{
	uint64_t tag, len;

	if (r->off >= r->len)
		return 0;
	if (rd_varint(r, &tag))
		return -1;
	*field = (unsigned)(tag >> 3);
	*wire = (unsigned)(tag & 7);
	if (v)
		*v = 0;
	if (buf)
		*buf = NULL;
	if (blen)
		*blen = 0;

	switch (*wire) {
	case 0:
		return rd_varint(r, v ? v : &tag) ? -1 : 1;
	case 1:
		if (r->off + 8 > r->len)
			return -1;
		if (buf)
			*buf = r->p + r->off;
		r->off += 8;
		return 1;
	case 2:
		if (rd_varint(r, &len) || len > r->len - r->off)
			return -1;
		if (buf)
			*buf = r->p + r->off;
		if (blen)
			*blen = (size_t)len;
		r->off += (size_t)len;
		return 1;
	case 5:
		if (r->off + 4 > r->len)
			return -1;
		if (buf)
			*buf = r->p + r->off;
		r->off += 4;
		return 1;
	default:
		return -1;
	}
}

int et_pb_find(const uint8_t *data, size_t n, unsigned field, unsigned wire,
	       uint64_t *v, const uint8_t **buf, size_t *blen)
{
	et_pb_rd r;
	unsigned f, w;
	uint64_t val = 0;
	const uint8_t *b = NULL;
	size_t l = 0;

	et_pb_rd_init(&r, data, n);
	while (et_pb_next(&r, &f, &w, &val, &b, &l) > 0) {
		if (f == field && w == wire) {
			if (v)
				*v = val;
			if (buf)
				*buf = b;
			if (blen)
				*blen = l;
			return 1;
		}
	}
	return 0;
}
