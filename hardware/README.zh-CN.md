[English](README.md) · **中文**

# 硬件

HSM 的 PL（FPGA）侧 RTL、cocotb 测试台、生成黄金向量的 Python 参考模型，以及 Vivado
脚本。PL 负责密码运算（ML-KEM、ML-DSA、SHA-3、AES、SM4、SM3）并承载 TRNG，不保存长期密钥：
密钥由 OP-TEE 可信应用（TA）以 blob 形式保管，只在一次运算期间装入 PL
（见 [../docs/ARCHITECTURE.zh-CN.md](../docs/ARCHITECTURE.zh-CN.md) 和
[../docs/SECURITY.zh-CN.md](../docs/SECURITY.zh-CN.md)）。

这些源码被综合进跑在 XCZU3EG 板子上的 bitstream。下面描述的仿真是第一道检查，
ACVP 和上板测试是第二道（见 [../docs/TESTING.zh-CN.md](../docs/TESTING.zh-CN.md)）。

```
hardware/
├── rtl/
│   ├── mlkem/      ML-KEM：NTT、基乘、约减、采样、压缩、编解码，
│   │               以及 KeyGen / Encaps / Decaps 三个整核
│   ├── mldsa/      ML-DSA：NTT、舍入、提示位、采样、打包、参数表，
│   │               KeyGen / Sign / Verify 三个整核，以及 mldsa_engine 适配层
│   ├── keccak/     keccak_f1600.v、sha3_core.v
│   ├── sym/        aes_core.v、sbox.v、sm4_core.v、sm3_core.v
│   ├── trng/       环振源、健康检测（RCT/APT）、调理、AXI 从机
│   ├── bus/        AXI4-Lite 防火墙、对称密钥寄存器、各核的 AXI 从机
│   ├── board/      axi4lite_xbar.v、zu3eg_hsm_top.v
│   ├── common/     ram_dp.v、sync_fifo.v
│   └── lint_waivers.vlt
├── platform/       风扇温控（非密码逻辑），见 platform/README.md
├── tb/cocotb/      cocotb 测试台、仅供仿真的汇总顶层、Makefile
├── tb/lint/        lint 用的厂商原语空壳（不参与综合）
├── model/          Python 参考模型、向量导出、独立预言机
└── syn/            Vivado out-of-context 综合与出 bitstream 的完整流程
```

算法核是不含厂商原语的纯 Verilog-2001。厂商原语和 IP 只有 `rtl/board/zu3eg_hsm_top.v`
里的 `BUFGCE_DIV` 与 PS 实例，以及 `platform/fan_ctrl/fan_sysmon.v` 里的 `SYSMONE4`。

## 板级顶层

`zu3eg_hsm_top` 把 PS 的 `M_AXI_HPM0_LPD`（AXI4，按 AXI4-Lite 处理，`rlast = rvalid`，
ID 原样回显）接到 `axi4lite_xbar`，后者从 `0x8000_0000`
起按每槽 64 KB 译码。密码核运行在 75 MHz（`pl_clk0` 为 150 MHz，经 `BUFGCE_DIV` 二分频）。
译码不命中的访问，以及被防火墙拒绝的访问，一律按 RAZ/WI 应答（读回 0、写丢弃、响应
OKAY），不会产生 AXI 错误。

| 槽 | 地址 | 模块 | 内容 |
|---|---|---|---|
| 0 | `0x8000_0000` | `trng_axi` | TRNG |
| 1 | `0x8001_0000` | `key_vault_axi` | 对称密钥寄存器（8 槽 x 256 bit） |
| 2 | `0x8002_0000` | `sym_axi` | AES / SM4 / SM3（槽 1 和槽 2 是 `sym_vault_top` 的两个从机） |
| 3 | `0x8003_0000` | `mlkem_axi` | ML-KEM-512/768/1024 |
| 4 | `0x8004_0000` | `key_vault_axi` | 金丝雀，槽 1 的副本，`SECURE_ONLY` 固定为 1，use 口不接任何东西 |
| 5 | `0x8005_0000` | `fan_ctrl_axi` | 风扇观测口（无 AxPROT 门控） |
| 6 | `0x8006_0000` | `mldsa_axi` | ML-DSA-44/65/87 |

默认构建里功能从机（槽 0、1、2、3、6）的 `SECURE_ONLY=1`：只有 `AxPROT[1]=0`（安全）
的事务能到达它们。用 `PQC_DEV_OPEN=1` 构建得到 `zu3eg_hsm_dev.bit`，这些从机改为
`SECURE_ONLY=0`，Linux 可以直接驱动它们跑 KAT。即便如此，密钥寄存器的装载口
（`SLOT_SEL`、`KEY_IN`、`SLOT_CTRL`）以及 `mlkem_axi`、`mldsa_axi` 的种子暂存口仍然只接受
安全写。寄存器布局见 [../docs/REGISTERS.zh-CN.md](../docs/REGISTERS.zh-CN.md)。

