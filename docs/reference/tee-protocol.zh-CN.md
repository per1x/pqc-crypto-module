# pqc-hsm OP-TEE TA：一条把密钥运算放进安全世界软件的设计

> **定位（必读）：这不是本项目当前的架构。**
>
> 本项目的密码引擎是 **FPGA 可编程逻辑里的硬件**——ML-KEM 512/768/1024、
> AES-128/256、SM4、SM3、环形振荡器 TRNG、密钥仓，外加一道按 AxPROT 门控的
> AXI 防火墙。密码边界就是那圈逻辑，并已在真硅上双向证明
> （见 [SECURITY.zh-CN.md](../SECURITY.zh-CN.md)）。
>
> 安全世界在本项目里只承担**一个最小角色**：发出一笔 AxPROT 安全事务，
> 以证明门控确实按安全属性区分。这件事最终是用**给 BL31 加一个 SiP 调用**
> 做成的（约二十行，见 `boot/atf/`），**没有用到 OP-TEE**。
>
> 本文记录的是另一条路线：把私钥运算放进 OP-TEE 的可信应用里，用软件
> 实现算法。它是一份**有价值但未被采用**的设计记录——保留它是因为其中的
> 密钥体系、包裹格式与威胁分析仍然成立，也因为它说明了为什么最终选了硬件：
> 软件 TA 的边界是 S-EL1 的地址空间，而 PL 里那道门是地址译码上的物理事实。


本文档描述 `tee/` 目录的实现：把 pqc-hsm 的私钥运算（ML-KEM/ML-DSA
keygen/decaps/sign、KDR/KEK 派生、PWRP 包裹）搬进 OP-TEE 可信应用（TA），
普通世界（REE）只保留槽位状态机/ACL/PIN/文件 I/O/PKCS#11 前端。
对应《Zynq 移植设计》(docs/reference/zynq-port.zh-CN.md) B.4 的分层设计，本节是其落地。

## 1. 安全边界与防护声明

- **私钥明文不出 TA。** KEYGEN 返回 `(公钥, PWRP 包裹私钥 blob)`；
  SIGN/DECAPS 以 blob 为输入，TA 内解包、用完即焚（`pqchsm_bzero`）。
- **防普通世界应用，不防普通世界内核。** REE 内核若被完全攻陷，可
  重放/滥用 TA 服务（拒绝服务、以 TA 为签名谕示机），但私钥明文仍不出
  S-EL1。REE 内核属于 TCB；要防内核级攻击需进一步上 XMPU/XPPU + 启动
  链度量（见续作 5 的隔离设计）。
- ML-KEM 的共享秘密 ss 按现架构会回到普通世界（与软件后端一致）——
  ss 是会话密钥不是长期私钥；若后续要求 ss 也不出 TA，在协议上加
  "TA 内派生会话密钥"命令即可，不动现有命令。
- KDR 取自 OP-TEE system PTA 的 `PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY`
  （HUK + TA UUID 派生，HUK 不出芯片，REE 拿不到同源材料）。

## 2. 密钥体系

```
HUK(芯片内) ──PTA_DERIVE_TA_UNIQUE_KEY──> KDR(32B, TA 实例内, 不落盘)
KDR ──KMAC256(KDR, salt, S="pqc-hsm/storage-kek")──> KEK(32B)
KEK ──AES-256-GCM──> PWRP blob 包裹的私钥（存普通世界文件系统）
```

- KEK 由 `CMD_KEK_SET` 携带密钥库 salt 现场派生并缓存在 TA 实例内，
  不落盘、不出 TA；salt 存密钥库头部（与软件路径一致）。
- PWRP blob 格式与 `include/pqchsm/wrap.h` **逐字节一致**：
  `"PWRP"‖ver(LE16)‖alg(LE16=1)‖aad_len(LE32)‖ct_len(LE32)‖nonce(12)‖ct‖tag(16)`，
  AAD = 头部 16B ‖ 调用方 aad。两侧实现的 blob 可互换解包。
- KDF 语义与 `src/crypto/kdf.c`（OpenSSL EVP_MAC KMAC-256）一致：
  `pqc_kdf(ikm, salt, label) = KMAC256(K=ikm, X=salt, L=out*8, S=label)`。
  TA 内是自建 Keccak 核上的流式实现（`ta/ta_kdf.c`），已用 OpenSSL 3.6
  生成的 KAT 锁死一致性（`tee/tests/test_kdf.c`）。

## 3. 目录

```
tee/
├── include/pqchsm_ta_proto.h   # 命令协议（两侧唯一事实来源）
├── ta/                         # TA 本体（OP-TEE TA dev kit 构建）
│   ├── pqchsm_ta.c             # 入口 + 命令分发 + KDR/KEK 管理
│   ├── ta_fips202.c/h          # Keccak 核（SHA3/SHAKE/cSHAKE 底座）
│   ├── ta_kdf.c/h              # KMAC-256（SP 800-185，流式）
│   ├── ta_wrap.c/h             # PWRP（TEE crypto AES-GCM / native OpenSSL）
│   ├── ta_pqc.c/h              # 算法调度（编号与 pqc_alg_t 一致）
│   ├── ta_random.c/h           # TEE_GenerateRandom / getrandom
│   ├── ta_mlkem{512,768,1024}.c  # mlkem-native 单编译单元×3
│   ├── ta_mldsa{44,65,87}.c      # mldsa-native 单编译单元×3
│   ├── config/                 # 两个库的集成配置 + fips202 胶水头
│   └── vendor/{mlkem,mldsa}/   # liboqs 0.16 的 ref 源码（逐字节未改）
├── host/pqchsm_ta_client.c/h   # libteec 客户端（形状对齐 pqc_backend_t）
└── tests/                      # native 测试（同套源码 x86_64 预演）
```

