[English](REGISTERS.md) · **中文**

# 寄存器参考

> ### ⚠️ 写任何寄存器之前先看这一段
>
> **默认位流（`zu3eg_hsm.bit`）对普通世界是零可达的。** 四个功能从机都按
> `SECURE_ONLY=1` 综合，而 Linux 发出的每一笔事务 `AxPROT[1]` 恒为 1，
> 因此在总线上一律被拒。只有安全世界（EL3，经 BL31 的 SiP）驱动得了它们。
>
> **绝对不要发一笔你预期会被拒的写。** 被拒的**读**是同步的 DECERR，
> Linux 转成 `SIGBUS`，程序接得住；被拒的**写**不一样：AXI 的写是 posted 的，
> 错误过一会儿才以 **SError** 回来，它不属于任何一条指令，内核只能 panic。
> **代价是一次断电**，而这块板断电会清掉 `CSU_MULTI_BOOT`。
>
> 所以会写寄存器的程序（`hsm_hwtest`、`hsm_kem3`）必须对着**开发位流**跑，
> 不是默认那份：
>
> ```
> PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
> ```
>
> 那份构建把功能从机设成 `SECURE_ONLY=0`，产物叫 `zu3eg_hsm_dev.bit`，
> 名字不同就不会拿错。它是调试形态，不是交付形态。


软件与设计中每一个 AXI 从机之间的契约。两侧都照着本文档编写，其中每一条都由
cocotb testbench 单独验证。

所有寄存器均为 32 位且按字对齐。每个从机都位于 `axi4lite_firewall` 之后；
落在从机窗口之外的地址，以及对 `SECURE_ONLY=1` 实例的非安全访问，一律以
DECERR 应答，且无副作用。

