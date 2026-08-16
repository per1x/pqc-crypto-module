# ML-DSA Verify 硬核设计（FIPS 204 §6，external + pure）

打法与 Sign 一致（见 `mldsa-sign-design.zh-CN.md`）：**增量搭建、每段对 oracle 验一段、
绿了才提交**，最后整体对 **ACVP sigver**。黄金判据是 `mldsa_oracle.py` 的 `mldsa_verify()`
（预言机 E，已对上全部 ACVP sigver）。

> **状态：44 / 65 / 87 三个参数集全部对上 ACVP sigver**（应通过 true、应拒绝 false），
> 并各自补了 ACVP 不覆盖的两类自造反例。参数化见文末第 7 节。

**第一目标：ML-DSA-44 对上 ACVP sigver —— 应通过的返回 true、应拒绝的返回 false。**

---

## 1. Verify 与 Sign 的关键不同

1. **没有拒绝循环**：一趟直线跑完，没有 κ。控制上比 Sign 简单得多。
2. **判据是一个布尔**：不像 Sign 输出 2420 字节，Verify 只输出 `valid`。这让"错误地
   返回 true"成为最危险的失败模式 —— 所以**拒绝类向量必须逐类覆盖**，不能只测通过类。
3. **要做结构合法性检查**：签名是攻击者可控的输入，sigDecode 必须校验 hint 的编码结构，
   非法就直接拒绝。Sign 侧没有这一环。

## 2. ACVP sigver（ML-DSA-44）的实际构成 —— 决定了必须覆盖哪几类

用 oracle 分类 15 条 ML-DSA-44 向量：

| 类别 | 条数 | 判据来源 |
|---|---|---|
| 应通过 | 3 | c̃' == c̃ 且各项检查通过 |
| hint 结构非法 | 3 | 2 条**下标非严格递增**、1 条**填充区非零** |
| c̃ 不匹配 | 9 | 主路径算出的 c̃' ≠ c̃ |
| ‖z‖∞ 越界 | 0 | **ACVP 没给这类**（65/87 同样没有） |

**含义**：`‖z‖∞` 与 hint 计数单调/越界这两条 ACVP 不覆盖，但 FIPS 204 要求，
仍要实现，并**自造反例测**（把合法签名的某个 z 系数改大、把计数字节改成非单调），
否则这两条逻辑等于没验。

## 3. 数据流（与 oracle 的 `mldsa_verify` 一一对应）

```
sigDecode(σ): c̃(32) ‖ z(ℓ·576) ‖ hint(ω+k=84)
      z 每系数 18 位 → γ₁−v；‖z‖∞ ≥ γ₁−β ⇒ 拒
      HintBitUnpack：逐条读累计计数 end=buf[ω+i]
          end < index（非单调）或 end > ω ⇒ 拒
          同一条内下标必须**严格递增**（buf[index−1] ≥ buf[index] ⇒ 拒）
          h[i][buf[index]] = 1
          扫完后 buf[index..ω−1] 必须**全零** ⇒ 否则拒
pkDecode(pk): ρ(32) ‖ t₁(k·320)，每系数 10 位，无变换
tr = H(pk)（吸收整个 pk 1312 字节）→ 64 字节
μ  = H(tr ‖ M')，M' = [0,|ctx|] ‖ ctx ‖ msg
c  = SampleInBall(c̃)；ĉ = NTT(c)
ẑ  = NTT(z)；t̂₁ = NTT(t₁·2ᴰ)
对每个 i：acc = Σ_j Â[i][j]∘ẑ[j] − ĉ∘t̂₁[i]      ← 先 reduce32 再 invNTT
          w' = caddq(invNTT(acc))
          w'₁[n] = UseHint(h[i][n], w'[n])         ← 直接喂 mldsa_use_hint
          w'₁ → 6 位打包进 w1pk
c̃' = H(μ ‖ w1pk)
valid = (c̃' == c̃) 且 !zbad 且 !hbad
```

**ML-DSA-44 常量**：k=ℓ=4, γ₁=2¹⁷, γ₂=(q−1)/88（use_hint **MODE=0**）, τ=39, β=78,
ω=80, D=13, c̃=32B。σ=2420，pk=1312。

