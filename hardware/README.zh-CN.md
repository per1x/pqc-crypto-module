[English](README.md) · **中文**

# 硬件

RTL 源码、验证环境、生成黄金向量的参考模型，以及综合脚本。

> **本目录已经不只是仿真了。** 在分支 `zu3eg-fpga-crypto` 上，这些源码被综合成
> bitstream 并跑在一块 XCZU3EG 板子上：ML-KEM 512/768/1024 **在真硅上**与 NIST ACVP
> 向量逐字节一致，AXI 防火墙的访问门控也已双向证明。下面描述的仿真环境仍然是第一道
> 门（197 个 cocotb 测试），但不再是最后一道。
> 见 [../docs/SECURITY.zh-CN.md](../docs/SECURITY.zh-CN.md)。

```
hardware/
├── rtl/            密码逻辑
│   ├── mlkem/      NTT、基乘、约减、采样、压缩、编解码，
│   │               以及 KeyGen / Encaps / Decaps 三个整核
│   ├── mldsa/      ML-DSA 算子：NTT、舍入、提示位、采样
│   ├── keccak/     keccak_f1600.v、sha3_core.v
│   ├── sym/        aes_core.v、sbox.v、sm4_core.v、sm3_core.v
│   ├── trng/       环振源、健康检测（RCT/APT）、调理、FIFO、AXI 封装
│   ├── bus/        防火墙、密钥仓、各核的 AXI 封装
│   ├── board/      axi4lite_xbar.v、zu3eg_hsm_top.v
│   └── common/     ram_dp.v、sync_fifo.v
├── platform/       非密码的织构逻辑（风扇温控），见该目录的 README
├── tb/cocotb/      cocotb 测试台、仅供仿真的汇总顶层、Makefile
├── tb/lint/        厂商原语空壳，只进 lint，不参与综合
├── model/          Python 参考模型、向量导出、独立预言机
└── syn/            Vivado out-of-context 综合与出 bitstream 的完整流程
```

所有模块都是可推断的纯 Verilog-2001，未实例化任何厂商原语，因此同一份源码可原样综合
到 Xilinx、Intel 或 Lattice。

## 现有模块

