// Java 端到端 demo：把 pqc-hsm 当作 PKCS#11 模块调用
//
// 【为什么不用 SunPKCS11 做密码运算】
// JDK 26 的 JCA **本身**认识 ML-DSA / ML-KEM（KeyPairGenerator.getInstance("ML-DSA") 可用），
// SunPKCS11 也**能成功加载**本模块（provider 建得起来）。但 SunPKCS11 的
// 「PKCS#11 机制码 → JCA 算法名」映射表**还没有加入 v3.2 的 CKM_ML_DSA / CKM_ML_KEM**，
// 所以它对本模块只暴露 1 个服务（KeyStore/PKCS11），
//     KeyPairGenerator.getInstance("ML-DSA", p11Provider)  →  NoSuchAlgorithmException
// 这一条由 SunP11Probe 实测确认，结论写在 demo/java/README.md 里。
//
// 【所以走低层直调】
// 用 JDK 22+ 自带的 Foreign Function & Memory API（java.lang.foreign）直接调 C ABI，
// 机制码按数值传。相比 IAIK PKCS#11 Wrapper / jacknji11 的好处是**零外部依赖**：
// 不需要下载任何 jar，JDK 自带。本模块把 27 个 C_* 都导出成了全局符号，
// 所以连 CK_FUNCTION_LIST 的偏移都不用数，直接按名字 lookup。
//
// 运行（单文件源码模式，不需要先 javac）：
//   java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java <模块路径>

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

public class PqcHsmDemo {

    // ---- PKCS#11 v3.2 常量（按数值直传，不依赖任何封装认不认识）----
    static final long CKM_ML_KEM_KEY_PAIR_GEN = 0x0000000FL;
    static final long CKM_ML_KEM              = 0x00000017L;
    static final long CKM_ML_DSA_KEY_PAIR_GEN = 0x0000001CL;
    static final long CKM_ML_DSA              = 0x0000001DL;
    static final long CKK_ML_KEM              = 0x00000049L;
    static final long CKK_ML_DSA              = 0x0000004AL;
    static final long CKA_CLASS               = 0x00000000L;
    static final long CKA_VALUE               = 0x00000011L;
    static final long CKA_KEY_TYPE            = 0x00000100L;
    static final long CKA_PARAMETER_SET       = 0x0000061DL;
    static final long CKA_DECAPSULATE         = 0x00000634L;
    static final long CKP_ML_DSA_65           = 2L;
    static final long CKP_ML_KEM_768          = 2L;
    static final long CKO_PRIVATE_KEY         = 3L;
    static final long CKF_SERIAL_SESSION      = 0x00000004L;
    static final long CKF_RW_SESSION          = 0x00000002L;
    static final long CKU_SO                  = 0L;
    static final long CKU_USER                = 1L;
    static final long CKR_OK                  = 0L;
    static final long CKR_ATTRIBUTE_SENSITIVE = 0x00000011L;

    static final String SO_PIN = "12345678";
    static final String USER_PIN = "1234abcd";

    static int pass = 0, fail = 0;

    static void check(boolean cond, String what) {
        if (cond) { pass++; System.out.println("  ✓ " + what); }
        else      { fail++; System.out.println("  ✗ " + what); }
    }

    static Linker linker = Linker.nativeLinker();
    static SymbolLookup lib;

    /** 按名字取一个 C_* 函数并绑定签名 */
    static MethodHandle fn(String name, MemoryLayout... args) {
        MemorySegment addr = lib.find(name)
                .orElseThrow(() -> new RuntimeException("模块里没有导出 " + name));
        return linker.downcallHandle(addr, FunctionDescriptor.of(ValueLayout.JAVA_LONG, args));
    }

    static final ValueLayout.OfLong  L = ValueLayout.JAVA_LONG;
    static final ValueLayout.OfByte  B = ValueLayout.JAVA_BYTE;
    static final AddressLayout       P = ValueLayout.ADDRESS;

    /** CK_MECHANISM { CK_ULONG mechanism; void *pParameter; CK_ULONG ulParameterLen; } */
    static MemorySegment mechanism(Arena a, long mech) {
        MemorySegment m = a.allocate(24);
        m.set(L, 0, mech);
        m.set(P, 8, MemorySegment.NULL);
        m.set(L, 16, 0L);
        return m;
    }

    /** CK_ATTRIBUTE { CK_ULONG type; void *pValue; CK_ULONG ulValueLen; } */
    static void setAttr(MemorySegment arr, int idx, long type, MemorySegment val, long len) {
        long off = idx * 24L;
        arr.set(L, off, type);
        arr.set(P, off + 8, val);
        arr.set(L, off + 16, len);
    }

