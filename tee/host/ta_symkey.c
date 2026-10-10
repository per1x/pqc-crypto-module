/* ta_symkey —— D7：TA 的对称密钥装载在真硅上走一遍（正反两面）
 *
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -I../include -o ta_symkey \
 *       ta_symkey.c pqchsm_ta_client.c -I<optee_client>/public -lteec
 *
 * ============================================================================
 * 【证什么，不证什么】
 * ============================================================================
 * 证：TA 那三条命令（SYMKEY_NEW / LOAD / KAT）的**接口纪律**在真 TA 上成立 ——
 *   · 正向：生成 → blob；blob 重装；KAT 进槽 7；
 *   · 反向：槽 7 不收产品密钥、产品槽不收 KAT 密钥、篡改 blob / 种子 blob /
 *     非法长度 / 非法编号一律拒。**反向那几条成功反而是故障。**
 * 不证：密钥装进 PL 之后算得对不对 —— 那由 sdf_demo [5]/[7] 用标准向量对
 *   （本程序在普通世界，读不到金库，也不该读得到）。
 *
 * ⚠️ TA 会经 PTA → EL3 访问 PL。PL 不在时 EL3 上的取数错误没有人接得住，
 *    所以先查 fpga_manager，不是 operating 就不发第一条命令。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pqchsm_ta_client.h"
#include "pqchsm_ta_proto.h"

#define TEE_ERROR_ACCESS_DENIED_   0xFFFF0001u
#define TEE_ERROR_BAD_PARAMETERS_  0xFFFF0006u
#define TEE_ERROR_MAC_INVALID_     0xFFFF3071u

static int fail;

static void expect_ok(const char *what, int rc)
{
	if (rc) { printf("  ✗ %s：0x%08x\n", what, (unsigned)rc); fail = 1; }
	else      printf("  ✓ %s\n", what);
}

/* 反向：必须被拒。期望码给 0 表示"任何非 0 都算拒"。 */
static void expect_reject(const char *what, int rc, uint32_t want)
{
	if (rc == 0) {
		printf("  ✗ %s：**被接受了** —— 这是故障\n", what);
		fail = 1;
	} else if (want && (uint32_t)rc != want) {
		printf("  ✗ %s：拒了，但码是 0x%08x（期望 0x%08x）\n", what,
		       (unsigned)rc, (unsigned)want);
		fail = 1;
	} else {
		printf("  ✓ %s：被拒（0x%08x）\n", what, (unsigned)rc);
	}
}

static int pl_operating(void)
{
	char st[64] = { 0 };
	FILE *f = fopen("/sys/class/fpga_manager/fpga0/state", "r");

	if (!f)
		return 0;
	if (!fgets(st, sizeof st, f))
		st[0] = 0;
	fclose(f);
	return strstr(st, "operating") != NULL;
}

