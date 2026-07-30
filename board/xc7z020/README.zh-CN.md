[English](README.md) · **中文**

# XC7Z020（Zynq-7000）板级支持

本目录是 `board/xc7z020` 分支专属的内容，**不合并回 `main`**。`main` 保持厂商
中立：那里的 RTL 不含任何厂商原语，那里的软件不假定任何具体器件。板级的东西
（Vivado 工程、引脚约束、器件容量取舍、真实 MMIO 地址、镜像集成）全部留在这里。

通用 RTL 由本目录**直接引用**主干路径，不复制一份——复制出来的副本迟早与主干
漂移，而且会让"cocotb 验的是不是综合进去的那份"变得说不清。

## 目录

```
board/xc7z020/
├── rtl/            板级包装 pqc_accel_zynq（端口改成 AXI 命名约定 + 状态 LED）
├── vivado/         生成工程与 block design 的 Tcl、出比特流的 Tcl
├── constraints/    timing.xdc 与板无关；boards/<板名>.xdc 只放引脚与电平
├── src/            真实 MMIO 后端 accel_zynq.c（UIO / /dev/mem + AXI-DMA）
├── include/        地址表与 AXI-DMA 寄存器偏移
├── tests/          MMIO 后端的时序验证（映射换成内存，线程扮演硬件）
├── tools/          离线检查、资源预算、交叉编译
├── pynq/           PYNQ overlay 与上板自检
├── petalinux/      设备树片段与构建配置
└── docs/           资源预算、Vivado 流程、交叉编译
```

## 系统结构

```
        Cortex-A9 (armv7)                        PL
 ┌───────────────────────────┐   ┌────────────────────────────────┐
 │ libpqchsm                 │   │                                │
 │   pqc_backend_accel       │   │  pqc_accel_zynq                │
 │     accel_transport_zynq ─┼───┼─► AXI4-Lite  0x43C0_0000       │
 │       控制面：寄存器读写   │   │     控制/状态寄存器             │
 │       数据面：AXI-DMA ────┼───┼─► AXI4-Stream ─► keccak_f1600  │
 └───────────────────────────┘   └────────────────────────────────┘
        DDR 保留段 0x1F00_0000        axi_dma  0x4040_0000
```

命令时序与 `docs/register-map.zh-CN.md` 完全一致，与仿真后端也完全一致：
配 MODE/IN_LEN → 把输入搬进 PL → 写 CTRL.START → 轮询 STATUS.DONE → 把结果搬回。
换到仿真、换到真板，改的只是"事务怎么发出去"这一层。

## 交付配置里有什么

只有 Keccak-f[1600] 核。ML-KEM 的 NTT 核放不进 XC7Z020，依据是实测的
58350 LUT（器件容量 53200）——完整的论证、代价评估与"要放回来该怎么做"见
[docs/resource-budget.zh-CN.md](docs/resource-budget.zh-CN.md)。

信任根、PUF、安全启动、eFUSE 这一套**本期不做**。选 XC7Z020 就意味着密钥派生根
继续是软件常量，设备绑定不成立。这一点在主干的
[docs/security-policy.zh-CN.md](../../docs/security-policy.zh-CN.md) 第 13 节里
已经作为送检差距列明，本分支没有改变它。

## 不用板子就能验的（已全部通过）

```bash
board/xc7z020/tools/board_checks.sh          # 下面四项一次跑完
board/xc7z020/tools/board_checks.sh --fast   # 跳过资源预算（Yosys 综合几分钟）
```

| 检查 | 判据 | 结果 |
|---|---|---|
| 板级 RTL 静态检查 | Verilator `-Wall` 零告警 + Yosys 可综合 | 通过 |
| Vivado Tcl 离线检查 | 括号配对 + 桩执行跑到最后一行 | 两个脚本均通过 |
| 交付配置的 cocotb 回归 | `INCLUDE_NTT=0` 下操作码 7/8 返回 ERRCODE=3，Keccak 数据面正确 | 9 通过 2 跳过 |
| 资源预算 | 交付配置不超过器件容量的 70% | 34.4%，通过 |

另外两项在主机上跑：

```bash
ctest --test-dir build -R accel_zynq          # MMIO 后端的寄存器与 DMA 时序
board/xc7z020/tools/armv7_test.sh             # armv7 交叉编译 + QEMU 下的回归
```

`accel_zynq` 用例把映射换成内存，另起线程扮演硬件，按 AXI-DMA 与寄存器契约的
语义响应。它查出过一个真实缺陷：DMA 没搬回结果时驱动仍然返回成功。

## 必须上板才能做的

以下每一项都备好了脚本或文档，但**从未执行过**，因为开发机上没有 Vivado、
没有 PetaLinux、没有板子。

| 步骤 | 怎么做 | 需要 |
|---|---|---|
| 生成 Vivado 工程 | `vivado -mode batch -source vivado/create_project.tcl` | Vivado |
| 出比特流与 .hwh/.xsa | `vivado -mode batch -source vivado/build_bitstream.tcl -tclargs -proj <path>.xpr` | Vivado |
| 核对资源占用 | 比对 `outputs/utilization.rpt` 与 docs/resource-budget | Vivado |
| 确认时序收敛 | `outputs/timing.rpt` 的 WNS/WHS 为正（脚本已在为负时失败退出） | Vivado |
| 确认 PS7 的 DDR/MIO | 装板级文件套官方预设，否则镜像起不来 | 板级文件 |
| 核对 LED 引脚 | 与手上这块板的官方 master XDC 逐条对照 | 板子资料 |
| 构建可启动镜像 | 见 [petalinux/README.zh-CN.md](petalinux/README.zh-CN.md) | PetaLinux |
| 上板自检 | 见 [pynq/README.zh-CN.md](pynq/README.zh-CN.md) | 板子 |
| 确认地址映射一致 | 读 VERSION 应得 0x0001_0000 | 板子 |
| 实测端到端加速比 | 板上跑 `pqchsm-prim-bench` | 板子 |

第一次上板的排查顺序：LED0 不亮 → 复位没释放或时钟没配；读 VERSION 读不到
0x0001_0000 → 地址表与实际映射不一致；VERSION 对但 DONE 不置位 → 数据面或
时钟域有问题；DONE 置位但结果不对 → 先怀疑 DMA 缓冲的 cache 属性。

## 换到别的 XC7Z020 板子

只需要两件事：

1. 照 `constraints/boards/pynq_z2.xdc` 的格式补一份 `<板名>.xdc`，填 LED 引脚与
   I/O 电平。`constraints/timing.xdc` 与 RTL、block design、软件都不用动。
   `constraints/boards/ax7020.xdc` 是留好的模板，引脚待填。
2. 用 `-board <板名>` 生成工程，并按该板的资料确认 PS7 的 DDR/MIO 预设。

引脚号本目录不写猜测值：LED 引脚在不同厂商的 XC7Z020 板子之间没有任何通用性，
填错的后果是把输出脚接到另一个输出脚上。