- [槽位映射](#槽位映射)
- [`trng_axi`](#trng_axi--槽位-0)
- [`key_vault_axi`](#key_vault_axi--槽位-1)
- [`sym_axi`](#sym_axi--槽位-2)
- [`mlkem_axi`](#mlkem_axi--槽位-3)
- [`pqc_accel_axi`](#pqc_accel_axi--仿真与主机路径)
- [通用约定](#通用约定)

## 槽位映射

基址 `0x8000_0000`，槽位取自 `addr[18:16]`，每槽 64 KB。译码为何是精确的，见
[ARCHITECTURE.zh-CN.md](ARCHITECTURE.zh-CN.md#地址映射)。

| 槽位 | 地址 | 从机 | `SECURE_ONLY` |
|---|---|---|---|
| 0 | `0x8000_0000` | `trng_axi` | 0 |
| 1 | `0x8001_0000` | `key_vault_axi` | 0 |
| 2 | `0x8002_0000` | `sym_axi` | 0 |
| 3 | `0x8003_0000` | `mlkem_axi` | 0 |
| 4 | `0x8004_0000` | 哨兵（`key_vault_axi`） | **1** |
| 5 | `0x8005_0000` | `fan_ctrl_axi` | 0 |

## `trng_axi` — 槽位 0

与其他从机不同，这是一个**自由运行、始终在线**的外设：没有命令／完成的循环，
只有预热 → 就绪 → 取字，外加健康状态。RTL 见
`hardware/rtl/trng/trng_axi.v`，测试见
`hardware/tb/cocotb/test_trng_axi.py`。

| 偏移 | 名称 | 访问 | 说明 |
|---|---|---|---|
| `0x00` | `CTRL` | RW | `[0]` ENABLE（电平），`[1]` ZEROIZE（写 1 脉冲），`[2]` CLEAR_ALARM（写 1 脉冲） |
| `0x04` | `STATUS` | R | 见下 |
| `0x08` | `RDATA` | R | **每读一次弹出一个 32 位随机字** |
| `0x0C` | `HEALTH` | R | `[31:16]` 当前 APT 窗口计数，`[15:0]` 当前 RCT 连长 |
| `0x10` | `APT_INDEX` | R | 本 APT 窗口内已处理的样本数 |
| `0x14` | `STARTUP` | R | 启动健康测试已通过的样本数 |
| `0x18` | `BLOCKS` | R | 后处理器已吸收的 rate 块数 |
| `0x1C` | `WORDS` | R | 已交付给软件的字数 |
| `0x20` | `VERSION` | R | 常量 `0x0001_0000` |
| `0x24` | `PARAM0` | R | `{DECIM, NUM_RO, RATE_LANES, OUT_LANES}`，各占一字节 |
| `0x28` | `PARAM1` | R | `{APT_CUTOFF[15:0], RCT_CUTOFF[15:0]}` |
| `0x2C` | `PARAM2` | R | `{STARTUP_SAMPLES[15:0], APT_WINDOW[15:0]}` |

`STATUS`：`[0]` READY（启动测试已过、无报警、未在擦除），`[1]` DATA_VALID，
`[2]` ALARM（锁存），`[3]` RCT_ALARM，`[4]` APT_ALARM，`[5]` STARTUP_DONE，
`[6]` FIFO_WIPING，`[7]` ENABLED，`[8]` UNDERRUN（锁存）。

**`RDATA` 读即弹出。** 读一次消耗一个字。软件**不得**回读校验，不得为确认而
重读该地址，也不得在调试器里 dump 这段范围——每一次读都在消耗熵。AXI4-Lite
没有推测读，所以一次读恰好等于一次消耗。

**空读返回 0 并锁存 `UNDERRUN`。** 返回 0 本身是危险的：不查状态就读的驱动会
把 0 当成随机数用。`UNDERRUN` 的存在是为了事后抓出这种驱动 bug；它是锁存的，
只能经 `ZEROIZE` 或 `CLEAR_ALARM` 清除。

> 正确的驱动用法：在每一次读 `RDATA` **之前**读 `STATUS` 并确认 `DATA_VALID`。
> 取完一批之后再读一次 `STATUS`，确认 `UNDERRUN` 与 `ALARM` 都是清的——否则
> 整批丢弃。

**`ALARM` 是锁存电平，不是脉冲**，理由与别处的 `DONE` 相同：软件在任意时刻
轮询，单个周期会被错过。

`PARAM1` 里的门限由实测最小熵推导得出，不是假设的；见
[TESTING.zh-CN.md](TESTING.zh-CN.md)。

## `key_vault_axi` — 槽位 1

窗口 `0x00`–`0x3F`；超出一律 DECERR。RTL 见
`hardware/rtl/bus/key_vault_axi.v`，测试见
`hardware/tb/cocotb/test_key_vault.py`。

这个外设的存在，是为了把"密钥不离开密码边界"变成**电路的性质，而不是一种
纪律**。常见做法是把密钥放在内存里、靠软件不去读它，这要求每一处访问点都记得
检查权限——漏掉一处就是一个无声的窟窿。这里在密钥寄存器与 `s_axi_rdata` 之间
干脆**没有连线**。软件可以写入密钥、把它标记为锁定、擦除它，以及询问某个槽位
是否已装载。要*使用*密钥，由 PL 里的核经 `use` 端口取走，这束信号从不离开
芯片。

| 偏移 | 名称 | 访问 | 说明 |
|---|---|---|---|
| `0x00` | `VERSION` | R | 常量 `0x0001_0000` |
| `0x04` | `CTRL` | W | `[0]` ZEROIZE——写 1 触发全局擦除，自清 |
| `0x08` | `STATUS` | R | `[0]` READY，`[1]` TAMPER_LATCHED，`[2]` DENY（有操作被拒；锁存，由 ZEROIZE 清除） |
| `0x0C` | `SLOT_SEL` | RW | `[2:0]` 当前操作所用的槽位 |
| `0x10` | `KEY_IN` | W | 一个 32 位字；字索引自动前进。**读回为 0** |
| `0x14` | `SLOT_CTRL` | W | `[0]` BEGIN，`[1]` COMMIT，`[2]` LOCK，`[3]` ERASE（写 1 触发） |
| `0x18` | `SLOT_STAT` | R | `[0]` VALID，`[1]` LOCKED，`[7:4]` 迄今已写入的字数 |
| `0x1C` | `VALID_MAP` | R | 每槽位一比特 |
| `0x20` | `LOCK_MAP` | R | 每槽位一比特 |
| `0x24` | `ZERO_CNT` | R | `[7:0]` 全局擦除次数，饱和计数 |
| `0x28` | `VIOL_CNT` | R | `{read violations[31:16], write violations[15:0]}`，饱和计数 |
| `0x2C` | `VIOL_INFO` | R | `[7:0]` 首次违规地址，`[8]` 为写操作，`[9]` NS 位，`[10]` 有效 |
| `0x30` | `PARAM0` | R | `{WORDS[15:8], SLOTS[7:0]}`，供驱动自检 |

**本表中没有任何地址会返回密钥材料。** `KEY_IN` 只写，读回为 0——不是"被门控
成 0"，而是压根就没有接到密钥寄存器上。`test_key_never_readable` 用八个槽位
各装一把不同的密钥，逐字扫遍整个 256 字节地址空间，断言 64 个密钥字一个都不
出现，以此验证这一点。这个测试是咬人的：把任意一个密钥字接进读多路选择器，
它立刻就会报出泄漏的地址和值。

装载一把密钥：

```
SLOT_SEL  = n            select the slot
SLOT_CTRL = BEGIN        clear that slot's word index and VALID
KEY_IN    × 8            eight 32-bit words (256 bits)
SLOT_CTRL = COMMIT       set VALID
SLOT_CTRL = LOCK         optional; afterwards it can be neither written nor erased
```

**装了一半的密钥不算密钥**：只有 `COMMIT` 才让槽位可用。**擦除在一个周期内
完成**——整个密钥仓，而不是逐字扫过——所以不存在半把密钥残存的窗口。**防拆是
单向的**：能擦除，永远不能反擦除。

## `sym_axi` — 槽位 2

窗口 `0x00`–`0x7F`。RTL 见 `hardware/rtl/bus/sym_axi.v`。

**本表里没有 `KEY` 寄存器，这是有意的。** 常见设计是软件把密钥写进密钥寄存器
供密码核读取——这既把密钥放上了总线，又让它躺在寄存器里，于是谁能读这段地址，
谁就拿到了密钥。这里软件只写 `KEY_SLOT`，一个 3 位的槽位号；密钥本身经
`key_vault` 的 `use` 端口，走 256 根从不离开 PL 的连线过来。软件说的是"用槽位
3 里的密钥"，而不是"密钥是 0x…"。所以"读密钥"这件事**在总线上没有地址**，而
不是有一个被门控的地址。

| 偏移 | 名称 | 访问 | 说明 |
|---|---|---|---|
| `0x00` | `VERSION` | R | 常量 |
| `0x04` | `CTRL` | W | `[0]` ZEROIZE（写 1，自清；擦除全部三个核） |
| `0x08` | `STATUS` | R | `[0]` BUSY，`[1]` DONE，`[2]` KEY_READY，`[3]` KV_VALID |
| `0x0C` | `ALG` | RW | `[1:0]` 0 = AES-128，1 = AES-256，2 = SM4，3 = SM3；`[2]` DECRYPT |
| `0x10` | `KEY_SLOT` | RW | `[2:0]` 使用密钥仓的哪个槽位 |
| `0x14` | `CMD` | W | `[0]` LOAD_KEY，`[1]` BLOCK，`[2]` HASH_START，`[3]` HASH_FINAL |
| `0x18` | `HASH_IN` | W | `[7:0]` 向 SM3 送入一个字节 |
| `0x1C` | `VIOL_CNT` | R | `{read[31:16], write[15:0]}` |
| `0x20`–`0x2C` | `DIN0..3` | W | 输入分组；`DIN0` 为最高的 32 位 |
| `0x30`–`0x3C` | `DOUT0..3` | R | 输出分组 |
| `0x40`–`0x5C` | `DIGEST0..7` | R | SM3 摘要 |
| `0x70` | `PARAM0` | R | 支持算法位图 |

## `mlkem_axi` — 槽位 3

RTL 见 `hardware/rtl/bus/mlkem_axi.v`，测试见
`hardware/tb/cocotb/test_mlkem_axi.py`。

| 偏移 | 名称 | 访问 | 说明 |
|---|---|---|---|
| `0x00` | `VERSION` | R | 常量 `0x0001_0000` |
| `0x04` | `CTRL` | W | START、ZEROIZE |
| `0x08` | `STATUS` | R | `[0]` BUSY，`[1]` DONE，`[2]` HASH_OK，`[3]` TAMPER，`[4]` WIPING，`[5]` PARAM_ERR |
| `0x0C` | `MODE` | RW | `[1:0]` 操作（0 KeyGen，1 Encaps，2 Decaps），`[3:2]` 参数集（0 = 512，1 = 768，2 = 1024） |
| `0x10` | `IN_DATA` | W | 输入字节流；写指针自动前进 |
| `0x14` | `IN_PTR` | RW | 输入写指针 |
| `0x18` | `OUT_DATA` | R | 输出字节流 |
| `0x1C` | `OUT_LEN` | R | 输出长度，由硬件写入 |
| `0x20` | `OUT_RD` | RW | 输出读指针 |
| `0x24` | `VIOL_CNT` | R | 防火墙违规计数 |
| `0x28` | `PARAM0` | R | 能力字 |

**所有输入都经同一个缓冲区送入，顺序按标准的定义**，因为三个核的输入形状完全
不同，若每种形状配一组寄存器，就会得到三张互不相干的寄存器表和三份软件封送
代码：

```
KeyGen : d(32) ‖ z(32)
Encaps : m(32) ‖ ek(384k+32)
Decaps : dk(768k+96) ‖ c(32·(du·k+dv))
```

那些必须并行呈现的 256 位量（`d`、`z`、`m`）由本模块从缓冲区头部取出，所以
软件永远不必知道哪些输入是流、哪些是并行端口。

**所有长度都由 RTL 从 `pset` 算出。** 软件不上报长度，也就报不错——否则那会是
一个悄悄产生错误答案的输入。

**`mode` 与 `pset` 是 2 位，但只有 0/1/2 存在。** 值 3 不是另一种配置，它是
一个不存在的东西。写入它会置 `PARAM_ERR`，`BUSY` 永不拉高，先前的
`DONE`／`OUT_LEN` 全部作废。读仍然允许，好让软件还能轮询。

**Zeroize 是真的擦掉 BRAM。** 早先的版本只清
`in_ptr`／`out_len`／`out_rd`／`seed`。从软件看什么都读不到（`OUT_LEN = 0`），
可上一次操作的 `dk` 每一个字节都还留在那两块 8 KB BRAM 里——这等于撕掉了目录
而正文还在。回去的路条条都通着：下一次操作只覆盖它用到的那段范围，旧私钥留在
尾部；位流回读或扫描链可以把它取出来；或者干脆把 `in_ptr` 推到旧区域再发起一次
操作。所以这里有一台真正的擦除机——由防拆或 zeroize 的**上升沿**触发，两块
BRAM 并行地逐地址写零，8192 个周期，全程 `WIPING = 1` 且拒绝输出读。

**什么进得了缓冲区，什么进不了。** ML-KEM 的 `dk` 确实会返回给软件——它是协议
的私钥，由模块包裹后存到外部——所以私钥字节真的能从 `OUT_DATA` 读出来。这是
接口定义，不是泄漏。边界在于：**任何中间值**（ŝ、ê、Â、r̂、m′，或重加密得到
的 c′）都不会进入缓冲区；它们只存在于核内部的 BRAM 里。这里 `DEBUG_BANK` 硬接
为 0，多项式存储的读端口根本没有引出来，而一根防拆线就能把三个核与两个缓冲区
一起放倒。软件拿到的恰好是算法定义说它该拿到的那些字节。（`dk` 究竟*该不该*
返回，见 [SECURITY.zh-CN.md](SECURITY.zh-CN.md)。）

## `pqc_accel_axi` — 仿真与主机路径

这是 `include/pqchsm/accel.h` 背后的从机，由主机软件栈驱动，而不是由板上程序
驱动。控制面走 AXI4-Lite，数据面走 AXI4-Stream。译码用 `[4:2]`，所以 `0x20`
及以上的地址会回绕成别名。

| 偏移 | 名称 | 访问 | 说明 |
|---|---|---|---|
| `0x00` | `CTRL` | W | `[0]` START，`[1]` SOFT_RESET |
| `0x04` | `STATUS` | R | `[0]` DONE，`[1]` BUSY，`[2]` ERR |
| `0x08` | `MODE` | RW | 操作码 |
| `0x0C` | `PARAM` | RW | 参数集 |
| `0x10` | `IN_LEN` | RW | 输入长度，单位字节 |
| `0x14` | `OUT_LEN` | R | 输出长度，由硬件写入 |
| `0x18` | `ERRCODE` | R | 失败详情，由硬件写入 |
| `0x1C` | `VERSION` | R | 常量 `0x0001_0000` |

| 编码 | 操作 | `IN_LEN` | `OUT_LEN` |
|---|---|---|---|
| 7 | NTT，正变换 | 512 | 512 |
| 8 | NTT，逆变换 | 512 | 512 |
| 9 | Keccak-f[1600] | 200 | 200 |
| 10 | SHAKE / SHA3，完整海绵 | 0…512 | 1…512 |

编码 10 是唯一使用 `PARAM` 的，里面打包了三个字段：`[7:0]` 域后缀（`0x1F` 为
SHAKE，`0x06` 为 SHA3），`[15:8]` 以字节计的 rate（168/136/72，8 的倍数），
`[31:16]` 请求的输出长度。把输出长度放在这里而不是另加一个寄存器，是为了保持
寄存器表不变。

编码 9 与 10 共用一个 `keccak_f1600`：模式 10 驱动 `sha3_core` 里的海绵，
模式 9 则经一个只在海绵空闲时才有效的直通端口借用底下的置换。同一时刻只有一条
命令在跑，所以不存在争用——而再放一个置换核大约要花 3500 个 LUT。模式 10 在
报告 `DONE` 之前会清掉海绵，因为挤出没有自然的终点（SHAKE 的输出长度是任意
的），否则海绵永远回不到空闲，也就永远不再把置换借出去。

其他任何编码，或与编码不匹配的长度，都会以 `ERR` 和 `ERRCODE = 3`（"模式未
实现"）结束。报告失败是有意的：悄悄退回到另一种实现，会让一个只实现了一部分
的加速器看起来是完整的。

**数据面。** 位宽 32，缓冲区按字寻址，小端以匹配软件。每个输入包都从缓冲区
偏移 0 开始写；带 `TLAST` 的那一拍被接受之后，写指针回到 0，所以软件不需要
"复位写指针"这样的寄存器。`BUSY` 置位期间 `TREADY` 为低。完成后，`OUT_LEN`
换算成字之后可读；拉高 `TREADY` 即可把结果抽走，最后一拍带 `TLAST`，这会复位
读指针并撤下 `TVALID`——一个结果恰好只能取走一次。

```
stream input packet (TLAST on the last beat)
write MODE, IN_LEN
write CTRL = START
poll STATUS until DONE
if ERR:  read ERRCODE
else:    read OUT_LEN, then drain the output stream
```

## 通用约定

**START 自清。** 写 1 启动一条命令，硬件在同一周期清掉该位，所以 `CTRL` 回读
永远是 0。没有这一条，软件对 `CTRL` 的任何读-改-写都会把命令再触发一遍。

**DONE 是锁存电平，不是脉冲。** 它在完成时置位，一直保持到下一次 START。软件
在任意时刻轮询，单周期脉冲会被错过。清除绑定在 START 上，而不是绑在对
`STATUS` 的读上，这样读寄存器就绝不会毁掉它所报告的状态。

**ERR 与 DONE 同时出现**，在同一周期，详情在 `ERRCODE` 里。没有另外一条需要
轮询的失败通路。

**BUSY 反映数据通路**，不是一个锁存位：从 START 起一直高到完成。

**状态、长度与错误寄存器由硬件写入，对软件只读。** 写入被忽略，并以 `OKAY`
而非 `SLVERR` 应答：在 AXI4-Lite 上，对只读寄存器的写是无害的，而错误应答会
把不了解这套约定的主设备卡死在那里。

**被拒的访问没有副作用。** 特别是被拒的读绝不弹出 TRNG 的 FIFO——否则一次拿
不到数据的非安全读仍然能抽干熵池。

**是 DECERR，绝不是 OKAY 加返回零。** 悄悄返回零会让普通世界以为自己读到了
东西。
