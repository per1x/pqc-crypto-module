/* ta_keyops —— 批 2：TA 的**密钥操作**在真硅上端到端跑一遍
 *
 * ============================================================================
 * 【这个程序证的是什么，不证什么】
 * ============================================================================
 * ta_probe 证的是「TA 能加载、能建会话」。那只是门槛。这个程序再往前一步：
 * **TA 里的 PQC 真的算得对，而且私钥全程没有以明文出现在普通世界。**
 *
 * 两条链，各自的判据都不是"没报错"，而是**用另一侧独立算出来的东西对上**：
 *
 *   ML-DSA：TA KeyGen → TA Sign → **普通世界用 mldsa-native 验签**。
 *           验得过就说明 TA 里那把私钥与它交出来的公钥是真的一对，
 *           而且签名算法实现正确。再把签名改一个字节，必须验不过 ——
 *           少了这一半，一个"永远返回验证通过"的验签器也能让上一半通过。
 *
 *   ML-KEM：TA KeyGen → **普通世界用 mlkem-native 对着公钥 Encaps** →
 *           TA Decaps → 两边的共享密钥必须逐字节相同。
 *
 * 私钥在普通世界这一侧**只以 PWRP blob 的形式存在**（TA 内 KEK 包裹，
 * KEK 不出 TA）。这个程序从头到尾没有一个能放明文私钥的缓冲区 —— 这不是
 * 疏忽，是判据的一部分：接口形状本身就该让"把私钥拉回来"写不出来。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pqchsm_ta_client.h"
#include "pqchsm_ta_proto.h"

/* 普通世界这一侧的独立实现：与 TA 用的是**同一份 vendored 源码**
 * （third_party/pqc-native），但编在这个进程里、跑在普通世界。
 *
 * ⚠️ 说清楚这一点的边界：同源意味着它验不出"算法实现本身有共同的错"。
 *    它能验的是**另一件同样要紧的事** —— TA 交出来的 pk 与它内部那把私钥
 *    确实是一对，签名/解封装的输入输出接得上。算法本身对不对由 ACVP 向量
 *    钉着（tee/tests 的 test_pqc 与板上 hsm_kem3 都在跑）。 */
#include "ta_pqc.h"

static int fail;

static void bad(const char *m)
{
	printf("    ✗ %s\n", m);
	fail = 1;
}

int main(void)
{
	pqchsm_ta t;
	uint8_t salt[16];
	int i;

	memset(&t, 0, sizeof t);
	for (i = 0; i < 16; i++)
		salt[i] = (uint8_t)(i * 17 + 3);

	printf("=== 批 2：TA 密钥操作上板端到端 ===\n");

	if (pqchsm_ta_open(&t)) {
		printf("✗ 打不开 TA 会话（TA 在 /lib/optee_armtz/ 吗？"
		       "tee-supplicant 在跑吗？）\n");
		return 2;
	}
	printf("✓ TA 会话已建立\n");

	if (pqchsm_ta_kek_set(&t, salt, sizeof salt)) {
		printf("✗ CMD_KEK_SET 失败 —— 后面的 KeyGen 没有 KEK 可用\n");
		pqchsm_ta_close(&t);
		return 3;
	}
	printf("✓ CMD_KEK_SET —— KEK 已在 TA 内派生并缓存（它没有出口）\n");

	/* ---------------- ML-DSA-44：签 → 普通世界独立验 ---------------- */
	{
		const ta_pqc_dims_t *d = ta_pqc_dims(TA_ALG_ML_DSA_44);
		uint8_t pk[2592];
		uint8_t blob[4096];
		uint8_t sig[5000];
		size_t  blob_len = 0, sig_len = 0;
		const uint8_t msg[] = "batch2-on-board";
		int rc;

		printf("\n[ML-DSA-44] TA KeyGen → TA Sign → 普通世界验签\n");
		if (pqchsm_ta_keygen(&t, TA_ALG_ML_DSA_44, pk,
				     blob, sizeof blob, &blob_len)) {
			bad("CMD_KEYGEN 失败");
			goto kem;
		}
		printf("    ✓ KeyGen：pk %u 字节，私钥以 PWRP blob 返回 %u 字节"
		       "（明文私钥一个字节都没出 TA）\n",
		       (unsigned)d->pk_len, (unsigned)blob_len);

		if (pqchsm_ta_sign(&t, TA_ALG_ML_DSA_44, blob, blob_len,
				   NULL, 0, msg, sizeof msg - 1,
				   sig, sizeof sig, &sig_len)) {
			bad("CMD_SIGN 失败");
			goto kem;
		}
		printf("    ✓ Sign：σ %u 字节\n", (unsigned)sig_len);

		rc = ta_pqc_verify(TA_ALG_ML_DSA_44, pk, msg, sizeof msg - 1,
				   NULL, 0, sig, sig_len);
		if (rc != 0) {
			bad("普通世界验签**没通过** —— TA 交出来的 pk 与它内部那把"
			    "私钥不是一对，或签名算法不对");
			goto kem;
		}
		printf("    ✓ 普通世界用 mldsa-native 独立验签通过\n");

		/* 反证：改一个字节必须验不过 */
		sig[sig_len / 2] ^= 0x01;
		rc = ta_pqc_verify(TA_ALG_ML_DSA_44, pk, msg, sizeof msg - 1,
				   NULL, 0, sig, sig_len);
		if (rc == 0)
			bad("改了一个字节还验得过 —— 验签器本身是坏的，"
			    "上面那条'通过'不算数");
		else
			printf("    ✓ 改一字节即验不过（验签器本身是有效的）\n");
	}

kem:
	/* ---------------- ML-KEM-768：普通世界 Encaps → TA Decaps ---------------- */
	{
const ta_pqc_dims_t *d = ta_pqc_dims(TA_ALG_ML_KEM_768);
		uint8_t pk[1600];
		uint8_t blob[4096];
uint8_t ct[1200];
uint8_t ss_host[32];
		uint8_t ss_ta[32];
		size_t  blob_len = 0;

		printf("\n[ML-KEM-768] TA KeyGen → 普通世界 Encaps → TA Decaps\n");
		if (pqchsm_ta_keygen(&t, TA_ALG_ML_KEM_768, pk,
				     blob, sizeof blob, &blob_len)) {
			bad("CMD_KEYGEN 失败");
			goto done;
		}
		printf("    ✓ KeyGen：pk %u 字节，blob %u 字节\n",
		       (unsigned)d->pk_len, (unsigned)blob_len);

		if (ta_pqc_encaps(TA_ALG_ML_KEM_768, pk, ct, ss_host) != 0) {
			bad("普通世界 Encaps 失败");
			goto done;
		}
		printf("    ✓ 普通世界对着这把 pk 做了 Encaps\n");

		if (pqchsm_ta_decaps(&t, TA_ALG_ML_KEM_768, blob, blob_len,
				     ct, d->ct_len, ss_ta, sizeof ss_ta)) {
			bad("CMD_DECAPS 失败");
			goto done;
		}
		if (memcmp(ss_host, ss_ta, sizeof ss_ta) != 0)
			bad("两边的共享密钥**不一致** —— TA 解封装出来的不是同一个");
		else
			printf("    ✓ 两边共享密钥逐字节一致 —— "
			       "一次完整的 KEM 在 TA 里跑通\n");
	}

done:
	pqchsm_ta_close(&t);
	printf("\n%s\n", fail ? "ta_keyops: 有失败"
			      : "ta_keyops: 全部通过 —— TA 里的 PQC 算得对，"
				"且明文私钥从未出过安全世界");
	return fail;
}