| 模块 | 结构 | 周期数 |
|---|---|---|
| `mont_reduce`、`barrett_reduce` | 组合逻辑 | — |
| `butterfly_ct`、`butterfly_gs` | 组合逻辑；同样由 `_head` + `_tail` 拼成，`ntt_core` 在两半之间插了一级寄存器 | — |
| `ntt_core` | 单蝶形单元，ML-KEM 的 7 层 NTT，系数存在真双口 BRAM（`ram_dp`）里，实例化上述模块 | 3457 / 次变换（一个蝶形三拍） |
| `mlkem_basemul` | 组合逻辑，五次 Montgomery 乘法；由 `_head` + `_tail` 拼成，要收时序的核可以在两半之间插一级寄存器 | — |
| `mlkem_bmzeta` | 基乘用的 ζ 表（ZETAS[64..127]）与取法，三个核共用 | — |
| `mlkem_compress`、`mlkem_decompress` | 组合逻辑，位宽 `D` 由参数给出 | — |
| `mlkem_cbd2`、`mlkem_cbd3` | 组合逻辑，位并行的中心二项分布采样 | — |
| `mlkem_rej_pair` | 组合逻辑，取候选 | — |
| `mlkem_rej_uniform` | 收集器，每周期吃一组 3 字节，结果存在 `ram_dp` 里（读延迟 1 拍） | 约 430 / 个多项式 |
| `mlkem_encode12`、`mlkem_decode12` | 组合逻辑 | — |
| `mldsa_mont_reduce`、`mldsa_reduce32`、`mldsa_caddq` | 组合逻辑 | — |
| `mldsa_butterfly_ct`、`mldsa_butterfly_gs` | 组合逻辑 | — |
| `mldsa_ntt_core` | 单蝶形单元，ML-DSA 的完整 8 层 NTT，系数存在 `ram_dp` 块 RAM 里 | 正 2049 / 逆 2561（一个蝶形两拍） |
| `mldsa_power2round`、`mldsa_decompose` | 组合逻辑，参数集由 `MODE` 选 | — |
| `mldsa_make_hint`、`mldsa_use_hint` | 组合逻辑，参数集由 `MODE` 选 | — |
| `mldsa_rej_uniform`、`mldsa_rej_eta` | 组合逻辑 | — |
| `mldsa_rej_uniform_buf` | 收集器，每周期吃一组 3 字节，结果存在 `ram_dp` 里（读延迟 1 拍） | 约 340 / 个多项式 |
| `keccak_f1600` | 单轮迭代，`round_cnt` 走 24 轮 | 24 / 次置换 |
| `mlkem_cbd_stream` | PRF 字节流 → 256 个 CBD 系数，流式吐出 | η=2 约 384 / η=3 约 448 |
| `mlkem_bitpack`、`mlkem_bitunpack` | 变宽度 ByteEncode_d / ByteDecode_d，`d` 是运行时输入（du/dv 随参数集变） | 每拍一字节或一系数 |
| `mlkem_keygen` | **完整 ML-KEM.KeyGen_internal**：G/PRF/XOF/H、采样、NTT、基乘、打包全在 PL | 约 50.2 k / 次（768） |
| `mlkem_encaps` | **完整 ML-KEM.Encaps_internal**：H(ek)、G、A 矩阵、r̂/e₁/e₂ 采样、NTT/逆 NTT、压缩打包全在 PL | 约 50.6 k / 次（768） |
| `mlkem_decaps` | **完整 ML-KEM.Decaps_internal**：解密 + 例化 `mlkem_encaps` 重加密 + **常量时间**密文比对 + 隐式拒绝 J(z‖c) | 约 78.0 k / 次（768） |
| `ram_dp` | 参数化真双口同步 RAM，推断成块 RAM | 读延迟 1 拍 |
| `axi4lite_regs` | AXI4-Lite 从机，控制/状态寄存器 | — |
| `axi4lite_firewall` | AXI4-Lite 防火墙：AxPROT / 地址窗口 / tamper 门控，**不合规的事务不进下游**，带违规计数 | 每笔访问 3~4 拍 |
| `key_vault` | PL 内密钥仓，8 槽 × 256 bit 寄存器；只写入口 + 只给 PL 内部的使用口；tamper 一拍全清并锁存 | 擦除 1 拍 |
| `key_vault_axi` | 密钥仓的 AXI4-Lite 从机 = 防火墙 + 元数据寄存器 + 仓 | — |
| `aes_sbox`、`aes_inv_sbox`、`sm4_sbox` | 组合查表，由 `sym_oracle.py --emit-sbox` 生成 | — |
| `aes_core` | AES-128/256 分组加解密（FIPS 197），密钥扩展与分组是两条命令 | 装密钥 41/53，每分组 11/15 |
| `sm4_core` | SM4 分组加解密（GB/T 32907），解密复用加密通路只倒轮密钥 | 装密钥 32，每分组 33 |
| `sm3_core` | SM3 杂凑（GB/T 32905），填充在核内做 | 65 / 64 字节块 |
| `sym_axi` | AES/SM4/SM3 的 AXI4-Lite 从机；**没有 KEY 寄存器**，密钥从 `key_vault` 的 use 口进来 | — |
| `sym_vault_top` | 密钥仓 + 对称核的顶层，两个从机各带一个防火墙，共用一根 tamper | — |
| `pqc_accel_axi` | 加速器顶层：AXI4-Lite + AXI4-Stream + 算法核 | — |

ML-KEM 与 ML-DSA 是两套不同的算术：模数不同（3329 对 8380417）、Montgomery 基不同
（2¹⁶ 对 2³²）、系数位宽不同，所以两组模块彼此独立、不共用任何东西。ML-DSA 的 NTT 做满
8 层，因此变换域里的乘法就是逐点标量乘，而不像 ML-KEM 停在 7 层之后还要做 2×2 的基乘。

`mldsa_decompose` 与 `mldsa_reduce32` 同样用"乘倒数再右移"代替对常数的除法。两个 `MODE`
在测试台里并排例化、同时比对，因此只在一个参数集里写错的常数不会蒙混过关。

`mlkem_compress` 把对 `q` 的除法换成乘 `ceil(2^33/q)` 再右移。两者在整个输入域上相等，
而输入域只有 3329 个取值，因此测试台是**穷举**验证这一替换，而不是抽样。

