# ML-DSA Sign 硬核设计（FIPS 204 §6，external + pure）

> **状态（ML-DSA-44）：完成，整体对上 ACVP siggen 逐字节。**
> 逐段搭建 ①–⑩ 全部落地并各自对上 oracle 的 κ=0 轮中间量；顶层拒绝循环跑到接受轮，
> 完整 2420 字节签名逐字节对上 **ACVP siggen**（`test_sign_acvp_det` 15 条确定性、
> `test_sign_acvp_rnd` 15 条非确定性）。RTL：`sign.v` + 新子模块 `unpack.v`（位解包）、
> `expand_mask.v`（ExpandMask）、`sample_in_ball.v`（SampleInBall）。
> 途中实际踩到的坑（已修，见文末「实测坑」）：状态常量位宽、SampleInBall 常量强转、
> w₁/z 打包器换条 clr、hint 填充区 BRAM 残留、hint 权重位宽、**吸收长度为 rate 整数倍时的 flush 时机**。
> 65/87 参数化待做；Verify 待做。

本文记录 ML-DSA Sign 硬核的设计与逐段规划，写法照抄 KeyGen 那份
（`mldsa-keygen-design.zh-CN.md`）：**增量搭建、每加一段就对黄金模型验一段、
绿了才提交**。黄金判据是 `hardware/model/mldsa_oracle.py` 的 `mldsa_sign()`
（预言机 D，已逐字节对上全部 ACVP siggen）以及它调用的分步算子。

**第一目标：ML-DSA-44 整体对上 ACVP siggen 逐字节。** 参数化留到 44 通了再提。

---

## 0. Sign 与 KeyGen 的两个根本不同

1. **拒绝采样主循环。** Sign 不是一趟直线跑完：κ 从 0 起、每轮 +ℓ 重采 y，
   算到 z / r₀ / hint 时若任一范数或权重越界就整轮作废、κ 加 ℓ 重来。
   迭代次数取决于随机性 —— 因此**先做确定性签名**（`deterministic=1`，rnd 全零），
   把迭代次数变成对每条向量固定、可复现的数，逐段读中间量对 oracle。

2. **要读私钥并做多次逆变换。** KeyGen 只把 s₁ 正变换一次；Sign 要
   skDecode 出 s₁/s₂/t₀ 并各做一次 NTT，然后在每一轮里对同一个 NTT 核发起
   **ℓ + 3k 次** invNTT（w、cs₁、cs₂、ct₀）。「done 是电平」那个坑（KeyGen 坑表第 7 条）
   在这里会被反复触发，是本核最密的雷区。

---

## 1. 逐段读中间量的办法（关键：按「当前迭代」比对）

拒绝循环让「整体签名」这个终判据离得很远。中间段怎么验？

**约定：所有中间量的调试读口，暴露的都是 FSM 当前所在那一轮 κ 的值。**
测试台复现 oracle 主循环的循环体（不是只调 `mldsa_sign`，而是 import
`sk_decode / expand_mask / sample_in_ball / ntt / decompose / polyw1_pack …`
自己按 κ=0,ℓ,2ℓ… 逐轮重算），与 RTL 读出的当前轮值逐系数比。这正是 KeyGen
测试台的做法（它 import `rej_eta_poly / ntt / …` 重算中间量，而不是只比最终 pk）。

大多数段用 **κ=0 的第一轮**就能验：y、w₁、c̃、c、z、r₀、ct₀、hint 在 κ=0
那一轮都有确定值，无论这一轮最终是否被接受。等各段都对上，再加拒绝循环控制，
用「跑到接受的那一轮、整包对 ACVP」收口。

---

## 2. 数据流（与 oracle 的 `mldsa_sign` 一一对应）

