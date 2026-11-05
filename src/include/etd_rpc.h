/*
 * etd_rpc.h - EasyTier core 的自研 RPC 客户端（rpc-portal，默认 127.0.0.1:15888）
 *
 * 帧格式（实测）：
 *   [u32 LE 帧长 = 16 + payload][PeerManagerHeader 16B][RpcPacket protobuf]
 * PeerManagerHeader：u32 from, u32 to, u8 packet_type(8=req,9=resp),
 *                    u8 flags, u8 forward_counter, u8 reserved, u32 payload_len
 * 客户端身份固定 from_peer = to_peer = 1。
 */
#ifndef ETD_RPC_H
#define ETD_RPC_H

#include "etd_pb.h"

#include <stdint.h>

typedef struct {
	int fd;
	int64_t txid;
} et_rpc;

/* addr 形如 "127.0.0.1:15888" */
int et_rpc_open(et_rpc *c, const char *addr);
void et_rpc_close(et_rpc *c);

/* 调一次 RPC，成功时 out 里是响应的 body（protobuf 字节） */
int et_rpc_call(et_rpc *c, const char *service, unsigned method,
		const uint8_t *body, size_t body_len, et_pb *out);

/* WebClientService/5：取当前网络实例的 24 字节 UUID */
int et_rpc_instance_uuid(et_rpc *c, uint8_t uuid[24]);

#endif /* ETD_RPC_H */
