/* roundtrip —— 算法**怎么被调用**：签名/验签的完整往返，以及它的反例
 *
 * ============================================================================
 * 【这个程序补的是哪一块】
 * ============================================================================
 * mechs 回答"支持什么算法"，但支持不等于能用。这里走完整条调用链：
 *
 *   C_GenerateKeyPair → C_SignInit/C_Sign → C_VerifyInit/C_Verify
 *
 * 并且**把消息改一个 bit 再验一次** —— 那一次必须失败。只演"验过了"证明不了
 * 什么：一个永远返回 true 的验签函数也能通过。**反例才是判据。**
 *
 * ML-KEM 那半（封装/解封装 + KEM-DEM）由 board/demo/p11_hw_demo.c 覆盖，
 * 两个程序合起来才是完整的算法调用面。
 *
 * 它只 dlopen 模块，不链接任何密码库 —— 所以它打印的"验签通过"只可能来自
 * 模块自己。
 *
 *   cc -O2 -I third_party/pkcs11-v3.2 -I src/p11 -o roundtrip demo/functions/roundtrip.c
 *   PQCHSM_KEYSTORE=/tmp/ks.bin ./roundtrip <模块>
 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "p11_config.h"
#include "pkcs11.h"

static CK_FUNCTION_LIST_PTR F;

static int fail(const char *what, CK_RV rv)
{
	fprintf(stderr, "  ✗ %s（rv=0x%lx）\n", what, (unsigned long)rv);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "用法: %s <PKCS#11 模块>\n", argv[0]); return 2; }

	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "打不开模块: %s\n", dlerror()); return 1; }
	CK_C_GetFunctionList get = (CK_C_GetFunctionList)dlsym(h, "C_GetFunctionList");
	CK_RV rv;
	if (!get || get(&F) != CKR_OK) return fail("C_GetFunctionList", 0);
	if ((rv = F->C_Initialize(NULL)) != CKR_OK) return fail("C_Initialize", rv);

	CK_SLOT_ID slots[8];
	CK_ULONG ns = 8;
	if ((rv = F->C_GetSlotList(CK_TRUE, slots, &ns)) != CKR_OK || ns == 0)
		return fail("C_GetSlotList", rv);

	static CK_UTF8CHAR so_pin[]   = "12345678";
	static CK_UTF8CHAR user_pin[] = "87654321";
	static CK_UTF8CHAR label[32]  = "ROUNDTRIP                       ";

	if ((rv = F->C_InitToken(slots[0], so_pin, sizeof so_pin - 1, label)) != CKR_OK)
		return fail("C_InitToken", rv);

	CK_SESSION_HANDLE s;
	if ((rv = F->C_OpenSession(slots[0], CKF_SERIAL_SESSION | CKF_RW_SESSION,
	                           NULL, NULL, &s)) != CKR_OK)
		return fail("C_OpenSession", rv);
	if ((rv = F->C_Login(s, CKU_SO, so_pin, sizeof so_pin - 1)) != CKR_OK)
		return fail("C_Login(SO)", rv);
	if ((rv = F->C_InitPIN(s, user_pin, sizeof user_pin - 1)) != CKR_OK)
		return fail("C_InitPIN", rv);
	F->C_Logout(s);
	if ((rv = F->C_Login(s, CKU_USER, user_pin, sizeof user_pin - 1)) != CKR_OK)
		return fail("C_Login(USER)", rv);

	/* ① 生成 —— 参数集是一个**属性**，不是一个机制码。同一个
	 *    CKM_ML_DSA_KEY_PAIR_GEN 配不同的 CKA_PARAMETER_SET 出 44/65/87。 */
	printf("① C_GenerateKeyPair(CKM_ML_DSA_KEY_PAIR_GEN, CKA_PARAMETER_SET=ML-DSA-44)\n");
	CK_MECHANISM kg = { CKM_ML_DSA_KEY_PAIR_GEN, NULL, 0 };
	CK_ULONG pset = CKP_ML_DSA_44;
	CK_BBOOL yes = CK_TRUE, no = CK_FALSE;
	CK_ATTRIBUTE pub[] = {
		{ CKA_PARAMETER_SET, &pset, sizeof pset },
		{ CKA_VERIFY, &yes, sizeof yes },
	};
	CK_ATTRIBUTE prv[] = {
		{ CKA_PARAMETER_SET, &pset, sizeof pset },
		{ CKA_SIGN, &yes, sizeof yes },
		{ CKA_EXTRACTABLE, &no, sizeof no },
	};
	CK_OBJECT_HANDLE hpub, hprv;
	if ((rv = F->C_GenerateKeyPair(s, &kg, pub, 2, prv, 3, &hpub, &hprv)) != CKR_OK)
		return fail("C_GenerateKeyPair", rv);
	printf("   公钥对象 %lu，私钥对象 %lu\n\n",
	       (unsigned long)hpub, (unsigned long)hprv);

	/* ② 签名 */
	static CK_BYTE msg[] = "the module signs this, not the application";
	CK_MECHANISM sg = { CKM_ML_DSA, NULL, 0 };
	CK_BYTE sig[8192];
	CK_ULONG siglen = sizeof sig;
	printf("② C_SignInit/C_Sign —— 私钥只以对象句柄出现，应用碰不到它\n");
	if ((rv = F->C_SignInit(s, &sg, hprv)) != CKR_OK) return fail("C_SignInit", rv);
	if ((rv = F->C_Sign(s, msg, sizeof msg - 1, sig, &siglen)) != CKR_OK)
		return fail("C_Sign", rv);
	printf("   签名 %lu 字节（ML-DSA-44 应为 2420）%s\n\n",
	       (unsigned long)siglen, siglen == 2420 ? "  ✓" : "  ✗ 长度不对");

	/* ③ 验签 —— 正例 */
	printf("③ C_VerifyInit/C_Verify（原消息）\n");
	if ((rv = F->C_VerifyInit(s, &sg, hpub)) != CKR_OK) return fail("C_VerifyInit", rv);
	rv = F->C_Verify(s, msg, sizeof msg - 1, sig, siglen);
	if (rv != CKR_OK) return fail("C_Verify 应当通过却没通过", rv);
	printf("   ✓ 验签通过\n\n");

	/* ④ 验签 —— 反例。这一步才是判据：只演正例的话，一个恒返回 true 的
	 *    实现也能"通过"。 */
	printf("④ 把消息改一个 bit，再验一次 —— **必须失败**\n");
	CK_BYTE bad[sizeof msg];
	memcpy(bad, msg, sizeof msg);
	bad[0] ^= 0x01;
	if ((rv = F->C_VerifyInit(s, &sg, hpub)) != CKR_OK) return fail("C_VerifyInit", rv);
	rv = F->C_Verify(s, bad, sizeof msg - 1, sig, siglen);
	if (rv == CKR_OK) {
		fprintf(stderr, "   ✗ 篡改后居然验过了 —— 验签是假的\n");
		return 1;
	}
	printf("   ✓ 被拒（rv=0x%lx）—— 改一个 bit 就验不过\n\n", (unsigned long)rv);

	/* ⑤ 签名本身也不能改 */
	printf("⑤ 消息不动，改签名的一个 bit —— 同样必须失败\n");
	sig[siglen / 2] ^= 0x01;
	if ((rv = F->C_VerifyInit(s, &sg, hpub)) != CKR_OK) return fail("C_VerifyInit", rv);
	rv = F->C_Verify(s, msg, sizeof msg - 1, sig, siglen);
	if (rv == CKR_OK) {
		fprintf(stderr, "   ✗ 改了签名居然还验得过\n");
		return 1;
	}
	printf("   ✓ 被拒（rv=0x%lx）\n", (unsigned long)rv);

	F->C_Logout(s);
	F->C_CloseSession(s);
	F->C_Finalize(NULL);
	return 0;
}
