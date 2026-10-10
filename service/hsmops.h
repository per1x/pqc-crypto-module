/* hsmops.h —— 密码机业务操作（密钥库 + 加解密 / 签名 / 封装 …）
 *
 * hsmctl（命令行）与 hsm_httpd（HTTPS 网关）共用这一份。两边只是外壳：
 * 一个把结果打到终端，一个把结果包成 JSON —— 业务语义只在这里写一次，
 * 免得"命令行能做、HTTP 做不了"或两边对同一把密钥的理解不一致。
 *
 * 和 sdf_demo 一样**不做任何密码运算**：一切经 libsdfe 交给密码机
 * （密钥在 TEE、运算在 FPGA）。这里只管密钥库文件与 PKCS#7 填充。
 *
 * 密钥库：$HSM_KEYSTORE（默认 ./keystore），一把密钥一个 <名字>.key（0600）。
 * 文件里**没有明文密钥**：对称 = TA 包裹的 blob；后量子 = 公钥 + TA 包裹的种子 blob。
 *
 * 返回值约定：0 成功；HSM_NO（2）业务上的"否"（验签不过）；-1 出错（hsm_err() 给原因）。
 * 非线程安全（一个进程一个会话，daemon 那侧本来就串行）。
 */
#ifndef PQCHSM_HSMOPS_H
#define PQCHSM_HSMOPS_H

#include <stdint.h>
#include <stddef.h>

#define HSM_NO        2
#define HSM_KIND_MLKEM 0
#define HSM_KIND_MLDSA 1
#define HSM_KIND_SYM   2

typedef struct {
	uint8_t  kind, param;     /* param：ML-KEM/ML-DSA 的 pset，或 SDFE_ALG_* */
	uint16_t sym_len;
	uint32_t created;
	uint32_t pk_len, blob_len;
	uint8_t  pk[2592];
	uint8_t  blob[256];
} hsm_key;

const char *hsm_err(void);
const char *hsm_keystore(void);
void        hsm_close(void);                 /* 结束会话（daemon 随即擦 PL 里的残留） */

/* 名字只许 [A-Za-z0-9_-]，1..32 个字符 —— HTTP 那侧名字来自 URL，
 * 不在这里卡住就是一个现成的路径穿越。 */
int  hsm_name_ok(const char *name);

const char *hsm_alg_name(const hsm_key *k);
int  hsm_key_gen(const char *alg, const char *name, hsm_key *out);
int  hsm_key_load(const char *name, hsm_key *k);
int  hsm_key_destroy(const char *name);
/* 逐把回调；返回把数，出错 -1 */
int  hsm_key_list(void (*cb)(const char *name, const hsm_key *k, void *u), void *u);

/* 对称：CBC + PKCS#7，IV 取自 PL TRNG；密文 = IV(16) ‖ 数据。*out 由调用方 free */
int  hsm_encrypt(const char *name, const uint8_t *in, uint32_t n, uint8_t **out, uint32_t *outlen);
int  hsm_decrypt(const char *name, const uint8_t *in, uint32_t n, uint8_t **out, uint32_t *outlen);

int  hsm_sign(const char *name, const uint8_t *m, uint32_t n, uint8_t *sig, uint32_t *siglen);
int  hsm_verify(const char *name, const uint8_t *m, uint32_t n, const uint8_t *sig, uint32_t sl);
int  hsm_encap(const char *name, uint8_t *ct, uint32_t *ctlen, uint8_t ss[32]);
int  hsm_decap(const char *name, const uint8_t *ct, uint32_t ctlen, uint8_t ss[32]);

int  hsm_sm3(const uint8_t *d, uint32_t n, uint8_t out[32]);
int  hsm_random(uint32_t n, uint8_t *out);
int  hsm_info(char *buf, size_t cap);

#endif