```
skDecode(sk): ρ(32) ‖ K(32) ‖ tr(64) ‖ s₁(ℓ·96) ‖ s₂(k·96) ‖ t₀(k·416)
              s₁/s₂ 每系数 3 位 → η−v；t₀ 每系数 13 位 → 2^(D−1)−v
ŝ₁=NTT(s₁)  ŝ₂=NTT(s₂)  t̂₀=NTT(t₀)                    ← 各 ℓ/k/k 条，就地存
Â = ExpandA(ρ)                                          ← 现采现用，不存（同 KeyGen ④）
μ  = H(tr ‖ M'),  M' = [0, |ctx|] ‖ ctx ‖ msg          ← SHAKE256 → 64 B
ρ''= H(K ‖ rnd ‖ μ)                                     ← SHAKE256 → 64 B
κ = 0
loop:
    y   = ExpandMask(ρ'', κ)     l 条，系数 (−γ₁, γ₁]，18 位解包（γ₁−v）
    κ  += ℓ
    ŷ   = NTT(y)
    对每个 i：w=invNTT(Σ_j Â[i][j]∘ŷ[j]); caddq; (w0,w1)=Decompose(w)
    c̃  = H(μ ‖ w1Encode(w₁))     SHAKE256 → 32 B（λ/4）
    c   = SampleInBall(c̃)        τ=39 个 ±1；ĉ = NTT(c)
    z[j]  = y[j] + invNTT(ĉ∘ŝ₁[j])            ‖z‖∞ ≥ γ₁−β ?  → 作废重来
    r₀[i] = w0[i] − invNTT(ĉ∘ŝ₂[i])           ‖r₀‖∞ ≥ γ₂−β ? → 作废重来
    对每个 i：ct₀=invNTT(ĉ∘t̂₀[i]);  ‖ct₀‖∞ ≥ γ₂ ? → 作废重来
             h[i]=MakeHint(−ct₀, w−cs₂+ct₀)   等价于 MakeHint(r₀+ct₀, w₁)
    Σ权重 > ω ? → 作废重来
    σ = c̃ ‖ zEncode(z) ‖ HintBitPack(h)
```

**ML-DSA-44 常量**（`SIG_PARAMS` + `PARAMS`）：k=ℓ=4, η=2, D=13, q=8380417,
γ₁=2¹⁷=131072, γ₂=(q−1)/88=95232（decompose **MODE=0**）, τ=39, β=78, ω=80,
λ=128, c̃ 长度=32 B。sig 长度 = 32 + ℓ·(256·18/8) + (ω+k) = 32 + 4·576 + 84 = 2420。

---

## 3. 现成积木（尽量复用，别重写）

| 模块 | 用在 Sign 的哪 |
|---|---|
| `mldsa_ntt_core` | s₁/s₂/t₀ 的正变换；每轮 ŷ 的正变换；w/cs₁/cs₂/ct₀ 的 invNTT |
| `mldsa_mont_reduce` | 所有 NTT 域逐点乘 Â∘ŷ、ĉ∘ŝ、ĉ∘t̂₀ |
| `mldsa_reduce32` | MAC 累加后、以及 z/r₀ 折算前的规约 |
| `mldsa_caddq` | Decompose 前把 w 折进 [0,q) |
| `mldsa_decompose #(.MODE(0))` | w →(w0,w1)；也被 UseHint 内部用（这里只签名，不用 UseHint） |
| `mldsa_make_hint #(.MODE(0))` | h[i][n] 的 1 比特 |
| `mldsa_poly_uniform` | ExpandA 采 Â（同 KeyGen ④，nonce=256·i+j，seed=ρ） |
| `mldsa_ram_dp`（common/ram_dp） | 所有系数/字节存储 |

**新做（对应 oracle 的分步算子）：**

