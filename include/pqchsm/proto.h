/* pqchsm/proto.h —— 自定义命令集
 *
 * 【为什么要有这一层】
 * 库接口天然假设"调用方是可信的"：缓冲区所有权清晰、指针有效、错误信息随便给。
 * 把它逼成**进程外接口**会暴露这些隐含假设 —— 这正是做这一层的价值，
 * 而不只是"能远程调用"。
 *
 * 【分层：命令分派与传输解耦】
 * pqc_proto_dispatch() 只做「请求字节 → 响应字节」，不碰 socket。
 * 传输层（cli/pqchsmd.c 的 TCP）是可替换的薄壳。板子到手后把 TCP 换成 UART，
 * 分派逻辑一行不改 —— 与 对加速器桩的做法是同一个思路。
 *
 * 【帧格式】（全部显式小端）
 *   请求： magic "PQ"(2) | version u8 | cmd u8 | seq u32 | payload_len u32 | payload
 *   响应： magic "PQ"(2) | version u8 | 0xFF  | seq u32 | payload_len u32 | status u16 | payload
 *
 * payload 是一串 TLV：tag u16 | len u32 | value。
 * 定长头 + 显式长度，是为了让 UART 上的逐字节收包也能正确分帧。
 *
 * 【安全边界】
 * 本协议**不做**认证与加密：它对应的是设备内部/受控链路（UART 或本机回环）。
 * 走网络必须外套 TLS —— 这条写在这里，免得将来有人直接把 daemon 暴露出去。
 * 私钥永远不出现在任何响应里：协议里根本没有"导出私钥"的命令码。
 */
#ifndef PQCHSM_PROTO_H
#define PQCHSM_PROTO_H

#include "pqchsm/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PQC_PROTO_VERSION 1
#define PQC_PROTO_HDR_LEN 12
#define PQC_PROTO_MAX_PAYLOAD (1u << 20)   /* 1 MiB，够放最大的签名与公钥 */

/* 命令码 */
typedef enum {
	CMD_PING          = 0x01,
	CMD_SLOT_LIST     = 0x02,   /* → COUNT */
	CMD_SLOT_INFO     = 0x03,   /* SLOT → LABEL, ALG, STATE, USAGE, POLICY, COUNT(use) */
	CMD_INIT_TOKEN    = 0x04,   /* SLOT, LABEL, PIN */
	CMD_SET_USER_PIN  = 0x05,   /* SESSION, PIN */
	CMD_SESSION_OPEN  = 0x06,   /* SLOT → SESSION */
	CMD_SESSION_CLOSE = 0x07,   /* SESSION */
	CMD_LOGIN         = 0x08,   /* SESSION, ROLE, PIN */
	CMD_LOGOUT        = 0x09,   /* SESSION */
	CMD_GENERATE      = 0x0A,   /* SESSION, ALG, USAGE, POLICY → HANDLE */
	CMD_PUBKEY        = 0x0B,   /* SESSION, HANDLE → DATA */
	CMD_SIGN          = 0x0C,   /* SESSION, HANDLE, DATA → SIG */
	CMD_DECAPS        = 0x0D,   /* SESSION, HANDLE, DATA(ct) → DATA(ss) */
	CMD_DESTROY       = 0x0E,   /* SESSION, HANDLE */
	CMD_ZEROIZE       = 0x0F,   /* SESSION, SLOT */
	CMD_UNLOCK        = 0x10,   /* SESSION, SLOT */
	CMD_SAVE          = 0x11,   /* — 落盘密钥库 */
	CMD_ROTATE_KEK    = 0x12,   /* — 显式 KEK 轮换 */
} pqc_cmd_t;

/* TLV 标签 */
typedef enum {
	TAG_SLOT    = 0x0001,   /* u32 */
	TAG_SESSION = 0x0002,   /* u64 */
	TAG_HANDLE  = 0x0003,   /* u64 */
	TAG_ALG     = 0x0004,   /* u32（pqc_alg_t） */
	TAG_USAGE   = 0x0005,   /* u32 */
	TAG_POLICY  = 0x0006,   /* u32 */
	TAG_ROLE    = 0x0007,   /* u32（hsm_role_t） */
	TAG_STATE   = 0x0008,   /* u32（slot_state_t） */
	TAG_COUNT   = 0x0009,   /* u64 */
	TAG_PIN     = 0x0010,   /* 字节串 */
	TAG_LABEL   = 0x0011,   /* 字节串 */
	TAG_DATA    = 0x0012,   /* 字节串 */
	TAG_SIG     = 0x0013,   /* 字节串 */
	TAG_ERRMSG  = 0x0014,   /* 字节串，人类可读 */
} pqc_tag_t;

/* 服务端上下文。keystore_path 可为 NULL（不落盘）。 */
typedef struct {
	hsm_token_t *tok;
	const char  *keystore_path;
} pqc_proto_ctx_t;

/* 请求字节 → 响应字节。
 * 返回 0 表示产出了一个合法响应（哪怕业务上失败，status 里会带 HSM 错误码）；
 * 返回 -1 表示连响应都构造不出来（缓冲太小），调用方应当断开连接。
 *
 * req 必须是**完整一帧**；分帧由传输层负责（见 pqc_proto_frame_len）。 */
int pqc_proto_dispatch(pqc_proto_ctx_t *ctx,
                       const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t resp_cap, size_t *resp_len);

/* 已读到 n 字节时，这一帧总共需要多少字节。
 * n < PQC_PROTO_HDR_LEN 时返回 PQC_PROTO_HDR_LEN（先把头读齐）。
 * 帧头非法返回 -1，调用方应当断开连接。 */
long pqc_proto_frame_len(const uint8_t *buf, size_t n);

/* 客户端用：拼一个请求帧。成功返回 0。 */
int pqc_proto_build_req(uint8_t cmd, uint32_t seq,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t cap, size_t *out_len);

/* ---- TLV 读写 ----------------------------------------------------------- */
typedef struct {
	uint8_t *buf;
	size_t   cap;
	size_t   len;
	int      err;
} tlv_writer_t;

void tlv_init(tlv_writer_t *w, uint8_t *buf, size_t cap);
void tlv_put(tlv_writer_t *w, uint16_t tag, const void *val, size_t len);
void tlv_put_u32(tlv_writer_t *w, uint16_t tag, uint32_t v);
void tlv_put_u64(tlv_writer_t *w, uint16_t tag, uint64_t v);

/* 在 payload 里找一个 tag。找到返回值指针并回填长度，否则返回 NULL。 */
const uint8_t *tlv_find(const uint8_t *p, size_t n, uint16_t tag, size_t *out_len);
int tlv_get_u32(const uint8_t *p, size_t n, uint16_t tag, uint32_t *out);
int tlv_get_u64(const uint8_t *p, size_t n, uint16_t tag, uint64_t *out);

/* 响应里的 status（HSM_OK / HSM_ERR_*）。resp 必须是完整一帧。 */
int pqc_proto_resp_status(const uint8_t *resp, size_t n, uint16_t *status,
                          const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_PROTO_H */
