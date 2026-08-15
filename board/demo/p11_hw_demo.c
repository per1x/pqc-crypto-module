/* p11_hw_demo —— 用**标准 PKCS#11 接口**驱动 FPGA 密码机
 *
 * ============================================================================
 * 【它证明的那件事，是前面几段证不了的】
 * ============================================================================
 * run_demo.sh 的第 ① 段用的是 SDF 接口（SDFE_*），那是国密惯用的一套。
 * 而 PKCS#11 是**跨厂商的工业标准接口** —— 一个不认识这块板的应用，
 * 拿一个标准 C_* 调用过来，密钥就在 FPGA 里生成、私钥永不离开硬件。
 *
 * 具体证三件事：
 *   ① C_GenerateKeyPair 生成 ML-KEM 密钥对 —— 私钥进 PL 的片内金库，
 *      应用只拿到公钥和一个对象句柄，dk 连一个字节都没经过软件；
 *   ② C_Encapsulate（用公钥）→ C_Decapsulate（用句柄）两端共享密钥一致；
 *   ③ 私钥对象的 CKA_EXTRACTABLE 是 false —— 想导出会被拒。
 *
 * 后端由环境变量选：PQCHSM_BACKEND=sdfe 打开 FPGA 路径。不设就是软件后端，
 * 同一段代码、同样通过 —— 这正是"标准接口"的意思：应用不需要知道底下是谁。
 *
 * 编译（不需要 liboqs/OpenSSL，纯 dlopen）：
 *   cc -I third_party/pkcs11-v3.2 -I src/p11 -o p11_hw_demo p11_hw_demo.c
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p11_config.h"
#include "pkcs11.h"

static CK_FUNCTION_LIST_PTR F;
static CK_FUNCTION_LIST_3_2_PTR F32;   /* 3.2 表：Encap/Decap 只在这里 */

static int die(const char *m, CK_RV rv)
{
	fprintf(stderr, "  ✗ %s（rv=0x%lx）\n", m, (unsigned long)rv);
	return 1;
}

