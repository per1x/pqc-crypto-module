# 无硬件推进报告（《现状与后续计划》第 3 节 10 项）

> 日期 2026-07-29。起点：22 个 ctest / 2313 断言。
> 终点：**32 个 ctest / 2610 断言**，11 个新提交。

## 逐项状态

| # | 项 | 状态 | 关键产出 |
|---|----|------|---------|
| 3.1 | 审计链 ML-DSA 签名固化 | **done** | `anchor.c`，堵上纯哈希链的洞 |
| 3.2 | KDR provider 抽象 + KEK 轮换 | **done** | provider vtable + 结构性回归检查 |
| 3.3 | PKCS#11 前端 + SoftHSMv2 对比 | **done** | 27 个 C_* 接口，18/18 对比通过 |
| 3.4 | TCP+TLV 命令接口与 CLI | **done** | 分派与传输解耦，端到端 28/28 |
| 3.5 | 安全密钥注入（§8.5） | **done** | ML-KEM 封装 CEK 的自举闭环 |
| 3.6 | Profiling 与热点量化 | **部分 done** | 基线+Amdahl 工具已做；**符号级归因失败，已如实记录** |
| 3.7 | RTL / cocotb 纯仿真 | **done** | 参考模型→向量→RTL→cocotb 闭环，3000 条三方一致 |
| 3.8 | Vivado 综合估资源 | **部分 done** | cycle 预算已算；**综合脚本未经实机验证（无 Vivado）** |
| 3.9 | fuzz / 更多负测试 | **done** | libFuzzer 138 万次执行无崩溃 |
| 3.10 | 交叉编译 aarch64 + QEMU | **done** | 真 aarch64 Linux 32/32，并抓到一个 GCC 独有警告 |

## 每项的关键决策与发现

### 3.1 审计链头签名固化
锚点签的是 `(count, head, timestamp)`，校验要同时满足四条：签名有效、
公钥就是调用方 out-of-band 带来的那把、日志链自身完好、**日志前 count 条的
链哈希等于被签过的 head**。第四条才是真正堵洞的。

`test_anchor` 里有一条**对照**：同一份被整体重写的假日志，
`audit_verify_file` 返回 0（哈希链发现不了），`anchor_verify` 返回
`HSM_ERR_INTEGRITY`。洞被堵上没有，就看这一条。

局限也写进头文件了：锚点只覆盖**前缀**，两次锚定之间的窗口仍可被改写，
所以 §8.6 里"定期"两个字是本质的。

### 3.2 KDR provider 抽象
把根密钥抽象成 vtable（stub / 将来 bbram / efuse / puf），`hardware_backed`
标志供启动自检判断信任根有没有落地。

附带做了一条**结构性回归**：`tools/check_no_readback.py` 扫 `kdr.h`，
确保没人加回读出根密钥的接口（先剥注释再扫，已用反证验证 —— 加一个
`pqc_kdr_get()` 会立刻报错）。这是 Phase 7 那条"PS 侧无读回路径"在软件阶段的对应物。

### 3.3 PKCS#11 v3.2 前端
vendored 了 OASIS 官方 v3.2 头 —— v3.2 才**原生定义** `CKM_ML_KEM`/`CKM_ML_DSA`
与 `CKP_*` 参数集，系统上装的 OpenSC 通常还是 v3.0/3.1。

映射几乎是"翻译"而非"实现"，因为槽位管理器本来就是照 PKCS#11 设计的。
一槽一密钥对 ⇒ 一个槽位对外恰好两个对象，句柄可互推，不需要对象表。

**私钥的 `CKA_VALUE` 恒返回 `CKR_ATTRIBUTE_SENSITIVE`** —— HSM 的意义就在这条。

与 SoftHSMv2 在 8 项可比操作上行为一致；两处有意差异（机制集、
重复 `C_InitToken` 我们拒绝）在脚本顶部写明原因。

**已知上游限制**：OpenSC 0.27 的 CLI 还不认 ML-DSA 密钥类型，`--keypairgen`
走不通，所以 PQC 密钥生成由 `test_p11` 直接驱动 `C_GenerateKeyPair`。

### 3.4 命令接口
`pqc_proto_dispatch` 只做"请求字节 → 响应字节"，不碰 socket。
板子到手把 TCP 换成 UART 只改 `cli/pqchsmd.c`。

