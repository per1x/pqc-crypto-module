# RTL 静态审查报告 —— pqc-hsm / ML-KEM NTT 核

**审查日期**：2026-07-29
**审查范围**：`rtl/mlkem/` 全部 RTL、`tb/cocotb/` 对拍、`syn/` 综合脚本与约束、`src/hal/` 的 Verilator 桥、`model/ref_model.py` 参考模型
**审查方式**：grep 文本扫描 + 逐行人工审查（不改动任何 RTL 源码）
**背景校正**：本项目目前**只有 NTT 侧写成了 RTL**（Keccak 尚未开始）。用户清单里 “DSP=0” 是 Keccak 专用硬指标，**不适用于 NTT**——NTT 必然含模乘，用 DSP 是正常且正确的，本报告按 NTT 的预期（蝶形结构、模约减、位宽、旋转因子 ROM）来判。

---

## 0. 审查对象清单

| 文件 | 行数 | 性质 | 结论一句话 |
|------|------|------|-----------|
| `rtl/mlkem/mont_reduce.v` | 29 | 纯组合 Montgomery 约减 | 位宽显式、与 C 逐位等价，干净 |
| `rtl/mlkem/butterfly.v` | 54 | 纯组合 barrett + CT/GS 蝶形 | 干净；但**未被 NTT 核例化**（见 D） |
| `rtl/mlkem/ntt_core.v` | 222 | 时序：256 点 NTT/INTT 状态机 | **单蝶形/周期迭代版**，功能对得上模型；有若干工程性建议 |
| `tb/cocotb/test_ops.py` | 76 | mont 对拍 + 定义式性质断言 | 好 |
| `tb/cocotb/test_butterfly.py` | 60 | CT/GS 蝶形对拍 | 好 |
| `tb/cocotb/test_ntt_core.py` | 133 | 正/逆 NTT + 往返性质 | 好，但“三方”实为“两方”（见 E） |
| `syn/ooc_synth.tcl` / `syn/constraints/ooc.xdc` | — | OOC 综合脚本 + 约束 | **XDC 已与 ntt_core 脱节，必须修** |
| `src/hal/verilator/ntt_sim.cpp` | 128 | Verilator→C 桥 | 功能对；依赖 done 脉冲逐周期轮询 |
| `model/ref_model.py` / `export_vectors.py` | — | 独立参考模型 + 向量导出 | NTT 无独立预言机（见 E） |

**没有找到任何 AXI / 寄存器接口 RTL 文件**（`grep -i axi/hal/reg_if/apb` 命中的都是 C 头文件或 C 实现）。这一点直接回答问题②，详见第 6 节。

---

## 第一轮 · 文本 / 逐行可综合性审查

grep 扫描结果（设计文件 `rtl/mlkem/*.v`，不含 tb）：

| 检查项 | 结果 |
|--------|------|
| `#` 延时 | **无** |
| `initial` | 仅 1 处 = `ntt_core.v:41` 的 zetas ROM 初始化（属允许例外）|
| `while / fork / forever / repeat` | **无** |
| `$display / $random / $monitor / $finish` | **无** |
| `for` 循环 | **无**（关键：说明不是全展开，见问题①）|
| 第二个时钟名 | **无**，全设计只有一个 `clk` |
| always 块总数 | 1 个，且为 `@(posedge clk or negedge rst_n)` |

逐条结论：

**1. 不可综合语法** —— 【可放过】
设计文件里没有 `#`延时 / `while` / `$display` / `$random` / `fork`。唯一的 `initial` 是给 `zetas[0:127]` 旋转因子 ROM 赋初值，正是清单里“BRAM/ROM 初始化除外”的例外，Xilinx/Vivado 可综合成带初值的 ROM。
提示：若将来移植到 ASIC 或部分国产器件综合流程，`initial` 初值不被吃，届时应改成 `$readmemh` 或复位期加载。当前放过。

**2. 阻塞 / 非阻塞** —— 【可放过】（写得对）
- 唯一的时序块（`ntt_core.v:134`）**全部使用 `<=`**（已逐行 + grep 双重确认，无一处混用）。
- `mont` / `barr` 两个 `function`（组合）内部**全部使用 `=`**——这正是函数体应有的写法。
- `butterfly.v` / `mont_reduce.v` 全用 `assign` 连续赋值，无过程块。
这是“仿真对、上板错”头号来源，此处**没有问题**，做得规范。

**3. 组合块敏感列表** —— 【可放过】
没有任何手写 `@(a or b)` 组合 always 块；组合逻辑一律用 `assign` / `function`，不存在敏感列表遗漏。