`pqc_accel_axi` 的数据缓冲区按硬件**实际实现**的最大操作定尺寸——NTT 的 512 字节——
而不是照抄 `ACCEL_BUF_MAX` 那个 16 KiB 的软件侧上界。按 16 KiB 写就是 131072 位；
按已实现的操作码定尺寸后是 4096 位。

它原来是一个带多个组合读口的寄存器数组 —— 这一点是实测出来的，不是猜的：
Vivado 报出 `LUT as Memory = 0`，整块摊成了约 30000 个 LUT 的选择树。
现在是一块 `ram_dp` 块 RAM（A 口分时给输入流/装载/结果回写，B 口专给输出流），
加速器顶层因此从占满片子的 88.89% 降到 6.76%。
`tools/rtl_synth_check.sh` 会把每一处摊成寄存器的存储列出来，正是为了这个。

`d < 12` 的 `ByteEncode_d` 刻意没有做成模块：把已经落在 `d` 位内的系数拼进字节流纯粹是
导线，没有可写错的逻辑。只有 12 位这一路多一步"把有符号系数折回 `[0, q)`"，才是模块。

`keccak_f1600` 刻意**不做** 24 轮全展开。展开会把轮逻辑复制 24 份，而在此调用频度下
毫无收益：100 MHz 下 24 周期即 240 ns/次置换，而一次 ML-KEM-768 密钥生成需要 43 次
置换，合计约 1000 周期，相较 NTT 的约 20700 周期微不足道。瓶颈不在这里。

三个对称核**零 BRAM、零 DSP** —— 全是查表与移位异或，与 ML-KEM 那边
（72 个 DSP）正好是两种完全不同的形状。`aes_core` 比 `sm4_core` 大三倍半，
差的就是那套逆变换：16 个逆 S 盒加 InvMixColumns 的四个 GF(2⁸) 常数乘法；
SM4 的解密是同一条通路倒着用轮密钥，一分钱不用多花。

`pqc_accel_axi` 是系统视角下的加速器：控制/状态寄存器走 AXI4-Lite，成块数据走
AXI4-Stream，底下挂算法核。它实现的正是 `include/pqchsm/accel.h` 当初据以编写的那套
寄存器语义——START 自清、DONE 电平锁存、状态寄存器由硬件写而软件只读。
[docs/REGISTERS.zh-CN.md](../docs/REGISTERS.zh-CN.md) 是这份契约，
`test_axi.py` 用手写的总线功能模型逐条验证，不引入任何第三方 AXI 库。

算法核同样通过这套寄存器接口由 C 侧驱动，因此同一份 RTL 既被 cocotb 对拍，
也被 C 测试套件覆盖。

## 验证

```bash
./tools/rtl_sim.sh                        # 完整 cocotb 回归，Icarus Verilog
./tools/rtl_lint.sh                       # Verilator -Wall + Icarus，逐模块作顶层
./tools/rtl_synth_check.sh                # Yosys 可综合性检查，厂商中立
python3 hardware/model/ntt_oracle.py      # 两道独立的 NTT 预言机
python3 hardware/model/mlkem_oracle.py    # ML-KEM 数据通路其余算子的预言机
python3 hardware/model/mldsa_oracle.py    # ML-DSA 数据通路的预言机
```

C 侧构建同样会 verilate 两个核，并断言仿真 RTL 与软件桩逐字节一致
（`tests/unit/test_accel.c`）。

### 独立预言机

结果与本项目自己的参考模型一致，并不足以说明它正确——一张自洽但错误的旋转因子表同样
能通过这种检查。因此每个核都被钉到外部来源上：

- **NTT 预言机 A**——`Z_q[x]/(x^256+1)` 上的 schoolbook 负循环卷积，完全不触碰旋转
  因子表，用于验证 `invntt(basemul(ntt(a), ntt(b))) == schoolbook(a, b)`。它检验的是
  变换的**语义**，而非自洽性。
- **NTT 预言机 B**——用 Python 重写 FIPS 203 的 K-PKE 密钥生成，其中调用参考模型的
  `ntt()`，并逐字节重现 NIST ACVP 的 `ek`/`dk`。这把该变换钉到 ML-KEM 实际使用的那个
  变换上。
