# ZynqMP XMPU/XPPU 隔离设计（TZDRAM 围护）

目标：OP-TEE 的 TZDRAM（`0x60000000`-`0x6FFFFFFF`，256MB）在**普通世界
内核不可信**的威胁模型下仍不可被 REE 读写。手段是 DDR XMPU 的硬件访
问过滤，与 dtb `reserved-memory`(no-map) 形成纵深：

- no-map 只挡"守规矩的"Linux 内核（不建立映射）；
- XMPU 在 AXI 层挡**任何**非安全事务，包括被攻陷内核发起的 devmem/
  物理地址直读。

事实来源：寄存器布局以 Xilinx QEMU 的 ZynqMP XMPU 设备模型
（`hw/misc/xlnx-zynqmp-xmpu.c`，由寄存器规范自动生成）为准，并与
PMUFW 的 `xpfw_xpu.c`（状态/中断偏移、错误位、master ID 表）和
XAPP1320《Isolation Methods in Zynq UltraScale+ MPSoCs》交叉核对。
TRM 对应章节：UG1085 系统保护单元、UG1087 寄存器手册。

## 1. DDR XMPU 硬件要点（ZynqMP）

- 6 个 DDR XMPU 实例，守在 DDRC 六个 AXI 端口：`0xFD000000` …
  `0xFD050000`（另有 OCM XMPU `0xFFA70000`、FPD XMPU `0xFD5D0000`）。
  同一配置须**广播到全部 6 个**，否则存在绕行端口。
- 每个 XMPU 16 个 region；region 匹配 = master ID 匹配 **且** 地址落入
  [START, END)。扫描顺序 15→0，**先匹配者生效**；无匹配走 CTRL 默认权限。
- NS 检查（relaxed，`NSCHECKTYPE=0`）：安全事务可访问任何 region；
  非安全事务只能访问 `REGIONNS=1` 的 region——这正是"REE 不可读安全
  区"的开关。
- 违规处置：`POISONCFG=1` + `POISON.ATTRIB=1` = 按属性 poison
  （断言 AxUser[10]，事务到 DDRC 被丢弃），XAPP1320 推荐的 DDR 方式；
  同时 ISR 记录违规（含 master ID、地址）。
- 配置寄存器**只接受安全 APB 事务**：REE 即使未锁也写不动 XMPU 配置
  （只能读……实际非安全读也报错）。上锁（`LOCK.REGWRDIS=1`）是第二道，
  且 XMPU 锁不影响 ISR/IDS 清中断（XPPU 锁才有这个坑，见 §4）。

寄存器（每实例，偏移）：

| 偏移 | 名称 | 关键位 |
|---|---|---|
| 0x00 | CTRL | bit0 DEFRDALLOWED, bit1 DEFWRALLOWED, bit2 POISONCFG, bit3 ALIGNCFG（复位值 0xB） |
| 0x0C | POISON | bit20 ATTRIB, [19:0] BASE（地址毒药用，本设计不用） |
| 0x10/14/18/1C | ISR/IMR/IEN/IDS | bit1 RD 违规, bit2 WR 违规, bit3 安全违规 |
| 0x20 | LOCK | bit0 REGWRDIS |
| 0x100+n·0x10 | Rnn START/END | 4KB 单位；ALIGNCFG=1 时按 **1MB** 单位（[27:0] = addr>>20），END 开区间；START==END 表示一个块 |
| 0x108+n·0x10 | Rnn MASTER | [9:0] ID, [25:16] MASK；匹配条件 `(MASK&ID)==(MASK&masterID)` |
| 0x10C+n·0x10 | Rnn CONFIG | bit0 ENABLE, bit1 RD, bit2 WR, bit3 REGIONNS, bit4 NSCHECKTYPE |

相关 master ID（PMUFW `xpfw_xpu.c` LUT）：APU 0x80–0xBF，PMU 0x40，
CSU 0x50/0x51，DAP(调试器) 0x62，ADMA 0x68–0x6F，SD1 0x71，
AFI_FM(PL) 0x200–0x3BF。

## 2. 配置（region 方案）

对全部 6 个 DDR XMPU 广播相同配置（ALIGNCFG 保持复位值 1，地址按
1MB 单位）：

