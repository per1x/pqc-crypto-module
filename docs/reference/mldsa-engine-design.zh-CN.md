# ML-DSA 共享引擎设计（mldsa_engine）

本文记录**接口契约**与**输入/输出缓冲地址布局**。AXI 从机那条线照本文写，
两边对同一份。算法本身见 `mldsa-keygen-design`/`mldsa-sign-design`/`mldsa-verify-design`。

> 状态（2026-08-16）：
> * **接口与地址布局已定死**（见下），`mldsa_axi` 已按此接上。
> * **engine 本体已落地**（`hardware/rtl/mldsa/mldsa_engine.v`）——
>   按重划后的第一期范围，它只当**适配层**：把一个字节口翻译成三个核各自的
>   原生端口，**三个核内部一个字没动**，各自保留自己的 NTT 与算术。
> * **第二期（提取共享 NTT / 乘法链）按实测没有必要**：engine-87 只占
>   LUT 17.9% / BRAM 30.1% / DSP 15.8%，且已在 100MHz 下 MET。
> * **整片实测已出（见 §5.1）**：ML-DSA-87 塞得下，WNS +2.537 / WHS +0.009
>   双双 MET，bitstream 已生成。但 **CLB 到了 95.98%** —— 装下了，没有余地了。
> * **这份 bitstream 里的 ML-DSA 是 87**，由顶层参数 `MLDSA_PSET` 显式决定
>   （`zu3eg_hsm_top` → `mldsa_axi` → `mldsa_engine` 一路传到底）。
>   在此之前三层都不传参数，engine 取默认值 —— 也就是说**板上那份其实一直是
>   ML-DSA-44**，而所有面积估算都是按 87 算的。这种"默认值决定产品形态"
>   不写在任何地方，只能从三层默认参数里推出来，现在改成显式的。
> * **仍未做：运行时选参数集没打通到核那一层。** 叶子模块（打包/解包、
>   decompose/hint、SampleInBall、ExpandMask）已全部运行时化，
>   但三个核自己的 K/ℓ/η/τ/ω/β/c̃ 仍是编译期参数，所以 engine 也仍是
>   编译期参数化的；`pset` 端口存在且**对不上就拒绝启动**，不会按错参数集算。
>   "同一次仿真里先 44 再 65 再 87"这条验收因此还不成立。

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

### ⚠️ 上表是**地址空间**算出来的，真实上限更低：8192

做 engine 时发现的：`in_addr` 有 15 位不假，但**三个核内部的 msg 缓冲是
`ram_dp #(.DW(8), .AW(13))`，即 8192 字节**（sign.v / verify.v 里的 `u_msg`）。
瓶颈在核里，不在缓冲区 —— 上表那些两万多的数字**够不着**。

所以真实的上限是：

| 项 | 上限 |
|---|---|
| `msg_len` | **≤ 8192**（所有 op / 所有 pset，由核内 u_msg 的容量决定） |
| `ctx_len` | ≤ 255（FIPS 204 的单字节长度，与实现无关） |

`mldsa_engine` 在 START 时校验这两条，超了**拒绝启动**（置 param_err、立刻 done），
不让高位地址安静回绕 —— 回绕会算出一个长度对、格式对但是错的签名。

上面那张按地址空间算的表**保留**，因为它仍是"缓冲区能装下多少"的正确答案；
要把 8192 提上去，得先把核里的 u_msg 加宽，那是另一件事。

对原型演示与 ACVP 向量而言绰绰有余：ACVP 的消息最长也就几百字节。

## 4. 输出缓冲布局（`out_addr` / `out_len`）

| op | 内容 | out_len |
|---|---|---|
| KeyGen | pk(PK) ‖ sk(SK) | PK+SK |
| Sign | sig(SIG) | SIG |
| Verify | 无（结果看 `verify_ok`） | 0 |

### ⚠️ 4.1 段边界差一拍（已知缺陷，当前由从机侧绕开）

engine 的出口是

```verilog
assign out_data = (op_r == OP_KEYGEN)
                    ? ((out_addr < PKLEN) ? kg_pk_data : kg_sk_data)
                    : sg_sig_data;
```

`kg_pk_data` / `kg_sk_data` 来自核里的 `ram_dp`，是**同步读**（本拍的数据对应
**上一拍**摆的 `out_addr`）；而选择器判的是**本拍**的 `out_addr`。两者差一拍。

