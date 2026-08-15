# ML-DSA KeyGen 顶层 FSM —— 设计与已知的坑

本文记录 ML-DSA KeyGen 硬核的设计。**RTL 尚未完成**：算子、NTT、三种打包、
两条采样器都已完成并对上黄金模型（见 `hardware/rtl/mldsa/`），
缺的只有把它们串起来的顶层 FSM。

写这份文档是因为那个 FSM 是**一整块、不可再拆**的东西：它的复杂度全在编排上
（海绵复用、多级流水、反压交汇），拆成两半交付只会得到一个不能验证的中间态。
下一次动手时从这里开始，不必重新推导。

## 现成的积木

| 模块 | 状态 | 判据 |
|---|---|---|
| `mldsa_mont_reduce` / `reduce32` / `caddq` / `power2round` | 完成 | 穷举 / 大批量对拍 |
| `mldsa_ntt_core`（正/逆变换） | 完成 | 对拍 schoolbook 负循环卷积 |
| `mldsa_polyt1_pack` / `polyt0_pack` / `polyeta_pack` | 完成 | 逐字节对 `mldsa_oracle.py` |
| `mldsa_poly_uniform`（RejNTTPoly, SHAKE128） | 完成 | 逐系数对黄金模型 |
| `mldsa_poly_eta`（RejBoundedPoly, SHAKE256） | 完成 | 逐系数对黄金模型 |
| **KeyGen 顶层 FSM** | **进行中**：①H ②ExpandS ③NTT(s₁) ④MAC(Â∘ŝ₁) 已逐段验 | 目标：ACVP `ML-DSA-keyGen-FIPS204` |

黄金模型 `hardware/model/mldsa_oracle.py` 的 `mldsa_keygen()` 已对过 ACVP，
是 FSM 的唯一判据，也是逐段调试时的中间量来源。

## 数据流（与 oracle 的 `mldsa_keygen` 一一对应）

```
H(ξ‖k‖ℓ) → ρ(32) ‖ ρ'(64) ‖ K(32)          SHAKE256，128 字节
s₁[j] = RejBounded(ρ', j)      j = 0..ℓ-1   → 存 + 打包进 sk
s₂[i] = RejBounded(ρ', ℓ+i)    i = 0..k-1   → 存 + 打包进 sk
ŝ₁[j] = NTT(s₁[j])                          就地覆盖
对每个 i：
    acc = Σ_j mont(Â[i][j] ∘ ŝ₁[j])         Â 现采现用，不存
    acc = invNTT(reduce32(acc)) + s₂[i]
    (t₁,t₀) = Power2Round(caddq(acc))
    t₁ → pk；t₀ → sk
tr = H(pk) → 填进 sk 第三段
```

**Â 不存**：k×ℓ×256×4 B = 16 KB，每个只用一次。现采现用把存储降到 0，
代价是重跑 SHAKE。这个核不追求吞吐，换存储划算。

**建议第一版只做 ML-DSA-44**（k=ℓ=4, η=2）。三个参数集只差 (k, ℓ, η) 三个数，
先跑通再提参数比一开始写通用版更快。注意 65/87 的 η=4 会让 `polyeta_pack`
的位宽从 3 位变 4 位。

## 存储预算（44）

| 用途 | 尺寸 |
|---|---|
| s₁ / ŝ₁（就地覆盖） | 4×256×32b = 4 KB |
| s₂ | 4 KB |
| 累加器 | 256×32b = 1 KB |
| pk 缓冲 | 32 + 4×320 = 1312 B |
| sk 缓冲 | 32+32+64 + 8×96 + 4×416 = 2560 B |

sk 的段偏移：`ρ`=0, `K`=32, `tr`=64, `s₁pack`=128, `s₂pack`=128+ℓ·96,
`t₀pack`=128+(ℓ+k)·96。

## 已经踩明白的坑（照抄即可，别重新发现）

### 1. 一个海绵，四种用法，必须显式三选一

SHAKE 用在四处：H、ExpandA（在采样器里）、ExpandS（在采样器里）、tr。
面积上只放得下一个 `sha3_core`，所以它的接口要在「FSM」「均匀采样器」
「η 采样器」之间切换。

用一个显式的 `owner` 三选一 mux，**不要**让三方各自驱动同一组线 ——
那在仿真里是 X、在综合里是多驱动。

**换手只能在各自空闲时做。** 中途换手会让海绵停在一个谁都不认识的状态上：
吸收到一半的消息不报错，只会算出一个合法但错误的结果。

### 2. 空敏感列表：`always @(*)` 里全是常量 = 永远不触发

```verilog
always @(*) begin sha_rate = RATE; sha_suffix = SUFFIX; end   // ❌ 输出恒 X
assign sha_rate = RATE;                                        // ✅
```

这个坑让采样器卡了一整轮：`sha3_core` 在 `start` 那拍锁存到 X 的 rate/suffix，
补位写到 X 地址上，置换一跑整个海绵状态变 X。**症状在被驱动侧，故障在驱动侧** ——
而 `sha3_core` 自己的用例 9/9 全过（它的测试台显式驱动 rate/suffix），
这条"证据"会把人往错方向带。

