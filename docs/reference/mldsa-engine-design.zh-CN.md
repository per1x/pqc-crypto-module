# ML-DSA 共享引擎设计（mldsa_engine）

本文记录**接口契约**与**输入/输出缓冲地址布局**。AXI 从机那条线照本文写，
两边对同一份。算法本身见 `mldsa-keygen-design`/`mldsa-sign-design`/`mldsa-verify-design`。

> 状态：接口与地址布局已定死（见下）。引擎本体（三套 FSM 时分复用一套算术）
> 与运行时参数集切换尚未实现 —— 当前仓库里仍是 keygen/sign/verify 三个独立核，
> 各自例化自己的 NTT 与采样器。本文先把**对外契约**固定下来，
> 好让 AXI 那条线不必等引擎落地就能动工。

## 1. 端口

```verilog
module mldsa_engine (
    input  wire        clk, rst_n,
    input  wire        start,          // 脉冲
    input  wire [1:0]  op,             // 0=KeyGen 1=Sign 2=Verify
    input  wire [1:0]  pset,           // 0=44 1=65 2=87
    output wire        busy, done,
    output wire        verify_ok,      // op=Verify 时有效
    input  wire        in_we, input wire [14:0] in_addr, input wire [7:0] in_data,
    input  wire [15:0] msg_len,        // 消息字节数
    input  wire [15:0] ctx_len,        // context 字节数
    input  wire [14:0] out_addr, output wire [7:0] out_data,
    output wire [15:0] out_len,
    input  wire        zeroize,        // 把内部所有存储逐地址写 0
    output wire        wiping,         // 擦除进行中，此间拒绝 start
    output wire sha_start, output wire [7:0] sha_rate, sha_suffix,
    output wire sha_in_valid, output wire [7:0] sha_in_data, output wire sha_in_flush,
    input  wire sha_in_ready, sha_out_valid, input wire [7:0] sha_out_data,
    output wire sha_out_ready
);
```

### 1.1 三条端口表之外的约定（`mldsa_axi` 已按此实现，必须对齐）

1. **`done` 是电平**，置位后保持到下一次 `start`（与 `mldsa_ntt_core` 同一约定，
   理由也相同：AXI 轮询在任意时刻采样会漏掉一拍的脉冲）。
2. **`out_data` 是 ram_dp 式同步读**：`out_addr` 摆上去，**下一拍**才出数。
   引擎的输出缓冲本来就是 BRAM，BRAM 没有组合读口。
3. **`in_we/in_addr/in_data` 是普通同步写口，且空闲时可写**：
   START 之前灌数据、以及从片内金库搬 sk，走的都是它。

### 1.2 `zeroize` / `wiping` —— 逐地址擦除

**为什么不能靠复位**：BRAM 的存储阵列**不因复位清零**（ram_dp 的文件头也写了这一条：
一旦给存储加复位，综合就退回 LUT，前功尽弃）。所以从机把引擎按在复位上并不能
清掉内部缓冲 —— 擦除之后 sk 字节可能仍然残留在引擎里。
金库擦干净了、残渣却留在旁边，这与"私钥不出硬件"的口径直接冲突。

要求：

* 覆盖**所有**存过私钥或中间敏感量的存储：sk 解包后的 s₁/s₂/t₀、y、z、c，
  以及输入/输出字节缓冲。公开量（pk/sig/msg）一起擦也无妨 —— **简单为上**，
  漏擦一块的代价远大于多擦几块的代价。
* 擦除期间 `wiping` 置起（`busy` 也可一并置起），`start` 被忽略；擦完自动落下。
* 写法照 `hardware/rtl/bus/mlkem_axi.v` 里 dk 金库那个 wipe FSM：
  **逐地址写 0**，不是靠复位。那边有现成范例与注释。

触发来源在从机侧：`CTRL[2]=ZEROIZE` 与 tamper。

#### ⚠️ 加这两个口会当场打断 `mldsa_axi` —— 需要从机侧配合改两行

现在 `hardware/rtl/bus/mldsa_axi.v` 例化 engine 时**没有**这两个口，
靠 `.rst_n(rst_n && !zeroize_all)` 把它按在复位上（该文件第 119–122 行自己
记了这条缺口，并写明"补法是给 engine 加一个 zeroize 口，那是 engine 那条线的改动"）。

engine 一旦长出 `zeroize`/`wiping`，那个例化就少了两个端口 ——
Verilator 的 **PINMISSING 是刻意保持开启的**（见 `lint_waivers.vlt` 末段：
放过的只有显式留空 `.x()`，省略端口一条都不放过），于是整仓 lint 会红。

