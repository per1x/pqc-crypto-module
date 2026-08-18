/* mechs —— 列出密码机支持的算法（PKCS#11 标准问法）
 *
 * 「你支持什么算法」在 PKCS#11 里有标准答案：C_GetMechanismList 拿机制表，
 * C_GetMechanismInfo 拿每个机制的密钥长度范围和用途标志。这比读一份文档强，
 * 因为它问的是**这个模块自己**，不是问它的说明书 —— 两者不一致时，这里说了算。
 *
 * 它只 dlopen 模块，不链接任何密码库。
 *
 *   cc -O2 -I third_party/pkcs11-v3.2 -I src/p11 -o mechs demo/functions/mechs.c
 *   ./mechs <pqchsm-pkcs11.so|dylib>
 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "p11_config.h"
#include "pkcs11.h"

/* 机制码 → 人话。表里没有的按 0x%lx 原样打出来 —— 宁可打个数字，
 * 也不要把不认识的机制悄悄漏掉：这份清单的意义就在于它是完整的。 */
static const char *mech_name(CK_MECHANISM_TYPE m)
{
	switch (m) {
	case CKM_ML_KEM_KEY_PAIR_GEN: return "CKM_ML_KEM_KEY_PAIR_GEN";
	case CKM_ML_KEM:              return "CKM_ML_KEM";
	case CKM_ML_DSA_KEY_PAIR_GEN: return "CKM_ML_DSA_KEY_PAIR_GEN";
	case CKM_ML_DSA:              return "CKM_ML_DSA";
	case CKM_AES_GCM:             return "CKM_AES_GCM";
	default:                      return NULL;
	}
}

/* 用途标志。一个机制"能做什么"是查得到的，不必看文档猜。 */
static void print_flags(CK_FLAGS f)
{
	static const struct { CK_FLAGS bit; const char *name; } T[] = {
		{ CKF_HW,          "硬件" },
		{ CKF_ENCRYPT,     "加密" },
		{ CKF_DECRYPT,     "解密" },
		{ CKF_DIGEST,      "摘要" },
		{ CKF_SIGN,        "签名" },
		{ CKF_VERIFY,      "验签" },
		{ CKF_GENERATE,    "生成密钥" },
		{ CKF_GENERATE_KEY_PAIR, "生成密钥对" },
		{ CKF_WRAP,        "包裹" },
		{ CKF_UNWRAP,      "解包裹" },
		{ CKF_ENCAPSULATE, "封装" },
		{ CKF_DECAPSULATE, "解封装" },
	};
	int first = 1;
	for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
		if (!(f & T[i].bit)) continue;
		printf("%s%s", first ? "" : "、", T[i].name);
		first = 0;
	}
	if (first) printf("（无）");
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "用法: %s <PKCS#11 模块>\n", argv[0]); return 2; }

	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "打不开模块: %s\n", dlerror()); return 1; }

	CK_C_GetFunctionList get = (CK_C_GetFunctionList)dlsym(h, "C_GetFunctionList");
	if (!get) { fprintf(stderr, "模块里没有 C_GetFunctionList\n"); return 1; }

	CK_FUNCTION_LIST_PTR F;
	if (get(&F) != CKR_OK || F->C_Initialize(NULL) != CKR_OK) {
		fprintf(stderr, "C_Initialize 失败\n"); return 1;
	}

	CK_INFO info;
	if (F->C_GetInfo(&info) == CKR_OK) {
		printf("  模块  %.32s\n", info.libraryDescription);
		printf("  厂商  %.32s\n", info.manufacturerID);
		printf("  版本  PKCS#11 %d.%d\n",
		       info.cryptokiVersion.major, info.cryptokiVersion.minor);
	}

	CK_SLOT_ID slots[16];
	CK_ULONG ns = 16;
	if (F->C_GetSlotList(CK_TRUE, slots, &ns) != CKR_OK) {
		fprintf(stderr, "取槽位表失败\n"); return 1;
	}
	printf("  槽位  %lu 个\n\n", (unsigned long)ns);
	if (ns == 0) return 0;

	CK_MECHANISM_TYPE mechs[128];
	CK_ULONG nm = sizeof mechs / sizeof mechs[0];
	if (F->C_GetMechanismList(slots[0], mechs, &nm) != CKR_OK) {
		fprintf(stderr, "取机制表失败\n"); return 1;
	}

	printf("  支持 %lu 个机制：\n", (unsigned long)nm);
	for (CK_ULONG i = 0; i < nm; i++) {
		CK_MECHANISM_INFO mi;
		memset(&mi, 0, sizeof mi);
		F->C_GetMechanismInfo(slots[0], mechs[i], &mi);
		const char *n = mech_name(mechs[i]);
		if (n) printf("    %-24s", n);
		else   printf("    0x%08lx%14s", (unsigned long)mechs[i], "");
		if (mi.ulMaxKeySize) printf("  密钥 %lu..%lu 字节  ",
		                            (unsigned long)mi.ulMinKeySize,
		                            (unsigned long)mi.ulMaxKeySize);
		else printf("  ");
		print_flags(mi.flags);
		printf("\n");
	}

	F->C_Finalize(NULL);
	return 0;
}
