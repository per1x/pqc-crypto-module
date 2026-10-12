[English](README.md) · **中文**

# 硬件抽象层

本目录位于主机侧密码代码（槽位管理器、密钥库、备份、审计、PKCS#11）和实际做运算的
实体之间。上层只使用句柄，不依赖当前装的是哪个后端。

## 两个接口

`pqc_backend_t`（`include/pqchsm/pqc.h`）是密钥生成 / 封装 / 解封装 / 签名 / 验签的
vtable，有三个实现：

| 实现 | 位置 | 做什么 |
|---|---|---|
| `src/crypto/pqc_native.c` | 软件 | vendored 的 mlkem-native / mldsa-native，与 OP-TEE TA 用同一份源码 |
| `pqc_accel.c` | 本目录 | 驱动下面的 `accel_transport_t` 寄存器接口 |
| `pqc_sdfe.c` | 本目录 | 经 `libsdfe` 和 `pqchsm_fpgad` 调用板上的密码核 |

`accel_transport_t`（`include/pqchsm/accel.h`）是一张 AXI 风格的寄存器表：`CTRL`、
`STATUS`、`MODE`、`PARAM`、`IN_LEN`、`OUT_LEN`、`ERRCODE`，批量数据另行搬运。它对应
`hardware/rtl/bus/` 里的 `pqc_accel_axi` 加速器，这个加速器不在板上的位流里（原因见
`hardware/rtl/board/zu3eg_hsm_top.v`）。板上的 ML-KEM 和 ML-DSA 核通过 `pqc_sdfe.c` 访问。

## 文件

| 文件 | 作用 |
|---|---|
| `pqc_accel.c` | 在寄存器接口上实现 `pqc_backend_t`；另有 `accel_ntt()`、`accel_keccak_f1600()`、`accel_shake()` |
| `accel_stub.c` | 软件 transport。实现寄存器语义，内部调用 `pqc_backend_native()`。始终可用 |
| `accel_verilator.c` | 驱动 Verilator 仿真 RTL 的 transport。只在有 Verilator 时编入，否则 `accel_transport_verilator()` 返回 `NULL` |
| `accel_axi.c` | 用 AXI4-Lite 寄存器事务和 AXI4-Stream 数据包驱动仿真 RTL 的 transport（契约见 `docs/REGISTERS.zh-CN.md`）。编入条件同上，否则 `accel_transport_axi()` 返回 `NULL` |
| `verilator/ntt_sim.cpp`、`keccak_sim.cpp`、`axi_sim.cpp` | 对 Verilated 的 `ntt_core`、`keccak_f1600` 和带 AXI 前端的加速器的 C 包装 |
| `accel_mmap.c` | `/dev/mem` + `mmap` transport。构建时没有定义 `PQCHSM_ACCEL_MMAP_BASE` 和 `PQCHSM_ACCEL_MMAP_BUF` 就返回 `NULL`。没有对着真实 PL 跑过 |
| `hwrng.c` | TRNG 寄存器契约（`trng_axi`，`docs/REGISTERS.zh-CN.md`）的驱动 |
| `hwrng_stub.c`、`hwrng_stub.h` | `trng_axi` 寄存器的软件模型。FIFO 由 OpenSSL 填充，只模拟接口，不提供硬件熵。头文件里有测试用的故障注入钩子 |
| `hwrng_mmap.c` | TRNG 的 `/dev/mem` + `mmap` transport。构建时没有定义 `PQCHSM_HWRNG_MMAP_BASE` 就返回 `NULL`。默认位流（`SECURE_ONLY=1`）下普通世界访问不到这些寄存器，所以只能配合调试位流使用 |
| `hwrng_sdfe.c` | 字节级熵源 `hwrng_byte_source_sdfe()`：经 `libsdfe` 和 `pqchsm_fpgad` 取 TRNG 字节。这是板上走得通的路径 |
| `pqc_sdfe.c` | `pqc_backend_sdfe()`：经服务在 PL 上做 ML-KEM 和 ML-DSA。封装和 `*_hw` 系列（按句柄的 keypair、decaps、sign）走板子。对 ML-KEM / ML-DSA，字节形式的 keypair、decaps、sign 返回 `PQC_ERR_UNSUPPORTED`，私钥不会以字节形式交出。验签留在软件里，与硬件签名形成独立的交叉核对 |
| `sdfe_conn.c`、`sdfe_conn.h` | `hwrng_sdfe.c` 和 `pqc_sdfe.c` 共用的、到服务的唯一一条连接（本机 socket；设了 `PQCHSM_SDFE_HOST` 和 `PQCHSM_SDFE_PKI` 则走 mTLS TCP，另外还读 `PQCHSM_SDFE_PORT`、`PQCHSM_SDFE_DEVICE_CN`）。daemon 一次只服务一个连接，同一进程开两条连接会卡住 |

