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

## 第 2 步：槽位管理器 ✅

`ctest` 14/14 通过，**607 条断言**；ASan + UBSan 下同样 14/14；macOS `leaks` 查得 0 泄漏。

| 测试 | 断言 | 覆盖 |
|------|-----:|------|
| `test_slot_fsm` | 94 | 5 状态 × 9 事件 **全 45 组合**穷举 + 性质断言 |
| `test_slot` | 196 | ACL、PIN 锁定/解锁、句柄失效、用途互斥、种子存储、zeroize |
| `test_slot_meta` | 31 | KMAC 标签逐字段篡改检测 + 跨设备 sealing |
| `test_kdf` | 55 | NIST KMAC256 官方向量 #4/#5/#6 |
| （第 1 步的 4 个） | 231 | 见上 |

### 关键决策

1. **状态机写成显式转移表 + 纯函数**（`src/slot/fsm.c`），非法转移是"表里没有"而不是
   "某个 if 忘了写"。测试里的期望表是**照 §7.1 的图独立写第二遍**的，不是从实现抄的 ——
   否则只是在证明"表等于它自己"。45 组合中 30 组非法，这个数字本身也被断言，
   将来谁放宽了限制会立刻暴露。
2. **SO 解锁后恢复到锁定前的状态，而不是一律回"已装载"**。§7.1 的图写的是回已装载，
   但锁定也可能发生在空槽位上，那时回"已装载"等于谎报槽位里有密钥。这是对图的有意偏离。
3. **User PIN 失败锁槽位，SO PIN 失败只计数不锁**。§7.3 说"超限锁定槽位，仅 SO 可解锁" ——
   如果 SO 自己也被锁死，就没人能解锁，设备直接变砖。SO 凭证的兜底恢复归 §8.4 的
   Shamir M-of-N 仪式管（第 4 步）。**这条是安全性与可用性的权衡，值得复核。**
4. **句柄带 generation**：`handle = (generation << 32) | (slot_id + 1)`，
   destroy / zeroize 递增 generation，旧句柄立刻变 `HSM_ERR_BAD_HANDLE`。
5. **生成与由种子装载走同一条代码路径**（都先有种子再展开）。这样 §7.6 的种子存储策略
   不会变成一条没人走的旁路。SEED_STORAGE 槽位在装载时**立即销毁刚算出的私钥**，
   只留 64/32 B 种子，签名/解封装时现场重展开、用后即清 —— 测试里连续两次解封装
   都得到与主机一致的共享秘密，证明确实是重展开而不是留了副本。
6. **PIN 验证值 = KDF(KDR, slot_id ‖ role ‖ salt ‖ pin)**，不存明文也不存可离线爆破的哈希：
   没有设备根密钥就无法离线枚举 PIN。比较走 `pqc_ct_equal` 常量时间。
7. **元数据 KMAC 标签绑定 slot_id**，所以把 slot 3 的记录整条搬到 slot 5 也会被检出；
   同时绑定 KDR，换一台"设备"旧标签立刻失效（§8.3 sealing 的雏形，已有测试）。
8. **KDR 是桩**（`src/crypto/kdr.c` 里的固定假根密钥，源码里写明"Phase 7 这段必须消失"），
   符合 §5.7.2「设备绑定只能设计与桩实现，真实绑定必须有板」。

### 本步发现并修掉的两个问题

- `test_slot_meta` 里"把 PIN 失败计数改成 0"的篡改测试，因为基准值本来就是 0，
  等于什么都没改 —— 测试自己暴露了自己是假的，已改为非零起点。
- `hsm_slot_load_seed` 原先先动状态机再校验种子长度（虽有回滚）。已把便宜的参数校验
  提到状态机之前：不该因为一个长度写错的调用就去动槽位状态。

### 已知边界（不要当成已完成）

- 槽位内容目前**只在内存里**，没有持久化 —— 密钥库的密文落盘是第 3 步。
- 并发：尚未加每槽位互斥锁（§7.4）。当前接口不是线程安全的。
- 每槽位一个密钥对象（对应 §7.6 的 8 KB/槽预算），不是 PKCS#11 的任意多对象。

## 下一步：第 3 步 KEK 包裹与密文存储

1. 密钥层级 KDR → KDF(KMAC256) → KEK（§8.1），KEK 每次开机现场派生、不落盘；
2. wrap/unwrap：AES-256-GCM，blob 格式 `版本‖算法ID‖槽位元数据‖nonce‖密文‖tag`，
   **元数据进 AAD**（防"改元数据复用密文"），每次 wrap 用新鲜 nonce、严禁复用（§8.2）；
3. 密钥库文件的原子写入与掉电一致性（§5.7.3：在写入的每个位置注入 kill -9）；
4. 密钥库被篡改时装载必须失败（GCM tag 校验，Phase 5 验收项）。

之后是第 4 步（备份恢复 + Shamir M-of-N）、第 5 步（审计哈希链 + zeroize 验证）。