协议里**没有导出私钥的命令码**，`test_proto` 有一条扫全部命令码、
确认响应里不出现 4032 字节（ML-DSA-65 私钥）的反向断言。

**实测踩到的坑**：macOS 的 `signal()` 默认带 `SA_RESTART`，被信号打断的
`accept()` 会自动重启，daemon 收到 SIGTERM 永不退出。改用 `sigaction`
且不设 `SA_RESTART`。

### 3.5 安全密钥注入
注入端只需设备的 ML-KEM 公钥，`Encaps` 出一次性 CEK 包裹**种子**（不是完整私钥），
设备端 `Decaps` 得到同一 CEK、解出种子直入槽位。链路上只有密文。

已装载的槽位必须带 `SLOT_POLICY_INJECTABLE` 才允许被注入更新 ——
不能靠注入无声顶掉不该被替换的密钥。

制造模式是桩：明文注入仅在其打开时可用，且"熔断"后进程内**不可再打开**
（模拟 eFUSE 不可逆）。

### 3.6 Profiling（部分 done —— 这一项要特别说明）
**做成了**：算法级基线（20 项实测）、`tools/amdahl.py` 切分决策工具
（用文献占比跑出的 1.37/2.09/4.82/2.45 与路线图 §5.2.3 逐项吻合，说明计算无误）。

顺带证实：`keygen_seed` 与 `keygen` 几乎同价（ML-KEM-768 是 9.5 vs 10.0 µs），
§7.6 种子存储的代价可以接受。

**没做成**：符号级热点归因。三轮排查的根因是 liboqs 0.16 的 ML-KEM/ML-DSA
走**手写 aarch64 汇编**、没有帧指针，统计采样在原理上穿不过去
（`nm` 查得 65 个 `aarch64_asm` 符号；样本 89% 落在调用点 `run_op`）。

**没有硬凑数字** —— 用错的占比做硬件决策比没有数字更危险。
`docs/reports/profiling.md` 里记了完整排查过程，并给出在目标平台上用
`perf --call-graph dwarf` 或确定性计数拿真数字的做法。

### 3.7 RTL 纯仿真闭环
`hardware/model/ref_model.py` 是**独立重写**的 ML-KEM 参考模型；
`export_vectors.py` 导出 §5.1.3 要求的分层向量（L0 算子 / L1 蝶形 / L2 NTT /
Keccak / SHAKE），每个文件头写明字段与字节序。

`hardware/rtl/mlkem/` 下是纯 RTL（不依赖厂商 IP），cocotb 做**三方比对**：
RTL == 向量文件 == 模型现算。3000 条全过。

**独立模型抓到的约定坑**：`invntt(ntt(x))` 不是恒等，而是 ≡ x·2¹⁶ (mod q)
（参考实现 invntt 里的 `f = mont²/128`）。这类约定不重写一遍就发现不了 ——
这正是 §5.1.3 要求"两份独立实现"的理由。

### 3.8 综合估资源（部分 done）
**做成了**：`tools/cycle_budget.py` —— §5.8.1 说这张表"在没有任何硬件、
甚至没有 RTL 的情况下就能算出来"。复现路线图算例：896 个蝶形，
1 并行 910 cycles/6.07 µs，8 并行 126 cycles/0.84 µs，附并行度 → BRAM bank 的取舍。

**没做成**：实际综合。Vivado 需 AMD 账号且几十 GB，按约定不在本机装。
`hardware/syn/ooc_synth.tcl`（OOC 综合 + 布局布线 + 直接算 Fmax）已写全并在 README
里**明确标注"未经实机验证"**，附常用 part 对照表。

### 3.9 fuzz
四个解析入口（密钥库 / 备份 / 审计 / 命令协议）——都在按文件或报文自称的长度
索引缓冲，是最典型的内存安全面。

双模式 harness：libFuzzer（brew llvm）+ 自带独立驱动（接进 ctest，
CI 不能依赖 brew 装没装 llvm）。独立驱动从**合法语料**出发变异，
含专打"声明长度 > 实际长度"的截断变异。

结果：ASan 下独立驱动 20000 轮无崩溃；**libFuzzer + ASan + UBSan 跑 46 s、
138 万次执行、cov 410、无崩溃**。

### 3.10 aarch64 Linux 回归
开发机本身是 arm64，所以 `linux/arm64` 容器是**原生执行不是模拟**，
比 `qemu-aarch64` 快一个量级，且跑的是真 glibc + 真 Linux 系统调用。