**从机侧需要的改动（我没动 `rtl/bus/`，按分工留给对方）**：

```verilog
// mldsa_axi.v 的 mldsa_engine u_eng 例化里加两行：
        .zeroize(zeroize_all),
        .wiping (eng_wiping),      // 新增 wire，并入 STATUS 的 WIPING 位
```

同时 `rst_n` 建议改回纯 `rst_n`：既然有了真擦除口，就不该再靠复位去"擦"
（复位本来也擦不掉 BRAM，那正是这条缺口的由来）。

`hardware/tb/cocotb/stub_mldsa_engine.v` 与 `hardware/tb/lint/mldsa_engine.v`
两个替身同理要补口；后者在真 engine 落地后由 `rtl_lint.sh` 自动不再参与。

## 2. 参数集常数（FIPS 204 Table 1 / Table 2）

| pset | 名称 | k | ℓ | η | τ | γ₁ | γ₂ | ω | β | c̃ | pk | sk | sig |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | ML-DSA-44 | 4 | 4 | 2 | 39 | 2¹⁷ | (q−1)/88 | 80 | 78 | 32 | 1312 | 2560 | 2420 |
| 1 | ML-DSA-65 | 6 | 5 | 4 | 49 | 2¹⁹ | (q−1)/32 | 55 | 196 | 48 | 1952 | 4032 | 3309 |
| 2 | ML-DSA-87 | 8 | 7 | 2 | 60 | 2¹⁹ | (q−1)/32 | 75 | 120 | 64 | 2592 | 4896 | 4627 |

存储与数据通路一律按最大的 87 开；k/ℓ/η/τ/γ₁ 位宽/γ₂ 模式/ω/β/c̃ 长度
在 `start` 那一拍按 `pset` 锁存进配置寄存器，循环边界与地址计算用寄存器值。

## 3. 输入缓冲布局（`in_addr`）

字节流按 op 分三种，**段的顺序固定，偏移由 op + pset + ctx_len 唯一确定**：

```
KeyGen : ξ(32)
Sign   : sk(sk_len) ‖ rnd(32) ‖ ctx(ctx_len) ‖ msg(msg_len)
Verify : pk(pk_len) ‖ sig(sig_len) ‖ ctx(ctx_len) ‖ msg(msg_len)
```

偏移表（`SK`/`PK`/`SIG` 按 pset 取上表的值）：

| op | 段 | 起始偏移 | 长度 |
|---|---|---|---|
| KeyGen | ξ | 0 | 32 |
| Sign | sk | 0 | SK |
| Sign | rnd | SK | 32 |
| Sign | ctx | SK+32 | ctx_len |
| Sign | msg | SK+32+ctx_len | msg_len |
| Verify | pk | 0 | PK |
| Verify | sig | PK | SIG |
| Verify | ctx | PK+SIG | ctx_len |
| Verify | msg | PK+SIG+ctx_len | msg_len |

### 3.1 三条约定（都不是可选项）

**① `rnd` 永远出现在流里，固定 32 字节。**
不设 mode 位区分确定性/hedged：确定性签名就写 32 个零字节。
理由是验证能力本身 —— hedged 每次取新随机数，签出来不可能匹配固定期望值，
**没有喂 rnd 的入口就没法拿 ACVP siggen 验签名**。ACVP 的确定性条目正是 rnd=0³²。

**② `ctx` 紧接在 `rnd` 之后、`msg` 之前。**
external + pure 的 M′ 封装由引擎内部拼：`M' = 0x00 ‖ len(ctx) ‖ ctx ‖ msg`
（即 `bytes([0, len(ctx)]) + ctx + msg`）。
`ctx_len = 0` 是常见情况（PKCS#11 路径恒为 0），但 ACVP 向量里有非空 ctx，**必须支持**。

**③ sk 恒占 `[0, SK)`，不设"这次流里有没有 sk"的端口。**
走片内金库时，由 AXI 从机把 sk 从金库流进 `in_*` 口，写的就是这段地址 ——
对引擎而言两条路径没有区别，它只看到字节按上面的顺序到达。
这样偏移只依赖 `op`/`pset`/`ctx_len`，不需要额外的模式位，也就少一处两边可能对不齐的地方。

### 3.2 消息长度上限（**已拍板：软件保证 + 超长明确拒绝**）

`in_addr` 只有 15 位 ⇒ 输入缓冲共 **32768 字节**，而 `msg_len` 是 16 位
（最大 65535）。两者对不上。

**结论：不加宽 `in_addr`。** 由软件保证不超，超长由软件侧**显式拒绝**
（不是截断、不是回绕 —— 那两种都会安静地算出一个错签名）。
改流式喂虽然更"正确"，但要动 AXI 侧喂数模型与两条线的约定，
为一个够用的限制不值得。

