[English](ARCHITECTURE.md) · **中文**

# 架构

模块是怎么搭起来的，以及边界为什么落在这些地方。寄存器级细节见
[REGISTERS.zh-CN.md](REGISTERS.zh-CN.md)；边界声称保证了什么，见
[SECURITY.zh-CN.md](SECURITY.zh-CN.md)。

- [拓扑](#拓扑)
- [地址映射](#地址映射)
- [时钟与复位](#时钟与复位)
- [密码核](#密码核)
- [密钥仓与防火墙](#密钥仓与防火墙)
- [熵源](#熵源)
- [软件栈](#软件栈)
- [主机侧密钥层次](#主机侧密钥层次)
- [集成注意事项](#集成注意事项)

## 拓扑

器件是 Zynq UltraScale+ MPSoC：硬核 ARM（PS）与 FPGA 逻辑阵列（PL）同处一片
裸片，由 AXI 相连。所有密码学部分都在 PL 里。PS 跑 Linux 并下发命令，它自己
不持有任何密钥材料。

```
   ┌──────────────────────── PS · Cortex-A53 ────────────────────────┐
   │  application ──▶ libsdfe (SDF-style)  ──▶ pqchsm_fpgad          │
   │                                              │                  │
   │  ······················· normal world ·······│················  │
   │                                       /dev/secmmio  →  EL3 SiP  │
   └──────────────────────────────────────────────│──────────────────┘
                       M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                                  ▼
   ┌──────────────────────── PL · FPGA fabric ───────────────────────┐
   │              axi4lite_xbar  (full decode, one address per reg)  │
   │   ┌──────┬──────────┬──────────┬──────────┬─────────┬────────┐  │
   │   │ slot0│  slot1   │  slot2   │  slot3   │  slot4  │ slot5  │  │
   │   │ trng │key_vault │   sym    │  mlkem   │ canary  │  fan   │  │
   │   └──────┴────┬─────┴────▲─────┴──────────┴─────────┴────────┘  │
   │               └──────────┘  use_key: private wire, not the bus  │
   │      every slot sits behind axi4lite_firewall (AxPROT gate)     │
   │                                                                 │
   │   SYSMONE4 ──▶ fan_ctrl ──▶ pin AA11   (no AXI in this path)    │
   └─────────────────────────────────────────────────────────────────┘
```

有两条通路是刻意绕开总线的：

- **`use_key`** —— 密钥仓通过私有连线把密钥交给对称密码核。密钥材料在总线上
  根本不存在可读通路；见[密钥仓与防火墙](#密钥仓与防火墙)。
- **风扇控制** —— 结温经 DRP 从 PL 自带的 SYSMONE4 读出，直接驱动风扇引脚。
  Linux 不工作时散热也必须工作：上电头几秒、U-Boot 停在提示符时、内核挂死时。
  AXI 槽位 5 的接口只用于观测；把它删掉，风扇照样转。

## 地址映射

基址 `0x8000_0000`，槽位由 `addr[18:16]` 选中，每槽 64 KB。

| 地址 | 从机 | `SECURE_ONLY` | 用途 |
|---|---|---|---|
| `0x8000_0000` | `trng_axi` | 0 | 熵源。读 `0x08` 弹出一个 32 位字 |
| `0x8001_0000` | `key_vault_axi` | 0 | 密钥仓。只有写入通路；没有读出通路 |
| `0x8002_0000` | `sym_axi` | 0 | AES-128/256、SM4、SM3 |
| `0x8003_0000` | `mlkem_axi` | 0 | ML-KEM 512/768/1024，参数集由 `MODE[3:2]` 决定 |
| `0x8004_0000` | 哨兵（`key_vault_axi`） | **1** | 与槽位 1 同一个模块，只有这一个参数不同 |
| `0x8005_0000` | `fan_ctrl_axi` | 0 | 风扇温度／占空比观测 |

**译码是一一对应的：一个寄存器有且只有一个地址能够到它。** `axi4lite_xbar`
检查孔径高位、槽位号、槽内偏移的高位以及 32 位对齐；四项必须全部成立，
否则事务**就地**被拒 —— 读回 0、写被丢弃 —— 从机什么都看不到。

这份严格不是做样子。PS 侧没有任何保护单元覆盖这个窗口——UG1085 v2.5 把
`0x8000_0000` 排在 XPPU 的孔径表之外，也不在 FPD_XMPU 的通路上——所以译码
加上 PL 里的防火墙，是这条路径上*唯一*的强制层。镜像地址等于给这一道防线
开了成千上万个等价入口，每一个都返回 OKAY 且不留痕迹。`test_xbar` 逐字扫完
整个 64 KB 槽位：恰好 `0x00`–`0xFC` 这 64 个地址可达，其余 16,320 个全部
读回 0。

**四个功能从机现在全是 `SECURE_ONLY=1`，以及这一步是怎么走到的。**
Linux 跑在普通世界，它经 `/dev/mem` 发出的每一笔事务都带 `AxPROT[1] = 1`。
四个全设成 1 之后，Linux 一个密码核寄存器都读不到。

第一版**有意**没这么做：四个功能从机设成 `SECURE_ONLY=0`，让 Linux 能直接跑
KAT，另在槽位 4 上放一个**只有**这一个参数不同的哨兵实例来演示门控。那一版
证明了两件真事（算法在真硅上对；AxPROT 门控对着真实的非安全主设备确实生效），
但留了个缺口：**被证明受保护的是那个空壳哨兵，不是密码核本身。**

现在这一版把缺口补上了。四个全是 `SECURE_ONLY=1`，整套 KAT 改由**安全世界**
驱动 —— 打过补丁的 BL31 提供一个受限的安全 MMIO SiP，白名单正好覆盖这几个核的
合法寄存器偏移（`boot/atf/patch_atf_secmmio.py`），普通世界的测试程序把每一笔
核访问都经 SMC 转过去。哨兵留在槽位 4 作同参数对照。`PQC_DEV_OPEN=1` 仍然能出
原来那份开放构建（`zu3eg_hsm_dev.bit`），用于调试。

## 时钟与复位

```
pl_clk0 (150 MHz, from PS)
   └─ BUFGCE_DIV ÷2 ──▶ clk_sys (75 MHz) ──▶ all crypto cores + fan + xbar
                                          └─▶ PS maxihpm0_lpd_aclk

pl_resetn0 (async) ──▶ 2-stage synchroniser ──┐
                                              ├─ & ──▶ rst_n
fabric power-on reset (count to 255 after GSR)┘
```

- **75 MHz，不是 150。** 最慢的核 `mlkem_decaps` 在 out-of-context 下收敛于
  108.5 MHz。75 MHz 给它留了 45 % 余量，而这正是全部接起来之后时钟树与总线
  集成所吃掉的量。
- **用 `BUFGCE_DIV`，不用 MMCM。** MMCM 有锁定要等，有复位次序要摆对——多出
  来的状态就是多出来的出错可能。纯分频器从上电那一刻起就是对的。
- **另外还有一路由逻辑阵列自己产生的上电复位。** 配置完成后 FPGA 的每个触发器
  都是 0（GSR），所以那个计数器必然从 0 开始。有了它，即使运行时重配置之后
  `pl_resetn0` 没有干净释放，设计照样能起来——少一个只在真硅上才现形的依赖。

## 密码核

全程是可推断的纯 Verilog-2001：任何密码模块里都没有例化厂商原语，所以同一份
源码不作改动即可面向 Xilinx、Intel 或 Lattice。（厂商原语只出现在板级顶层，
而 `hardware/tb/lint/vendor_stubs.v` 提供了空壳，使顶层仍能通过 lint，而不是
被排除在 lint 之外。）

| 目录 | 模块数 | 内容 |
|---|---|---|
| `hardware/rtl/mlkem/` | 27 | NTT、基乘、Montgomery／Barrett 约简、采样、压缩、编码／解码，以及完整的 KeyGen / Encaps / Decaps 核 |
| `hardware/rtl/mldsa/` | 13 | ML-DSA 算子——NTT、舍入、提示、采样。已验证，但未串成完整的核 |
| `hardware/rtl/keccak/` | 2 | `keccak_f1600` 置换与 `sha3_core` 海绵（G / PRF / XOF / H 共用一个实例） |
| `hardware/rtl/sym/` | 6 | AES-128/256、SM4、SM3 |
| `hardware/rtl/trng/` | 6 | 环形振荡器源、RCT／APT 健康测试、SHA-3 后处理、可擦除 FIFO、AXI 封装 |
| `hardware/rtl/bus/` | 8 | AXI4-Lite 防火墙、密钥仓、各核的 AXI 封装 |
| `hardware/rtl/board/` | 2 | `axi4lite_xbar` 交叉开关与板级顶层 `zu3eg_hsm_top` |
| `hardware/platform/fan_ctrl/` | 4 | 风扇控制——**不是密码部分**，单独放一棵树；只共用时钟与复位 |

算法清单、参数集与验证状态列表见
[reference/security-policy.zh-CN.md](reference/security-policy.zh-CN.md)。

`sha3_core` 用一个实例服务 ML-KEM 里全部四种 Keccak 用途。Decaps 的重加密
步骤直接例化 Encaps 核——这不是走捷径，FIPS 203 §7.3 对该运算的定义就是如此。

恒定时间只在一处不容商量：Decaps 的隐式拒绝比较会比完**每一个**字节再做选择，
绝不在第一个不同处提前返回。

## 密钥仓与防火墙

不变量不是"密钥很难被读到"，而是**根本没有通路**。

密钥经总线写入 `key_vault`，只经 `use_key` 连线出去、进入对称密码核。RTL 里
不存在对密钥寄存器的总线侧读取，所以没有任何软件——包括装载这把密钥的那个
守护进程——能把它取回来。相关规则：

- **装了一半的密钥不算密钥。** 只有 `COMMIT` 之后密钥才可用；写了一部分的
  用不了。
- **擦除只要一个周期。** 整个密钥仓在一个时钟内清空，而不是逐字扫过，所以
  不存在半把密钥还在的窗口。
- **防拆是单向的。** 防拆输入能擦除，永远不能反擦除。
- **违规会留痕**，所以被拒的访问与从未发生过的访问可以区分开。

`axi4lite_firewall` 为每个从机做门控。`SECURE_ONLY=1` 时它要求
`AxPROT[1] == 0`；否则**读回 0、写被丢弃**，且不产生总线错误（RAZ/WI）。
有两个细节要紧：

- **被拒的读绝不弹出 FIFO。** 否则一次拿不到数据的非安全读仍然会消耗随机字
  ——那就成了普通世界抽干熵池的手段。改 RAZ/WI 之后这一条**更**要紧了：
  响应码不再区分放行与拒绝，"FIFO 没动"成了唯一能证明"事务没往下走"的观测点。
- **是 RAZ/WI，不是 DECERR —— 这一条以前写的正好相反。** 原文是"是 DECERR，
  不是 OKAY 加返回零：拒绝必须是可见的"。它被一个更坏的失败方式推翻了：
  被拒的**写**是 posted 的，它的 DECERR 以 SError 回来，内核只能 panic ——
  一个打错的地址，一次断电。这一下板子真挨过，而且是内核自己的 GPIO 探测
  打的，不是攻击者。

  拒绝仍然可见，只是不再经由响应码：每个核的 `VERSION` 都是非零常量，
  所以读到 `0` 本身就是那个信号；违规计数器（只有安全世界读得到）留下审计
  痕迹。真正让出去的是"打错地址的可诊断性"，换来的是**任何用户态程序都
  搞不崩这块板**。

## 熵源

```
ring-osc array ──sample/decimate──┬──▶ continuous health tests (RCT + APT)
                                  └──▶ Keccak sponge conditioning ──▶ FIFO ──▶ AXI
```

八个长度不同的环形振荡器（13/15/…/27 级），两级同步器，然后抽取。

- **后处理用的是 Keccak 海绵，不是自制白化器。** SP 800-90B §3.1.5.1.2 列出了
  经审定的后处理构造；用其中之一，就可以直接把输出熵计为
  `min(output length, input entropy)`，而自创的白化器得自己另行论证。它还复用
  了 `keccak_f1600`，不额外占面积。
- **健康测试消费的是抽取之后的流**——与后处理器消费的是同一条流。测的东西不是
  用的东西，什么也证明不了。
- **报警同时会清空熵池。** 报警 → `ready` 拉低 → FIFO 擦除 → 后处理器与海绵
  状态复位 → 重跑启动测试。标准只要求停止输出；但报警意味着源可能已经劣化了
  一段时间，所以留着池子比丢掉更危险。
- **门限来自实测熵**，不是假设。见 [TESTING.zh-CN.md](TESTING.zh-CN.md)。

## 软件栈

```
   PKCS#11 v3.2 shared library          SDF-style library + daemon
   src/p11/                             service/
 ─────────────────────────────────────────────────────────────────
   slot manager     keystore        backup / recovery    audit
   src/slot/        src/store/      src/backup/          src/audit/
 ─────────────────────────────────────────────────────────────────
   pqc_backend_t          include/pqchsm/pqc.h
 ─────────────────────────────────────────────────────────────────
   accel_transport_t      include/pqchsm/accel.h
   stub | Verilator RTL | AXI | /dev/mem + mmap
```

每一层只依赖它下面那一层的接口，`pqc_backend_t` 虚表以上的一切都只操作不透明
句柄。**`include/pqchsm/` 里声明的函数没有一个返回私钥材料**：密钥生成返回
句柄，签名接收句柄与消息，解封装接收句柄与密文。句柄编码了槽位以及该槽位的
世代计数，所以销毁一个对象会让先前为它发出的所有句柄失效，而不是悄悄把它们
重新绑到一把新密钥上。

硬件接缝先于任何 RTL 存在。`accel.h` 把寄存器契约定死——`CTRL`、`STATUS`、
`MODE`、`PARAM`、`IN_LEN`、`OUT_LEN`、`ERRCODE`——所以接口上的错误先在软件里
暴露；有四种 transport 以完全相同的方式实现它：软件桩、Verilator 仿真的 RTL、
经真实 AXI4-Lite 与 AXI4-Stream 驱动的同一批 RTL，以及板上的 `/dev/mem` +
`mmap`。桩与仿真 RTL 经由它必须给出逐字节相同的结果，这一点由
`tests/unit/test_accel.c` 断言。

`accel_shake()` 用 C 实现海绵——填充、rate、吸收、挤出——只在做置换时调用
transport。只做置换的核更小，而且 SHAKE128、SHAKE256、SHA3-256 与 SHA3-512
一律照用。

> **哪些是在真硅上跑过的。** ACVP 结果、对称算法向量、熵采集与边界证明，来自
> `board/src/` 下的独立程序，以及它们之上的 `service/`。`accel_transport_t`
> 之上的 `src/` 主机栈**没有**对着真实可编程逻辑跑过；把它接起来是另一件事。

## 主机侧密钥层次

```
KDR  (device-bound root, 32 bytes — currently a stub)
 └── KEK        wraps key material at rest; cannot leave the device
RMK  (recovery master key, randomly generated)
 └── BEK        wraps backup blobs; portable to a replacement device
      └── Shamir M-of-N shares over GF(256)
```

之所以有两把包裹密钥，是因为目的相互冲突：受设备绑定密钥保护的材料很安全，
但设备一坏就找不回来；受可携带密钥保护的材料能找回来，但其保护强度不超过
分片保管的强度。每个槽位都带一个策略位，决定它的密钥是否参与备份，所以设备
身份密钥可以被有意做成不可恢复。

一个密钥库文件由逐槽位的 blob 加上整文件 MAC 构成：

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

元数据只做认证而不加密，所以不解包就能读到某个槽位的算法、用途与策略，同时
仍然可发现篡改。写是原子的——临时文件 → `fsync` → `rename` → `fsync(dir)`
——所以崩溃之后留下的要么是旧文件，要么是新文件。

每一次状态迁移都追加一条审计记录，其 SHA3-256 哈希覆盖前一条记录的哈希。纯
链条挡不住把整个文件重写一遍的攻击者，所以链头由 ML-DSA 设备身份密钥签名，
并锚定在设备之外。

**密钥注入**在不暴露密钥的前提下完成灌装：灌装主机对设备自己的 ML-KEM 公钥
做封装，并用得到的共享秘密包裹一个*种子*。FIPS 203 与 204 的密钥完全由种子
决定，所以这样搬动的敏感字节更少，而且设备可以自己验证展开结果。

## 集成注意事项

`hardware/rtl/board/zu3eg_hsm_top.v` 在一个文件里把所有东西接起来。其中有三
件事是在真实硬件上付过代价的：

**`M_AXI_HPM0_LPD` 是 AXI4，不是 AXI4-Lite。** 当成 Lite 处理，`bid`、`rid`
与 `rlast`——都由 PL 驱动、都要回给 PS——就悬空了。PS 会永远等 `rlast`，CPU
就挂在那一条 load 指令上。综合、布局布线、时序与位流生成全都干净，板子甚至
报告"运行中"。AXI4-Lite 就是把突发长度固定为 1 的 AXI4，所以 `rlast = rvalid`，
`bid`／`rid` 回显 `awid`／`arid`。实现流程现在带了一条综合后断言来盯这件事。

**声明顺序不是风格问题。** 在 Vivado 里引用一个后面才声明的 `wire` 不会报错
——它会在那个端口上悄悄新建一根同名、无驱动的网线。Icarus 则直接拒绝同样的
代码，这正是 lint 流程要有厂商空壳的原因：把板级顶层排除在 lint 之外，就等于
关掉了唯一能自动抓到这件事的检查。

**风扇安全一律偏向多吹风。** 最小占空比是 25 % 而不是 0；≥80 °C 强制 100 %，
直到降到 74 °C 才释放；温度陈旧强制 100 %，因为温度未知时唯一安全的假设就是
它很高。第四条规则是一次上板之后加的：读数一个比特都不变，同样强制 100 %。
第一版 SYSMON 配置错得很巧妙——DRP 照样应答，寄存器里也照样是看着合理的
32.5 °C——只是 ADC 根本没在转换。"读不到"这个条件从未成立，所以陈旧规则触发
不了。真实的结温总在抖，所以完美的稳定本身就是故障信号。
