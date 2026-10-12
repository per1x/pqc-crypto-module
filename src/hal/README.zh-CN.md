[English](README.md) · **中文**

# 硬件抽象层

本目录实现密码核心与实际执行算术运算的实体之间的接缝。其上的一切——槽位管理器、
密钥库、包裹、备份、审计、PKCS#11——都基于句柄编写，从不感知当前使用的是哪个后端。

## 两级抽象

**`pqc_backend_t`**（声明于 `include/pqchsm/pqc.h`）是密钥生成/封装/解封装/签名/验签
的 vtable。`src/crypto/pqc_native.c` 以软件方式实现它（vendored 的 mlkem-native /
mldsa-native，与 OP-TEE TA 共用同一棵源码树）；本目录的 `pqc_accel.c` 在寄存器接口
之上实现同一张表，`pqc_sdfe.c` 则通过 `pqchsm_fpgad` 服务调用 FPGA 来实现它。

**`accel_transport_t`**（声明于 `include/pqchsm/accel.h`）就是那个寄存器接口：一张
AXI 风格的寄存器表，含 `CTRL`、`STATUS`、`MODE`、`PARAM`、`IN_LEN`、`OUT_LEN` 与
`ERRCODE`；批量数据单独搬运，因为真实设计会使用 AXI-DMA 而非逐字读寄存器。这张表在
编写任何 RTL 之前就已固定，使接口层面的错误在软件阶段暴露，而不是在板上调试时。

## 文件

| 文件 | 作用 |
|---|---|
| `pqc_accel.c` | 在寄存器接口之上实现 `pqc_backend_t`；同时提供 `accel_ntt()`、`accel_keccak_f1600()`、`accel_shake()` |
| `accel_stub.c` | 软件 transport。精确实现寄存器语义，内部调用软件后端（`pqc_backend_native()`）。始终可用。 |
| `accel_verilator.c` | 驱动 Verilator 仿真 RTL 的 transport。仅在存在 Verilator 时编入，否则 `accel_transport_verilator()` 返回 `NULL`。 |
| `accel_axi.c` | 经真正的 AXI4-Lite 寄存器事务与 AXI4-Stream 数据包驱动仿真 RTL 的 transport（走 `docs/REGISTERS.zh-CN.md` 的契约）。仅在存在 Verilator 时编入，否则 `accel_transport_axi()` 返回 `NULL`。 |
| `verilator/ntt_sim.cpp` | 对 Verilated `ntt_core` 的 C 包装 |
| `verilator/keccak_sim.cpp` | 对 Verilated `keccak_f1600` 的 C 包装 |
| `verilator/axi_sim.cpp` | 对 Verilated 带 AXI 前端加速器的 C 包装，供 `accel_axi.c` 使用 |
| `accel_mmap.c` | 打真 PL 的 `/dev/mem` + `mmap` transport。构建时未给出 `PQCHSM_ACCEL_MMAP_BASE` 与 `PQCHSM_ACCEL_MMAP_BUF` 时返回 `NULL`。尚未对着真 PL 驱动过。 |
| `hwrng.c` | PL 熵源（`trng_axi`）驱动，照 `docs/REGISTERS.zh-CN.md` 的契约实现 |
| `hwrng_stub.c` | `trng_axi` 寄存器语义的软件模型。FIFO 由 OpenSSL 填 —— 模拟的是**接口**，不是熵。 |
| `hwrng_mmap.c` | PL 里 TRNG 的 `/dev/mem` + `mmap` transport。构建时未给出 `PQCHSM_HWRNG_MMAP_BASE` 时返回 `NULL`；交付形态（`SECURE_ONLY`）下普通世界够不到这些寄存器，所以它只用于调试位流和作为 TA 驱动的原型 |
| `hwrng_sdfe.c` | 字节级熵源（`hwrng_byte_source_sdfe()`）：经 `libsdfe` → `pqchsm_fpgad` 取 TRNG 字节；交付形态下真正走得通的那条路 |
| `pqc_sdfe.c` | 经服务在 FPGA 上跑 ML-KEM / ML-DSA 的 `pqc_backend_t`（`pqc_backend_sdfe()`）。字节版 keygen/decaps/sign 有意不实现：私钥留在硬件里，只经句柄使用 |
| `sdfe_conn.c`、`sdfe_conn.h` | 到服务的唯一一条共享连接（本机 socket；设了 `PQCHSM_SDFE_HOST` + `PQCHSM_SDFE_PKI` 则走 mTLS TCP），`hwrng_sdfe.c` 与 `pqc_sdfe.c` 共用；daemon 一次只服务一个连接 |
| `hwrng_stub.h` | 软件 TRNG 模型的内部故障注入钩子（仅供测试） |