**4. inferred latch（每条路径都赋值）** —— 【可放过】
没有 `always @(*)` 组合块，因此没有组合 latch 风险。`mont`/`barr` 函数对返回值无条件赋值。时序块里未赋值的寄存器是“保持”（触发器本意），非 latch。`case (state)` 带 `default: state <= S_IDLE;`，安全。

**5. 复位** —— 【建议改】
- 极性：全设计只有 `rst_n`（低有效），**一致**。
- `done` 复位为 `1'b0`，**符合 accel.h 契约里 “DONE 复位必须为 0”**。
- 但**数据通路 / 计数器寄存器 `len` `grp` `j` `k` `inv_r` `scale_i` 没有复位分支**。功能上安全——它们都在 `start` 命中时（S_IDLE）或进入 S_SCALE 时被无条件加载，X 值不会传播到输出；但这不符合清单“计数器/控制寄存器要有复位”，且会让复位后头几个周期在 4-state 仿真（Icarus）里出现 X。建议在 `!rst_n` 分支给确定初值。
- `mem[0:255]` 系数存储不复位——可放过（数据 RAM，写后才读，标准做法）。

**6. 循环展开 / 并行度** —— 【可放过】（正是想要的结构）
**没有任何 `for` 循环**。256 点 NTT 用 `len/grp/j/k` 四个计数器在状态机 `S_RUN` 里**每周期做一个蝶形**迭代完成，不是把 256 点摊成 256 个乘法器。头注释亦明写“首版：1 蝶形/周期，约 910 cycles @150MHz ≈ 6.1µs”。这是问题①的直接证据——**单蝶形迭代版**，资源上只需 1~2 个乘法器分时复用（详见问题①与第 5 节）。

**7. 位宽** —— 【可放过】（处理得很仔细，是亮点）
逐个核对了易错点，均正确：
- **64 拆 32 / 索引拼接**：不涉及 64 拆 32；旋转因子索引 `zetas[k[6:0]]`（128 项、7 位地址，`k` 走 1..127，`k[6:0]` 正确寻址；forward 末尾 `k` 瞬时到 128 但该周期已转 S_SCALE，不再读）。
- **模乘中间结果位宽**：所有乘法都显式 `$signed({{16{x[15]}}, x})` 32 位符号扩展后相乘再赋给 `[31:0]`，无隐式截断算错（`ct_prod`/`gs_prod`/`ct_t` 等）。`mont` 里 `m = a[15:0]*QINV` 显式截断到 16 位有符号——**对应 C 的 `(int16_t)m`，逐位等价**。
- **移位量**：`>>> 26`、`>>> 16` 均为算术右移，作用于 32 位有符号中间量，正确。
- `j_hi = j + len`（9 位）最大 255，`mem` 用 `[7:0]` 索引，无越界。`grp + 2*len` 因 `2` 是无尺寸字面量在 32 位下计算，比较 `< 9'd256` 无溢出。
`barr` / `mont` 函数末端的 `a - t16*Q`、`diff[31:16]` 虽在 16 位上下文里回卷，但因真值落在合法范围且二进制补码模运算低位正确，**结果与 C 参考逐位一致**（已推导确认）。**未发现位宽 bug。**

**8. 跨时钟域** —— 【可放过】
原型阶段只有一个 `clk`，无第二时钟、无 CDC。符合预期。

---

## 第二轮 · AXI / 寄存器接口专项

**结论：项目里没有 AXI 从机 RTL，也没有寄存器接口 RTL——既不是 Vivado 生成，也不是手写，是“还没写”。**

- `include/pqchsm/accel.h` 定义了一张寄存器表（`CTRL/STATUS/MODE/PARAM/IN_LEN/OUT_LEN/ERRCODE`），并声明 STATUS.DONE 硬件写软件只读、CTRL.START 写 1 触发等语义。但这张表**目前完全由 C 实现**：`src/hal/accel_verilator.c` + `ntt_sim.cpp`（Verilator 桥）和 stub 后端在软件里模拟寄存器语义，**不是 RTL**。真正的 AXI-Lite 从机 + AXI-DMA 属于后续 Phase，尚未编写。
- `ntt_core.v` 用的是**自研握手端口**：`start`(单周期脉冲) / `inverse` / `done` + 系数写口(`wr_en/wr_addr/wr_data`) + 系数读口(`rd_addr/rd_data` 组合读)。与 accel.h 的“写系数→START→等 DONE→读系数”语义**对得上形状**，但两者之间的映射今天是靠 C 桥完成的。