int main(void)
{
	static const uint8_t salt[16] = {
		'p','q','c','h','s','m','-','b','2','-','k','e','k','-','v','1'
	};
	pqchsm_ta t;
	uint8_t blob[256], bad_blob[256];
	size_t blen = 0, i;
	uint32_t ver = 0, feat = 0;

	printf("=== D7：TA 对称密钥装载（正反两面）===\n");
	if (!pl_operating()) {
		printf("✗ PL 不是 operating —— 不发任何会经 EL3 碰 PL 的命令\n");
		return 2;
	}
	memset(&t, 0, sizeof t);
	if (pqchsm_ta_open(&t)) {
		printf("✗ 打不开 TA 会话\n");
		return 2;
	}
	if (pqchsm_ta_get_info(&t, &ver, &feat) == 0 && (feat & 0x10u))
		printf("  ✓ GET_INFO 特性位 0x%02x 含 bit4（对称装载）—— 板上是 D7 那份 TA\n",
		       (unsigned)feat);
	else {
		printf("  ✗ GET_INFO 特性位 0x%02x 没有 bit4 —— 板上的 TA 不是 D7 那份\n",
		       (unsigned)feat);
		pqchsm_ta_close(&t);
		return 3;
	}
	expect_ok("KEK_SET（与 daemon 同一个 salt）",
		  pqchsm_ta_kek_set(&t, salt, sizeof salt));

	printf("\n[正向]\n");
	expect_ok("SYMKEY_NEW 槽 3、16 字节 → blob",
		  pqchsm_ta_symkey_new(&t, 3, 16, blob, sizeof blob, &blen));
	printf("    blob %zu 字节（应为 16+16+12+16 = 60）\n", blen);
	if (blen != 60) { printf("  ✗ blob 长度不对\n"); fail = 1; }
	expect_ok("SYMKEY_LOAD 同一 blob → 槽 4",
		  pqchsm_ta_symkey_load(&t, 4, blob, blen));
	expect_ok("SYMKEY_KAT 槽 7、SM4 GB/T 32907",
		  pqchsm_ta_symkey_kat(&t, TA_SYM_KAT_SLOT, TA_SYM_KAT_SM4_GBT32907));
	{
		uint8_t b32[256]; size_t l32 = 0;
		expect_ok("SYMKEY_NEW 槽 5、32 字节",
			  pqchsm_ta_symkey_new(&t, 5, 32, b32, sizeof b32, &l32));
	}

	printf("\n[反向：槽位纪律]\n");
	{
		uint8_t tmp[256]; size_t lt = 0;
		expect_reject("SYMKEY_NEW 进槽 7（KAT 专用）",
			      pqchsm_ta_symkey_new(&t, 7, 16, tmp, sizeof tmp, &lt),
			      TEE_ERROR_BAD_PARAMETERS_);
		expect_reject("SYMKEY_NEW 长度 24",
			      pqchsm_ta_symkey_new(&t, 3, 24, tmp, sizeof tmp, &lt),
			      TEE_ERROR_BAD_PARAMETERS_);
	}
	expect_reject("SYMKEY_LOAD 进槽 7（KAT 专用）",
		      pqchsm_ta_symkey_load(&t, 7, blob, blen),
		      TEE_ERROR_BAD_PARAMETERS_);
	expect_reject("SYMKEY_KAT 进产品槽 0（已知密钥替换）",
		      pqchsm_ta_symkey_kat(&t, 0, TA_SYM_KAT_SM4_GBT32907),
		      TEE_ERROR_ACCESS_DENIED_);
	expect_reject("SYMKEY_KAT 编号 99",
		      pqchsm_ta_symkey_kat(&t, TA_SYM_KAT_SLOT, 99),
		      TEE_ERROR_BAD_PARAMETERS_);

	printf("\n[反向：blob]\n");
	memcpy(bad_blob, blob, blen);
	bad_blob[blen - 1] ^= 1;
	expect_reject("篡改过 tag 的 blob",
		      pqchsm_ta_symkey_load(&t, 3, bad_blob, blen),
		      TEE_ERROR_MAC_INVALID_);
	for (i = 0; i < blen; i++)
		bad_blob[i] = (uint8_t)(i * 7);
	expect_reject("伪造的 blob",
		      pqchsm_ta_symkey_load(&t, 3, bad_blob, blen), 0);
	/* 种子 blob（AAD 空）不能当对称密钥装：32 字节的 ξ 恰好是 AES-256 的长度 */
	{
		TEEC_Operation op;
		uint32_t origin = 0;
		uint8_t sblob[256];
		int rc;

		memset(&op, 0, sizeof op);
		op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INOUT,
						 TEEC_MEMREF_TEMP_OUTPUT,
						 TEEC_NONE, TEEC_NONE);
		op.params[0].value.a = TA_SEED_TGT_MLDSA;
		op.params[1].tmpref.buffer = sblob;
		op.params[1].tmpref.size   = sizeof sblob;
		rc = (int)TEEC_InvokeCommand(&t.sess, TA_PQCHSM_CMD_SEED_NEW,
					     &op, &origin);
		if (rc) {
			printf("  ? 拿不到种子 blob（0x%08x），跳过这一条（**不计通过**）\n",
			       (unsigned)rc);
		} else {
			expect_reject("ML-DSA 种子 blob 当对称密钥装",
				      pqchsm_ta_symkey_load(&t, 3, sblob,
							    op.params[1].tmpref.size),
				      TEE_ERROR_MAC_INVALID_);
		}
	}

	pqchsm_ta_close(&t);
	printf("\n%s\n", fail ? "ta_symkey: 有失败"
			      : "ta_symkey: 全部通过 —— 装载只走 TA，槽位纪律与 blob 域分隔在真 TA 上成立");
	return fail;
}