## 熵源是独立外设，没有并进加速器的操作码

`hwrng.h` 是第二条 transport 接缝，**有意**不并进 `accel.h` 的操作码空间。三条理由
都是架构性的，与 RTL 侧把它们分成两个从机的理由完全相同：

- **生命周期不同**：加速器是"发命令 → 等完成"，TRNG 是常开自由运行的；
- **访问权限不同**：熵源通常比算法加速器管得更严；
- **故障域不同**：TRNG 告警要能独立上报，不该被加速器的忙闲状态挡住。

并进操作码会把这三件事搅在一起。

装上硬件熵源（寄存器级 transport 或字节源）之后，`pqc_random_bytes()` 改从它取，
且**取不到时不回退到软件源**。软件桩的 `is_hardware = 0`，装它不会改变
`pqc_random_bytes()` 的取数来源（OpenSSL）。静默回退会让"熵来自硬件"这句话恰好在最要紧的
时刻悄悄变成假话，而调用方无从知道。

## 软硬件的分工

`accel_shake()` 有两条路，优先走硬件海绵：

- **模式 10（`ACCEL_MODE_SHAKE`）**：整条海绵在 PL 里跑完，总线上只走消息与摘要。
  这是默认路径，条件是消息与输出都不超过 `ACCEL_SHAKE_MAX`（512 字节，PL 侧缓冲区
  的尺寸）。rate、域分隔后缀与输出长度打包在 `PARAM` 里（见 `ACCEL_SHAKE_PARAM`）。
- **模式 9（`ACCEL_MODE_KECCAK_F1600`）**：消息超限，或该 transport 没实现模式 10
  （回 `ERRCODE=3`）时，退回 C 侧做 framing、只把置换交给硬件。

**为什么优先第一条**：不是为了快，是为了海绵的中间状态不离开密码边界。走模式 9
时每置换一次就要把 200 字节状态搬进搬出，而 SHAKE 的中间状态在 ML-KEM 里就是
ρ/σ 展开的上下文。

**这个回落与熵源的"绝不回落"不是一回事**。SHAKE 是公开函数，退回软件 framing
不损失任何机密性保证，损失的只是那条纵深；熵源回落到软件 RNG 会直接改变安全根基。
两者的差别就在这里，所以一个允许回落、一个宁可失败。

**回落只认 `ERRCODE=3`**（该模式没实现）。其他任何失败都如实向上报错 ——
一块坏了的加速器如果每次都被软件兜住，就再也没人会发现它坏了。

## 仿真后端的覆盖范围

Verilator 与 AXI 两条 transport 实现了 `NTT_FWD`、`NTT_INV` 与 `KECCAK_F1600`。其余
模式一律置 `STATUS.ERR` 并给出 `ERRCODE=3`，向上呈现为 `PQC_ERR_UNSUPPORTED`。它们
刻意**不**回退到软件：静默回退会让"RTL 路径已跑通"变成假象。完整的 ML-KEM/ML-DSA 核
（`hardware/rtl/mlkem`、`hardware/rtl/mldsa`）够不到这两条仿真 transport；板上经
`pqc_sdfe.c` 使用它们。

## 一致性要求

软件桩与仿真 RTL 必须经由同一寄存器接口产出逐字节相同的结果。
`tests/unit/test_accel.c`（直连）与 `tests/unit/test_accel_axi.c`（经 AXI 事务）对两个
NTT 方向以及整条 SHA3/SHAKE 路径断言这一点，并将后者与 OpenSSL 交叉比对。同样的要求也将适用于真实硬件 transport。

本层如何嵌入模块整体，参见 [docs/ARCHITECTURE.zh-CN.md](../../docs/ARCHITECTURE.zh-CN.md)。
