/* PKCS#11 前端测试：dlopen 真正的 .dylib，只经 C_GetFunctionList 拿到的函数表调用
 *
 * 为什么用 dlopen 而不是直接链接：这样测的是**发布出去的那个动态库**，
 * 包括符号导出、函数表填得对不对。直接链接会绕过这些。
 *
 * 分工：pkcs11-tool 能驱动的部分（init-token / init-pin / list-slots / list-objects）
 * 由 tools/p11_smoke.sh 覆盖；OpenSC 0.27 的 CLI 还不认 ML-DSA 密钥类型，
 * 所以 v3.2 的 PQC 流程（C_GenerateKeyPair / C_Sign / C_Verify）只能在这里驱动。
 */
#include "testlib.h"
#include "p11_config.h"
#include "pqchsm/pqc.h"

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static CK_FUNCTION_LIST_PTR F;
static CK_FUNCTION_LIST_3_2_PTR F32;   /* v3.2 表：C_EncapsulateKey 等只在这张表里 */
static char g_ks[160];

#define CKCHECK(expr, want) do {                                            \
	CK_RV _rv = (expr);                                                 \
	g_checks++;                                                         \
	if (_rv != (want)) {                                                \
		g_fails++;                                                  \
		fprintf(stderr, "FAIL %s:%d [%s] %s -> 0x%lx (期望 0x%lx)\n", \
		        __FILE__, __LINE__, g_case, #expr,                  \
		        (unsigned long)_rv, (unsigned long)(want));         \
	}                                                                   \
} while (0)

static CK_ATTRIBUTE mk_ulong(CK_ATTRIBUTE_TYPE t, CK_ULONG *v)
{
	CK_ATTRIBUTE a;
	a.type = t;
	a.pValue = v;
	a.ulValueLen = sizeof(*v);
	return a;
}

