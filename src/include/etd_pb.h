/*
 * etd_pb.h - 最小 protobuf 编解码（只覆盖 EasyTier RPC 需要的部分）
 *
 * 只实现 varint / length-delimited / fixed64 三种 wire type，
 * 不依赖任何库，供 etd_rpc.c 与 easytier-cli 的表格解析使用。
 */
#ifndef ETD_PB_H
#define ETD_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 写缓冲 */
typedef struct {
	uint8_t *p;
	size_t len;
	size_t cap;
} et_pb;

void et_pb_init(et_pb *b);
void et_pb_free(et_pb *b);
void et_pb_reset(et_pb *b);

int et_pb_varint(et_pb *b, uint64_t v);
int et_pb_tag(et_pb *b, unsigned field, unsigned wire);
int et_pb_uint(et_pb *b, unsigned field, uint64_t v);
int et_pb_bytes(et_pb *b, unsigned field, const void *data, size_t n);
int et_pb_str(et_pb *b, unsigned field, const char *s);
/* 把已编好的子消息整体作为 field 写入（自动加长度前缀） */
int et_pb_msg(et_pb *b, unsigned field, const et_pb *sub);

/* 读游标 */
typedef struct {
	const uint8_t *p;
	size_t len;
	size_t off;
} et_pb_rd;

void et_pb_rd_init(et_pb_rd *r, const void *data, size_t n);

/* 读下一个字段：返回 1=成功，0=结束，-1=格式错误。
 * wire=0 时 v 有效；wire=2 时 buf/blen 指向内容。 */
int et_pb_next(et_pb_rd *r, unsigned *field, unsigned *wire, uint64_t *v,
	       const uint8_t **buf, size_t *blen);

/* 在给定消息里找某个字段：wire 必须匹配；返回 1=找到 */
int et_pb_find(const uint8_t *data, size_t n, unsigned field, unsigned wire,
	       uint64_t *v, const uint8_t **buf, size_t *blen);

#endif /* ETD_PB_H */
