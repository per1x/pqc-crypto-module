[English](algorithms.md) · **中文**

# 算法清单

模块用到的每一个密码算法、它实现在哪里、正确性钉在什么上。这张表是 FIPS 140-3
或 GM/T 0028 送检材料的起点：每一行都是一个需要单独取得算法证书的算法，
证据一列写明支撑它的产物。

## 核准算法

| 算法 | 标准 | 参数集 | 在模块中的用途 | 实现 | 证据 |
|---|---|---|---|---|---|
| ML-KEM | FIPS 203 | 512、768、1024 | 密钥封装；密钥注入；备份传输 | liboqs（`src/crypto/pqc_liboqs.c`） | 180 条 ACVP 向量，逐字节一致 |
| ML-DSA | FIPS 204 | 44、65、87 | 签名与验签；审计链锚定；设备身份 | liboqs（`src/crypto/pqc_liboqs.c`） | 210 条 ACVP 向量，逐字节一致 |
| AES-256-GCM | FIPS 197、SP 800-38D | 256 位密钥、96 位随机数、128 位标签 | 静态密钥包裹；密钥库记录 | OpenSSL 3（`src/store/wrap.c`） | OpenSSL 已验证实现；往返与篡改检测测试 |
| SHA3-256 | FIPS 202 | — | 审计哈希链；Shamir 分片校验和 | OpenSSL 3（`src/crypto/kdf.c`、`src/audit/audit.c`） | 与 OpenSSL 及一份独立的 Keccak 置换比对 |
| SHAKE128 / SHAKE256 | FIPS 202 | — | ML-KEM 与 ML-DSA 内部的 XOF；建在 RTL Keccak 核之上的海绵 | OpenSSL 3；RTL 路径见 `src/hal/pqc_accel.c` | 与 OpenSSL 和 `hashlib` 逐字节一致 |
| KMAC256 | SP 800-185 | 256 位密钥 | 密钥派生（KEK、BEK）；密钥库整文件 MAC | OpenSSL 3（`src/crypto/kdf.c`） | 与 NIST 示例值及 OpenSSL 比对 |

每个核准算法另配一条上电已知答案测试，期望值来自模块之外，见
[security-policy.zh-CN.md](security-policy.zh-CN.md#10-自测试)。

## 非核准但允许的功能

| 功能 | 标准 | 用途 | 实现 | 说明 |
|---|---|---|---|---|
| GF(256) 上的 Shamir 秘密共享 | — | M-of-N 备份与恢复 | `src/backup/shamir.c` | 信息论安全；GF(256) 乘法为常量时间。分片校验和不带密钥，只能发现损坏，不能发现篡改。 |
| 随机比特生成 | — | 种子、随机数、恢复主密钥 | `RAND_bytes`（OpenSSL） | OpenSSL 的 DRBG。带 SP 800-90B 健康检测的硬件噪声源已有 RTL（`hardware/rtl/trng/`），但尚未接入软件路径。 |

## 硬件实现

RTL 核是算术构件，不是完整的算法实现。单独列出是因为它们的验证状态不同：
它们在仿真中对着独立预言机、以及用这些算子重建出来的 NIST 向量验证，
但不存在任何完整算法的硬件实现。

| 核 | 在算法中的位置 | 验证方式 |
|---|---|---|
| `ntt_core`、`mlkem_basemul` | ML-KEM 的变换域 | schoolbook 负循环卷积；ACVP 的 `ek`/`dk` 重建 |
| `mlkem_compress`、`mlkem_decompress` | FIPS 203 §4.2.1 | 对整个输入域穷举，与有理数定义式比对 |
| `mlkem_cbd2`、`mlkem_cbd3` | FIPS 203 Alg 8 | FIPS 203 的逐比特定义，比特组穷举 |
| `mlkem_rej_pair`、`mlkem_rej_uniform` | FIPS 203 Alg 7 | 真实 SHAKE128 流上的独立 `SampleNTT` |
| `mlkem_encode12`、`mlkem_decode12` | FIPS 203 的 ByteEncode/Decode | 往返还原与 ACVP 重建 |
| `mldsa_ntt_core`、`mldsa_mont_reduce` | ML-DSA 的变换域 | schoolbook 负循环卷积；ACVP 的 `pk`/`sk` 重建 |
| `mldsa_power2round`、`mldsa_decompose` | FIPS 204 §7 | 分解式与取值范围 |
| `mldsa_make_hint`、`mldsa_use_hint` | FIPS 204 §7 | FIPS 204 依赖的那条还原性质 |
| `mldsa_rej_uniform`、`mldsa_rej_eta` | FIPS 204 Alg 30、31 | 独立实现；接受门限逐值验证 |
| `keccak_f1600` | FIPS 202 的置换 | 公开的全零置换输出；海绵与 OpenSSL 比对 |
| `trng_health` | SP 800-90B §4.4 | 阈值按定义现算；卡死源、偏置源、均匀源三种情形 |

## 密钥与参数清单

| 项目 | 类型 | 生成方式 | 存储 | 清零 |
|---|---|---|---|---|
| KDR | 256 位根秘密 | 设备绑定（当前是固定常量，见局限） | 不出模块 | 常量状态下不适用 |
| KEK | 256 位 AES 密钥 | 由 KDR 经 KMAC256 派生 | 按需派生，不落盘 | 每次使用后清零 |
| BEK | 256 位 AES 密钥 | 由恢复主密钥经 KMAC256 派生 | 按需派生 | 每次使用后清零 |
| 恢复主密钥 | 256 位秘密 | `RAND_bytes` | 拆成 M-of-N 分片，从不整体存储 | 拆分后清零 |
| 槽位私钥 | ML-KEM / ML-DSA 私钥 | 设备内生成，或在封装出的会话密钥保护下注入 | 以 KEK 做 AES-256-GCM 包裹 | 销毁槽位时清零 |
| 设备身份密钥 | ML-DSA 私钥 | 设备内生成 | 与普通槽位密钥同样包裹 | 设备重置时清零 |
| PIN | 用户与安全管理员的鉴别数据 | 由操作员设置 | 带槽位盐值的 KMAC256 摘要 | 比较后清零明文缓冲 |

## 与送检相关的局限

以下每一条都足以阻断送检，写在这里是为了避免把清单读成"已经就绪"。

- 密钥派生根是固定常量，因此没有任何密钥真正与设备绑定。真实设备应从 eFUSE、
  BBRAM 或 PUF 取得该值。
- 随机比特来自由操作系统播种的 OpenSSL DRBG，而不是模块边界内的噪声源。
- 模块边界是一个进程地址空间。运算期间明文密钥材料存在于该地址空间中。
- 尚未取得任何算法证书。ACVP 向量是在本地对着厂商实现跑的，这是正确性证据，
  不是证书。