**32/32 全过**（GCC 12 / glibc / aarch64）。

**换平台抓到的真问题**（clang 不报、GCC 报）：`pqc_secure_alloc` 里先 `mlock`
后 `memset`，GCC `-Wmaybe-uninitialized` 告警 —— glibc 把 `mlock` 声明成读
`const void *`，它说的是**被指内存**未初始化。改成先清零再锁页，
语义上也更合理。现在 GCC 全警告集下 0 warning。

## 总测试情况

| 指标 | 起点 | 终点 |
|------|-----:|-----:|
| ctest 用例 | 22 | **32** |
| 断言 | 2313 | **2610** |
| ASan + UBSan | 通过 | 通过（29/29，排除依赖外部工具的 smoke） |
| TSan（并发） | 0 race | 0 race |
| aarch64 Linux | 未测 | **32/32** |
| libFuzzer | 无 | **138 万次执行无崩溃** |
| cocotb RTL 对拍 | 无 | **3000 条三方一致** |

## 第二轮（2026-07-29 续）：Keccak 核、PKCS#11 补齐、确定性 profiling

### A. Keccak-f[1600] 核 —— 按 Amdahl 的第一顺位

`hardware/rtl/keccak/keccak_f1600.v`，**单轮迭代**（`round_cnt` 走 24 轮），不是 24 轮
全展开。理由在文件头写了：全展开是 24 份轮逻辑、面积没必要地爆掉，而
24 cycle @100 MHz = 240 ns 对 PS 侧调用频度完全够。接口与 `ntt_core` 同形，
`done` 是电平语义，可直接挂到 `accel.h` 的寄存器契约上。

**独立预言机（照 NTT 那套，三个互不相干的来源）**

1. **Keccak 官方公开向量**：全零态一次置换的 25 个 lane，硬编码常量，
   不由本项目任何代码生成。cocotb 与 `test_accel` 各钉一遍。
2. **hashlib / OpenSSL EVP**：在 RTL 核之上手工搭海绵做
   SHAKE128/256、SHA3-256/512，逐字节比对。**这条最强** —— 它验的不只是
   置换，padding、rate、lane 小端序、多块吸收与多块挤压全在内。
3. `hardware/model/ref_model.py` 的 Python 置换只作辅助定位，不算独立来源。

**反证**：把 RC[17] 改一个 bit 重编，`test_accel` 从 791/791 变成 57 条失败；
改回来又全绿 —— 断言确实是活的。

**接进寄存器缝**：新增 `ACCEL_MODE_KECCAK_F1600` 与
`accel_keccak_f1600()` / `accel_shake()`。置换在"硬件"侧（RTL 仿真或软件桩），
海绵在 C 侧 —— 真 PL 上 Keccak 核通常也只暴露置换，framing 由 PS 做，
这样 SHAKE128/256 与 SHA3-256/512 共用同一个核。**于是 SHA3/SHAKE 整条
路径都能跑在仿真 RTL 上**，且与软件桩的结果逐字节相同。

顺带修了一个之前埋下的问题：`ntt_core` 改成实例化子模块后，CMake 里的
verilator 调用没跟着更新（缺 `mont_reduce.v`/`butterfly.v`，且多文件时
verilator 挑了第一个文件当 top，生成的是 `Vmont_reduce` 而不是 `Vntt_core`）。
补 `--top-module` 定死，两个模块各用各的 `-Mdir`。

### B. PKCS#11 补齐四组接口

`C_CreateObject`、`C_SignUpdate/Final`、`C_VerifyUpdate/Final`、
`C_EncapsulateKey/C_DecapsulateKey`，外加 v3.0 引入的
`C_GetInterface/C_GetInterfaceList`。

**为什么必须做 `C_GetInterface`**：`CK_FUNCTION_LIST` 是 **2.40 形状**的结构，
里面根本没有 `C_EncapsulateKey`/`C_DecapsulateKey` 字段。只填
`C_GetFunctionList` 的话，新实现的 KEM 接口对调用方是**不可达的**。
现在两张表并存，指向同一批实现。

**三处设计取舍**（都写在各自函数上方）