| 新模块 | 判据（oracle 里的谁） | 备注 |
|---|---|---|
| `mldsa_bitunpack`（通用位解包） | — | `mldsa_bitpack` 的逆；字节进、W 位系数出，再做逆变换 |
| skDecode 用 3 位/13 位解包 | `polyeta_unpack` / `polyt0_unpack` | 变换：v→η−v、v→2^(D−1)−v |
| `mldsa_sample_in_ball` | `sample_in_ball` | SHAKE256(c̃) 流，τ 个 ±1，Fisher–Yates 式 |
| `mldsa_expand_mask` + 18 位解包 | `expand_mask`/`polyz_unpack` | SHAKE256(ρ''‖nonce)，每条 32·18=576 B，v→γ₁−v |
| `mldsa_polyz_pack`（18 位，γ₁−z） | `polyz_pack` | z 的打包，进 sig |
| `mldsa_polyw1_pack`（6 位） | `polyw1_pack`（GAMMA2_88 分支） | 只喂 c̃ 的 SHAKE，不进 sig |
| `mldsa_hintpack` | `hint_pack` | ω+k 字节：各条 1 的下标顺次写、末尾 k 字节存累计计数 |
| `mldsa_chknorm` | `chknorm` | 组合：|居中系数| ≥ bound ⇒ 1 |

`sample_in_ball` / `expand_mask` 都复用**同一个共享 sha3_core**，所以要进
海绵三选一（其实是「四选一」了：FSM / ExpandA-uniform / SampleInBall / ExpandMask，
或把后三个都做成 SHAKE 使用者、由 owner 选择）。换手只在空闲时切 —— 同 KeyGen 坑表第 1 条。

---

## 4. I/O 接口（sign.v 顶层）

msg 可达 8192 B、ctx 达 255 B、sk=2560 B，都太大不能像 KeyGen 的 `xi` 那样走端口位向量，
所以做成**测试台在 start 前预载的字节写口**，FSM 内部按地址读：

```
input  start;               // 脉冲
input  [11:0] sk_wr_addr;  input [7:0] sk_wr_data;  input sk_wr_en;   // 预载 sk（2560）
input  [12:0] msg_wr_addr; input [7:0] msg_wr_data; input msg_wr_en;  // 预载 msg（≤8192）
input  [7:0]  ctx_wr_addr; input [7:0] ctx_wr_data; input ctx_wr_en;  // 预载 ctx（≤255）
input  [13:0] msg_len;     input [7:0] ctx_len;
input  [255:0] rnd;        // 32 B（deterministic=1 时全零）
output done;

// 共享 sha3_core 握手：同 KeyGen（start/rate/suffix/in_valid/in_data/in_flush/
//                        in_ready/out_valid/out_ready/out_data）
// 调试读口：done（或段末）后读中间量
input  [4:0] dbg_sel; input [8:0] dbg_idx; output signed [31:0] dbg_coef;
output [511:0] mu; output [511:0] rhopp;                 // 早段直接验
// sig 输出缓冲：按字节读
input  [11:0] sig_addr; output [7:0] sig_data;
```

dbg_sel 编码（暂定，随段扩充）：
```
0..3  → s₁[sel]（后被 NTT 覆盖成 ŝ₁）      8..11 → s₂[sel−8]
12..15→ t₀[sel−12]                          16..19→ y[sel−16]（当前 κ 轮）
20..23→ w1[sel−20]  24    → c              25..27→ z / r₀ / ct₀ …（随段定）
```

sig 布局：c̃(32) ‖ z₀..z₃ pack(各 576) ‖ hint(ω+k=84) = 2420。

---

## 5. FSM 骨架（分段，逐段提交时 `done` 往后挪）