## PL 里的密钥处理

- `key_vault`（外面包一层 `key_vault_axi`）是 8 个 256 bit 的密钥寄存器。密钥经装载口写入
  （`SLOT_SEL`，`SLOT_CTRL` 的 BEGIN，八个 `KEY_IN` 字，`SLOT_CTRL` 的 COMMIT），只有对称核
  通过 use 口读取。没有任何寄存器地址会返回密钥内容。
- 普通世界对装载口的写被丢弃，并计入 `LOAD_REJ`（`0x34`）。`CTRL.ZEROIZE` 不做门控；
  `sym_axi` 也会通过 `ext_zeroize` 清这些寄存器。
- `tamper` 到来时寄存器一拍全清，之后拒绝装载，直到复位。板上的 `tamper` 来自 `SYSMONE4`
  的过温输出。
- ML-KEM 和 ML-DSA 的 `MODE[4]=CHAIN` 让 PL 从暂存的种子展开私钥、执行运算，然后擦除展开
  的密钥。软件在每次运算后等待 `KEYSTAT[0]`（`EXP_WIPING`）清零。`CTRL.SEED_LOCK` 是只有
  掉电重启才能清除的闩锁。ACVP 的 KeyGen 路径（由软件提供 `d‖z`）仍然经 `OUT_DATA` 返回
  `dk`。

## 模块

### `rtl/mlkem/`

| 文件 | 模块 |
|---|---|
| `mont_reduce.v` | `mont_reduce`（组合逻辑） |
| `butterfly.v` | `barrett_reduce`、`butterfly_ct`、`butterfly_gs`，后两者各由 `_head` + `_tail` 拼成 |
| `ntt_core.v` | `ntt_core`：单蝶形单元，7 层 NTT，系数存在 `ram_dp`；每次变换 3457 拍 |
| `basemul.v` | `mlkem_basemul`（五次 Montgomery 乘法，`_head` + `_tail`）、`mlkem_bmzeta`（基乘用的 ζ 表） |
| `compress.v` | `mlkem_compress`、`mlkem_decompress`，位宽 `D` 为参数 |
| `sample.v` | `mlkem_cbd2`、`mlkem_cbd3`、`mlkem_rej_pair`、`mlkem_rej_uniform` |
| `cbd_stream.v` | `mlkem_cbd_stream`：SHAKE256 字节流转 256 个 CBD 系数 |
| `pack.v` | `mlkem_encode12`、`mlkem_decode12` |
| `bitpack.v` | `mlkem_bitpack`、`mlkem_bitunpack`：ByteEncode_d / ByteDecode_d，`d` 为运行时输入 |
| `keygen.v`、`encaps.v`、`decaps.v` | `mlkem_keygen`、`mlkem_encaps`、`mlkem_decaps`：完整的 FIPS 203 内部算法。Decaps 通过实例化 `mlkem_encaps` 重新加密，常数时间比较密文，并做隐式拒绝 J(z‖c) |

### `rtl/mldsa/`

| 文件 | 模块 |
|---|---|
| `mont_reduce.v` | `mldsa_mont_reduce`、`mldsa_mont_mul_pipe`（5 级流水的 `mont(x·y)`，NTT 与逐点运算共用） |
| `butterfly.v` | `mldsa_butterfly_ct`、`mldsa_butterfly_gs`（参照形式）、`mldsa_butterfly_pipe`（CT / GS / 缩放共用一个乘法器） |
| `ntt_core.v` | `mldsa_ntt_core`：流水线，8 层，乒乓 `ram_dp`；正变换 1081 拍，逆变换 1344 拍 |
| `reduce.v`、`rounding.v` | `mldsa_reduce32`、`mldsa_caddq`、`mldsa_power2round`、`mldsa_decompose`、`mldsa_make_hint`、`mldsa_use_hint` |
| `sample.v` | `mldsa_rej_uniform`、`mldsa_rej_eta`、`mldsa_rej_uniform_buf` |
| `sampler.v`、`expand_mask.v`、`sample_in_ball.v` | `mldsa_poly_uniform`、`mldsa_poly_eta`、`mldsa_expand_mask`、`mldsa_sample_in_ball`（驱动共享的 `sha3_core`） |
| `pack.v`、`unpack.v` | `mldsa_bitpack`、`mldsa_polyt1_pack`、`mldsa_polyt0_pack`、`mldsa_polyeta_pack`、`mldsa_bitunpack` |
| `params.v` | `mldsa_params`：按参数集索引的 FIPS 204 参数表 |
| `keygen.v`、`sign.v`、`verify.v` | `mldsa_keygen`、`mldsa_sign`、`mldsa_verify` |
| `mldsa_engine.v` | `mldsa_engine`：三个核前面的单一字节流写口。参数集（44/65/87）在每次运算时运行时选择 |

