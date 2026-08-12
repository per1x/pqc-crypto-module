[English](testing.md) · **中文**

# 测试说明

测了什么、用什么手段测的、每个结果怎么复现。目标是：本仓库任何地方引用的数字，
都能在干净检出上用最后一列的命令重新跑出来。

## 汇总

| 检查项 | 结果 | 命令 |
|---|---|---|
| 单元、集成与 KAT 测试 | 45 / 45，4059 条断言 | `ctest --test-dir build` |
| NIST ACVP 向量 | 390 条逐字节一致，60 条如实跳过 | `./tools/kat_evidence.sh` |
| AddressSanitizer + UndefinedBehaviorSanitizer | 45 / 45 | `ctest --test-dir build-asan` |
| ThreadSanitizer | 0 条竞争 | `ctest --test-dir build-tsan` |
| aarch64 Linux（GCC 12） | 45 / 45 | `./tools/aarch64_test.sh` |
| libFuzzer | 138 万次执行，无崩溃 | `./tools/fuzz.sh` |
| cocotb RTL 回归 | 19 个顶层共 117 个测试 | `./tools/rtl_sim.sh` |
| RTL lint | 31 个模块，0 条告警 | `./tools/rtl_lint.sh` |
| RTL 可综合性（Yosys） | 31 个模块全部可综合 | `./tools/rtl_synth_check.sh` |
| 常量时间源码审计 | 0 条未标注问题，1 条带理由的白名单 | `python3 tools/ct_audit.py` |
| 清零结构性检查 | 0 条遗漏，9 条带理由的豁免 | `python3 tools/check_zeroize.py` |
| 独立预言机 | 全部通过，每道自带反证 | `python3 hardware/model/*_oracle.py` |

## 方法

测试源码里贯穿三个习惯，它们也解释了这些文件为什么这么长。

**独立预言机。** 结果与本项目自己的模型一致并不足以说明它正确。凡是模型与向量
同源之处，都会写明，并另加一道真正独立的校验：完全不碰旋转因子表的 schoolbook
卷积、用有理数算出的 FIPS 定义式、用各个算子重建密钥生成并与 NIST 向量逐字节
比对。

**反证。** 每一条本应能失败的断言，都被演示过确实会失败：扰动一个旋转因子、
丢掉一层 NTT、翻转 Keccak 轮常数的一个比特、在 ThreadSanitizer 下移除一把锁、
加入一个假的密钥读回函数、给一个故意提前退出的比较计时、把健康检测阈值调高到
不可能触发。一条不会失败的检查什么都证明不了，而对照正是区分这两种情况的东西。

**结构性检查。** 功能测试看不见的性质——不存在秘密相关的分支、每个密钥字段都被
其销毁函数抹掉、密钥派生根没有读回接口——写成接进 `ctest` 的扫描器。每个扫描器
在被允许报告"真实源码干净"**之前**，先要在合成样本上自检通过。

## 软件测试

`ctest` 共 45 个用例。除各模块的单元测试外，值得单独点出的有：

| 用例 | 它确立了什么 |
|---|---|
| `selftest` | 五项已知答案测试通过，且错误状态确实拦下每一个密码入口——包括参数非法时，因此非法调用也绕不过闸门 |
| `accel` | 寄存器接口后端与 liboqs 在每个算法与参数集上输出逐字节相同 |
| `accel_axi` | 同上，但经真实 AXI4-Lite 与 AXI4-Stream 事务驱动仿真 RTL，并覆盖软件视角下的寄存器契约 |
| `zeroize` | 密钥结构体整体被抹掉；清零原语在 `-O2` 下存活，而同一位置的普通 `memset` 不会 |
| `ct_timing` | 常量时间比较的 Welch t 检验，带必须被判出的泄漏对照与不得报警的空对照 |
| `slot_concurrent` | 并发会话下的槽位管理器；通过移除一把锁并观察 ThreadSanitizer 报出竞争来验证有效性 |
| `audit_integration` | 审计链任一记录被篡改都会被发现，包括整文件重写——那一步由 ML-DSA 锚点抓住 |
| `kat_*` | 390 条 NIST ACVP 向量，逐字节一致 |

## 硬件测试

`./tools/rtl_sim.sh` 在 Icarus Verilog 下跑 19 个顶层共 117 个 cocotb 测试。
同一份 RTL 另由 Verilator 编译并被 C 测试套件驱动，因此两个仿真器在位宽截断
语义上的分歧会表现为测试失败，而不是悄悄的差异。

| 顶层 | 覆盖内容 |
|---|---|
| `mont_reduce`、`butterfly_ct`、`butterfly_gs` | ML-KEM 算子对向量与定义式 |
| `ntt_core` | ML-KEM 的 7 层 NTT、电平锁存的 `done`、干净复位、往返性质 |
| `mlkem_basemul` | 基乘对环上定义式 |
| `mlkem_compress`、`mlkem_decompress` | 对整个输入域穷举，覆盖每个 `d` |
| `tb_mlkem_units` | 二项采样、拒绝采样及其收集器、12 位编解码 |
| `tb_mldsa_units` | ML-DSA 算子、高低位拆分、提示位、两类采样、两个参数集 |
| `mldsa_ntt_core` | ML-DSA 的完整 8 层 NTT，正变换 2049 周期、逆变换 2561 周期 |
| `keccak_f1600` | 公开的置换向量、与 `hashlib` 比对的海绵 |
| `pqc_accel_axi` | 寄存器映射契约逐条验证、两条数据通路、流握手空拍 |
| `tb_trng_health` | SP 800-90B 连续健康检测，附空对照实例 |

## 复现证据

```bash
./tools/fetch_vectors.sh                   # NIST ACVP 向量，提交号已固定
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./tools/kat_evidence.sh                    # 重新生成 ACVP 证据表
./tools/rtl_sim.sh                         # cocotb 回归
./tools/rtl_lint.sh                        # Verilator + Icarus lint
./tools/rtl_synth_check.sh                 # Yosys 可综合性检查
python3 tools/ct_audit.py --self-test && python3 tools/ct_audit.py
python3 tools/check_zeroize.py --self-test && python3 tools/check_zeroize.py
python3 hardware/model/ntt_oracle.py
python3 hardware/model/mlkem_oracle.py
python3 hardware/model/mldsa_oracle.py
python3 hardware/model/trng_health_model.py
```

可选组件是被检测而不是被要求的：没装 Verilator 时仿真 RTL 后端不编入、对应的
transport 返回 `NULL`；没有 `cocotb`、`iverilog` 或 `pkcs11-tool` 时，受影响的
测试会带说明跳过而不是失败。

## 测试没有确立的事情

- 一切都没有在硬件上跑过。没有综合结果、没有时序收敛、没有功耗实测，
  也没有对物理噪声源做过熵评估。
- liboqs 与 OpenSSL 内部的算法实现是原样使用的；ACVP 向量证明的是本模块正确地
  驱动了它们，而不是这两个库自身没有缺陷。
- 常量时间审计是词法层面的，不是编译器层面的：它分析源码，不分析生成的指令。
- 没有做过任何形式的侧信道实测。