```
S_IDLE
① skDecode  → S_UNPACK（ρ/K/tr 直搬；s₁/s₂/t₀ 位解包，存系数）
② derive     → μ = H(tr‖M')：S_MU_ABS(tr) → S_MU_ABS2(hdr+ctx+msg) → GAP/FLU/SQ
              ρ'' = H(K‖rnd‖μ)：S_RP_ABS → GAP/FLU/SQ
③ NTT prep  → 对 s₁(ℓ) / s₂(k) / t₀(k) 各 NTT，就地覆盖（S_NT_LD/GO/ST/WB ×(ℓ+2k)）
====== 拒绝循环入口（κ 计数、owner 反复切）======
④ y = ExpandMask(ρ'',κ)：调 expand_mask 采样器 ×ℓ，存 y；κ += ℓ
⑤ ŷ=NTT(y)；对每 i：w=invNTT(Σ Â∘ŷ)；caddq；Decompose→(w0,w1)  存 w0、w1
⑥ c̃ = H(μ ‖ w1pack)；c = SampleInBall(c̃)；ĉ = NTT(c)
⑦ z[j]=y[j]+invNTT(ĉ∘ŝ₁[j])；chknorm(γ₁−β) → 越界跳回 ④
⑧ r₀[i]=w0[i]−invNTT(ĉ∘ŝ₂[i])；chknorm(γ₂−β) → 越界跳回 ④
⑨ 对每 i：ct₀=invNTT(ĉ∘t̂₀[i])；chknorm(γ₂) 越界跳 ④；
          h[i]=MakeHint(r₀[i]+ct₀, w1[i])；Σ权重 > ω 跳 ④
⑩ σ = c̃ ‖ zpack ‖ hintpack → S_FIN
```

**分段提交顺序**（每段一个 cocotb 用例对 oracle）：
1. `①` skDecode：s₁/s₂/t₀ + ρ/K/tr 对 `sk_decode`（**不需要 NTT/海绵**，最先做）。
2. `②` μ、ρ'' 对 `h_shake256(tr+M')` / `h_shake256(K+rnd+μ)`。
3. `③` ŝ₁/ŝ₂/t̂₀ 对 `ntt(sk_decode 出来的 s₁/…)`。
4. `④` y 对 `expand_mask(ρ'',0,…)`（κ=0 轮）。
5. `⑤` w₁ 对 oracle κ=0 轮的 w₁（w0 一并读出比）。
6. `⑥` c̃、c 对 `h_shake256(μ‖w1pack)` 与 `sample_in_ball`。
7. `⑦` z 对 κ=0 轮的 z，含「该拒绝的要真拒绝」（挑一条 κ=0 被拒的向量）。
8. `⑧⑨` r₀ / ct₀ / hint 权重。
9. `⑩` 整体对 **ACVP siggen** 逐字节（跑到接受轮）。

---

## 6. 坑表（Sign 会新踩或重踩的，提前避开）

### S1. 拒绝循环里海绵要反复重开，owner 在一轮内切多次
一轮里 SHAKE 被用于：ExpandMask（④）、c̃ 的 H（⑥）、SampleInBall（⑥）。中间还夹着
NTT/invNTT。每次换手都必须在换出方**空闲**时切（KeyGen 坑第 1 条）。κ 递增只在
④ 采完 y 之后做一次（+ℓ），别在跳回时重复加。

### S2. 「done 是电平」在这里密集触发
一轮里对同一个 NTT 核连发 ŷ(ℓ)、w 的 invNTT(k)、cs₁(ℓ)、cs₂(k)、ct₀(k) —— 十几次连算。
每一次都要用 KeyGen 坑第 7 条的「先见 done 落一次、再等它起」（`nt_lowseen`）。漏一处，
那一条 NTT 被跳过、结果读到上一条的残留，且**第一条常因复位蒙对**，非连算不暴露。

### S3. skDecode 的两个逆变换：13 位 t₀ 要先解包再「2^(D−1)−v」
t₀ 打包时存的是 2^(D−1)−a（pack.v 说明），解包要反着来：先取 13 位无符号 v，
再算 2^(D−1)−v 得回有符号系数。s₁/s₂ 同理（η−v）。**漏掉这步减法**：解包出来是
一个大正数，NTT 之后全错，而「位解包自洽」查不出来（打包解包两次错抵消）——
只有对 `sk_decode`（对过 ACVP）才暴露。位解包的**逐系数交错**（3 位跨字节）照 oracle
`polyeta_unpack` 的下标表抄，别自己推。