| region | START/END | MASTER | CONFIG | 效果 |
|---|---|---|---|---|
| R0 | 0x600 / 0x700（即 0x60000000–0x70000000） | ID=0x080, MASK=0x3C0（APU 全部） | ENABLE\|RD\|WR，REGIONNS=0，NSCHECKTYPE=0 | APU 安全态（EL3/S-EL1）可读写；**REE（NS APU）触发安全违规被 poison** |
| R1 | 同上 | ID=0x062, MASK=0x3FF（DAP 精确） | ENABLE（不给 RD/WR） | JTAG 调试器显式拒绝（eFUSE 关 JTAG 之前的过渡期保护） |

CTRL = DEFRDALLOWED|DEFWRALLOWED|ALIGNCFG|POISONCFG（0xF）；
POISON = ATTRIB（0x00100000）。

行为分析：

- REE Linux 读 TZDRAM：master=APU 命中 R0，NS 事务访问 REGIONNS=0 →
  拒绝 + SECURITYVIO，数据被 poison。✔
- ATF/OP-TEE（APU 安全态）：R0 命中，secure 放行。✔
- DAP：R1 命中且无任何权限 → 拒绝（扫描 15→0，R1 先于 R0 命中）。✔
- **已知缺口**：ADMA/AFI(PL)/USB/SD 等其它 master 不匹配任何 region，
  走 CTRL 默认放行——即"非 APU master 仍可读 TZDRAM"。当前 PL 侧无
  自研 DMA master、SD/USB DMA 不指向 TZDRAM，风险可接受；P2 引入 PL
  加速核（带 DMA）时必须补一条对应 master 的 region（或整体切换
  默认拒绝 + 全子系统映射，那是 XAPP1320 的完整隔离形态，留待子系统
  规划明确后做）。

## 3. 生效点与实现路径

FSBL 已自带挂点：`xfsbl_handoff.c` 的 `psu_protection()` /
`psu_protection_lock()`，但本项目 XSA 未配隔离，生成的
`psu_init.c` 里 `psu_ddr_xmpu*_data()` 全是空壳，且
`xfsbl_hw.h:917` 定义了 `XFSBL_PROT_BYPASS`（默认旁路，只做 OCM）。

两条路：

1. **重 Vivado 路径（正路，重）**：在 XSA 的 PCW→Isolation 里配上表，
   重出 psu_init.c，去掉 XFSBL_PROT_BYPASS。要动 Vivado 工程。
2. **FSBL 补丁路径（轻，推荐，与看门狗补丁同渠道）**：
   `tools/fsbl_xmpu/xmpu_protection_draft.c` 是寄存器直写实现，把它编进
   已打看门狗补丁的 FSBL（`meta-user/recipes-bsp/fsbl/`），在
   `psu_protection()` 里调用，然后 `psu_protection_lock()` 上锁。
   改动集中在我们的 FSBL 补丁文件，不碰 XSA。

**风险与顺序**：XMPU 配错会拦掉合法 DDR 流量、全机挂死。必须等
看门狗三阶段实测通过、黄金镜像回退验证之后再上板点这个补丁（流程
已就绪：挂死→WDT 复位→multiboot 回 BOOT0001.BIN 黄金镜像）。
上板验证方法：Linux 下 `devmem 0x60000000` 应读到 poison/总线错
（在此之前先确认 OP-TEE 活着、TZDRAM 内有货）。

## 4. XPPU 结论（本阶段不动）

- XPPU（`0xFF980000`）保护外设/IPI/QSPI。本项目 REE 需要正常用
  SD/EMMC/UART/ETH，OP-TEE 不独占任何 LPD 外设，默认配置已够。
- **不要锁 XPPU**：XAPP1320 明确——锁 XPPU 会把清中断寄存器也锁死，
  违规中断无法清除（参考设计因此把 XPPU master 交给 PMU）。
- CSU/eFUSE 寄存器自有访问控制（PMU/secure-only），不依赖 XPPU。
- 后续若要把 SWDT0、CSUDMA 划给安全世界独占，再回来配 XPPU aperture。

## 5. 待办（上板阶段）

- [ ] 看门狗三阶段过后，把 `xmpu_protection_draft.c` 编入 FSBL 补丁，
      出新 BOOT.BIN 测试；挂死会自动回黄金镜像。
- [ ] devmem 验证 REE 读 TZDRAM 被拒 + OP-TEE 功能正常（TA 自检序列）。
- [ ] P2 PL 加速核立项时，补 PL master 的 region 设计。