`keygen.v`、`sign.v`、`verify.v` 的文件头注释仍然写的是第一阶段的实现进度，文件里是完整算法。

### 其他目录

| 文件 | 模块 | 说明 |
|---|---|---|
| `keccak/keccak_f1600.v` | `keccak_f1600` | 每周期一轮，一次置换 24 拍 |
| `keccak/sha3_core.v` | `sha3_core` | 置换外面的海绵结构（填充、吸收、挤出） |
| `sym/aes_core.v` | `aes_core` | AES-128/256 加解密；装载密钥 41（AES-128）或 53（AES-256）拍，每个分组 11 或 15 拍 |
| `sym/sm4_core.v` | `sm4_core` | 密钥扩展 32 拍，每个分组 33 拍 |
| `sym/sm3_core.v` | `sm3_core` | SM3，填充在核内完成 |
| `sym/sbox.v` | `aes_sbox`、`aes_inv_sbox`、`sm4_sbox` | 由 `sym_oracle.py --emit-sbox` 生成的查找表 |
| `trng/` | `ring_osc`、`trng_source`、`trng_health`、`trng_cond`、`trng_top`、`trng_axi` | 多环振噪声源、RCT/APT 健康检测（SP 800-90B）、Keccak 调理器、FIFO、AXI 从机 |
| `common/ram_dp.v` | `ram_dp` | 真双口同步 RAM，推断为块 RAM，读延迟 1 拍 |
| `common/sync_fifo.v` | `sync_fifo` | 首字直通 FIFO；`flush` 会把内容覆写为 0 |
| `bus/axi4lite_firewall.v` | `axi4lite_firewall` | 按 `AxPROT`、地址窗口和 `tamper` 门控；被拒的事务到不了下游口。策略由参数决定，不是寄存器。记录违规次数 |
| `bus/key_vault.v`、`bus/key_vault_axi.v` | `key_vault`、`key_vault_axi` | 对称密钥寄存器及其 AXI 从机（见上） |
| `bus/sym_axi.v`、`bus/sym_vault_top.v` | `sym_axi`、`sym_vault_top` | AES/SM4/SM3 从机（没有密钥寄存器，只选密钥槽号），以及把它与密钥寄存器连起来的封装 |
| `bus/mlkem_axi.v` | `mlkem_axi` | 三个 ML-KEM 核的从机，带 8 KB 输入和输出缓冲 |
| `bus/mldsa_axi.v` | `mldsa_axi` | `mldsa_engine` 的从机 |
| `bus/axi4lite_regs.v`、`bus/pqc_accel_axi.v` | `axi4lite_regs`、`pqc_accel_axi` | 寄存器块与加速器顶层（AXI4-Lite 控制、AXI4-Stream 数据），供 C 仿真后端和 cocotb 使用，不在板上镜像里 |
| `board/axi4lite_xbar.v` | `axi4lite_xbar` | 一主 N 从，每槽 64 KB |
| `board/zu3eg_hsm_top.v` | `zu3eg_hsm_top` | 板级顶层 |

## 设计说明

- ML-KEM（q = 3329，Montgomery 基 2^16）和 ML-DSA（q = 8380417，基 2^32）不共用算术模块。
  ML-DSA 的 NTT 做满 8 层，变换域乘法是逐点乘；ML-KEM 只做 7 层，之后需要 2x2 的基乘。
- `mlkem_compress` 把除以 q 换成乘以 `ceil(2^33/q)` 再移位。输入域只有 3329 个值，测试台
  全部验证。`mldsa_decompose` 和 `mldsa_reduce32` 用同样的做法，测试台把两个 `MODE` 值同时
  例化，只在一个参数集里写错的常数会被测出来。
- `d < 12` 的 `ByteEncode_d` 没有做成模块：系数本来就在 `d` 位以内，打包只是连线。
  `mlkem_encode12` 存在是因为它还要把有符号系数折回 `[0, q)`。
- `keccak_f1600` 每周期做一轮，而不是 24 轮展开。ML-KEM-768 一次密钥生成需要 43 次置换，
  约 1000 拍，NTT 部分约 20700 拍。
- 大块存储写成 `ram_dp` 以映射到块 RAM。带多个组合读口的寄存器数组会被摊到 LUT 上
  （`pqc_accel_axi` 的数据缓冲曾因此占去约 30000 个 LUT）。`tools/rtl_synth_check.sh`
  会列出 Yosys 摊成寄存器的每一处存储。
