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

### 3.2 ⚠️ `in_addr` 只有 15 位 —— 消息长度受限

15 位 = 32768 字节，而 `msg_len` 是 16 位（最大 65535）。两者对不上，所以：

```
最大可喂消息 = 32768 − 段前缀长度
  Sign   87：32768 − 4896 − 32 − ctx_len = 27840 − ctx_len
  Verify 87：32768 − 2592 − 4627 − ctx_len = 25549 − ctx_len
```

超过这个长度的消息**当前布局装不下**。三种处理方式，需要拍板：
1. 维持现状，由软件保证不超（PKCS#11 的签名对象通常远小于此）；
2. `in_addr` 加宽到 16/17 位；
3. 消息改成"流式喂"（不进缓冲，直接进海绵），这样 msg 不占地址空间。

从密码机的用法看 ③ 最正确（消息只被海绵吸收一次，本来就不需要随机访问），
但它改变 AXI 侧的喂数模型，需要两条线一起定。**先按 ① 实现，把限制写在这里。**

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

## 6. 海绵

引擎自带**一份** `sha3_core`，与 ML-KEM 那份是两份，不跨模块共享。
Keccak 理论上能共享，但那要跨 AXI 从机仲裁、动到已验证的 ML-KEM 安全边界，
这一轮不做（面积有余量：ML-DSA-87 Sign 单核 post-route 才 5.4k LUT / 28 BRAM tile）。

引擎内部的海绵归属由 `owner` 选通（FSM / ExpandMask / ExpandA / SampleInBall），
只在换手方空闲时切 —— 这条是 KeyGen 踩过的坑，见 keygen 设计文档。
