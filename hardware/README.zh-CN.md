[English](README.md) · **中文**

# 硬件

RTL 源码、验证环境、生成黄金向量的参考模型，以及综合脚本。此处的一切都在仿真中运行
——没有涉及任何开发板。

```
hardware/
├── rtl/
│   ├── mlkem/      mont_reduce.v、butterfly.v（CT 与 GS）、ntt_core.v
│   └── keccak/     keccak_f1600.v
├── tb/cocotb/      cocotb 测试台与 Makefile
├── model/          Python 参考模型、向量导出、独立预言机
└── syn/            Vivado out-of-context 综合脚本与约束
```

## 现有模块

| 模块 | 结构 | 周期数 |
|---|---|---|
| `mont_reduce`、`barrett_reduce` | 组合逻辑 | — |
| `butterfly_ct`、`butterfly_gs` | 组合逻辑 | — |
| `ntt_core` | 单蝶形单元，ML-KEM 的 7 层 NTT，实例化上述模块 | 1153 / 次变换 |
| `keccak_f1600` | 单轮迭代，`round_cnt` 走 24 轮 | 24 / 次置换 |

`keccak_f1600` 刻意**不做** 24 轮全展开。展开会把轮逻辑复制 24 份，而在此调用频度下
毫无收益：100 MHz 下 24 周期即 240 ns/次置换，而一次 ML-KEM-768 密钥生成需要 43 次
置换，合计约 1000 周期，相较 NTT 的约 6900 周期微不足道。瓶颈不在这里。

两个核都通过 `include/pqchsm/accel.h` 的寄存器接口由 C 侧驱动，因此同一份 RTL 既被
cocotb 对拍，也被 C 测试套件覆盖。

## 验证

```bash
./tools/rtl_sim.sh                        # 完整 cocotb 回归，Icarus Verilog
python3 hardware/model/ntt_oracle.py      # 两道独立的 NTT 预言机
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
- **Keccak 预言机 1**——公开的全零输入 Keccak-f[1600] 置换输出，以常量硬编码，不由本
  仓库任何代码生成。
- **Keccak 预言机 2**——在 RTL 核**之上**搭出 SHAKE128/256 与 SHA3-256 的海绵结构，
  与 `hashlib` 逐字节比对（C 侧则与 OpenSSL 比对）。它覆盖 padding、rate、lane 小端
  字节序以及多块吸收与挤压，而不只是置换本身。

每道预言机都配有反证——扰动一个旋转因子、丢掉一层 NTT、翻转 Keccak 轮常数的一个
比特——并确认在每种情况下都会失败。

## 仿真器选择

自包含的算法核以 Verilator 与 Icarus Verilog 在 cocotb 下验证；cocotb 不支持 Vivado
`xsim`，因此今后包含厂商 IP 的顶层需要单独仿真。这一约束的副作用是有益的：它迫使算法
核不依赖厂商原语，而这正是它们可移植的原因。

Verilator 的 2-state 语义与 Icarus 的 4-state 语义在位宽截断上并不一致，这种差异曾
暴露过真实缺陷。因此两者都运行。

## 综合

脚本位于 `syn/`，**从未执行过**——开发机上没有安装 Vivado。参见
[syn/README.zh-CN.md](syn/README.zh-CN.md)。