PKCS#11 模块用环境变量选择板上路径（`src/p11/p11_module.c`）：`PQCHSM_HWRNG=sdfe` 装上
硬件熵源，`PQCHSM_BACKEND=sdfe` 装上 `pqc_backend_sdfe()`。

## 熵源

`hwrng.h` 是第二套 transport 接口，没有并进 `accel.h` 的操作码。加速器是"发命令、等完成"，
TRNG 是自由运行的；TRNG 的访问控制通常更严；TRNG 告警要能独立上报，不能被加速器的忙闲
状态挡住。RTL 里也因为同样的原因把它们做成两个从机。

装上硬件熵源（寄存器 transport 或字节源）之后，`pqc_random_bytes()` 改从它取，取不到就
返回错误，不会回退到软件。软件桩的 `is_hardware = 0`，装上它 `pqc_random_bytes()` 仍然用
OpenSSL。装上字节源后，寄存器级的调用（`hwrng_status`、`hwrng_zeroize`、健康计数）不可用。

## SHAKE 与 Keccak

`accel_shake()` 有两条路径：

- 模式 10（`ACCEL_MODE_SHAKE`）：整条海绵在 PL 里完成，总线上只传消息和摘要。消息和输出
  都不超过 `ACCEL_SHAKE_MAX`（512 字节，PL 侧缓冲区大小）时使用。rate、域分隔后缀和输出
  长度打包在 `PARAM` 里（`ACCEL_SHAKE_PARAM`）。
- 模式 9（`ACCEL_MODE_KECCAK_F1600`）：消息或输出更长，或 transport 回 `ERRCODE=3`
  （该模式未实现）时使用。填充和吸收 / 挤压的分块在 C 里做，只有置换在硬件里执行，每次
  置换要搬进搬出 200 字节状态。

只有 `ERRCODE=3` 会触发回落，其他失败原样返回给调用方。SHAKE 是公开函数，回落到软件分块
不影响机密性；熵源回落到软件会影响，所以熵源不允许回落。

## 仿真 transport

Verilator 和 AXI 两条 transport 实现了 `NTT_FWD`、`NTT_INV` 和 `KECCAK_F1600`。其他模式一律
置 `STATUS.ERR`、`ERRCODE=3`，上层得到 `PQC_ERR_UNSUPPORTED`。这两条 transport 里没有
软件回落。完整的 ML-KEM 和 ML-DSA 核（`hardware/rtl/mlkem`、`hardware/rtl/mldsa`）不能
通过它们访问。

## 一致性测试

软件桩和仿真 RTL 经同一个寄存器接口必须返回相同的字节。`tests/unit/test_accel.c`（直连）
和 `tests/unit/test_accel_axi.c`（经 AXI 事务）检查两个 NTT 方向和 SHA3 / SHAKE 路径，
后者与 OpenSSL 对比。真实硬件 transport 也要通过同样的测试。

本层在整个模块中的位置见 [docs/ARCHITECTURE.zh-CN.md](../../docs/ARCHITECTURE.zh-CN.md)。
