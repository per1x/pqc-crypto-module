# 进度

> 对应路线图 §5（零硬件阶段）与 Phase 5/6。

## 第 1 步：PQC 库集成 + KAT ✅

`ctest` 10/10 通过；KAT 共 **390 条向量逐字节比对通过，0 失败，60 条显式跳过**。

| 向量文件 | 内容 | pass | skip |
|---------|------|-----:|-----:|
| `mlkem_keygen.kat` | ML-KEM-512/768/1024 KeyGen(d,z) → (ek,dk) | 75 | 0 |
| `mlkem_encaps.kat` | Encaps(ek,m) → (c,k)，AFT | 75 | 0 |
| `mlkem_decaps.kat` | Decaps(dk,c) → k，VAL | 30 | 0 |
| `mlkem_keycheck.kat` | ek/dk 格式校验（FIPS 203 §7.2/7.3） | 0 | 60 |
| `mldsa_keygen.kat` | ML-DSA-44/65/87 KeyGen(ξ) → (pk,sk) | 75 | 0 |
| `mldsa_siggen.kat` | Sign，deterministic + hedged 各半 | 90 | 0 |
| `mldsa_sigver.kat` | Verify，含应当失败的负向量 | 45 | 0 |

向量来源钉死在 ACVP-Server `ad33b3d`（2026-07-28），见 `tools/fetch_vectors.sh`。
**全部为最终版 FIPS 203/204，非 round-3**（路线图 §5.1.1 的版本陷阱）。

### 已跳过的部分（不要当成"全绿"）

| 跳过项 | 数量 | 原因 | 何时补 |
|--------|-----:|------|--------|
| ML-KEM `encapsulationKeyCheck` / `decapsulationKeyCheck` | 60 | liboqs 不暴露独立的密钥格式校验 API | Phase 7 由硬件核输入校验路径覆盖 |
| ML-DSA `signatureInterface=internal` | 6 组 | liboqs 只暴露 external（含域分隔前缀）接口 | 需要时可直接调 PQClean 的 `_internal` 变体 |
| ML-DSA `preHash`（HashML-DSA） | 6 组 | liboqs 未暴露 | 若 PKCS#11 前端需要再补 |

### 这一步验证掉的三个非平凡结论

1. **liboqs 的 `keypair_derand` coins 就是 `d‖z`**（FIPS 203 KeyGen 的两个输入按序拼接）
   —— 75 条 keyGen 向量逐字节相符，顺序猜错的话一条都过不了。
2. **ML-DSA 可以用注入随机源做到完全确定性**：liboqs 没有 `keypair_derand`，
   本项目用 `OQS_randombytes_custom_algorithm` 脚本化喂入 ξ / rnd，
   keyGen 75 条 + sigGen 90 条全部相符，且实测消费字节数恰为 32 —— 模型正确。
   这条同时给路线图 §7.6 的**种子存储优化**扫清了障碍。
3. **签名的 `rnd` 语义可控**：deterministic 组（rnd=0³²）与 hedged 组（ACVP 给定 rnd）
   都能复现出与向量一致的签名，说明 `pqc_sign` 的 rnd 参数确实透传到了算法内部
   （`test_pqc_roundtrip` 里还有一条"换 rnd 必须换签名"的反向断言兜底）。

## 下一步：第 2 步 密钥库 + 槽位管理器

对应路线图 §7 与 Phase 5：

1. slot/token 模型与句柄化对象引用（PKCS#11 风格，但先走自定义命令集）；
2. 生命周期状态机 `未初始化 → 空 → 已装载 → 使用中`，外加任意状态可达且不可逆的 `zeroize`，
   以及 PIN 连续错误进入的 `锁定` 态（§7.1）；配**全状态 × 全事件的穷举测试**，
   非法转移必须被显式拒绝（§5.7.3）；
3. 槽位元数据（§7.2）：算法参数集、用途互斥、策略位、计数器、每条记录一个 KMAC 完整性标签；
4. SO / User 双角色 PIN：常量时间比较、失败计数持久化、超限锁定、仅 SO 可解锁（§7.3）。

之后是第 3 步（KEK 包裹）、第 4 步（备份恢复 + Shamir）、第 5 步（审计哈希链 + zeroize 验证）。