上限 = `32768 − 定长前缀 − ctx_len`。逐参数集写死如下
（`ctx_len` 按 FIPS 204 是单字节长度，取值 0…255）：

| op | pset | 定长前缀 | 前缀字节数 | **msg_len 上限** |
|---|---|---|---|---|
| Sign | 44 | sk(2560) + rnd(32) | 2592 | **30176 − ctx_len** |
| Sign | 65 | sk(4032) + rnd(32) | 4064 | **28704 − ctx_len** |
| Sign | 87 | sk(4896) + rnd(32) | 4928 | **27840 − ctx_len** |
| Verify | 44 | pk(1312) + sig(2420) | 3732 | **29036 − ctx_len** |
| Verify | 65 | pk(1952) + sig(3309) | 5261 | **27507 − ctx_len** |
| Verify | 87 | pk(2592) + sig(4627) | 7219 | **25549 − ctx_len** |

KeyGen 只吃 ξ(32)，与消息无关，不受此限。

最紧的一格是 **Verify-87：25549 − ctx_len 字节**。软件侧若只想记一个数，
按 **25549 − ctx_len** 卡全局最严即可（对所有 op/pset 都安全）。

对原型演示与 ACVP 向量而言绰绰有余：ACVP 的消息最长也就几百字节。

## 4. 输出缓冲布局（`out_addr` / `out_len`）

| op | 内容 | out_len |
|---|---|---|
| KeyGen | pk(PK) ‖ sk(SK) | PK+SK |
| Sign | sig(SIG) | SIG |
| Verify | 无（结果看 `verify_ok`） | 0 |

## 5. 时序现状（OOC 实测，Vivado 2020.1 / xazu3eg-sfvc784-1-i / 100MHz 约束）

全部是 **post-route** 的 WNS（post-synth 偏乐观，不作数）。Fmax 由 `1/(10ns − WNS)` 反推。

| ML-DSA-87 | 改造前 WNS | 改造前 Fmax | 改造后 WNS | 改造后 Fmax | 75MHz 下余量 |
|---|---|---|---|---|---|
| KeyGen | −1.479ns | 87.1 MHz | **+3.422ns（MET）** | **152.1 MHz** | +6.76ns |
| Verify | −3.487ns | 74.1 MHz | **+0.993ns（MET）** | **111.0 MHz** | +4.33ns |
| Sign | −3.332ns | 75.0 MHz | −1.611ns | 86.1 MHz | +1.72ns |

面积（ML-DSA-87 Sign）：LUT 5831→5474，FF 3707→4324，BRAM 26.5→28 tile，**DSP 38→20**。
DSP 少了近一半：原来 CT/GS/缩放/逐点/MAC 各自例化一份组合 `mldsa_mont_reduce`，
现在全工程只有一条 `mldsa_mont_mul_pipe`，各段分时复用。

### 做了什么

1. **NTT 蝶形切成 5 级流水**，旋转因子 ROM 读出也打一拍；
   系数存储改成**乒乓两块**（读一块写另一块），层内 RAW 在结构上不存在，
   层间靠排空解决。吞吐顺带从 2 拍/蝶形变成 1 拍/蝶形
   （正 2049→1081，逆 2561→1344 cycles）。
2. **逐点乘 / MAC 的组合 mont 链也换成同一条流水链**（keygen 的 Â∘ŝ₁、
   sign 的 Â∘ŷ 与 ĉ∘ŝ、verify 的 Â∘ẑ 与 ĉ∘t̂₁）。这一步是必需的：
   只做第 1 步时 Sign 的 WNS 只从 −3.332 动到 −3.356 —— 关键路径**原地搬家**
   到了这些链上，形状一模一样（BRAM → 三次 32×32 乘 → BRAM）。

### ⚠️ Sign 还差 1.611ns —— 瓶颈已不在乘法

Sign 现在的关键路径是 **⑨ hint 写回段**，与乘法无关：

```
u_ntt（invNTT 结果）→ u_red2(reduce32) → a0 = w0_dout + comb_red
  → u_reda0(reduce32) → mldsa_make_hint(内含 decompose 的乘法与比较)
  → hint_bit → weight_reg 的 CE
```

逻辑层级 35，其中 **CARRY8=15** —— 两级 reduce32 加一次 decompose 的比较链串在一拍里。

**下一步**（尚未做）：把 S_Z_WB / S_R_WB / S_H_WB 三段按与逐点乘同样的办法流水化 ——
每拍发一个系数，写回地址与旁路量走 tag，段末排空。预计能把 Sign 也推过 100MHz。
在那之前，Sign 在 75MHz 下已有 +1.72ns 实打实的余量（改造前只有 +0.001ns）。