因此本轮的具体检查项（STATUS 硬件写软件只读 / START 自清 / DONE 清除时机 / VERSION 常量寄存器）**在 RTL 层暂无对象**。但把清单精神落到现有握手核上，有两点必须为将来的 AXI 包装层记账：

- **DONE 是 1 周期脉冲，与 “轮询 STATUS.DONE” 契约不一致** —— 【建议改】（AXI 阶段升级为【必须修】）
  `S_DONE` 置 `done<=1` 后立即回 `S_IDLE`，`S_IDLE` 首行又 `done<=0`，故 `done` 只高 1 拍。当前 cocotb / Verilator 桥都是**逐周期**轮询，能抓到；但 accel.h 明说软件“轮询 STATUS.DONE”，真实寄存器/AXI 轮询在任意时刻采样会**漏掉 1 周期脉冲**。建议核内把 `done` 锁存成电平，直到下一次 `START` 或 `SOFT_RESET` 才清（AXI 包装层再把它映射到 sticky 的 STATUS.DONE）。这条正是清单“DONE 清除时机软硬一致”的隐患点。
- **BUSY / ERR 无硬件来源** —— 【建议改】
  STATUS.BUSY / STATUS.ERR 在核里没有对应输出端。建议核暴露 `busy = (state != S_IDLE)`；ERR 对纯 NTT 可暂缺，但包装层需明确其常 0。
- START 自清 / VERSION 常量寄存器：待 AXI 从机写出来时再审。届时建议 START 位读恒为 0（写 1 自触发不回读），VERSION 用 `localparam` 常量硬连。

---

## 第三轮 · 综合后核对 / 预期

**现状：`syn/rpt/` 为空，没有任何 utilization / timing / Fmax 报告。** `syn/README.md` 与 `ooc_synth.tcl` 都注明“本机没装 Vivado，脚本未经实机验证”。因此**没有可贴的综合数字，需综合验证**。以下给预期与必须先修项。

**latch / multi-driven（零容忍）扫描** —— 通过。
无 `always @(*)`（无组合 latch）；每个寄存器只有单一驱动源（`mem` 仅在唯一 always 块内写，`rd_data` 仅一处 `assign`）；无 multi-driven。综合后 `report_synth` 若报 latch/multi-driver 反而要怀疑约束或流程问题。

**必须先修：`syn/constraints/ooc.xdc` 已与 `ntt_core` 脱节** —— 【必须修】
XDC 现在只 `create_clock -period 6.667 -name virt_clk`（虚拟时钟，**未绑定任何端口**），且文件注释仍写“当前 rtl/mlkem 下的模块都是纯组合逻辑”。这在 `ntt_core` 已是时序电路之后**不再成立**。若用此 XDC 综合 `ntt_core`，其 `clk` 端口上没有时钟约束 → 所有时序路径不被计时 → `report_timing_summary` 给出的是**失真/空**的结果，会让人误判“时序很好”。这与 `syn/` 存在的全部意义（买板前拿到可信时序）直接冲突。
修法（不在本次改动范围，仅记账）：综合 `ntt_core` 时改为
```
create_clock -period 6.667 -name clk [get_ports clk]
set_input_delay  -clock clk 1.0 [all_inputs]
set_output_delay -clock clk 1.0 [all_outputs]
```
并保留原虚拟时钟版本仅用于纯组合子模块（butterfly/mont/barrett）的 OOC。

**NTT 资源预期**（注意：**不适用 Keccak 的 DSP=0**）：
- **DSP**：NTT 必然用 DSP48 做 16×16 有符号模乘。单蝶形版预期 **约 1~3 个 DSP48**——`zeta·b` 一个乘、`mont` 内 `m·Q` 一个乘、Barrett 内 `V·a` 一个乘；正/逆分时且 mont 被 CT 与 GS 共用，可复用到 1~2 个。**DSP≠0 是正常的，别按 Keccak 判。**
- **系数存储** `mem 256×16`：因存在多个**异步读口**（`rd_addr` + 蝶形里的 `j`、`j_hi` + S_SCALE 的 `scale_i`），综合会落成**分布式 RAM(LUTRAM)**而非单块 BRAM，量级约几百 LUT。当前规模没问题；并行度提到 4/8 蝶形时需拆 bank（见 cycle_budget 注释）。
- **zetas 128×16 ROM**：分布式 ROM，几十~上百 LUT。
- **整体量级**：数百 LUT + 数百 FF + 1~3 DSP，非常小，任何目标板（KV260 / Z7-20 / A7-100T）都放得下。