- **压缩预言机**——按 FIPS 203 §4.2.1 的定义用有理数算出 `Compress_d` 与
  `Decompress_d`，与整数实现在全部 3329 个可能输入上逐值比对，覆盖 ML-KEM 用到的每个
  `d`；压缩往返误差另行与标准给出的 `round(q/2^(d+1))` 理论界比对。
- **二项采样预言机**——按 FIPS 203 Alg 8 的定义（数两段比特的汉明重量）与位并行实现
  比对，对每个系数占用的比特组穷举，其余比特分别取零、取一、取随机。这同时覆盖"每组
  算得对"与"组间不串扰"两件事。
- **拒绝采样预言机**——用真实 SHAKE128 流跑完整的 `SampleNTT`，与直接照 FIPS 203
  Alg 7 写出的独立实现逐系数比对。
- **数据通路整体预言机**——用 `rej_pair`、`cbd2`、`cbd3`、`basemul`、`encode12` 重建
  ML-KEM 密钥生成，逐字节重现 NIST ACVP 的 `ek`/`dk`。这把上述每个算子都钉到标准化后
  的算法上，而不只是钉到一组自洽的公式上。
- **ML-DSA NTT 预言机**——同样的 schoolbook 负循环卷积论证，环换成 `q = 8380417` 的
  `Z_q[x]/(x^256+1)`。ML-DSA 的变换做满 8 层，所以判据是
  `invntt(ntt(a)∘ntt(b)) == schoolbook(a, b)`，中间是普通的逐点乘。
- **高低位拆分与提示位预言机**——`Power2Round` 与 `Decompose` 对着各自的分解式与取值
  范围，在整个系数域的代表元上验证；提示位则对着 FIPS 204 真正依赖的那条性质：
  对任意 `|e| ≤ γ₂` 的扰动，`UseHint(r+e, MakeHint(r₀+e, r₁)) == r₁`。
- **ML-DSA 数据通路整体预言机**——用 `rej_uniform_coeff`、`rej_eta_coeff`、`ntt`、
  `invntt_tomont`、`montgomery_reduce`、`power2round` 重建 ML-DSA 密钥生成，
  逐字节重现 NIST ACVP 的 `pk`/`sk`。
- **Keccak 预言机 1**——公开的全零输入 Keccak-f[1600] 置换输出，以常量硬编码，不由本
  仓库任何代码生成。
- **Keccak 预言机 2**——在 RTL 核**之上**搭出 SHAKE128/256 与 SHA3-256 的海绵结构，
  与 `hashlib` 逐字节比对（C 侧则与 OpenSSL 比对）。它覆盖 padding、rate、lane 小端
  字节序以及多块吸收与挤压，而不只是置换本身。

每道预言机都配有反证——扰动一个旋转因子、丢掉一层 NTT、翻转 Keccak 轮常数的一个
比特、把压缩的舍入常数挪一格、翻转二项采样的符号约定、把基乘的旋转因子偏移一格——并
确认在每种情况下都会失败。`hardware/model/mlkem_oracle.py` 每次运行都会自带反证，因此
一道已经失去证伪能力的检查会自己报出来。

## 仿真器选择

自包含的算法核以 Verilator 与 Icarus Verilog 在 cocotb 下验证；cocotb 不支持 Vivado
`xsim`，因此今后包含厂商 IP 的顶层需要单独仿真。这一约束的副作用是有益的：它迫使算法
核不依赖厂商原语，而这正是它们可移植的原因。

Verilator 的 2-state 语义与 Icarus 的 4-state 语义在位宽截断上并不一致，这种差异曾
暴露过真实缺陷。因此两者都运行。

第三个工具回答的是两个仿真器都答不了的问题：同一份源码在综合工具眼里是不是同一个
意思。例如异步复位分支里还判了一个不在敏感表里的信号——两个仿真器都会照字面执行，
综合出来的却是另一个电路。`tools/rtl_synth_check.sh` 用 Yosys 逐模块跑这一遍。

## 综合

脚本位于 `syn/`。Mac 上没有 Vivado，所以它们在构建机（Vivado 2020.1）上跑；
[docs/TESTING.zh-CN.md](../docs/TESTING.zh-CN.md) 里的资源占用与 Fmax 数字全部出自那里，
且都是布线后的值。参见 [syn/README.zh-CN.md](syn/README.zh-CN.md)。
