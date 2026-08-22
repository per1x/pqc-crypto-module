/* pwrp_format.h —— PWRP 包裹线格式的**唯一**定义
 *
 * ============================================================================
 * 【为什么单独拆一个头出来】
 * ============================================================================
 * 同一个线格式有两份实现，而且**必须**有两份：
 *
 *   · 普通世界 `src/store/wrap.c`   —— AES-256-GCM 走 OpenSSL EVP；
 *   · 安全世界 `tee/ta/ta_wrap.c`   —— 走 TEE_ALG_AES_GCM（TA 里没有 OpenSSL）。
 *
 * 两份**实现**是正当的：后端本来就不同，硬合并只会得到一堆 #if。
 * 不正当的是两份**格式定义** —— magic、版本号、字段偏移、AAD 怎么拼，
 * 以前在两个头文件里各写了一遍（`PQC_WRAP_*` 与 `TA_WRAP_*`）。
 *
 * 那种重复的风险不是"重复"本身，是**漂移**：
 *   · 改一处忘另一处，编译器不会说话（两边是两个独立的宏名）；
 *   · 症状出现在最远的地方 —— TA 包好的 blob，普通世界解不开，
 *     错误码是"认证失败"，与"密钥不对/文件被改"完全无法区分；
 *   · 而这两条路平时各自都跑得好好的，只有跨世界那一次才碰头。
 *
 * 所以：**格式只在这里定义一次**，两边都 include 它，各自只保留自己的
 * 密码后端。两侧的公开 API（`pqc_wrap_*` / `ta_wrap_*`）不变 —— 那是接口，
 * 不是格式。
 *
 * 另有一组**跨实现 KAT 对拍**（tests/unit/test_kdf.c 的 cross 段与
 * tools/tee_native_tests.sh）钉死"同样的输入两边逐字节相同"：
 * 共享头挡的是"定义漂移"，对拍挡的是"实现漂移"，两者缺一不可。
 *
 * ============================================================================
 * 【线格式】（全部小端，逐字节显式编码）
 * ============================================================================
 *   偏移 0   magic  "PWRP"        4 字节
 *   偏移 4   ver    LE16 = 1
 *   偏移 6   alg    LE16 = 1（AES-256-GCM）
 *   偏移 8   aad_len LE32          调用方元数据长度（**不含**头部那 16 字节）
 *   偏移 12  pt_len  LE32          明文长度（= 密文长度）
 *   偏移 16  nonce  12 字节
 *   偏移 28  ct     pt_len 字节
 *   偏移 28+pt_len  tag 16 字节
 *
 * **AAD = 头部这 16 字节 ‖ 调用方给的 aad**。头部进 AAD 是关键的一条：
 * 少了它，攻击者可以改 aad_len/pt_len 而 tag 照样过。
 *
 * 解包时头里的 aad_len 必须与调用方给的长度**相等**才继续 —— 否则就是
 * 拿别处的元数据来配这份密文。
 *
 * 本头文件只依赖 <stddef.h>/<stdint.h>，不碰任何密码库：它要能同时被
 * 普通世界（OpenSSL）、TA（TEE API）、以及 tee/tests 的原生构建包含。
 */
#ifndef PQCHSM_PWRP_FORMAT_H
#define PQCHSM_PWRP_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWRP_VERSION        1
#define PWRP_ALG_AES256GCM  1

#define PWRP_HDR_LEN        16
#define PWRP_NONCE_LEN      12
#define PWRP_TAG_LEN        16
#define PWRP_OVERHEAD       (PWRP_HDR_LEN + PWRP_NONCE_LEN + PWRP_TAG_LEN)
#define PWRP_KEK_LEN        32

/* 头部内的字段偏移。写成常量而不是"手数字节"，是因为这几个数字正是
 * 两份实现最容易各写各的地方。 */
#define PWRP_OFF_MAGIC      0
#define PWRP_OFF_VER        4
#define PWRP_OFF_ALG        6
#define PWRP_OFF_AADLEN     8
#define PWRP_OFF_PTLEN      12
#define PWRP_OFF_NONCE      PWRP_HDR_LEN
#define PWRP_OFF_CT         (PWRP_HDR_LEN + PWRP_NONCE_LEN)

/* KEK 派生的域分隔串。两侧必须一致，否则同一台设备上 TA 与普通世界
 * 会派生出两把不同的 KEK，而症状同样是"认证失败"。 */
#define PWRP_KEK_LABEL      "pqc-hsm/storage-kek"

/* ---- 小端编解码（显式逐字节，不依赖主机字节序）---- */
static inline void pwrp_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void pwrp_put_u32(uint8_t *p, uint32_t v)
{
	int i;

	for (i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static inline uint16_t pwrp_get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t pwrp_get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	int      i;

	for (i = 0; i < 4; i++) {
		v |= (uint32_t)p[i] << (8 * i);
	}
	return v;
}

/* 组装头部 16 字节。两侧都必须用它，别再手写 memcpy(magic) + put_u16。 */
static inline void pwrp_hdr_put(uint8_t hdr[PWRP_HDR_LEN],
                                uint32_t aad_len, uint32_t pt_len)
{
	hdr[PWRP_OFF_MAGIC + 0] = 'P';
	hdr[PWRP_OFF_MAGIC + 1] = 'W';
	hdr[PWRP_OFF_MAGIC + 2] = 'R';
	hdr[PWRP_OFF_MAGIC + 3] = 'P';
	pwrp_put_u16(hdr + PWRP_OFF_VER, PWRP_VERSION);
	pwrp_put_u16(hdr + PWRP_OFF_ALG, PWRP_ALG_AES256GCM);
	pwrp_put_u32(hdr + PWRP_OFF_AADLEN, aad_len);
	pwrp_put_u32(hdr + PWRP_OFF_PTLEN, pt_len);
}

/* 校验并取出头部字段。返回 0 = 头部合法。
 *
 * ⚠️ 它**不**校验 aad_len 与调用方给的是否一致，也不校验 pt_len 与 blob
 *    实际长度是否吻合 —— 那两条要在调用方那里判，因为只有它知道自己
 *    给了多长的 aad、收到了多长的 blob。这里只管"这几个字节是不是一个
 *    我们认得的 PWRP 头"。 */
static inline int pwrp_hdr_parse(const uint8_t *hdr,
                                 uint32_t *aad_len, uint32_t *pt_len)
{
	if (hdr[PWRP_OFF_MAGIC + 0] != 'P' || hdr[PWRP_OFF_MAGIC + 1] != 'W' ||
	    hdr[PWRP_OFF_MAGIC + 2] != 'R' || hdr[PWRP_OFF_MAGIC + 3] != 'P') {
		return -1;
	}
	if (pwrp_get_u16(hdr + PWRP_OFF_VER) != PWRP_VERSION) {
		return -1;
	}
	if (pwrp_get_u16(hdr + PWRP_OFF_ALG) != PWRP_ALG_AES256GCM) {
		return -1;
	}
	if (aad_len) {
		*aad_len = pwrp_get_u32(hdr + PWRP_OFF_AADLEN);
	}
	if (pt_len) {
		*pt_len = pwrp_get_u32(hdr + PWRP_OFF_PTLEN);
	}
	return 0;
}

#ifdef __cplusplus
}
#endif
#endif /* PQCHSM_PWRP_FORMAT_H */