## 6. 运行时选参数集：进度与剩余工作

目标是**同一个 bitstream 运行时选 44/65/87**。做法是自底向上：
先把叶子模块里"随参数集变"的东西从**编译期参数**改成**运行时端口**，
调用方暂时仍喂自己的编译期常量（行为不变，九格矩阵可继续当安全网），
等 engine 落地时再由 engine 用 `pset` 锁存的配置寄存器去驱动。

### 已完成（本轮）

| 模块 | 原来 | 现在 | 验到哪一层 |
|---|---|---|---|
| `mldsa_bitpack` | `parameter W`（编译期） | `input [4:0] w`（运行时），端口按最宽 20 位开 | t₁/t₀ 打包对黄金模型 |
| `mldsa_bitunpack` | `parameter W` | `input [4:0] w` | 随 sign/verify 的九格 |
| `mldsa_polyeta_pack` | `parameter ETA` | `input [2:0] eta` | **同一次仿真里 η=2 与 η=4 都对上黄金模型** |
| `mldsa_decompose` | `parameter MODE` | `input mode` | 两种 γ₂ 对向量一致 |
| `mldsa_make_hint` | `parameter MODE` | `input mode` | 同上 |
| `mldsa_use_hint` | `parameter MODE` | `input mode` | 同上 |

`mldsa_polyeta_pack` 那条用例是**运行时可切的第一份证据**：
分两次编译各跑一个 η 证明不了运行时可切，所以那条用例改成一次仿真里连跑两种。

### 剩余工作（未做，按依赖顺序）

1. **`mldsa_sample_in_ball`**：`TAU`/`CTB` 改运行时。
   注意 `seed` 端口现在是 `CTB*8-1:0`，要固定成 512 位、只读前 `ctb` 字节；
   这会牵动 sign/verify 里 `ctilde` 寄存器的宽度（同样固定 512 位）。
2. **`mldsa_expand_mask`**：`GAMMA1`/`CBITS` 改运行时（γ₁ = 2¹⁷ 或 2¹⁹）。
3. **三个核自己的 K/L/η/τ/ω/β/c̃**：改成 start 时锁存的配置寄存器。
   K/L 出现的地方是集中的（`KM1`/`LM1` 循环边界、`SK_S1/SK_S2/SK_T0` 段偏移、
   `PKLEN`），keygen 里约 15 处，sign/verify 各多一些。
   段偏移里有 `L*PEB` 这类乘法，运行时化之后是一个小乘法器，不是问题。
4. **`mldsa_engine.v` 组装**：三套 FSM 时分复用一套 NTT / `mldsa_mont_mul_pipe` /
   采样器 / 打包解包器；对外换成**字节口**（15 位地址的输入缓冲 + 输出缓冲），
   接口照 `hardware/tb/cocotb/stub_mldsa_engine.v` 那份（它已被 19 条 AXI 用例验过）。
   ⚠️ 现在三个核的对外形态与 engine 差得很远（keygen 直接吃 256 位 `xi`、
   sign 用 sk/msg/ctx 三个独立写口与各自的地址空间、都带 `dbg_*` 观察口），
   这一层不是"包一层壳"，是要把三个核的输入输出全部改走统一字节缓冲。

### 验收怎么做（照协调方的要求）

* 九格矩阵全绿（三 op × 三参数集，各自的 ACVP）——每一步改完都要跑；
* **运行时切换专用用例**：同一次仿真里先 44 再 65 再 87，各自对上 ACVP。
  叶子这一层已经有了（`test_polyeta_pack`），核与 engine 那一层还没有；
* `zeroize` 反证：擦除后逐地址读内部存储必须是 0，照 `mlkem_axi` 的 wipe FSM 验法；
* OOC 时序不许退步：KeyGen/Verify 保持 MET，Sign 在 75MHz 下 ≥ +1.5ns 余量。
  合成一个引擎后关键路径会变，**必须重新量**。

## 7. 海绵

引擎自带**一份** `sha3_core`，与 ML-KEM 那份是两份，不跨模块共享。
Keccak 理论上能共享，但那要跨 AXI 从机仲裁、动到已验证的 ML-KEM 安全边界，
这一轮不做（面积有余量：ML-DSA-87 Sign 单核 post-route 才 5.4k LUT / 28 BRAM tile）。

引擎内部的海绵归属由 `owner` 选通（FSM / ExpandMask / ExpandA / SampleInBall），
只在换手方空闲时切 —— 这条是 KeyGen 踩过的坑，见 keygen 设计文档。