于是只要消费方在采数据的同一拍把 `out_addr` 推到下一个（同步读的常规做法，
为了让背靠背读不掉拍），跨过 `PKLEN` 的那一拍选择器就会提前翻面，交出
`kg_sk_data` 在地址 `(PKLEN-1) − PKLEN` 上的值（13 位回绕成 8191）。
**表现：pk 的最后一个字节读回 `0x00`，别的字节全对。**

engine 自己的用例碰不到它 —— `test_mldsa_engine.py` 的 `read_out` 是
"摆地址 → 等一个上升沿 → 采样"，采样时选择器与数据是同一个地址。
它是在 `mldsa_axi` 接上真 engine、把整段 pk 读回来对 ACVP 时才现形的
（`test_mldsa_axi.test_keygen_matches_acvp`，第 1311 个字节）。

**当前处置**：修在从机侧 —— `mldsa_axi` 的读路径不再提前一拍
（AXI 读握手最快隔两拍，同步读一拍就够，不损失吞吐）。
engine 本体**还没修**，因为它在另一条线的改动范围里。
正解是把选择器的判据改成上一拍的 `out_addr`（寄存一份 `out_addr_d` 或一个
`sel_sk` 位），改完可以把从机侧那段绕法还原。

## 5. 时序现状（OOC 实测，Vivado 2020.1 / xazu3eg-sfvc784-1-i / 100MHz 约束）

全部是 **post-route** 的 WNS（post-synth 偏乐观，不作数）。Fmax 由 `1/(10ns − WNS)` 反推。

| ML-DSA-87 | 最初 WNS / Fmax | 现在 WNS / Fmax | 75MHz 下余量 |
|---|---|---|---|
| KeyGen | −1.479ns / 87.1 MHz | **+3.422ns MET / 152.1 MHz** | +6.76ns |
| Verify | −3.487ns / 74.1 MHz | **+0.993ns MET / 111.0 MHz** | +4.33ns |
| Sign | −3.332ns / 75.0 MHz | **+0.258ns MET / 102.6 MHz** | +3.59ns |
| **engine（三核合一）** | −2.307ns / 81.3 MHz | **+0.003ns MET / 100.0 MHz** | **+3.34ns** |

engine 那一行的"最初"是把三个核装进适配层、但还没切 ⑨ 写回链时量的：
三个核挤进同一个设计后布局压力上来，比单核 Sign 的 −1.611ns 又掉了 0.7ns。
切完之后整个 engine 在 100MHz 下 MET。

engine-87 面积：LUT 12634(17.9%)、FF 10713(7.6%)、BRAM 65 tile(30.1%)、DSP 57(15.8%)。
**"不共享算术也塞得下"由实测证实 —— 第二期（提取共享 NTT / 乘法链）没有必要做。**

### 5.1 整片实测（post-route，**这才是"塞不塞得下"的答案**）

上面全部是 **OOC**（只有这一个模块，没有顶层、没有 PS、没有别的从机）。
OOC 说明不了整片装不装得下 —— 装不下的方式是**布局拥塞**，而拥塞只有在
整个设计一起布的时候才存在。所以下面这组数是完整实现流程量的：

`hardware/syn/impl_bitstream.tcl`，Vivado 2020.1，`xazu3eg-sfvc784-1-i`，
送检形态（`SECURE_ONLY_FUNCTIONAL=1`），`PQC_MLDSA_PSET=2`（ML-DSA-87），
`clk_sys` 周期 13.5 ns（≈74 MHz，PL0 经 `BUFGCE_DIV` 二分频）。

| | 加 ML-DSA 之前 | **ML-DSA-87 之后** | 器件总量 |
|---|---|---|---|
| CLB LUTs | 35812 (50.75%) | **52887 (74.95%)** | 70560 |
| CLB Registers | — | 38803 (27.50%) | 141120 |
| **CLB** | 6783 (**76.90%**) | **8465 (95.98%)** | 8820 |
| BRAM tile | 31.5 (14.58%) | **112.5 (52.08%)** | 216 |
| DSP | 140 (38.89%) | **197 (54.72%)** | 360 |
| WNS | +3.361 ns | **+2.537 ns**（MET，0 failing） | |
| WHS | +0.010 ns | **+0.009 ns**（MET，0 failing） | |
| 片上功耗 | — | 2.989 W（动态 2.642） | |

**结论：塞得下，时序也收敛，但 CLB 只剩 4%。**

三条要点，别只看"MET"两个字：

1. **CLB 95.98% 才是真正的边界，不是 LUT 的 74.95%。** LUT 还剩四分之一，
   但它们分布在只剩 355 个空 CLB 里 —— 再加逻辑首先撞的是 CLB，不是 LUT。
   布线器的初始拥塞报告也印证了这一点（global 8×8 / 2.41% tiles，
   short 16×16 / 8~12% tiles，都集中在 `INT_X16..X23` 那一条）。
   **这份设计已经没有"再塞一个模块"的余地了。**