## 4. 现成积木（全部复用，Verify 不新建算子）

`mldsa_bitunpack`（z 用 W=18、t₁ 用 W=10）、`mldsa_sample_in_ball`、`mldsa_poly_uniform`
（ExpandA）、`mldsa_ntt_core`、`mldsa_mont_reduce`、`mldsa_reduce32`、`mldsa_caddq`、
`mldsa_use_hint #(.MODE(0))`、`mldsa_bitpack #(.W(6))`（w1Encode）、`ram_dp`。
共享一个 `sha3_core`，owner 三选一（FSM / ExpandA / SampleInBall）。

## 5. 分段与逐段判据

| 段 | 内容 | 判据（oracle） |
|---|---|---|
| ① | sigDecode + pkDecode：c̃、z、hint 位、t₁、zbad/hbad | `polyz_unpack` / `hint_unpack` / `polyt1_unpack` |
| ② | tr=H(pk)、μ=H(tr‖M') | `h_shake256` |
| ③ | c=SampleInBall(c̃)、ĉ/ẑ/t̂₁ 三组 NTT | `sample_in_ball` / `ntt` |
| ④ | w'₁、c̃'、valid | **整体对 ACVP sigver（15 条）** + 自造 z-norm/计数反例 |

## 6. 坑表（Verify 特有的，加上从 Sign 继承的）

### V1. h 存储必须先清零
`h[i][buf[index]] = 1` 是**稀疏置位**，没被置到的位靠初值为 0。BRAM 无复位口，
连续验多条时上一条的 1 会残留 → 上一条的 hint 泄漏进这一条，可能让**本该拒绝的签名通过**。
这与 Sign 里 hint 填充区那个坑同源，但后果更严重（假阳性）。**入口先清 k×256 位。**

### V2. 结构非法时不要提前跳出，置标志继续跑到底
提前 `return false` 在硬件里意味着一堆状态要处理"半路终止"。更简单也更安全：
置 `hbad`/`zbad` 标志，照常跑完，最后 `valid = 匹配 && !zbad && !hbad`。
下标 `buf[index]` 恒在 0..255，写 h 不会越界，跑完无害。

### V3. 「严格递增」是同一条多项式**内**，跨条不比
`index > first` 那个条件不能漏：每条 i 的起点 `first` 重置比较链。写成全局比较会把
合法签名判成非法（假阴性）。

### V4. 减法在 NTT 域、reduce32 在 invNTT **之前**
`acc = reduce32(Σ Â∘ẑ − ĉ∘t̂₁)` 然后才 invNTT，最后 caddq。顺序照 oracle 抄，
别把 reduce32 挪到 invNTT 之后（数值会溢出到错误区间，且只在部分向量上现形）。

### V5. UseHint 的输入必须落在 [0,q)
`mldsa_use_hint` 内部调 `mldsa_decompose`，后者假定输入已在 [0,q)。所以
**先 caddq 再 use_hint**。

### V6.（继承 Sign）吸收长度恰为 rate 整数倍时的 flush 时机
`tr=H(pk)`：pk=1312=9·136+88，不是整数倍，安全。但 **μ 的吸收长度是变长的**
（66+|ctx|+|msg|），一定会有向量踩中 136 的整数倍。`S_D_GAP` 必须等 `sha_in_ready`
重新拉高再拉 `in_flush`（Sign 实测坑第 6 条）。

### V7.（继承 Sign）其余
状态常量与 `st` 位宽一起改；打包器只在最开头 clr 一次；NTT/sha3 的 done 是电平，
连算要先等它落一次；计数位宽够最长段（μ 吸收 ≤ 8449 → 14 位；pk 吸收 1312 → 11 位）。

### V8. 定宽寄存器 + 变长负载 ⇒ **比较必须自己划出有效范围**（2026-08-16 上板抓到）

`ctilde` / `ctilde_p` 是定宽 512 位（c̃ 最长 64 字节），而每次运算**只写低
ctb 字节**（44/65/87 → 32/48/64）。判定原来写成整 512 位相等，于是比的是
「这一次的 c̃」‖「**上一次运算**留下的高位」。

后果是一个**跨运算的假阴性**：只要上一次判过否（c̃'≠c̃ ⇒ 高位不等），之后
每一条 44/65 的**合法**签名都被拒，直到又来一条 87 把 64 个字节整个覆盖掉。
87 永远不受影响 —— 它的 ctb=64，根本没有"高位"。

上板实测（开发形态位流，同一条 44 合法签名连做 300 次）：44 与 65 失败
**300/300**，87 失败 **0/300**，与温度无关。这个 100%/0% 一度被当成
"运行时参数集没被应用"，因为两者对它的解释一样漂亮。**分开两者的实验**
（`board/src/mldsa_ctb_probe.c` 序列④）是：用一条判否的 **65** 去弄脏，
然后**先问 44 再问 65** —— 65 判否只弄脏 [0,48)，65 自己下一次会整段重写，
而 44 只写 [0,32)、读到的 [32,48) 正是脏的。于是同一时刻 **44 被拒、65 通过**。
任何"pset 没被应用"的说法都给不出这个分裂。板上 61/61 步全中。

顺带否掉一条**看起来成立的规避**："Verify 前只要有同参数集的运算就必过"。
KeyGen/Sign 根本碰不到 verify 的 c̃ 寄存器，夹进来毫无作用（序列⑤实测仍被拒）；
当初"夹了就好"是巧合 —— 真正洗干净它的是那次序列里**恰好在前面的一条通过的
87 Verify**。

修法不是"在别处把高位清干净"，而是**让判定根本不看高位**（`ct_eq` 逐字节比、
`gi >= cfg_ctb` 的位一律记作相等）。理由有两条：一是靠"入口清零"维持不变量
要给 1024 个触发器各加一条同步清零，而 87 那版 CLB 已经 95.98%；二是那样一来
判定的正确性仍旧依赖**别处**维持的不变量，而不是它自己。

**一般化的教训**：只要一个定宽寄存器承载变长负载，凡是拿它整体做判断的地方
（相等、非零、校验和）都必须自己划出本次的有效范围。仿真里看不见，是因为
复位把高位清成了 0，而**板上装载之后再也不复位**。


---

## 7. 参数化（44 / 65 / 87）

参数 K/L/TAU/G1LOG/MODE/OMG/BETA/CTB，分叉与 Sign 同源（γ₂→use_hint 的 MODE
与 w1Encode 位宽 6/4，γ₁→z 解包位宽 18/20）。随参数走的还有 PKLEN=32+k·320、
SIG_H0=c̃+ℓ·ZB、c̃' 吸收长度 64+k·W1B、清 h 的上限 k·256−1、‖z‖∞ 界 γ₁−β。

**三个参数集的 ACVP sigver 构成**（用 oracle 分的类，决定了自造反例必须补什么）：

| | 应通过 | hint 结构非法 | c̃ 不匹配 | ‖z‖∞ 越界 |
|---|---|---|---|---|
| ML-DSA-44 | 3 | 3 | 9 | **0** |
| ML-DSA-65 | 3 | 4 | 8 | **0** |
| ML-DSA-87 | 3 | 3 | 9 | **0** |

三个参数集**都没有** ‖z‖∞ 越界这一类，hint 计数非单调 / >ω 也都没有 ——
所以每个参数集都跑同一组自造反例（z 越界 / 计数非单调 / 计数 >ω），
且断言的是 `zbad`/`hbad` 标志本身而不是只看 `valid=false`
（c̃ 不匹配也会让 valid=false，只看它等于没验到那条路径）。

### 参数化时踩到的一条

hint 置位的地址是 `{poly[?:0], sig_rdata}`，加宽多项式下标时**批量替换按
`{poly[1:0], cnt}` 这个模式做，第二个字段是 `sig_rdata` 的这处漏网** →
65 的 poly=4/5 绕回 0/1，把它们的 hint 位并进 poly0/poly1，而读侧已是 11 位、
读 poly4/5 全空。定位办法：用 dbg 口把 RTL 与 oracle 各自「为 1 的下标」列出来，
一眼看出 `RTL poly0 = oracle poly0 ∪ oracle poly4`，比看波形快得多。