vendor 树是 liboqs 0.16.0 内 mlkem-native/mldsa-native 的 ref 实现，
**未改一字节**；通过各自支持的集成机制接入：

- `MLK_CONFIG_FILE` / `MLD_CONFIG_FILE` 指向 `config/pqchsm_{mlk,mld}_config.h`
  （命名空间前缀 `PQCHSM_MLK512_*` 等，三个参数集编译单元符号不冲突）；
- `*_CONFIG_SERIAL_FIPS202_ONLY`：只带一个软件 Keccak 核，关掉 x4 路径；
- `*_CONFIG_FIPS202_CUSTOM_HEADER`：换成 `config/pqchsm_fips202_{mlk,mld}.h`
  （static inline 套到 `ta_fips202.c`）；
- `*_CONFIG_CUSTOM_RANDOMBYTES`：换成 `ta_random.c`
  （TA 构建 = `TEE_GenerateRandom`，native 测试 = getrandom）。

## 4. 命令协议（摘要，权威定义在 proto 头）

| 命令 | 说明 |
|---|---|
| GET_INFO | 协议版本 + 特性位 |
| KDF_DERIVE | KDR 派生子密钥（label 强制域分隔） |
| KEK_SET | 按密钥库 salt 派生并缓存 KEK |
| WRAP / UNWRAP | KEK 包裹/解包（UNWRAP 认证失败清零输出） |
| KEYGEN | 返回 (公钥, 包裹私钥 blob) |
| KEYGEN_FROM_SEED | 种子展开（ML-KEM 64B d‖z / ML-DSA 32B ξ），种子存储优化路径 |
| DECAPS | blob 私钥解封装 → ss |
| SIGN | hedged 签名（rnd 由 TA 内 TRNG 现取），消息帧 = ctx_len‖ctx‖msg |

v1 未提供：显式 rnd 的确定性签名（KAT 驱动路径留在普通世界软件后端）、
VERIFY 命令（验签只用公钥，不需要进 TA）。输出缓冲一律
`TEE_ERROR_SHORT_BUFFER` + 回填所需长度的语义。

## 5. 构建

```bash
# TA（aarch64，WSL 构建机；产物 154KB 的 <uuid>.ta）
cd tee/ta && make          # 默认 TA_DEV_KIT_DIR/CROSS_COMPILE 指向
                           # p4_optee 的 optee_os 导出和 Vitis 2020.1 工具链

# native 测试（同套源码，x86_64 预演）
cd tee/tests && make && ./run_tests

# liboqs 互操作对拍（Mac，需 brew liboqs）
cd tee/tests && cc -O2 -I../ta -I../ta/config -I../include \
  -I/opt/homebrew/include ../ta/ta_fips202.c ../ta/ta_random.c \
  ../ta/ta_pqc.c ../ta/ta_mlkem*.c ../ta/ta_mldsa*.c test_interop_oqs.c \
  -L/opt/homebrew/lib -loqs -lcrypto -o interop_test && ./interop_test
```

## 6. 验证状态（无板阶段）

- Keccak 核：SHA3-256/512、SHAKE128/256 标准向量 KAT 全过；
  增量 API 与一次性 API 一致性过。
- KMAC-256：OpenSSL 3.6 生成的 5 组 KAT 全过（含 SP 800-185 sample1
  输入形状、空 custom/空 data、out=64）；label 域分隔、空 label 拒绝过。
- PWRP：往返、篡改 tag 拒绝且输出清零、AAD 不匹配拒绝、空明文边界过。
- PQC 六参数集：种子确定性（同种子同密钥对）、KEM 往返、篡改密文静默
  拒绝、签名往返、篡改签名/错误 ctx/篡改消息拒绝，全过。
- **liboqs 0.16 互操作（最接近 KAT 的验证）**：ML-KEM 三参数集同种子
  keypair 与 liboqs **逐字节相同**，双向 encaps/decaps 互通；ML-DSA 三
  参数集双向 sign/verify（含 ctx）互通。15/15 全过。
- 尚未做：TA 上板实测（依赖 P4 OP-TEE 镜像启动，当前卡在 BL32 入口，
  见进展报告）；堆叠 TA 栈用量实测（ML-DSA-87 签名栈峰值的真机确认，
  预留了 512KB）。

## 7. 上板部署（P4 修通后）

1. `tee/ta/out/4e2d9c1a-7b35-4f68-9a2c-d15e88406fa3.ta` 放入根fs
   `/lib/optee_armtz/`（Buildroot 包 `tee/` 目录或手动进镜像）。
2. 普通世界侧：`optee_client` 的 libteec + `tee/host/pqchsm_ta_client.c`，
   槽位管理器把后端从 `pqc_backend_native()` 切到 TA 客户端。
3. 自检顺序：GET_INFO → KEK_SET → KEYGEN(512) → DECAPS 往返 →
   SIGN/VERIFY（验签在普通世界用软件后端做）。
