[English](vivado.md) · **中文**

# Vivado 流程

**本仓库的开发机上没有 Vivado，以下流程从未执行过。** 脚本本身是完整的，
并通过了离线检查（括号配对、把 Vivado 命令换成桩之后完整执行一遍），
但"综合能过、时序能收敛、比特流能用"这三件事没有任何证据支撑。
第一次跑要逐条看报告。

## 为什么工程不入库

Vivado 的 `.xpr` 与 `.bd` 带绝对路径、带工具版本号，改一个设置就产生大片无法
审阅的 diff，而且换一台机器往往打不开。入库的是生成它们的 Tcl：工程随时可以删掉
重建，评审的对象是脚本。

## 生成工程

```bash
vivado -mode batch -source board/xc7z020/vivado/create_project.tcl
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `-board <名字>` | `pynq_z2` | 选 `constraints/boards/<名字>.xdc` 与 PS7 预设 |
| `-part <型号>` | `xc7z020clg400-1` | 器件型号 |
| `-outdir <路径>` | `<repo>/build-vivado` | 工程输出目录 |
| `-with-ntt` | 关 | 包含 ML-KEM 的 NTT 核。XC7Z020 上会超容，见资源预算 |

生成出来的 block design：

```
PS7 ── M_AXI_GP0 ──┬── pqc_accel_zynq  S_AXI      0x43C0_0000
                   └── axi_dma_0       S_AXI_LITE 0x4040_0000
PS7 ── S_AXI_HP0 ──── axi_dma_0 的两个存储映射主口
axi_dma_0 M_AXIS_MM2S ──► pqc_accel_zynq s_axis
axi_dma_0 S_AXIS_S2MM ◄── pqc_accel_zynq m_axis
FCLK_CLK0 = 100 MHz，复位经 proc_sys_reset
status_led[2:0] 引到顶层，由板级 XDC 分配引脚
```

地址必须与 `board/xc7z020/include/pqc_accel_zynq.h` 里的地址表一致。改一处就要
改另一处：地址错了软件照样读得到值，只是值没有意义，这类故障在板上最难查。

## 板级文件与 DDR

**PS7 的 DDR 时序、MIO 分配、时钟源频率都是板级参数**，不同板子完全不同，
脚本里无法凭空写出来。有两种情况：

**装了板级文件**（推荐）。脚本检测到对应的 board part 之后会套用官方预设，
DDR 与 MIO 都是对的。安装方式是把厂商提供的 board files 放到

```
<Vivado 安装目录>/data/xhub/boards/XilinxBoardStore/boards/
```

或设置 `BOARD_REPO_PATHS` 指向存放目录，重启 Vivado 后 `get_board_parts` 能列出来。
PYNQ-Z2 的 board files 由 TUL 提供，AX7020 由 ALINX 提供。

**没装板级文件**。脚本会明确提示，并只配置与 PL 相关的部分（GP0 主口、HP0 从口、
FCLK0 频率）。DDR 保持 Vivado 默认值——这样出来的工程可以综合、可以实现、
可以看资源与时序报告，**但不能拿去启动真实板子**，因为 DDR 参数不对。

## 出比特流

```bash
vivado -mode batch -source board/xc7z020/vivado/build_bitstream.tcl \
       -tclargs -proj <outdir>/pqc_accel_pynq_z2/pqc_accel_pynq_z2.xpr -jobs 8
```

产物放在工程目录下的 `outputs/`：

| 文件 | 用途 |
|---|---|
| `*_wrapper.bit` | 比特流 |
| `pqc_accel_bd.hwh` | 硬件交接文件，PYNQ 的 `Overlay` 需要它 |
| `*_wrapper.xsa` | 硬件平台描述，PetaLinux 需要它 |
| `utilization.rpt` | 资源占用，与 resource-budget 对照 |
| `timing.rpt` | 时序汇总 |

脚本在两处会主动失败退出，不把问题当警告放过去：

- 综合或实现没跑到 100%；
- WNS 或 WHS 为负。时序不收敛会让加速器的结果随机出错且没有规律，
  带着负余量往下走没有意义。若不收敛，降低 `PCW_FPGA0_PERIPHERAL_FREQMHZ`，
  或在 Keccak 的一轮组合逻辑中间插一级流水。

## 离线检查

改完 Tcl 之后，先跑

```bash
board/xc7z020/tools/tcl_check.sh
```

它做两件事：用 `info complete` 查大括号、方括号与引号是否配对；把所有 Vivado
命令换成桩，在真正的 `tclsh` 里把脚本完整执行一遍。第二步会真实走到每一个分支，
因此拼错的变量名、写反的条件、漏掉的 `incr` 都会当场暴露。

它证明不了 Vivado 命令的参数写对了——那只有装了 Vivado 才知道。
它证明的是"脚本本身能跑完"。
