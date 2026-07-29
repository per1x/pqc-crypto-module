[English](README.md) · **中文**

# 硬件

RTL 源码、验证环境、生成黄金向量的参考模型，以及综合脚本。此处的一切都在仿真中运行
——没有涉及任何开发板。

```
hardware/
├── rtl/
│   ├── mlkem/      mont_reduce.v、butterfly.v、ntt_core.v、basemul.v、
│   │               compress.v、sample.v、pack.v
│   └── keccak/     keccak_f1600.v
├── tb/cocotb/      cocotb 测试台、仅供仿真的汇总顶层、Makefile
├── model/          Python 参考模型、向量导出、独立预言机
└── syn/            Vivado out-of-context 综合脚本与约束
```

所有模块都是可推断的纯 Verilog-2001，未实例化任何厂商原语，因此同一份源码可原样综合
到 Xilinx、Intel 或 Lattice。

## 现有模块

| 模块 | 结构 | 周期数 |
|---|---|---|
| `mont_reduce`、`barrett_reduce` | 组合逻辑 | — |
| `butterfly_ct`、`butterfly_gs` | 组合逻辑 | — |
| `ntt_core` | 单蝶形单元，ML-KEM 的 7 层 NTT，实例化上述模块 | 1153 / 次变换 |
| `mlkem_basemul` | 组合逻辑，五次 Montgomery 乘法 | — |
| `mlkem_compress`、`mlkem_decompress` | 组合逻辑，位宽 `D` 由参数给出 | — |
| `mlkem_cbd2`、`mlkem_cbd3` | 组合逻辑，位并行的中心二项分布采样 | — |
| `mlkem_rej_pair` | 组合逻辑，取候选 | — |
| `mlkem_rej_uniform` | 收集器，每周期吃一组 3 字节 | 约 430 / 个多项式 |
| `mlkem_encode12`、`mlkem_decode12` | 组合逻辑 | — |
| `keccak_f1600` | 单轮迭代，`round_cnt` 走 24 轮 | 24 / 次置换 |

`mlkem_compress` 把对 `q` 的除法换成乘 `ceil(2^33/q)` 再右移。两者在整个输入域上相等，
而输入域只有 3329 个取值，因此测试台是**穷举**验证这一替换，而不是抽样。

`d < 12` 的 `ByteEncode_d` 刻意没有做成模块：把已经落在 `d` 位内的系数拼进字节流纯粹是
导线，没有可写错的逻辑。只有 12 位这一路多一步"把有符号系数折回 `[0, q)`"，才是模块。

`keccak_f1600` 刻意**不做** 24 轮全展开。展开会把轮逻辑复制 24 份，而在此调用频度下
毫无收益：100 MHz 下 24 周期即 240 ns/次置换，而一次 ML-KEM-768 密钥生成需要 43 次
置换，合计约 1000 周期，相较 NTT 的约 6900 周期微不足道。瓶颈不在这里。

两个核都通过 `include/pqchsm/accel.h` 的寄存器接口由 C 侧驱动，因此同一份 RTL 既被
cocotb 对拍，也被 C 测试套件覆盖。

## 验证

```bash
./tools/rtl_sim.sh                        # 完整 cocotb 回归，Icarus Verilog
python3 hardware/model/ntt_oracle.py      # 两道独立的 NTT 预言机
python3 hardware/model/mlkem_oracle.py    # 数据通路其余算子的预言机
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

## 综合

脚本位于 `syn/`，**从未执行过**——开发机上没有安装 Vivado。参见
[syn/README.zh-CN.md](syn/README.zh-CN.md)。