int main(void)
{
	snprintf(g_ks, sizeof(g_ks), "/tmp/pqchsm_p11_%d.ks", (int)getpid());
	unlink(g_ks);
	setenv("PQCHSM_KEYSTORE", g_ks, 1);
	setenv("PQCHSM_SLOTS", "4", 1);   /* 0/1/2 给原有用例，3 留给种子导入 */

	TCASE("dlopen 模块并取函数表");
	void *h = dlopen(PQCHSM_P11_MODULE, RTLD_NOW);
	if (!h) {
		fprintf(stderr, "dlopen 失败: %s\n", dlerror());
		return 1;
	}
	CK_RV (*get_list)(CK_FUNCTION_LIST_PTR_PTR) = dlsym(h, "C_GetFunctionList");
	CHECK(get_list != NULL);
	if (!get_list) {
		return 1;
	}
	CKCHECK(get_list(&F), CKR_OK);
	CHECK(F != NULL);
	CHECK_EQ_INT(F->version.major, 3);
	CHECK_EQ_INT(F->version.minor, 2);
	CHECK(F->C_GenerateKeyPair != NULL);
	CHECK(F->C_CreateObject != NULL);
	CHECK(F->C_SignUpdate != NULL);
	CHECK(F->C_SignFinal != NULL);
	/* 未实现的接口仍必须是 NULL 表项，而不是填一个返回错误的桩 */
	CHECK(F->C_Encrypt == NULL);
	CHECK(F->C_DeriveKey == NULL);

	TCASE("C_GetInterface：v3.2 表里才有 Encapsulate（2.40 表结构根本没这个字段）");
	{
		CK_RV (*get_iface)(CK_UTF8CHAR_PTR, CK_VERSION_PTR,
		                   CK_INTERFACE_PTR_PTR, CK_FLAGS) = dlsym(h, "C_GetInterface");
		CHECK(get_iface != NULL);
		CK_INTERFACE_PTR iface = NULL;
		CKCHECK(get_iface(NULL, NULL, &iface, 0), CKR_OK);
		CHECK(iface != NULL);
		if (iface) {
			CHECK(strcmp((const char *)iface->pInterfaceName, "PKCS 11") == 0);
			F32 = (CK_FUNCTION_LIST_3_2_PTR)iface->pFunctionList;
			CHECK_EQ_INT(F32->version.major, 3);
			CHECK_EQ_INT(F32->version.minor, 2);
			CHECK(F32->C_EncapsulateKey != NULL);
			CHECK(F32->C_DecapsulateKey != NULL);
			/* 两张表指向同一批实现 */
			CHECK(F32->C_Sign == F->C_Sign);
		}
		/* 名字写错要明确失败，不能悄悄给默认表 */
		CKCHECK(get_iface((CK_UTF8CHAR_PTR)"PKCS 12", NULL, &iface, 0), CKR_ARGUMENTS_BAD);
		/* fork-safe 我们做不到 —— 如实拒绝，不给做不到的承诺 */
		CKCHECK(get_iface(NULL, NULL, &iface, CKF_INTERFACE_FORK_SAFE), CKR_FUNCTION_FAILED);

		CK_ULONG ni = 0;
		CK_RV (*get_ifl)(CK_INTERFACE_PTR, CK_ULONG_PTR) = dlsym(h, "C_GetInterfaceList");
		CHECK(get_ifl != NULL);
		CKCHECK(get_ifl(NULL, &ni), CKR_OK);
		CHECK_EQ_INT(ni, 1);
	}

	TCASE("Initialize / GetInfo / 重复 Initialize");
	CKCHECK(F->C_Initialize(NULL), CKR_OK);
	CKCHECK(F->C_Initialize(NULL), CKR_CRYPTOKI_ALREADY_INITIALIZED);
	{
		CK_INFO info;
		CKCHECK(F->C_GetInfo(&info), CKR_OK);
		CHECK_EQ_INT(info.cryptokiVersion.major, 3);
		CHECK_EQ_INT(info.cryptokiVersion.minor, 2);
		CKCHECK(F->C_GetInfo(NULL), CKR_ARGUMENTS_BAD);
	}

	TCASE("GetSlotList：两段式查询");
	CK_SLOT_ID slots[8];
	CK_ULONG n = 0;
	CKCHECK(F->C_GetSlotList(CK_TRUE, NULL, &n), CKR_OK);
	CHECK_EQ_INT(n, 4);
	{
		CK_ULONG small = 1;
		CKCHECK(F->C_GetSlotList(CK_TRUE, slots, &small), CKR_BUFFER_TOO_SMALL);
		CHECK_EQ_INT(small, 4);
	}
	n = 8;
	CKCHECK(F->C_GetSlotList(CK_TRUE, slots, &n), CKR_OK);
	CHECK_EQ_INT(n, 4);
	CHECK_EQ_INT(slots[0], 0);

	TCASE("机制列表包含 ML-DSA 与 ML-KEM");
	{
		CK_MECHANISM_TYPE mechs[8];
		CK_ULONG mn = 8;
		CKCHECK(F->C_GetMechanismList(0, mechs, &mn), CKR_OK);
		CHECK_EQ_INT(mn, 4);
		int has_dsa = 0, has_kem = 0;
		for (CK_ULONG i = 0; i < mn; i++) {
			if (mechs[i] == CKM_ML_DSA) {
				has_dsa = 1;
			}
			if (mechs[i] == CKM_ML_KEM) {
				has_kem = 1;
			}
		}
		CHECK(has_dsa);
		CHECK(has_kem);
		CK_MECHANISM_INFO mi;
		CKCHECK(F->C_GetMechanismInfo(0, CKM_ML_DSA, &mi), CKR_OK);
		CHECK((mi.flags & CKF_SIGN) != 0);
		CHECK((mi.flags & CKF_VERIFY) != 0);
		CKCHECK(F->C_GetMechanismInfo(0, CKM_RSA_PKCS, &mi), CKR_MECHANISM_INVALID);
	}

	TCASE("InitToken / 未初始化槽位的 token 标志");
	{
		CK_TOKEN_INFO ti;
		CKCHECK(F->C_GetTokenInfo(0, &ti), CKR_OK);
		CHECK((ti.flags & CKF_TOKEN_INITIALIZED) == 0);
		CKCHECK(F->C_InitToken(0, (CK_UTF8CHAR_PTR)"12345678", 8,
		                       (CK_UTF8CHAR_PTR)"mytoken                         "), CKR_OK);
		CKCHECK(F->C_GetTokenInfo(0, &ti), CKR_OK);
		CHECK((ti.flags & CKF_TOKEN_INITIALIZED) != 0);
		CHECK(memcmp(ti.label, "mytoken", 7) == 0);
		/* 重复 InitToken 会毁掉全部内容，本实现拒绝 */
		CKCHECK(F->C_InitToken(0, (CK_UTF8CHAR_PTR)"12345678", 8, NULL),
		        CKR_ACTION_PROHIBITED);
		CKCHECK(F->C_GetTokenInfo(9, &ti), CKR_SLOT_ID_INVALID);
	}

	TCASE("会话与登录");
	CK_SESSION_HANDLE sess = 0;
	CKCHECK(F->C_OpenSession(0, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &sess),
	        CKR_OK);
	CHECK(sess != 0);
	/* 不带 CKF_SERIAL_SESSION 必须拒绝 */
	{
		CK_SESSION_HANDLE bad = 0;
		CKCHECK(F->C_OpenSession(0, 0, NULL, NULL, &bad),
		        CKR_SESSION_PARALLEL_NOT_SUPPORTED);
	}
	CKCHECK(F->C_Login(sess, CKU_SO, (CK_UTF8CHAR_PTR)"wrongpin", 8), CKR_PIN_INCORRECT);
	CKCHECK(F->C_Login(sess, CKU_SO, (CK_UTF8CHAR_PTR)"12345678", 8), CKR_OK);
	{
		CK_SESSION_INFO si;
		CKCHECK(F->C_GetSessionInfo(sess, &si), CKR_OK);
		CHECK_EQ_INT(si.state, CKS_RW_SO_FUNCTIONS);
	}
	CKCHECK(F->C_InitPIN(sess, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
	CKCHECK(F->C_Logout(sess), CKR_OK);
	CKCHECK(F->C_Login(sess, CKU_USER, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
	CKCHECK(F->C_Login(sess, 99, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_USER_TYPE_INVALID);
	CKCHECK(F->C_GetSessionInfo(999, NULL), CKR_SESSION_HANDLE_INVALID);

	TCASE("C_GenerateKeyPair（ML-DSA-65）");
	CK_OBJECT_HANDLE pub = 0, priv = 0;
	{
		CK_ULONG pset = CKP_ML_DSA_65;
		CK_ATTRIBUTE pubt[1] = { mk_ulong(CKA_PARAMETER_SET, &pset) };
		CK_MECHANISM mech = { CKM_ML_DSA_KEY_PAIR_GEN, NULL, 0 };
		/* 缺参数集 → 模板不完整 */
		CKCHECK(F->C_GenerateKeyPair(sess, &mech, NULL, 0, NULL, 0, &pub, &priv),
		        CKR_TEMPLATE_INCOMPLETE);
		/* 参数集非法 */
		{
			CK_ULONG bad = 99;
			CK_ATTRIBUTE bt[1] = { mk_ulong(CKA_PARAMETER_SET, &bad) };
			CKCHECK(F->C_GenerateKeyPair(sess, &mech, bt, 1, NULL, 0, &pub, &priv),
			        CKR_ATTRIBUTE_VALUE_INVALID);
		}
		CKCHECK(F->C_GenerateKeyPair(sess, &mech, pubt, 1, NULL, 0, &pub, &priv), CKR_OK);
		CHECK(pub != 0 && priv != 0);
		CHECK(pub != priv);
	}

	TCASE("C_GetAttributeValue：公钥可读、私钥的 CKA_VALUE 必须拒绝");
	{
		CK_OBJECT_CLASS cls = 0;
		CK_KEY_TYPE kt = 0;
		CK_ULONG pset = 0;
		CK_BBOOL sens = CK_FALSE, extr = CK_TRUE;
		CK_ATTRIBUTE t[4];
		t[0].type = CKA_CLASS;         t[0].pValue = &cls;  t[0].ulValueLen = sizeof(cls);
		t[1].type = CKA_KEY_TYPE;      t[1].pValue = &kt;   t[1].ulValueLen = sizeof(kt);
		t[2].type = CKA_PARAMETER_SET; t[2].pValue = &pset; t[2].ulValueLen = sizeof(pset);
		t[3].type = CKA_SENSITIVE;     t[3].pValue = &sens; t[3].ulValueLen = sizeof(sens);
		CKCHECK(F->C_GetAttributeValue(sess, priv, t, 4), CKR_OK);
		CHECK_EQ_INT(cls, CKO_PRIVATE_KEY);
		CHECK_EQ_INT(kt, CKK_ML_DSA);
		CHECK_EQ_INT(pset, CKP_ML_DSA_65);
		CHECK_EQ_INT(sens, CK_TRUE);

		t[3].type = CKA_EXTRACTABLE; t[3].pValue = &extr;
		CKCHECK(F->C_GetAttributeValue(sess, priv, t, 4), CKR_OK);
		CHECK_EQ_INT(extr, CK_FALSE);      /* 默认不可导出 */

		CKCHECK(F->C_GetAttributeValue(sess, pub, t, 1), CKR_OK);
		CHECK_EQ_INT(cls, CKO_PUBLIC_KEY);

		/* 私钥的 CKA_VALUE —— HSM 的意义就在这一条 */
		CK_ATTRIBUTE v = { CKA_VALUE, NULL, 0 };
		CKCHECK(F->C_GetAttributeValue(sess, priv, &v, 1), CKR_ATTRIBUTE_SENSITIVE);
		CHECK_EQ_INT(v.ulValueLen, CK_UNAVAILABLE_INFORMATION);
	}

	TCASE("公钥 CKA_VALUE 可读，长度与 ML-DSA-65 相符");
	uint8_t pkbuf[4096];
	CK_ULONG pklen = 0;
	{
		CK_ATTRIBUTE v = { CKA_VALUE, NULL, 0 };
		CKCHECK(F->C_GetAttributeValue(sess, pub, &v, 1), CKR_OK);
		CHECK_EQ_INT(v.ulValueLen, 1952);        /* FIPS 204 ML-DSA-65 公钥 */
		v.pValue = pkbuf;
		v.ulValueLen = sizeof(pkbuf);
		CKCHECK(F->C_GetAttributeValue(sess, pub, &v, 1), CKR_OK);
		pklen = v.ulValueLen;
		CHECK_EQ_INT(pklen, 1952);
	}

	TCASE("C_FindObjects：应当找到一个公钥 + 一个私钥");
	{
		CK_OBJECT_HANDLE found[8];
		CK_ULONG fn = 0;
		CKCHECK(F->C_FindObjectsInit(sess, NULL, 0), CKR_OK);
		CKCHECK(F->C_FindObjectsInit(sess, NULL, 0), CKR_OPERATION_ACTIVE);
		CKCHECK(F->C_FindObjects(sess, found, 8, &fn), CKR_OK);
		CHECK_EQ_INT(fn, 2);
		CKCHECK(F->C_FindObjectsFinal(sess), CKR_OK);
		/* 按类过滤 */
		CK_OBJECT_CLASS want = CKO_PRIVATE_KEY;
		CK_ATTRIBUTE t = { CKA_CLASS, &want, sizeof(want) };
		CKCHECK(F->C_FindObjectsInit(sess, &t, 1), CKR_OK);
		CKCHECK(F->C_FindObjects(sess, found, 8, &fn), CKR_OK);
		CHECK_EQ_INT(fn, 1);
		CHECK_EQ_INT(found[0], priv);
		CKCHECK(F->C_FindObjectsFinal(sess), CKR_OK);
		/* 未 Init 就 FindObjects */
		CKCHECK(F->C_FindObjects(sess, found, 8, &fn), CKR_OPERATION_NOT_INITIALIZED);
	}

	TCASE("C_Sign / C_Verify 往返");
	CK_BYTE msg[] = "hello pkcs11";
	CK_BYTE sig[8192];
	CK_ULONG siglen = 0;
	{
		CK_MECHANISM mech = { CKM_ML_DSA, NULL, 0 };
		CKCHECK(F->C_Sign(sess, msg, sizeof(msg), sig, &siglen),
		        CKR_OPERATION_NOT_INITIALIZED);
		/* 拿公钥去签名必须拒绝 */
		CKCHECK(F->C_SignInit(sess, &mech, pub), CKR_KEY_TYPE_INCONSISTENT);
		/* 机制不对 */
		{
			CK_MECHANISM bad = { CKM_RSA_PKCS, NULL, 0 };
			CKCHECK(F->C_SignInit(sess, &bad, priv), CKR_MECHANISM_INVALID);
		}
		CKCHECK(F->C_SignInit(sess, &mech, priv), CKR_OK);
		/* 只问长度 */
		siglen = 0;
		CKCHECK(F->C_Sign(sess, msg, sizeof(msg), NULL, &siglen), CKR_OK);
		CHECK_EQ_INT(siglen, 3309);          /* FIPS 204 ML-DSA-65 签名 */
		/* 缓冲不足 */
		siglen = 10;
		CKCHECK(F->C_Sign(sess, msg, sizeof(msg), sig, &siglen), CKR_BUFFER_TOO_SMALL);
		siglen = sizeof(sig);
		CKCHECK(F->C_Sign(sess, msg, sizeof(msg), sig, &siglen), CKR_OK);
		CHECK_EQ_INT(siglen, 3309);

		CKCHECK(F->C_VerifyInit(sess, &mech, pub), CKR_OK);
		CKCHECK(F->C_Verify(sess, msg, sizeof(msg), sig, siglen), CKR_OK);

		/* 篡改消息 → 验签失败 */
		msg[0] ^= 0x01;
		CKCHECK(F->C_VerifyInit(sess, &mech, pub), CKR_OK);
		CKCHECK(F->C_Verify(sess, msg, sizeof(msg), sig, siglen), CKR_SIGNATURE_INVALID);
		msg[0] ^= 0x01;
		/* 篡改签名 → 验签失败 */
		sig[0] ^= 0x01;
		CKCHECK(F->C_VerifyInit(sess, &mech, pub), CKR_OK);
		CKCHECK(F->C_Verify(sess, msg, sizeof(msg), sig, siglen), CKR_SIGNATURE_INVALID);
		sig[0] ^= 0x01;
	}

	TCASE("签出来的签名能被独立的 liboqs 验证（不经过本模块）");
	{
		CK_MECHANISM mech = { CKM_ML_DSA, NULL, 0 };
		CKCHECK(F->C_SignInit(sess, &mech, priv), CKR_OK);
		siglen = sizeof(sig);
		CKCHECK(F->C_Sign(sess, msg, sizeof(msg), sig, &siglen), CKR_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, pkbuf, msg, sizeof(msg), NULL, 0,
		                        sig, siglen), PQC_OK);
	}

	TCASE("ML-KEM 密钥对也能生成，且用途位正确");
	{
		CK_SESSION_HANDLE s2 = 0;
		CKCHECK(F->C_OpenSession(1, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &s2),
		        CKR_OK);
		CKCHECK(F->C_InitToken(1, (CK_UTF8CHAR_PTR)"12345678", 8, NULL), CKR_OK);
		CKCHECK(F->C_Login(s2, CKU_SO, (CK_UTF8CHAR_PTR)"12345678", 8), CKR_OK);
		CKCHECK(F->C_InitPIN(s2, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
		CKCHECK(F->C_Logout(s2), CKR_OK);
		CKCHECK(F->C_Login(s2, CKU_USER, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);

		CK_ULONG pset = CKP_ML_KEM_768;
		CK_ATTRIBUTE t[1] = { mk_ulong(CKA_PARAMETER_SET, &pset) };
		CK_MECHANISM mech = { CKM_ML_KEM_KEY_PAIR_GEN, NULL, 0 };
		CK_OBJECT_HANDLE kpub = 0, kpriv = 0;
		CKCHECK(F->C_GenerateKeyPair(s2, &mech, t, 1, NULL, 0, &kpub, &kpriv), CKR_OK);

		CK_KEY_TYPE kt = 0;
		CK_BBOOL dec = CK_FALSE, sgn = CK_TRUE;
		CK_ATTRIBUTE q[2];
		q[0].type = CKA_KEY_TYPE;    q[0].pValue = &kt;  q[0].ulValueLen = sizeof(kt);
		q[1].type = CKA_DECAPSULATE; q[1].pValue = &dec; q[1].ulValueLen = sizeof(dec);
		CKCHECK(F->C_GetAttributeValue(s2, kpriv, q, 2), CKR_OK);
		CHECK_EQ_INT(kt, CKK_ML_KEM);
		CHECK_EQ_INT(dec, CK_TRUE);
		/* KEM 私钥不该有 CKA_SIGN */
		q[0].type = CKA_SIGN; q[0].pValue = &sgn; q[0].ulValueLen = sizeof(sgn);
		CKCHECK(F->C_GetAttributeValue(s2, kpriv, q, 1), CKR_OK);
		CHECK_EQ_INT(sgn, CK_FALSE);
		/* 拿 KEM 私钥签名必须被底层的用途互斥挡住 */
		CK_MECHANISM sm = { CKM_ML_DSA, NULL, 0 };
		CKCHECK(F->C_SignInit(s2, &sm, kpriv), CKR_OK);
		CKCHECK(F->C_Sign(s2, msg, sizeof(msg), sig, &siglen), CKR_KEY_TYPE_INCONSISTENT);
		CKCHECK(F->C_CloseSession(s2), CKR_OK);
	}

	TCASE("厂商属性：默认可备份，模板可关掉");
	{
		#define CKA_PQCHSM_BACKUPABLE   (CKA_VENDOR_DEFINED | 0x01UL)
		#define CKA_PQCHSM_SEED_STORAGE (CKA_VENDOR_DEFINED | 0x02UL)
		CK_BBOOL b = CK_FALSE;
		CK_ATTRIBUTE q = { CKA_PQCHSM_BACKUPABLE, &b, sizeof(b) };
		CKCHECK(F->C_GetAttributeValue(sess, priv, &q, 1), CKR_OK);
		CHECK_EQ_INT(b, CK_TRUE);          /* 默认可备份 */
		q.type = CKA_PQCHSM_SEED_STORAGE;
		CKCHECK(F->C_GetAttributeValue(sess, priv, &q, 1), CKR_OK);
		CHECK_EQ_INT(b, CK_FALSE);         /* 默认不用种子存储 */

		/* 换个槽位显式关掉备份 + 打开种子存储 */
		CK_SESSION_HANDLE s4 = 0;
		CKCHECK(F->C_OpenSession(2, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &s4),
		        CKR_OK);
		CKCHECK(F->C_InitToken(2, (CK_UTF8CHAR_PTR)"12345678", 8, NULL), CKR_OK);
		CKCHECK(F->C_Login(s4, CKU_SO, (CK_UTF8CHAR_PTR)"12345678", 8), CKR_OK);
		CKCHECK(F->C_InitPIN(s4, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
		CKCHECK(F->C_Logout(s4), CKR_OK);
		CKCHECK(F->C_Login(s4, CKU_USER, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
		CK_ULONG ps = CKP_ML_DSA_44;
		CK_BBOOL no = CK_FALSE, yes = CK_TRUE;
		CK_ATTRIBUTE t4[3] = {
			mk_ulong(CKA_PARAMETER_SET, &ps),
			{ CKA_PQCHSM_BACKUPABLE, &no, sizeof(no) },
			{ CKA_PQCHSM_SEED_STORAGE, &yes, sizeof(yes) },
		};
		CK_MECHANISM m4 = { CKM_ML_DSA_KEY_PAIR_GEN, NULL, 0 };
		CK_OBJECT_HANDLE p4 = 0, v4 = 0;
		CKCHECK(F->C_GenerateKeyPair(s4, &m4, t4, 3, NULL, 0, &p4, &v4), CKR_OK);
		q.type = CKA_PQCHSM_BACKUPABLE;
		CKCHECK(F->C_GetAttributeValue(s4, v4, &q, 1), CKR_OK);
		CHECK_EQ_INT(b, CK_FALSE);         /* 被模板关掉了 */
		q.type = CKA_PQCHSM_SEED_STORAGE;
		CKCHECK(F->C_GetAttributeValue(s4, v4, &q, 1), CKR_OK);
		CHECK_EQ_INT(b, CK_TRUE);
		/* 种子存储的密钥照样能签名（用时重展开） */
		CKCHECK(F->C_SignInit(s4, &(CK_MECHANISM){ CKM_ML_DSA, NULL, 0 }, v4), CKR_OK);
		siglen = sizeof(sig);
		CKCHECK(F->C_Sign(s4, msg, sizeof(msg), sig, &siglen), CKR_OK);
		CHECK_EQ_INT(siglen, 2420);        /* ML-DSA-44 */
		CKCHECK(F->C_CloseSession(s4), CKR_OK);
	}


	TCASE("★ C_SignUpdate/C_SignFinal：分段与一次性签，结果必须都能验过");
	{
		/* ML-DSA 签名带随机 rnd，同一消息两次签出来的字节不同 ——
		 * 所以这里比的不是"字节相同"，而是**两条路径都得到有效签名**，
		 * 且由独立的 liboqs 验证（不经过本模块）。 */
		CK_MECHANISM mech = { CKM_ML_DSA, NULL, 0 };
		const char *p1 = "multi-part ";
		const char *p2 = "ML-DSA ";
		const char *p3 = "message";
		CK_BYTE whole[64];
		snprintf((char *)whole, sizeof(whole), "%s%s%s", p1, p2, p3);
		CK_ULONG whole_len = (CK_ULONG)strlen((char *)whole);

		CK_BYTE sig_multi[8192], sig_once[8192];
		CK_ULONG lm = sizeof(sig_multi), lo = sizeof(sig_once);

		CKCHECK(F->C_SignInit(sess, &mech, priv), CKR_OK);
		CKCHECK(F->C_SignUpdate(sess, (CK_BYTE_PTR)p1, (CK_ULONG)strlen(p1)), CKR_OK);
		CKCHECK(F->C_SignUpdate(sess, (CK_BYTE_PTR)p2, (CK_ULONG)strlen(p2)), CKR_OK);
		CKCHECK(F->C_SignUpdate(sess, (CK_BYTE_PTR)p3, (CK_ULONG)strlen(p3)), CKR_OK);
		/* 只问长度：按规范此时不能结束操作 */
		CK_ULONG probe = 0;
		CKCHECK(F->C_SignFinal(sess, NULL, &probe), CKR_OK);
		CHECK_EQ_INT(probe, 3309);        /* ML-DSA-65 */
		CKCHECK(F->C_SignFinal(sess, sig_multi, &lm), CKR_OK);

		CKCHECK(F->C_SignInit(sess, &mech, priv), CKR_OK);
		CKCHECK(F->C_Sign(sess, whole, whole_len, sig_once, &lo), CKR_OK);
		CHECK_EQ_INT(lm, lo);

		/* ★ 独立验证：直接用 liboqs 验，绕开本模块的 C_Verify */
		CK_ATTRIBUTE pkq = { CKA_VALUE, NULL, 0 };
		CKCHECK(F->C_GetAttributeValue(sess, pub, &pkq, 1), CKR_OK);
		uint8_t *mp_pk = malloc(pkq.ulValueLen);
		pkq.pValue = mp_pk;
		CKCHECK(F->C_GetAttributeValue(sess, pub, &pkq, 1), CKR_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, mp_pk, whole, whole_len, NULL, 0,
		                        sig_multi, lm), PQC_OK);
		CHECK_EQ_INT(pqc_verify(PQC_ALG_ML_DSA_65, mp_pk, whole, whole_len, NULL, 0,
		                        sig_once, lo), PQC_OK);

		/* 反证：分段签的签名换一个消息必须验不过（证明上面的断言是活的） */
		CHECK(pqc_verify(PQC_ALG_ML_DSA_65, mp_pk, (const uint8_t *)"other", 5,
		                 NULL, 0, sig_multi, lm) != PQC_OK);
		free(mp_pk);

		TCASE("C_VerifyUpdate/C_VerifyFinal：分段验签，且改一个 bit 必须失败");
		CKCHECK(F->C_VerifyInit(sess, &mech, pub), CKR_OK);
		CKCHECK(F->C_VerifyUpdate(sess, (CK_BYTE_PTR)p1, (CK_ULONG)strlen(p1)), CKR_OK);
		CKCHECK(F->C_VerifyUpdate(sess, (CK_BYTE_PTR)p2, (CK_ULONG)strlen(p2)), CKR_OK);
		CKCHECK(F->C_VerifyUpdate(sess, (CK_BYTE_PTR)p3, (CK_ULONG)strlen(p3)), CKR_OK);
		CKCHECK(F->C_VerifyFinal(sess, sig_multi, lm), CKR_OK);

		sig_multi[100] ^= 0x01;
		CKCHECK(F->C_VerifyInit(sess, &mech, pub), CKR_OK);
		CKCHECK(F->C_VerifyUpdate(sess, whole, whole_len), CKR_OK);
		CKCHECK(F->C_VerifyFinal(sess, sig_multi, lm), CKR_SIGNATURE_INVALID);
		sig_multi[100] ^= 0x01;

		TCASE("多段接口的状态机：没 SignInit 就 Update / Final 必须报未初始化");
		CKCHECK(F->C_SignUpdate(sess, whole, whole_len), CKR_OPERATION_NOT_INITIALIZED);
		CK_ULONG dummy = sizeof(sig_multi);
		CKCHECK(F->C_SignFinal(sess, sig_multi, &dummy), CKR_OPERATION_NOT_INITIALIZED);
		CKCHECK(F->C_VerifyUpdate(sess, whole, whole_len), CKR_OPERATION_NOT_INITIALIZED);
		CKCHECK(F->C_VerifyFinal(sess, sig_multi, lm), CKR_OPERATION_NOT_INITIALIZED);
	}

	TCASE("★ C_CreateObject：由种子导入，与同种子的原生 keypair 逐字节相同");
	{
		/* 独立预言机：不比"我们自己导入的结果"，而是拿同一个种子直接喂
		 * liboqs 的 keypair_from_seed，看公钥是不是同一串字节。 */
		CK_SESSION_HANDLE s4 = 0;
		/* C_InitToken 要求该槽位上没有打开的会话 —— 所以先 init 再开会话 */
		/* pLabel 按规范是**定长 32 字节空格填充、不带 NUL** —— 传短字符串
		 * 会让模块读越界（ASan 抓到过一次）。这里照规范补齐。 */
		CKCHECK(F->C_InitToken(3, (CK_UTF8CHAR_PTR)"12345678", 8,
		                       (CK_UTF8CHAR_PTR)"seedtok                         "),
		        CKR_OK);
		CKCHECK(F->C_OpenSession(3, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &s4),
		        CKR_OK);
		CKCHECK(F->C_Login(s4, CKU_SO, (CK_UTF8CHAR_PTR)"12345678", 8), CKR_OK);
		CKCHECK(F->C_InitPIN(s4, (CK_UTF8CHAR_PTR)"seedpin1", 8), CKR_OK);
		CKCHECK(F->C_Logout(s4), CKR_OK);
		CKCHECK(F->C_Login(s4, CKU_USER, (CK_UTF8CHAR_PTR)"seedpin1", 8), CKR_OK);

		CK_BYTE seed[32];
		for (int i = 0; i < 32; i++) {
			seed[i] = (CK_BYTE)(i * 5 + 1);
		}
		CK_OBJECT_CLASS cls = CKO_PRIVATE_KEY;
		CK_KEY_TYPE kt = CKK_ML_DSA;
		CK_ULONG pset = CKP_ML_DSA_65;
		CK_ATTRIBUTE tmpl[] = {
			{ CKA_CLASS, &cls, sizeof(cls) },
			{ CKA_KEY_TYPE, &kt, sizeof(kt) },
			{ CKA_PARAMETER_SET, &pset, sizeof(pset) },
			{ CKA_SEED, seed, sizeof(seed) },
		};
		CK_OBJECT_HANDLE imported = 0;
		CKCHECK(F->C_CreateObject(s4, tmpl, 4, &imported), CKR_OK);
		CHECK(imported != 0);

		/* ★ 独立预言机：同一种子直接喂 liboqs */
		const pqc_alg_info_t *di = pqc_alg_info(PQC_ALG_ML_DSA_65);
		uint8_t *ref_pk = malloc(di->pk_len), *ref_sk = malloc(di->sk_len);
		CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, seed, sizeof(seed),
		                                   ref_pk, ref_sk), PQC_OK);
		CK_ATTRIBUTE gv = { CKA_VALUE, NULL, 0 };
		CKCHECK(F->C_GetAttributeValue(s4, imported | (1ULL << 63), &gv, 1), CKR_OK);
		CHECK_EQ_INT(gv.ulValueLen, di->pk_len);
		uint8_t *got_pk = malloc(gv.ulValueLen);
		gv.pValue = got_pk;
		CKCHECK(F->C_GetAttributeValue(s4, imported | (1ULL << 63), &gv, 1), CKR_OK);
		CHECK_EQ_MEM(got_pk, ref_pk, di->pk_len);

		/* 反证：换一个种子就该不同 —— 证明上面的比较不是恒真 */
		seed[0] ^= 0xFF;
		uint8_t *other_pk = malloc(di->pk_len);
		CHECK_EQ_INT(pqc_keypair_from_seed(PQC_ALG_ML_DSA_65, seed, sizeof(seed),
		                                   other_pk, ref_sk), PQC_OK);
		CHECK(memcmp(other_pk, got_pk, di->pk_len) != 0);
		seed[0] ^= 0xFF;
		free(ref_pk); free(ref_sk); free(got_pk); free(other_pk);

		TCASE("C_CreateObject：明文私钥导入必须被拒（本项目没有这条通路）");
		{
			CK_BYTE fake_sk[64] = { 0 };
			CK_ATTRIBUTE bad[] = {
				{ CKA_CLASS, &cls, sizeof(cls) },
				{ CKA_KEY_TYPE, &kt, sizeof(kt) },
				{ CKA_PARAMETER_SET, &pset, sizeof(pset) },
				{ CKA_VALUE, fake_sk, sizeof(fake_sk) },
			};
			CK_OBJECT_HANDLE o = 0;
			CKCHECK(F->C_CreateObject(s4, bad, 4, &o), CKR_ATTRIBUTE_TYPE_INVALID);
		}
		TCASE("C_CreateObject：模板缺项与种子长度错误的处理");
		{
			CK_OBJECT_HANDLE o = 0;
			CK_ATTRIBUTE only_cls[] = { { CKA_CLASS, &cls, sizeof(cls) } };
			CKCHECK(F->C_CreateObject(s4, only_cls, 1, &o), CKR_TEMPLATE_INCOMPLETE);
			CK_BYTE short_seed[8] = { 0 };
			CK_ATTRIBUTE bad_len[] = {
				{ CKA_CLASS, &cls, sizeof(cls) },
				{ CKA_KEY_TYPE, &kt, sizeof(kt) },
				{ CKA_PARAMETER_SET, &pset, sizeof(pset) },
				{ CKA_SEED, short_seed, sizeof(short_seed) },
			};
			CKCHECK(F->C_CreateObject(s4, bad_len, 4, &o), CKR_ATTRIBUTE_VALUE_INVALID);
		}
		CKCHECK(F->C_CloseAllSessions(2), CKR_OK);
	}

	TCASE("★ C_EncapsulateKey / C_DecapsulateKey：两端共享秘密必须一致");
	{
		/* 槽位 1 上是前面生成的 ML-KEM-768 密钥对 */
		CK_SESSION_HANDLE sk1 = 0;
		CKCHECK(F->C_OpenSession(1, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &sk1),
		        CKR_OK);
		CKCHECK(F->C_Login(sk1, CKU_USER, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
		CK_OBJECT_HANDLE fo[8];
		CK_ULONG fn = 0;
		CKCHECK(F->C_FindObjectsInit(sk1, NULL, 0), CKR_OK);
		CKCHECK(F->C_FindObjects(sk1, fo, 8, &fn), CKR_OK);
		CKCHECK(F->C_FindObjectsFinal(sk1), CKR_OK);
		CHECK_EQ_INT(fn, 2);
		CK_OBJECT_HANDLE kpub = fo[0], kpriv = fo[1];

		CK_MECHANISM kmech = { CKM_ML_KEM, NULL, 0 };
		/* 演示用：显式要求可读，好把两端秘密拿出来比对 */
		CK_BBOOL yes = CK_TRUE, no = CK_FALSE;
		CK_ATTRIBUTE readable[] = {
			{ CKA_EXTRACTABLE, &yes, sizeof(yes) },
			{ CKA_SENSITIVE, &no, sizeof(no) },
		};

		/* 只问密文长度 */
		CK_ULONG ctlen = 0;
		CK_OBJECT_HANDLE ssA = 0, ssB = 0;
		CKCHECK(F32->C_EncapsulateKey(sk1, &kmech, kpub, readable, 2,
		                              NULL, &ctlen, &ssA), CKR_OK);
		CHECK_EQ_INT(ctlen, 1088);        /* ML-KEM-768 */
		CK_BYTE ct[2048];
		ctlen = sizeof(ct);
		CKCHECK(F32->C_EncapsulateKey(sk1, &kmech, kpub, readable, 2,
		                              ct, &ctlen, &ssA), CKR_OK);
		CHECK_EQ_INT(ctlen, 1088);
		CHECK(ssA != 0);

		CKCHECK(F32->C_DecapsulateKey(sk1, &kmech, kpriv, readable, 2,
		                              ct, ctlen, &ssB), CKR_OK);
		CHECK(ssB != 0);
		CHECK(ssA != ssB);                /* 两个不同的对象 */

		CK_BYTE va[64], vb[64];
		CK_ATTRIBUTE qa = { CKA_VALUE, va, sizeof(va) };
		CK_ATTRIBUTE qb = { CKA_VALUE, vb, sizeof(vb) };
		CKCHECK(F->C_GetAttributeValue(sk1, ssA, &qa, 1), CKR_OK);
		CKCHECK(F->C_GetAttributeValue(sk1, ssB, &qb, 1), CKR_OK);
		CHECK_EQ_INT(qa.ulValueLen, 32);
		CHECK_EQ_INT(qb.ulValueLen, 32);
		CHECK_EQ_MEM(va, vb, 32);         /* ★ 封装端与解封装端得到同一个共享秘密 */

		/* 反证：改一个 bit 的密文解出来必须不同（ML-KEM 的隐式拒绝会给出
		 * 一个确定但不同的值 —— 所以这里是"不等"而不是"报错"） */
		TCASE("反证：篡改密文后解出的共享秘密必须不同（ML-KEM 隐式拒绝）");
		{
			ct[5] ^= 0x01;
			CK_OBJECT_HANDLE ssC = 0;
			CKCHECK(F32->C_DecapsulateKey(sk1, &kmech, kpriv, readable, 2,
			                              ct, ctlen, &ssC), CKR_OK);
			CK_BYTE vc[64];
			CK_ATTRIBUTE qc = { CKA_VALUE, vc, sizeof(vc) };
			CKCHECK(F->C_GetAttributeValue(sk1, ssC, &qc, 1), CKR_OK);
			CHECK(memcmp(vc, va, 32) != 0);
			ct[5] ^= 0x01;
			CKCHECK(F->C_DestroyObject(sk1, ssC), CKR_OK);
		}

		TCASE("共享秘密默认 sensitive：不给 EXTRACTABLE 时 CKA_VALUE 必须拒绝");
		{
			CK_OBJECT_HANDLE ssD = 0;
			CK_ULONG cl = sizeof(ct);
			CKCHECK(F32->C_EncapsulateKey(sk1, &kmech, kpub, NULL, 0, ct, &cl, &ssD),
			        CKR_OK);
			CK_BYTE tmp[64];
			CK_ATTRIBUTE q = { CKA_VALUE, tmp, sizeof(tmp) };
			CKCHECK(F->C_GetAttributeValue(sk1, ssD, &q, 1), CKR_ATTRIBUTE_SENSITIVE);
			CHECK(q.ulValueLen == CK_UNAVAILABLE_INFORMATION);
			/* 长度这类非敏感属性照样可读 */
			CK_ULONG vl = 0;
			CK_ATTRIBUTE q2 = { CKA_VALUE_LEN, &vl, sizeof(vl) };
			CKCHECK(F->C_GetAttributeValue(sk1, ssD, &q2, 1), CKR_OK);
			CHECK_EQ_INT(vl, 32);
			CKCHECK(F->C_DestroyObject(sk1, ssD), CKR_OK);
		}

		TCASE("封装/解封装的参数校验");
		{
			CK_OBJECT_HANDLE o = 0;
			CK_ULONG cl = sizeof(ct);
			/* 拿私钥去封装 / 拿公钥去解封装 —— 都该被拒 */
			CKCHECK(F32->C_EncapsulateKey(sk1, &kmech, kpriv, NULL, 0, ct, &cl, &o),
			        CKR_KEY_TYPE_INCONSISTENT);
			CKCHECK(F32->C_DecapsulateKey(sk1, &kmech, kpub, NULL, 0, ct, ctlen, &o),
			        CKR_KEY_TYPE_INCONSISTENT);
			/* 机制不对 */
			CK_MECHANISM wrong = { CKM_ML_DSA, NULL, 0 };
			CKCHECK(F32->C_EncapsulateKey(sk1, &wrong, kpub, NULL, 0, ct, &cl, &o),
			        CKR_MECHANISM_INVALID);
			/* 密文长度不对 */
			CKCHECK(F32->C_DecapsulateKey(sk1, &kmech, kpriv, NULL, 0, ct, 10, &o),
			        CKR_ENCRYPTED_DATA_LEN_RANGE);
		}

		TCASE("会话密钥对象是会话生命期：CloseSession 后句柄失效");
		{
			CKCHECK(F->C_CloseSession(sk1), CKR_OK);
			CK_SESSION_HANDLE s5 = 0;
			CKCHECK(F->C_OpenSession(1, CKF_SERIAL_SESSION, NULL, NULL, &s5), CKR_OK);
			CK_BYTE tmp[64];
			CK_ATTRIBUTE q = { CKA_VALUE, tmp, sizeof(tmp) };
			CKCHECK(F->C_GetAttributeValue(s5, ssA, &q, 1), CKR_OBJECT_HANDLE_INVALID);
			CKCHECK(F->C_CloseSession(s5), CKR_OK);
		}
	}

	TCASE("C_DestroyObject 后对象消失");
	{
		CKCHECK(F->C_DestroyObject(sess, priv), CKR_OK);
		CK_OBJECT_HANDLE found[8];
		CK_ULONG fn = 0;
		CKCHECK(F->C_FindObjectsInit(sess, NULL, 0), CKR_OK);
		CKCHECK(F->C_FindObjects(sess, found, 8, &fn), CKR_OK);
		CHECK_EQ_INT(fn, 0);
		CKCHECK(F->C_FindObjectsFinal(sess), CKR_OK);
		/* 旧句柄失效 */
		CKCHECK(F->C_DestroyObject(sess, priv), CKR_OBJECT_HANDLE_INVALID);
	}

	TCASE("Finalize 后再调用必须报未初始化");
	CKCHECK(F->C_CloseSession(sess), CKR_OK);
	CKCHECK(F->C_Finalize(NULL), CKR_OK);
	CKCHECK(F->C_Finalize(NULL), CKR_CRYPTOKI_NOT_INITIALIZED);
	{
		CK_ULONG cnt = 0;
		CKCHECK(F->C_GetSlotList(CK_TRUE, NULL, &cnt), CKR_CRYPTOKI_NOT_INITIALIZED);
	}

	TCASE("跨进程持久化：重新 Initialize 后 token 与密钥还在");
	{
		CKCHECK(F->C_Initialize(NULL), CKR_OK);
		CK_TOKEN_INFO ti;
		CKCHECK(F->C_GetTokenInfo(0, &ti), CKR_OK);
		CHECK((ti.flags & CKF_TOKEN_INITIALIZED) != 0);
		CHECK(memcmp(ti.label, "mytoken", 7) == 0);
		/* slot 1 的 ML-KEM 密钥应当还在 */
		CK_SESSION_HANDLE s3 = 0;
		CKCHECK(F->C_OpenSession(1, CKF_SERIAL_SESSION, NULL, NULL, &s3), CKR_OK);
		CKCHECK(F->C_Login(s3, CKU_USER, (CK_UTF8CHAR_PTR)"1234abcd", 8), CKR_OK);
		CK_OBJECT_HANDLE found[8];
		CK_ULONG fn = 0;
		CKCHECK(F->C_FindObjectsInit(s3, NULL, 0), CKR_OK);
		CKCHECK(F->C_FindObjects(s3, found, 8, &fn), CKR_OK);
		CHECK_EQ_INT(fn, 2);
		CKCHECK(F->C_FindObjectsFinal(s3), CKR_OK);
		CKCHECK(F->C_CloseAllSessions(1), CKR_OK);
		CKCHECK(F->C_Finalize(NULL), CKR_OK);
	}

	dlclose(h);
	unlink(g_ks);
	return test_report("test_p11");
}