**时序 / WNS 预期**：
- 关键路径是**单周期蝶形内的串行链**：`异步读 mem → zeta·b（乘）→ mont(m·Q 乘 + 32 位减 + 取高 16)→ 加减 → 写回 mem`，**含两级串行乘法**。
- **100 MHz（10ns）：WNS 大概率为正**，符合清单预期。
- **150 MHz（6.667ns，脚本默认）：偏紧**，双串行乘法 + 模约减 + LUTRAM 读在 6.667ns 内收敛需实测；若不收敛，把蝶形**打一拍**（在 `mont` 输出后插流水寄存器，把乘法与加减分到两拍）即可，代价是每蝶形 2 拍、cycle 数翻倍但仍在预算内。
- 复核前提：**先修好 ooc.xdc**，否则上面的判断无法用报告验证。

---

## 第四轮 · 仿真对拍现状评估

现有 cocotb 对拍（`tb/cocotb/`，Icarus 默认，可切 Verilator）：

| 层 | 测试 | 对拍方式 | 覆盖 |
|----|------|---------|------|
| L0 mont | `test_ops::test_mont_reduce` | 1000 条向量三方比对 | 好 |
| L0 mont | `test_ops::test_mont_definition` | **性质断言** out·2^16≡in、范围 (-q,q) | 好（独立于向量）|
| L1 蝶形 | `test_butterfly` | CT/GS 各 1000 条三方比对 | 好 |
| L2 NTT | `test_ntt_core::test_ntt_forward/inverse` | 各 20 组三方比对 + 记录 cycle | 好 |
| L2 NTT | `test_ntt_core::test_roundtrip` | **性质断言** invntt(ntt(x))≡x·2^16 | 好（独立于向量）|

**优点**：分层导出向量（不是只有整机 KAT），既比对逐条向量又有性质断言（`test_mont_definition` / `test_roundtrip` 能防住“表和实现一起错”），并顺带采集 cycle 数作为无板阶段的性能数据。方向完全正确。

**不足（覆盖缺口）** —— 【建议改】：
- **“三方一致”实为“两方一致”**：`vectors/rtl/*.hex` 是 `export_vectors.py` 从 `ref_model` 生成的，**向量与模型同源**。cocotb 里 `RTL == 向量 == 模型` 实际只等价于 `RTL == ref_model`。对 Montgomery 有 `test_mont_definition` 独立兜底、对 Keccak 有 hashlib 交叉验证兜底，但**NTT 没有任何独立预言机**——`test_roundtrip` 只验证 invntt 能逆 ntt，一张“自洽但错误”的旋转因子表/层序也能通过往返。
- **ref_model 的 NTT 未与真实 C/liboqs 的 NTT 交叉验证**：存在“RTL 对得上模型、却对不上 HSM 里实际用的 ML-KEM”的风险（见第 7 节风险项）。
- 建议补两条独立校验：
  1. 用 **schoolbook 负循环卷积**（直接在 `mod (x^256+1, q)` 上做多项式乘）与 “ntt → 逐点乘 → invntt → 除 mont 因子” 互证，验证整条 NTT 的**语义**而非仅自洽。
  2. 把 `ref_model.ntt/invntt` 与 `src/` 实际链接的 ML-KEM（liboqs）内部 NTT 跑一次逐系数比对，钉死约定一致。
- 小缺口：`barrett.hex` 已导出但**无独立 cocotb 目标**（barrett 只在 `butterfly_gs` 内被间接覆盖）；`test_ntt_forward` 断言了 `len(pairs)==20` 而 `test_ntt_inverse` 未断言条数（轻微不一致）。

---

## 三个问题的直接回答

**① NTT 是单轮/单蝶形迭代，还是全展开？**
→ **单蝶形 / 周期的迭代版**（1 butterfly/cycle）。判据：设计里**没有任何 `for` 循环**；256 点用 `len/grp/j/k` 计数器在状态机 `S_RUN` 内每周期迭代一个蝶形，`S_SCALE` 再逐系数做最后的 Barrett/f 缩放；头注释自述“首版：1 蝶形/周期 ≈ 910 cycles @150MHz”。**绝非**把 256 点摊成 256 个乘法器的全展开（那会是灾难）。资源上只需 1~2 个乘法器分时复用。更高并行（4/8 蝶形）在注释里被明确列为“下一步、需拆 bank”，尚未实现。

**② AXI / 寄存器接口是 Vivado 生成还是手写？**
→ **都不是——目前没有 AXI / 寄存器接口 RTL**。`ntt_core` 用自研握手端口（start/inverse/done + 读写口）。accel.h 里的寄存器表（CTRL/STATUS/MODE/…）**由 C 实现**（`src/hal/accel_verilator.c` + `ntt_sim.cpp` 的 Verilator 桥，以及 stub 后端），不是 RTL。真正的 AXI-Lite 从机 + AXI-DMA 属后续 Phase，未编写。等它出来时按第 2 轮的记账项（DONE 锁存、START 自清、STATUS 只读、VERSION 常量、BUSY/ERR 来源）审。

