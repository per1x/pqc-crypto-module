# ML-DSA Verify 硬核设计（FIPS 204 §6，external + pure）

打法与 Sign 一致（见 `mldsa-sign-design.zh-CN.md`）：**增量搭建、每段对 oracle 验一段、
绿了才提交**，最后整体对 **ACVP sigver**。黄金判据是 `mldsa_oracle.py` 的 `mldsa_verify()`
（预言机 E，已对上全部 ACVP sigver）。

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
