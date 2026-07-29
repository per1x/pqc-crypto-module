[English](README.md) · **中文**

# 硬件抽象层

本目录实现密码核心与实际执行算术运算的实体之间的接缝。其上的一切——槽位管理器、
密钥库、包裹、备份、审计、PKCS#11——都基于句柄编写，从不感知当前使用的是哪个后端。

## 两级抽象

**`pqc_backend_t`**（声明于 `include/pqchsm/pqc.h`）是密钥生成/封装/解封装/签名/验签
的 vtable。`src/crypto/pqc_liboqs.c` 直接实现它；本目录的 `pqc_accel.c` 则在寄存器
接口之上实现同一张表。

**`accel_transport_t`**（声明于 `include/pqchsm/accel.h`）就是那个寄存器接口：一张
AXI 风格的寄存器表，含 `CTRL`、`STATUS`、`MODE`、`PARAM`、`IN_LEN`、`OUT_LEN` 与
`ERRCODE`；批量数据单独搬运，因为真实设计会使用 AXI-DMA 而非逐字读寄存器。这张表在
编写任何 RTL 之前就已固定，使接口层面的错误在软件阶段暴露，而不是在板上调试时。

## 文件

| 文件 | 作用 |
|---|---|
| `pqc_accel.c` | 在寄存器接口之上实现 `pqc_backend_t`；同时提供 `accel_ntt()`、`accel_keccak_f1600()`、`accel_shake()` |
| `accel_stub.c` | 软件 transport。精确实现寄存器语义，内部调用 liboqs。始终可用。 |
| `accel_verilator.c` | 驱动 Verilator 仿真 RTL 的 transport。仅在存在 Verilator 时编入，否则 `accel_transport_verilator()` 返回 `NULL`。 |
| `verilator/ntt_sim.cpp` | 对 Verilated `ntt_core` 的 C 包装 |
| `verilator/keccak_sim.cpp` | 对 Verilated `keccak_f1600` 的 C 包装 |

将来的 `accel_mmap.c` 会为真实可编程逻辑增加 `/dev/mem` + `mmap` transport。届时本
目录之上的一切都不需要改动。

## 软硬件的分工

`accel_shake()` 在 C 侧完成海绵结构——padding、rate、吸收、挤压——仅在 Keccak-f[1600]
置换处调用 transport。这与真实设计的划分方式一致：只暴露置换的核更小，且
SHAKE128、SHAKE256、SHA3-256 与 SHA3-512 可以共用，framing 交由处理系统负责。

## 仿真后端的覆盖范围

Verilator transport 实现了 `NTT_FWD`、`NTT_INV` 与 `KECCAK_F1600`。其余模式一律置
`STATUS.ERR` 并给出 `ERRCODE=3`，向上呈现为 `PQC_ERR_UNSUPPORTED`。它刻意**不**回退
到软件：静默回退会让"RTL 路径已跑通"变成假象。完整的 ML-KEM/ML-DSA 核属于后续工作。

## 一致性要求

软件桩与仿真 RTL 必须经由同一寄存器接口产出逐字节相同的结果。
`tests/unit/test_accel.c` 对两个 NTT 方向以及整条 SHA3/SHAKE 路径断言这一点，并将
后者与 OpenSSL 交叉比对。同样的要求也将适用于真实硬件 transport。

本层如何嵌入模块整体，参见 [docs/architecture.zh-CN.md](../../docs/architecture.zh-CN.md)。