    static long attrLen(MemorySegment arr, int idx) { return arr.get(L, idx * 24L + 16); }

    public static void main(String[] argv) throws Throwable {
        String modulePath = argv.length > 0 ? argv[0]
                : System.getProperty("user.dir") + "/build/pqchsm-pkcs11.dylib";
        System.out.println("模块 : " + modulePath);
        System.out.println();

        try (Arena arena = Arena.ofConfined()) {
            lib = SymbolLookup.libraryLookup(Path.of(modulePath), arena);

            MethodHandle C_Initialize   = fn("C_Initialize", P);
            MethodHandle C_Finalize     = fn("C_Finalize", P);
            MethodHandle C_GetSlotList  = fn("C_GetSlotList", B, P, P);
            MethodHandle C_GetMechList  = fn("C_GetMechanismList", L, P, P);
            MethodHandle C_InitToken    = fn("C_InitToken", L, P, L, P);
            MethodHandle C_InitPIN      = fn("C_InitPIN", L, P, L);
            MethodHandle C_OpenSession  = fn("C_OpenSession", L, L, P, P, P);
            MethodHandle C_CloseSession = fn("C_CloseSession", L);
            MethodHandle C_Login        = fn("C_Login", L, L, P, L);
            MethodHandle C_Logout       = fn("C_Logout", L);
            MethodHandle C_GenKeyPair   = fn("C_GenerateKeyPair", L, P, P, L, P, L, P, P);
            MethodHandle C_GetAttr      = fn("C_GetAttributeValue", L, L, P, L);
            MethodHandle C_SignInit     = fn("C_SignInit", L, P, L);
            MethodHandle C_Sign         = fn("C_Sign", L, P, L, P, P);
            MethodHandle C_VerifyInit   = fn("C_VerifyInit", L, P, L);
            MethodHandle C_Verify       = fn("C_Verify", L, P, L, P, L);
            MethodHandle C_FindInit     = fn("C_FindObjectsInit", L, P, L);
            MethodHandle C_Find         = fn("C_FindObjects", L, P, L, P);
            MethodHandle C_FindFinal    = fn("C_FindObjectsFinal", L);

            System.out.println("== 1. C_Initialize 与槽位 ==");
            check((long) C_Initialize.invoke(MemorySegment.NULL) == CKR_OK, "C_Initialize");

            MemorySegment cnt = arena.allocate(L);
            cnt.set(L, 0, 8L);
            MemorySegment slots = arena.allocate(L, 8);
            check((long) C_GetSlotList.invoke((byte) 1, slots, cnt) == CKR_OK, "C_GetSlotList");
            long nSlots = cnt.get(L, 0);
            System.out.println("  槽位数 = " + nSlots);
            long slot0 = slots.get(L, 0);

            System.out.println("\n== 2. 机制列表（应含 ML-DSA / ML-KEM）==");
            MemorySegment mcnt = arena.allocate(L);
            mcnt.set(L, 0, 16L);
            MemorySegment mechs = arena.allocate(L, 16);
            check((long) C_GetMechList.invoke(slot0, mechs, mcnt) == CKR_OK, "C_GetMechanismList");
            boolean hasDsa = false, hasKem = false;
            StringBuilder sb = new StringBuilder();
            for (long i = 0; i < mcnt.get(L, 0); i++) {
                long m = mechs.getAtIndex(L, i);
                sb.append(String.format("0x%x ", m));
                if (m == CKM_ML_DSA) hasDsa = true;
                if (m == CKM_ML_KEM) hasKem = true;
            }
            System.out.println("  机制码：" + sb);
            check(hasDsa, "含 CKM_ML_DSA (0x1d)");
            check(hasKem, "含 CKM_ML_KEM (0x17)");

            System.out.println("\n== 3. 初始化 token 与 PIN ==");
            MemorySegment soPin = arena.allocateFrom(SO_PIN);
            MemorySegment label = arena.allocateFrom("javaDemo                        ");
            long rv = (long) C_InitToken.invoke(slot0, soPin, (long) SO_PIN.length(), label);
            check(rv == CKR_OK, "C_InitToken (rv=0x" + Long.toHexString(rv) + ")");

            MemorySegment sessP = arena.allocate(L);
            check((long) C_OpenSession.invoke(slot0, CKF_SERIAL_SESSION | CKF_RW_SESSION,
                    MemorySegment.NULL, MemorySegment.NULL, sessP) == CKR_OK, "C_OpenSession");
            long sess = sessP.get(L, 0);

            check((long) C_Login.invoke(sess, CKU_SO, soPin, (long) SO_PIN.length()) == CKR_OK,
                    "C_Login(CKU_SO)");
            MemorySegment userPin = arena.allocateFrom(USER_PIN);
            check((long) C_InitPIN.invoke(sess, userPin, (long) USER_PIN.length()) == CKR_OK,
                    "C_InitPIN");
            check((long) C_Logout.invoke(sess) == CKR_OK, "C_Logout");
            check((long) C_Login.invoke(sess, CKU_USER, userPin, (long) USER_PIN.length()) == CKR_OK,
                    "C_Login(CKU_USER)");

            System.out.println("\n== 4. ML-DSA-65：生成 → 签名 → 验签 ==");
            MemorySegment psetVal = arena.allocate(L);
            psetVal.set(L, 0, CKP_ML_DSA_65);
            MemorySegment pubTmpl = arena.allocate(24);
            setAttr(pubTmpl, 0, CKA_PARAMETER_SET, psetVal, 8);
            MemorySegment hPub = arena.allocate(L), hPriv = arena.allocate(L);
            rv = (long) C_GenKeyPair.invoke(sess, mechanism(arena, CKM_ML_DSA_KEY_PAIR_GEN),
                    pubTmpl, 1L, MemorySegment.NULL, 0L, hPub, hPriv);
            check(rv == CKR_OK, "C_GenerateKeyPair(CKM_ML_DSA_KEY_PAIR_GEN)");
            long pub = hPub.get(L, 0), priv = hPriv.get(L, 0);

            // 私钥的 CKA_VALUE 必须取不出来 —— HSM 的意义就在这条
            MemorySegment sensTmpl = arena.allocate(24);
            setAttr(sensTmpl, 0, CKA_VALUE, MemorySegment.NULL, 0);
            rv = (long) C_GetAttr.invoke(sess, priv, sensTmpl, 1L);
            check(rv == CKR_ATTRIBUTE_SENSITIVE,
                    "私钥 CKA_VALUE 被拒（CKR_ATTRIBUTE_SENSITIVE，rv=0x"
                            + Long.toHexString(rv) + "）");

            // 密钥类型与参数集
            MemorySegment ktVal = arena.allocate(L);
            MemorySegment ktTmpl = arena.allocate(24);
            setAttr(ktTmpl, 0, CKA_KEY_TYPE, ktVal, 8);
            check((long) C_GetAttr.invoke(sess, priv, ktTmpl, 1L) == CKR_OK
                    && ktVal.get(L, 0) == CKK_ML_DSA, "CKA_KEY_TYPE = CKK_ML_DSA");
            MemorySegment psTmpl = arena.allocate(24);
            MemorySegment psOut = arena.allocate(L);
            setAttr(psTmpl, 0, CKA_PARAMETER_SET, psOut, 8);
            check((long) C_GetAttr.invoke(sess, priv, psTmpl, 1L) == CKR_OK
                    && psOut.get(L, 0) == CKP_ML_DSA_65, "CKA_PARAMETER_SET = CKP_ML_DSA_65");

            // 公钥可读，长度应为 1952
            MemorySegment pkQuery = arena.allocate(24);
            setAttr(pkQuery, 0, CKA_VALUE, MemorySegment.NULL, 0);
            C_GetAttr.invoke(sess, pub, pkQuery, 1L);
            long pkLen = attrLen(pkQuery, 0);
            check(pkLen == 1952, "ML-DSA-65 公钥 " + pkLen + " B（应为 1952）");

            byte[] msg = "hello from java via PKCS#11".getBytes();
            MemorySegment data = arena.allocateFrom(B, msg);
            check((long) C_SignInit.invoke(sess, mechanism(arena, CKM_ML_DSA), priv) == CKR_OK,
                    "C_SignInit(CKM_ML_DSA)");
            MemorySegment sigLen = arena.allocate(L);
            sigLen.set(L, 0, 0L);
            C_Sign.invoke(sess, data, (long) msg.length, MemorySegment.NULL, sigLen);
            long need = sigLen.get(L, 0);
            check(need == 3309, "签名长度 " + need + " B（ML-DSA-65 应为 3309）");
            MemorySegment sig = arena.allocate(need);
            sigLen.set(L, 0, need);
            check((long) C_Sign.invoke(sess, data, (long) msg.length, sig, sigLen) == CKR_OK,
                    "C_Sign");

            check((long) C_VerifyInit.invoke(sess, mechanism(arena, CKM_ML_DSA), pub) == CKR_OK,
                    "C_VerifyInit");
            check((long) C_Verify.invoke(sess, data, (long) msg.length, sig,
                    sigLen.get(L, 0)) == CKR_OK, "C_Verify 通过");

            // 负测试：改一个字节
            MemorySegment badData = arena.allocateFrom(B, msg);
            badData.set(B, 0, (byte) (msg[0] ^ 1));
            C_VerifyInit.invoke(sess, mechanism(arena, CKM_ML_DSA), pub);
            check((long) C_Verify.invoke(sess, badData, (long) msg.length, sig,
                    sigLen.get(L, 0)) != CKR_OK, "篡改消息后验签失败（负测试）");

            System.out.println("\n== 5. C_FindObjects ==");
            check((long) C_FindInit.invoke(sess, MemorySegment.NULL, 0L) == CKR_OK,
                    "C_FindObjectsInit");
            MemorySegment found = arena.allocate(L, 8);
            MemorySegment fcnt = arena.allocate(L);
            check((long) C_Find.invoke(sess, found, 8L, fcnt) == CKR_OK, "C_FindObjects");
            check(fcnt.get(L, 0) == 2, "找到 " + fcnt.get(L, 0) + " 个对象（1 公钥 + 1 私钥）");
            C_FindFinal.invoke(sess);

            System.out.println("\n== 6. ML-KEM-768 密钥对 ==");
            C_CloseSession.invoke(sess);
            long slot1 = slots.getAtIndex(L, 1);
            C_InitToken.invoke(slot1, soPin, (long) SO_PIN.length(),
                    arena.allocateFrom("javaKem                         "));
            MemorySegment s2p = arena.allocate(L);
            C_OpenSession.invoke(slot1, CKF_SERIAL_SESSION | CKF_RW_SESSION,
                    MemorySegment.NULL, MemorySegment.NULL, s2p);
            long s2 = s2p.get(L, 0);
            C_Login.invoke(s2, CKU_SO, soPin, (long) SO_PIN.length());
            C_InitPIN.invoke(s2, userPin, (long) USER_PIN.length());
            C_Logout.invoke(s2);
            C_Login.invoke(s2, CKU_USER, userPin, (long) USER_PIN.length());

            MemorySegment kpsVal = arena.allocate(L);
            kpsVal.set(L, 0, CKP_ML_KEM_768);
            MemorySegment kTmpl = arena.allocate(24);
            setAttr(kTmpl, 0, CKA_PARAMETER_SET, kpsVal, 8);
            MemorySegment kPub = arena.allocate(L), kPriv = arena.allocate(L);
            rv = (long) C_GenKeyPair.invoke(s2, mechanism(arena, CKM_ML_KEM_KEY_PAIR_GEN),
                    kTmpl, 1L, MemorySegment.NULL, 0L, kPub, kPriv);
            check(rv == CKR_OK, "C_GenerateKeyPair(CKM_ML_KEM_KEY_PAIR_GEN)");

            MemorySegment kktVal = arena.allocate(L);
            MemorySegment kktTmpl = arena.allocate(24);
            setAttr(kktTmpl, 0, CKA_KEY_TYPE, kktVal, 8);
            check((long) C_GetAttr.invoke(s2, kPriv.get(L, 0), kktTmpl, 1L) == CKR_OK
                    && kktVal.get(L, 0) == CKK_ML_KEM, "CKA_KEY_TYPE = CKK_ML_KEM");

            MemorySegment kpkQ = arena.allocate(24);
            setAttr(kpkQ, 0, CKA_VALUE, MemorySegment.NULL, 0);
            C_GetAttr.invoke(s2, kPub.get(L, 0), kpkQ, 1L);
            check(attrLen(kpkQ, 0) == 1184,
                    "ML-KEM-768 公钥 " + attrLen(kpkQ, 0) + " B（应为 1184）");

            // 用途互斥：KEM 私钥拿去签名必须被拒
            C_SignInit.invoke(s2, mechanism(arena, CKM_ML_DSA), kPriv.get(L, 0));
            MemorySegment dummyLen = arena.allocate(L);
            dummyLen.set(L, 0, 8192L);
            MemorySegment dummySig = arena.allocate(8192);
            check((long) C_Sign.invoke(s2, data, (long) msg.length, dummySig, dummyLen) != CKR_OK,
                    "KEM 私钥拿去签名被拒（用途互斥）");

            C_CloseSession.invoke(s2);
            check((long) C_Finalize.invoke(MemorySegment.NULL) == CKR_OK, "C_Finalize");
        }

        System.out.println();
        System.out.println("通过 " + pass + "，失败 " + fail);
        if (fail != 0) System.exit(1);
    }
}