2. **保持时间没有被 ML-DSA 拉坏。** WHS 从 +0.010 变成 +0.009，几乎没动 ——
   因为最差的那条保持路径**不在 ML-DSA 里**：它是
   `u_symvault/u_sym/u_sm3/w_reg[6][4] → w_reg[5][4]`，1 级 LUT4、
   数据 0.176 ns、时钟偏斜 +0.021 ns。加多少 ML-DSA 都不会影响它。
   报告里的 +0.009 已经扣过 0.100 ns 的保持不确定度，**真实物理余量 +0.109 ns**
   （`set_clock_uncertainty -hold 0.100`，理由见 impl_bitstream.tcl）。
3. **建立时间的瓶颈换人了 —— 现在在 ML-DSA 里。** 最差路径是
   `u_mldsa/u_eng/u_vf/u_ntt/u_mem1` → `u_mldsa/u_eng/u_vf/u_p6/acc_reg[2]`，
   28 级逻辑（10 个 CARRY8 + 2 组 DSP 链）、数据 10.627 ns / 周期 13.5 ns。
   余量还有 2.537 ns，但**再想提频就要先动 verify 的这条累加链**。

BRAM 112.5 比早先"96 tile"的估算高，差的正是 `mldsa_axi` 自己的
**64 KB 片内 sk 金库**（16 片 BRAM36）—— 那份估算只加了 engine 的 65 tile，
漏了从机侧的金库。

产物 `hardware/syn/impl/zu3eg_hsm.bit` / `.bin` 已生成（DRC 全过）。
⚠️ **本轮只做到"造得出来"为止，没有上板。**

### 5.2 ⚠️ 退参数集**省不出面积** —— "87→65→44"不是一根杠杆

塞不下时的第一反应是退到小一档的参数集。**实测证明这条路没有用。**
同一份 RTL、同一套流程，只把 `PQC_MLDSA_PSET` 从 2 改成 1 再跑一遍完整实现：

| | ML-DSA-87 | ML-DSA-65 | 差 |
|---|---|---|---|
| CLB LUTs | 52887 (74.95%) | 52900 (74.97%) | **+13** |
| **CLB** | 8465 (95.98%) | **8477 (96.11%)** | **+12（更差）** |
| BRAM tile | 112.5 | 112.5 | 0 |
| DSP | 197 | 197 | 0 |
| WNS | +2.537 ns | +1.992 ns | −0.545（更差） |
| WHS | +0.009 ns | +0.010 ns | +0.001 |

（post-synth 也一样：LUT 两边都是 53683，BRAM 112.5，DSP 197，
只有寄存器差了 374 个。`-generic` 确实生效 —— 综合日志里
`Parameter MLDSA_PSET bound to: 1`、`Parameter K bound to: 6`、`L bound to: 5`。）

**为什么 K/ℓ 变小而面积不变**，两条原因，都是这版实现的结构决定的：

1. **算术是时分复用的。** 全工程只有一条 `mldsa_mont_mul_pipe`，NTT / 逐点乘 /
   MAC 各段轮流用。K 与 ℓ 决定的是**循环跑多少轮**，不是**摆多少套硬件** ——
   所以 DSP 一个不少，组合逻辑也几乎不变。K/ℓ 只影响计数器位宽和一点状态机
   状态，那正是那 374 个寄存器。
2. **缓冲是定尺寸的，不随参数集缩。** 核里的 `u_msg` 是 `AW=13`（8192 B）、
   engine 的输入缓冲是固定 32 KB、`mldsa_axi` 的金库是固定 8 槽 × 8192 B。
   这些加起来就是 BRAM 的大头，与 K/ℓ 无关，所以 BRAM 一片不省。

顺带：两次跑**都**触发了同一条 `[Route 35-443] CLB routing congestion detected`
（各一次），也就是说拥塞的成因不是 ML-DSA 的参数集，是整体规模。

**所以真要腾地方，能动的是别的：** 提取三个核共享的 NTT / 乘法链（原先按 OOC
面积判断"没必要做"的第二期）、把 ML-DSA 与 ML-KEM 的 `sha3_core` 合成一份
（现在是两份）、或者砍掉一个从机。**退参数集不在其中。**

⚠️ 这条也意味着：ML-DSA-87 是**免费**的（相对 65 而言）。既然小一档既不省
面积也不省时序，就没有任何理由不上最高安全等级的那一套。