### S4. norm 检查的位宽与符号：喂进 chknorm 的必须是**居中**系数
`chknorm` 假定输入已居中（reduce32 输出即 (−q/2, q/2] 内）。z=reduce32(y+cs₁)、
r₀=reduce32(w0−cs₂)、ct₀=reduce32(invNTT(...)) 都要先过 reduce32 再判。
bound：z 用 γ₁−β=130994，r₀ 用 γ₂−β=95154，ct₀ 用 γ₂=95232。**三个 bound 别记混**。
`chknorm` 内部取绝对值，别在外面再取一次符号。

### S5. MakeHint 的双 invNTT 交汇 —— 参数与「加谁」
oracle 里 h=MakeHint(a0, w₁)，其中 a0 = reduce32(r₀[i] + ct₀[i])（不是 −ct₀ 与
w−cs₂+ct₀ 两个多项式各算一遍；数学等价但硬件只需 `r₀+ct₀`）。r₀ 是 ⑧ 已经算好的
w0−cs₂，ct₀ 是 ⑨ 新算的 —— 两个来自不同 invNTT 的量在这里相加，读地址/相位要对齐。
`mldsa_make_hint` 吃 (a0 signed, a1[5:0])，a1 就是 w₁[i][n]。

### S6. hint 打包的 ω 边界与「先算权重再决定接受」
HintBitPack：对每条 i，把 h[i] 里为 1 的下标**按 n 升序**顺次写进 out[index++]，
每条末尾 `out[ω+i]=index`（累计计数）。总权重 Σ = 最终 index。**权重判据 `>ω` 要在
写 sig 之前**：越界就整轮作废，不能已经写了一半 sig。ω+k=84 字节，前 ω=80 是下标区、
后 k=4 是计数区；下标区没用满的部分保持 0（HintBitUnpack 会检查填充零，见预言机 E）。

### S7. c̃ 的 H 要吸收 μ(64) 再吸收 k 条 w1pack（ML-DSA-44 每条 192 B）
w1pack（6 位/系数、GAMMA2_88 分支）每条 256·6/8 = 192 B，k=4 条共 768 B，加 μ 的 64
= 832 B 一次性吸收进 SHAKE256、挤 32 B。吸收计数器位宽要够（≥10 位）。别把 w1pack
的位宽记成 4 位（那是 GAMMA2_32/65/87 的分支）。

### S8. SampleInBall 的流式取字节与「b ≤ i 才停」
`sample_in_ball`：SHAKE256(c̃) 先取 8 字节当 signs 位串，之后每步从流里取字节 b，
`while b>i: 再取`（b 可能超过当前 i，要丢弃继续取），命中后 `c[i]=c[b]; c[b]=1−2·signbit`。
i 从 N−τ 到 N−1。SHAKE 是**流**：一个 136 B 块可能不够，要能接着挤下一块
（oracle 用 `digest(len+136)` 取更长前缀模拟）。硬件里就是 out_ready 持续抽，别提前关。

### S9.（沿用 KeyGen）空敏感列表→连续赋值；握手装下一字节；we 跟数据一起打拍；
计数位宽够最长段（这里最长是吸收 msg，≤8192+，要 14 位）；S_GAP 不能省；轮询上限随
存储放大；两 packer 并喂各自门控 ready。这些在 KeyGen 坑表里已写全，Sign 全部适用。

---

## 7. 存储预算（ML-DSA-44，粗算）

| 用途 | 尺寸 |
|---|---|
| s₁/ŝ₁（就地）ℓ·256·32b | 4 KB |
| s₂/ŝ₂ k·256·32b | 4 KB |
| t₀/t̂₀ k·256·32b | 4 KB |
| y / ŷ（就地）ℓ·256·32b | 4 KB |
| w0 / w1 k·256·(32b/6b) | 4 KB + 0.4 KB |
| c（±1，256 项） | 1 KB |
| sk/msg/ctx 输入缓冲 | 2560 + 8192 + 255 B |
| sig 输出缓冲 | 2420 B |

Â 现采现用不存（同 KeyGen）。这个核不追求吞吐，换存储划算。

---

## 8. 验证路径（照 KeyGen 那套，逐段缩小「哪段开始错」）