| 接口 | 取舍 | 理由 |
|------|------|------|
| `C_CreateObject` | 只收 `CKA_SEED`，**不收明文私钥** | `slot.h` 里压根没有"把 sk 字节装进槽位"的入口（§7.6 的结论）。给 `CKA_VALUE` 明确返回 `CKR_ATTRIBUTE_TYPE_INVALID`，不假装成功。FIPS 203/204 的密钥由种子完全决定，所以不削弱能力 |
| `C_SignUpdate/Final` | 累积后单次签，**不是流式** | ML-DSA 不是 hash-then-sign：μ 要用到整条消息，拒绝采样每次重试都要它。如实说明内存占用与消息等长 |
| `C_Encapsulate/Decapsulate` | 共享秘密落成**会话对象** | 槽位是持久的一槽一密钥对结构（§7.6 的 8 KB/槽预算），临时的 32 B 共享秘密塞进去既浪费也不对。默认 sensitive + 不可 extract |

**顺带修一个规范违背**：`C_CloseAllSessions` 原来忽略 `slotID` 一律全关。

**测试**（含独立预言机与反证）：分段签 vs 一次性签都用独立的 liboqs 验；
`C_CreateObject` 把同一种子直接喂 `pqc_keypair_from_seed` 逐字节比对；
Encap/Decap 两端共享秘密逐字节相同，且篡改密文后必须解出不同的值
（ML-KEM 隐式拒绝，是"不等"而非"报错"）。`test_p11` 154 → **254 断言**。

### C. 确定性 profiling —— 把上一轮的失败项做掉了

上一轮记的失败是"符号级归因穿不过 liboqs 的手写汇编"。这轮换了拆法：

```
占比 = 调用次数 × 单次代价 / 总耗时
```

次数由 `tools/prim_count.py` 从**已逐字节重现过 ACVP 向量**的 FIPS 203 参考
实现里精确数出（不用改 liboqs 一行）：ML-KEM-768 KeyGen 43 次置换 / 6 次 NTT。
计数与手推解析式 `1+9+6+27` 独立对上，自检已接进 ctest。

单次代价用差分法量（`t(2N)−t(N)` 抵掉 EVP 固定开销）：OpenSSL 一次置换 ~43 ns。

**结论与如实标注的失败项**

- Keccak 占比 KeyGen ≈22%、Encaps ≈21% —— 是**下界**。与文献 55% 的差距
  可解释：本机 Apple M 系列跑 NEON 汇编，分母被压小。
- **NTT 占比本机量不出可信值**：只有直白 C 版可作代表，代进去 >100%。
  这个荒谬结果本身就是结论，不硬凑数字。
- 硬件侧 Verilator 实测：`keccak_f1600` 24 cycle、`ntt_core` 1153 cycle。
  KeyGen 两核合计 7950 cycle = 79.5 µs @100 MHz —— **比 liboqs 的 9.7 µs 还慢**。
  直说不回避：本机不是能下这个结论的平台（目标是无 SIMD 的 A53）。
  但能确定两件事：Keccak 才占 1032/7950，**24 轮展开毫无必要**；
  瓶颈在 NTT 核，提速要加蝶形单元。

### D. ASan 抓到的真 bug

新写的用例给 `C_InitToken` 传了短字符串当 label。规范里 `pLabel` 是**定长
32 字节空格填充、不带 NUL**，模块照此读 31 字节 —— 于是读到了字符串常量区
外面。是用例违约，按规范补齐填充。

同时发现 ASan 编出的 `.dylib` 没法被普通进程 dlopen（macOS 平台策略），
三条 P11 脚本用例改为如实 SKIP —— 让它伪装成失败会淹掉真正的回归信号。

## 还剩什么

- **目标平台（aarch64/A53）的实测占比与真实加速比**：本机给不出，
  §2.6 的"直说"解释了为什么。需要交叉编译 + Linux `perf --call-graph dwarf`。
- **Vivado 实际综合**：需装 Vivado；脚本已就位但未验证（3.8）。
- **完整的 ML-KEM/ML-DSA 硬件核**：目前只有 NTT 与 Keccak 两个算子核。
  采样（SampleNTT/CBD）、编码/压缩、整机调度都还是软件 —— 这是 Phase 1–4 的主体。
- **HashML-DSA（`CKM_HASH_ML_DSA_*`）**：真正的流式签名要走这条，本模块未实现。
- 原《现状与后续计划》第 2 节里那些**必须有板**的项，一件没变，仍然必须有板。