### 5.3 运行时选参数集之后再量一次（合并顶点 edd51c4）

§5.1 / §5.2 那两组数是在**编译期参数化**的 RTL 上量的（一份 bitstream 一套参数集）。
之后另一条线把参数集做成了**运行时可选**（`hardware/rtl/mldsa/params.v`，
engine 与三个核的参数全部去掉、按 `pset` 端口切）。那是**另一份 RTL**，
所以上面的数字对它不成立，必须重量。同一套流程、同一块器件，实测：

| | 编译期 ML-DSA-87 | **运行时 44/65/87** | 差 |
|---|---|---|---|
| CLB LUTs | 52887 (74.95%) | 53670 (76.06%) | +783 |
| **CLB** | 8465 (95.98%) | **8587 (97.36%)** | **+122，只剩 233 个空 CLB** |
| BRAM tile | 112.5 | 112.5 | 0 |
| DSP | 197 | 199 | +2 |
| **WNS** | +2.537 ns | **+0.959 ns** | **−1.578（少了 62%）** |
| WHS | +0.009 ns | +0.010 ns | +0.001 |

**结论：仍然塞得下、仍然双双 MET、bitstream 出得来 —— 但两个方向都更薄了。**

面积的代价（+122 CLB）不大；**真正的代价在建立时间**。最差路径还是同一条
（`u_eng/u_vf/u_ntt/u_mem1` → `u_eng/u_vf/u_p6/acc_reg`），但**逻辑级数从
28 涨到 34**（多了 1 个 CARRY8 与 5 个 LUT），数据路径 10.627 → 12.222 ns ——
运行时选参数集的那些多路选择器**正好落在已经是最差的那条链上**。

也就是说：运行时可选这个功能不是白拿的，它花掉了 ML-DSA 建立余量的六成。
在 74 MHz 下还剩 +0.959 ns，够用；但**再想在这条链上加任何东西之前，
先看一眼这个数**。要买回余量，对症的仍是 §5.2 末尾那几条（切开 verify 的
累加链、共享算术、合并两份 `sha3_core`），而不是退参数集。

⚠️ 顺带一个**已经不成立的说法**：`hardware/syn/impl_bitstream.tcl` 现在仍然
打印"ML-DSA 参数集：ML-DSA-87"、仍然把 `-generic MLDSA_PSET=…` 传给
`synth_design`，而顶层**早已没有 `MLDSA_PSET` 这个参数**（edd51c4 删掉了）。
于是 `PQC_MLDSA_PSET=1` 会安静地产出一个叫 `zu3eg_hsm_mldsa65.bit`、
实际却支持全部三套的位流 —— 文件名断言了一个并不存在的限制。
那段（含改名逻辑）是在"一份 bitstream 一套参数集"的前提下写的，前提没了，
它就该跟着删。

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

### ⑨ hint 写回段：曾经的瓶颈，已切开

Sign 一度卡在 **⑨ hint 写回段**（与乘法无关）：

```
u_ntt（invNTT 结果）→ u_red2(reduce32) → a0 = w0_dout + comb_red
  → u_reda0(reduce32) → mldsa_make_hint(内含 decompose 的乘法与比较)
  → hint_bit → weight_reg 的 CE
```

逻辑层级 35，其中 **CARRY8=15** —— 两级 reduce32 加一次 decompose 的比较链串在一拍里。

**已解决**：从中间切一刀就够了，不必整段流水化。ct₀ 与它的越界判定先进寄存器
（`ct0_r` / `nb_r`），下一拍再做 +r₀ / reduce32 / MakeHint；S_H_WB 从两拍变三拍
（`hph` 相位），只影响 ⑨ 这一段的逐系数吞吐，不影响拒绝循环的轮数。
逻辑层级 35→25、CARRY8 15→9，关键路径挪到 ⑧ 的 r₀ 写回（`u_w0`）。

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
4. ~~`mldsa_engine.v` 组装~~ —— **第一期已完成**（`hardware/rtl/mldsa/mldsa_engine.v`）。
   按重划后的范围，engine 只当**适配层**：把一个字节口翻译成三个核各自的原生端口，
   核内部一个字没动。输出不另开缓冲，直接选通核里的 pk/sk/sig 读口。
   验收见 `test_mldsa_engine`（6 条，三个参数集各跑一遍，Verilator + Icarus 双跑）。

   **第二期**（把 NTT 与 `mldsa_mont_mul_pipe` 提出来共享）视整体综合的实测再定：
   三个核在 87 下约 12.4K LUT / 60 DSP，余量 34.7K / 220，**不共享也塞得下**。

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