1. 先 `①`：skDecode 出 s₁/s₂/t₀、ρ/K/tr，全对 `sk_decode`（**KAT 的 sk 直接喂**）。
2. 再 `②③`：μ/ρ''、ŝ/t̂₀。
3. 再 `④⑤⑥`：y、w₁、c̃/c（都取 κ=0 轮）。
4. 再 `⑦⑧⑨`：z、r₀、hint（含「该拒绝要拒绝」）。
5. 最后 `⑩` 整体对 **ACVP siggen** 逐字节（先确定性 15 条，再非确定性 15 条）。

逐段验的理由同 KeyGen：整体不对时，「哪一段开始错」比任何波形都值钱。

**逐段用例的一个坑（⑩ 之后才暴露）**：拒绝循环让 done 落在**接受轮**而非 κ=0 轮。
若某向量 κ=0 被拒，done 时的中间量存储（z/w1/r0/hint/y…）是接受轮的、不是 κ=0 的，
而逐段用例都对 oracle 的 κ=0 轮比 → 全崩。解法：逐段用例挑一条 **κ=0 就被接受** 的
向量（`first_d44` 用 oracle 现算筛选），此时接受轮 == κ=0 轮，done 的存储就是 κ=0 的。
另外「就地覆盖」的量（ŝ、ŷ、r₀ 覆盖 w0、ĉ 覆盖 c）在 done 读到的是覆盖后的，其原值
正确性靠双射/后继量间接验（同 KeyGen 的做法）。

---

## 9. 实测坑（②–⑩ 真正踩到的，65/87 与 Verify 照着避）

1. **状态常量 localparam 位宽**：状态数超过 32 后把 `reg st` 从 [4:0] 改成 [5:0]，
   但 `localparam [4:0]` 块头忘了一起改 → S_Z_GO=6'd32 被截成 5'd0、与 S_IDLE 撞车，
   FSM 落到 `default→S_IDLE` 卡死。**声明与常量块的位宽要一起改。**
2. **SampleInBall 的 START_I**：`8'(256-TAU)` 这种 SV 强转在 iverilog 下不稳；用
   `localparam integer` 中转再切片。（`8'd256-TAU` 会因 8 位回绕**恰好**=217 而蒙对，
   更阴。）
3. **w₁/z 流式打包器换条 clr**：每条 w₁(192B)/z(576B) 都字节对齐，4 条连续打包与逐条
   等价；但打包器最后 1~2 字节比最后一个系数**晚出**，换条时 clr 会把它们抹掉
   （KeyGen 靠采样间隔盖过这段滞后，Sign 各条背靠背没间隔就中招）。**只在最开头 clr 一次。**
4. **hint 填充区 BRAM 残留**：sig RAM 无复位口（BRAM 要求），HintBitPack 只写 hidx 个
   下标、其余靠 0 初值。但初值只在仿真开头有；连续签多条时上一条的下标残留在填充区，
   本条权重更小就覆盖不到 → σ 从 hint 段中间对不上。**sigEncode 前显式清 ω 字节填充区。**
5. **hint 权重位宽**：总权重最多 256·k=1024，9 位在 >511 时回绕，某些被拒轮的权重会
   绕回 ≤ω 被误判接受。**用 11 位。**
6. **吸收长度为 rate 整数倍时的 flush 时机**（最隐蔽）：μ 吸收长度恰是 SHAKE256 rate(136)
   整数倍时（如 tcId=193：66+161+7933=8160=60·136），最后一字节把块填满、触发一次置换，
   这拍 `sha_in_ready` 落下；若 `S_D_GAP` 只等一拍就拉 `in_flush`，flush 落在海绵置换中
   （非 S_ABSORB）被整个忽略 → 永远吸不完、卡在挤压（现象：kappa=0、st=S_D_SQ 死等）。
   **`S_D_GAP` 要等 `sha_in_ready` 重新拉高（置换完、回到 S_ABSORB）再冲刷**；非整数倍时
   in_ready 一直高、立刻通过，行为不变。KeyGen 的吸收都是定长非整数倍，从没暴露这条。