### 3. 握手那一拍要装**下一个**字节

```verilog
if (valid && ready) begin
    idx  <= idx + 1;
    data <= byte_at(idx + 1);   // ✅ 不能写 byte_at(idx)
end
```
非阻塞赋值下 `byte_at(idx)` 还是旧值，结果是第 0 个字节送两遍、整条消息错一位、
最后一个字节没送出去。采出来的东西完全合法，只是和标准的不一样。

### 4. 写进 BRAM 的值必须跟着 `we` 一起打进寄存器

`we`/`waddr` 是寄存器、下一拍才生效，而候选值往往是从当前数据组合出来的 ——
到写生效那一拍源数据已经翻过去了。症状极具迷惑性：判据对、计数对，
唯独存下来的值全错。

### 5. 计数器位宽要够到最长的那一段

tr 要吸收整个 pk（44 是 1312 字节），9 位的通用计数器装不下，会在 512 处回绕。
SHAKE 不会因此报错，只会给出一个合法但错误的摘要。

### 7. NTT / sha3 的 done 是**电平**，多条连算要先等它落一次

ntt_core 和 sha3_core 的 done「置位后保持到下一次 start 才清」。而 start 是
非阻塞赋值、要过一两拍核才吃到 —— 那之前 done 上挂着的还是**上一条**的 1。
直接 `if (done)` 会当场误判完成，把这一条整个跳过。

第一条恰好因为复位后 done=0 而蒙对，**非要多条连算才暴露**。所以等待要写成
「先看到 done 落一次（说明核真开始算了），再等它起」：

```verilog
S_GO: begin nt_start <= 1; low_seen <= 0; st <= S_WAIT; end
S_WAIT: begin
    if (!nt_done) low_seen <= 1;
    if (low_seen && nt_done) st <= S_NEXT;
end
```

④ 的 MAC 之后有 invNTT，用的是同一个 NTT 核，同一个坑。

### 6. 擦除/轮询上限随存储容量变

逐时钟周期轮询的循环，上限必须大于被等待的拍数本身。

## FSM 骨架（状态与要点）

```
S_IDLE → S_H_ABS → S_H_GAP → S_H_FLU → S_H_SQ      ① H
       → S_S_GEN → S_S_WAIT → S_S_MOVE  ×(ℓ+k)     ② ExpandS（存 + 打包）
       → S_NTT_LD → S_NTT_GO → S_NTT_ST → S_NTT_WB ×ℓ   ③ NTT(s₁)
       → 对每个 i：
            S_A_GEN → S_A_WAIT → S_MAC  ×ℓ          ④ Â 现采 + 逐系数 MAC
            → S_RED → S_INV_GO → S_INV_ST           ⑤ reduce32 + invNTT
            → S_T_PACK                              ⑥ +s₂ → power2round → 打包
       → S_TR_ABS → S_TR_GAP → S_TR_FLU → S_TR_SQ   ⑦ tr
       → S_FIN
```

**`S_GAP` 不能省**：`sha3_core` 的 `in_flush` 只在 `in_valid` 为低时被采样，
而 `in_valid` 是寄存器，最后一个字节的电平会拖到下一拍。同一拍拉 flush 会被
整个忽略，海绵永远吸不完，表现是"一直不结束"。

**所有存储都是同步读**，所以每一段都是两拍一个系数（摆地址 / 用数据）。

**MAC 的累加**：`j == 0` 时直接放、之后才累加，省掉 256 拍的清零。

**打包器有反压**（累加器满 8 位时 `in_ready` 拉低），推进要看握手；
`S_T_PACK` 同时喂 t₁(10 位) 与 t₀(13 位) 两个打包器，**两个都握上才推进** ——
位宽不同、空闲节奏不一样，只看一个会丢系数。

**三个打包器各有独立的落盘指针**：η 打包在 ExpandS 阶段、t₁/t₀ 打包在每个 i
结束时，共用一个指针会互相踩。还要确认 `pe_ov` 与 `p0_ov` 在时间上不重叠 ——
它们都写 sk，同拍触发会丢一个字节。这一条**要在用例里显式断言**，
不能只靠"设计上不会同时发生"。

## 验证路径

1. 先单独验 ①：H 的 128 字节输出对 `hashlib.shake_256(ξ+bytes([k,ℓ]))`。
2. 再验 ②③：s₁/s₂ 与 ŝ₁ 对 oracle 的中间量（oracle 里把它们暴露出来）。
3. 再验 ④⑤：t 对 oracle 的 `t`。
4. 最后整体对 ACVP 的 pk/sk 逐字节。

逐段验的理由：整体不对时，"哪一段开始错"这个信息比任何波形都值钱。
采样器那次就是靠"吸收的字节对不对、挤出的字节对不对"两步把范围缩到了驱动侧。