**③ 若已综合，贴 utilization 与 timing？**
→ **尚未综合，无数字**。`syn/rpt/` 为空，本机无 Vivado、脚本未实机验证。预期见第 3 轮：资源约 **数百 LUT + 数百 FF + 1~3 DSP48**（NTT 用 DSP 属正常，**不套 Keccak 的 DSP=0**）；**100 MHz WNS 预期为正，150 MHz 偏紧、必要时把蝶形打一拍**。**先决条件：修好 `ooc.xdc` 对 `clk` 端口的约束，否则综合出的时序报告不可信。**

---

## 清单之外的额外发现

- **【建议改】ntt_core 与 butterfly.v 是两份独立实现，且核未例化模块。** `ntt_core.v` 内联重写了 `mont`/`barr` 和 CT/GS 蝶形；而 `butterfly.v` / `mont_reduce.v` 只被独立 cocotb 测试使用，**并未被 `ntt_core` 例化**。`butterfly.v` 头注释说“刻意写成实例化以便共用同一块乘法器”，但核里并没这么做。结果是同一套数学有两处实现，改一处忘另一处会漂移。建议让 `ntt_core` 直接例化 `mont_reduce`/`butterfly_*`，或在注释里显式声明“需手工保持同步”。
- **【建议改】ref_model / RTL 与实际密码路径的一致性风险。** 见第 4 轮：NTT 缺独立预言机、ref_model 未与 liboqs NTT 交叉验证。ML-KEM 的 NTT 约定（Montgomery 域、只做 7 层、往返非恒等而是 ×2^16、正变换末尾统一 Barrett、逆变换末尾乘 f=1441）非常容易与另一实现的约定错位；RTL 现在“对得上模型”不等于“对得上 HSM 里真正做多项式乘法的那套 NTT”。上板前务必钉死。
- **【可放过】常量重复定义。** `Q/QINV/BARV(V)/FINV` 在 `ntt_core.v`、`butterfly.v`、`mont_reduce.v` 各自 `localparam` 一份。低风险但建议集中到一个 `params.vh`（`` `include ``）以防日后单点修改遗漏。
- **【可放过】Verilator 桥细节。** `ntt_sim.cpp` 读回时只 `eval()` 不推进时钟（组合读，正确）；`ensure_init` 复位 5 拍、cocotb `reset` 复位 3 拍，两处拍数不一致但都足够，无害。它依赖 `done` 脉冲期间逐周期轮询——与上面 DONE 建议相呼应：一旦 done 改成电平锁存，这里更稳。
- **【好评】代码质量整体高。** `` `default_nettype none `` 用于抓隐式线网、结尾恢复 `wire`；位宽一律显式符号扩展；注释把最易踩的坑（Montgomery 的 `(int16_t)` 截断、ML-KEM 只做 7 层、往返非恒等、两个仿真器都跑是为逼出 2-state/4-state 差异）都写清楚了，可维护性好。审查中**未发现会让当前测试流程失败的功能性 bug**——核对得上参考模型，往返性质成立。

---

## 处理优先级汇总

| 档位 | 条目 |
|------|------|
| **【必须修】** | 1) `syn/constraints/ooc.xdc` 对 `ntt_core` 失效：虚拟时钟未绑定 `clk` 端口、注释仍称“纯组合”，会导致时序报告失真——综合 `ntt_core` 前必须改为 `create_clock ... [get_ports clk]` 并补 I/O delay。 |
| **【建议改】** | 1) `done` 由 1 周期脉冲改为电平锁存（AXI 阶段升为必须修）；2) 数据通路寄存器 `len/grp/j/k/inv_r/scale_i` 补复位分支；3) `ntt_core` 例化 `mont_reduce/butterfly_*` 消除双实现；4) NTT 补独立预言机 + `ref_model` 与 liboqs NTT 交叉验证；5) 核暴露 `busy` 供 STATUS.BUSY。 |
| **【可放过】** | 1) zetas 用 `initial` 初始化（Xilinx 允许，ASIC 再议）；2) Q/QINV/BARV/FINV 常量集中到 params 头；3) `mem` 多异步读口→分布式 RAM，当前规模 OK；4) `barrett.hex` 无独立测试目标、正逆 NTT 测试断言不对称。 |

*本报告仅为静态审查，未运行综合/仿真；“需综合验证”的判断以修好 XDC 后的 Vivado OOC 报告为准。*