- `pqc_accel_axi` 实现 `include/pqchsm/accel.h` 的寄存器语义：START 自清，DONE 为锁存电平，
  状态寄存器由硬件写、软件只读。契约见 [../docs/REGISTERS.zh-CN.md](../docs/REGISTERS.zh-CN.md)，
  `test_axi.py` 用手写的总线功能模型逐条验证。C 侧构建会把 `ntt_core`、`keccak_f1600` 和
  `pqc_accel_axi` 做 Verilator 仿真，`tests/unit/test_accel.c` 比较仿真 RTL 与软件桩的输出。

## 验证

```bash
./tools/rtl_sim_fast.sh                   # 按模块在 Verilator 上跑 cocotb（默认一组）
./tools/rtl_sim_fast.sh --list            # 可用的名字
./tools/rtl_sim.sh                        # 在 Icarus Verilog 上跑完整 cocotb 回归
./tools/rtl_lint.sh                       # Verilator -Wall + Icarus，每个模块各作一次顶层
./tools/rtl_synth_check.sh                # Yosys 可综合性检查
python3 hardware/model/ntt_oracle.py      # NTT 预言机
python3 hardware/model/mlkem_oracle.py    # ML-KEM 数据通路预言机
python3 hardware/model/mldsa_oracle.py    # ML-DSA 数据通路预言机
```

`tb/cocotb` 里共有 272 个 `@cocotb.test` 用例。`rtl_sim_fast.sh` 的默认一组几分钟跑完。
`rtl_sim.sh` 约 40 分钟，合并前应当跑一次，因为 Verilator 是二值仿真、不传播 X：
有三个 bug 只在 Icarus 下暴露（新加的端口悬空、输入端口无人驱动、读了从未写过的 RAM）。
cocotb 不支持 Vivado `xsim`，所以被测源码保持不含厂商原语；`rtl_lint.sh` 使用 `tb/lint`
里的空壳。

Yosys 检查的是两个仿真器都看不到的问题：源码在综合器看来是不是同一个意思。例如异步复位
分支里同时判断一个不在敏感表中的信号，仿真按字面执行，综合出的电路却不同，Yosys 会直接
报错。改完 RTL 要跑一次 `tools/rtl_synth_check.sh`；同一个 reg 被两个
`always @(posedge clk)` 块赋值，lint 和仿真都能通过，只有综合会拒绝。

### 独立预言机

黄金向量来自 `ref_model.py` 和 `mldsa_model.py`，所以与向量一致只说明 RTL 和模型一致。
预言机拿独立的来源去检查模型里的算子。`mlkem_oracle.py` 和 `mldsa_oracle.py` 每次运行还会
做反证（把常数或表改坏，预言机必须报错）。

- NTT，预言机 A：`Z_q[x]/(x^256+1)` 上的 schoolbook 负循环卷积，不使用旋转因子表，
  与 `invntt(basemul(ntt(a), ntt(b)))` 比较。
- NTT，预言机 B：在参考模型的 `ntt()` 之上用 Python 重写 FIPS 203 的 K-PKE 密钥生成，
  重现 NIST ACVP 的 `ek` 和 `dk`。
- ML-KEM 压缩：按 FIPS 203 §4.2.1 的定义用精确有理数计算 `Compress_d` / `Decompress_d`，
  对用到的每个 `d` 覆盖全部 3329 个输入，并检查 `round(q/2^(d+1))` 的误差界。
- 二项采样：FIPS 203 Alg 8 的定义与位并行实现比较，对每个系数的比特组穷举，其余比特分别取
  0、1 和随机。
- 用单个算子重建完整算法并与 ACVP 比较：ML-KEM 的 KeyGen（`ek`/`dk`）、Encaps（`c`、`K`）、
  Decaps（`K`，含隐式拒绝）；ML-DSA 的 KeyGen（`pk`/`sk`）、siggen 和 sigver。
- ML-DSA NTT：模 8380417 下 `invntt(ntt(a) * ntt(b)) == schoolbook(a, b)`。
- 舍入与提示位：`Power2Round`、`Decompose` 对照其定义分解；`|e| <= gamma2` 时
  `UseHint(r+e, MakeHint(r0+e, r1)) == r1`。
- Keccak：`test_keccak.py` 里写死的、公开的全零输入 Keccak-f[1600] 输出；以及在 RTL 置换
  之上搭出的 SHAKE128/256、SHA3-256，与 `hashlib` 比较（C 侧与 OpenSSL 比较）。
- AES、SM4、SM3：`sym_oracle.py`，按标准正文编写，不调用密码库。
- TRNG：`trng_health_model.py` 按定义计算 RCT/APT 阈值，`trng_cond_model.py` 逐比特建模
  调理器。

## 综合

脚本在 `syn/`。Vivado 2020.1 只在构建机上运行，见 [syn/README.zh-CN.md](syn/README.zh-CN.md)。