int main(int argc, char **argv)
{
	void *h;
	CK_RV (*getlist)(CK_FUNCTION_LIST_PTR_PTR);
	CK_SESSION_HANDLE s;
	CK_SLOT_ID slots[8];
	CK_ULONG n = 8;
	CK_RV rv;

	if (argc < 2) { fprintf(stderr, "用法: %s <模块.so/.dylib>\n", argv[0]); return 2; }
	h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
	getlist = dlsym(h, "C_GetFunctionList");
	if (!getlist || getlist(&F) != CKR_OK) return die("C_GetFunctionList", 0);

	/* Encapsulate/Decapsulate 是 PKCS#11 3.2 新增的，2.40 的 CK_FUNCTION_LIST
	 * 结构里根本没有这两个字段 —— 要经 C_GetInterface 拿 3.2 表。 */
	{
		CK_RV (*get_if)(CK_UTF8CHAR_PTR, CK_VERSION_PTR,
		                CK_INTERFACE_PTR_PTR, CK_FLAGS) = dlsym(h, "C_GetInterface");
		CK_INTERFACE_PTR iface = NULL;
		if (!get_if || get_if(NULL, NULL, &iface, 0) != CKR_OK || !iface)
			return die("C_GetInterface", 0);
		F32 = (CK_FUNCTION_LIST_3_2_PTR)iface->pFunctionList;
	}

	if ((rv = F->C_Initialize(NULL)) != CKR_OK) return die("C_Initialize", rv);
	if (F->C_GetSlotList(CK_TRUE, slots, &n) != CKR_OK || n == 0)
		return die("C_GetSlotList", 0);

	/* 初始化 + 登录：标准的 SO→设 user PIN→USER 流程 */
	F->C_InitToken(slots[0], (CK_UTF8CHAR *)"12345678", 8, (CK_UTF8CHAR *)"demo");
	if (F->C_OpenSession(slots[0], CKF_SERIAL_SESSION | CKF_RW_SESSION,
			     NULL, NULL, &s) != CKR_OK) return die("OpenSession", 0);
	F->C_Login(s, CKU_SO, (CK_UTF8CHAR *)"12345678", 8);
	F->C_InitPIN(s, (CK_UTF8CHAR *)"87654321", 8);
	F->C_Logout(s);
	F->C_Login(s, CKU_USER, (CK_UTF8CHAR *)"87654321", 8);

	const char *be = getenv("PQCHSM_BACKEND");
	int hw = be && !strcmp(be, "sdfe");
	printf("=== 标准 PKCS#11 接口 → %s ===\n\n",
	       hw ? "FPGA 密码机" : "软件后端（未设 PQCHSM_BACKEND=sdfe）");

	/* ① C_GenerateKeyPair(ML-KEM-768) */
	CK_MECHANISM mech = { CKM_ML_KEM_KEY_PAIR_GEN, NULL, 0 };
	CK_ULONG pset = CKP_ML_KEM_768;
	CK_BBOOL yes = CK_TRUE, no = CK_FALSE;
	CK_ATTRIBUTE pub[] = {
		{ CKA_PARAMETER_SET, &pset, sizeof pset },
		{ CKA_ENCAPSULATE, &yes, sizeof yes },
	};
	CK_ATTRIBUTE prv[] = {
		{ CKA_PARAMETER_SET, &pset, sizeof pset },
		{ CKA_DECAPSULATE, &yes, sizeof yes },
		{ CKA_EXTRACTABLE, &no, sizeof no },
	};
	CK_OBJECT_HANDLE pk, sk;
	if ((rv = F->C_GenerateKeyPair(s, &mech, pub, 2, prv, 3, &pk, &sk)) != CKR_OK)
		return die("C_GenerateKeyPair", rv);
	printf("① C_GenerateKeyPair(ML-KEM-768)\n");
	printf("   公钥对象 %lu，私钥对象 %lu\n", (unsigned long)pk, (unsigned long)sk);
	printf("   ← %s\n\n",
	       hw ? "私钥在 FPGA 片内金库，应用只拿到句柄"
	          : "私钥在软件后端内存里（本机演示逻辑用）");

	/* ② 私钥不可导出：读 CKA_VALUE 必须被拒 */
	CK_BYTE buf[64];
	CK_ATTRIBUTE q = { CKA_VALUE, buf, sizeof buf };
	rv = F->C_GetAttributeValue(s, sk, &q, 1);
	printf("② 想导出私钥（读 CKA_VALUE）→ rv=0x%lx %s\n\n",
	       (unsigned long)rv,
	       rv == CKR_OK ? "❌ 居然给了！" : "✅ 被拒，私钥不出硬件");
	if (rv == CKR_OK) return 1;

	/* ③ Encapsulate → Decapsulate 一致 */
	CK_MECHANISM km = { CKM_ML_KEM, NULL, 0 };
	CK_BYTE ct[1200], ss1[32], ss2[32];
	CK_ULONG ctlen = sizeof ct, ss1len = sizeof ss1, ss2len = sizeof ss2;
	CK_OBJECT_HANDLE ssobj1, ssobj2;

	/* 共享密钥默认是**不可导出的敏感对象** —— 这才是对的：它是会话密钥，
	 * 本不该以明文出接口。这里为了演示能把两端拿出来比一比，显式在模板里
	 * 要求 EXTRACTABLE=true / SENSITIVE=false。生产里不会这么做。 */
	CK_ATTRIBUTE readable[] = {
		{ CKA_EXTRACTABLE, &yes, sizeof yes },
		{ CKA_SENSITIVE, &no, sizeof no },
	};
	rv = F32->C_EncapsulateKey(s, &km, pk, readable, 2, ct, &ctlen, &ssobj1);
	if (rv != CKR_OK) return die("C_EncapsulateKey", rv);
	rv = F32->C_DecapsulateKey(s, &km, sk, readable, 2, ct, ctlen, &ssobj2);
	if (rv != CKR_OK) return die("C_DecapsulateKey", rv);

	CK_ATTRIBUTE g1 = { CKA_VALUE, ss1, sizeof ss1 };
	CK_ATTRIBUTE g2 = { CKA_VALUE, ss2, sizeof ss2 };
	F->C_GetAttributeValue(s, ssobj1, &g1, 1);
	F->C_GetAttributeValue(s, ssobj2, &g2, 1);
	ss1len = g1.ulValueLen; ss2len = g2.ulValueLen;
	printf("③ C_Encapsulate（公钥）→ C_Decapsulate（句柄）\n");
	printf("   %s（%lu / %lu 字节）\n",
	       (ss1len == ss2len && !memcmp(ss1, ss2, ss1len))
	         ? (hw ? "✅ 两端共享密钥一致 —— 完整 KEM，全程私钥没离开 FPGA"
	            : "✅ 两端共享密钥一致 —— 完整 KEM（软件后端）")
	         : "❌ 不一致",
	       (unsigned long)ss1len, (unsigned long)ss2len);

	F->C_CloseSession(s);
	F->C_Finalize(NULL);
	return (ss1len == ss2len && !memcmp(ss1, ss2, ss1len)) ? 0 : 1;
}
