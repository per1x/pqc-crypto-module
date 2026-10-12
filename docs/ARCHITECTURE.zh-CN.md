[English](ARCHITECTURE.md) · **中文**

# 架构

模块的组成，以及各条边界落在哪里。寄存器级细节见
[REGISTERS.zh-CN.md](REGISTERS.zh-CN.md)；边界声称保证什么，见
[SECURITY.zh-CN.md](SECURITY.zh-CN.md)。

- [拓扑](#拓扑)
- [地址映射](#地址映射)
- [时钟与复位](#时钟与复位)
- [密码核](#密码核)
- [对称密钥寄存器与防火墙](#对称密钥寄存器与防火墙)
- [熵源](#熵源)
- [密钥管理](#密钥管理)
- [软件栈](#软件栈)
- [密钥层次](#密钥层次)
- [集成注意事项](#集成注意事项)

## 拓扑

器件是 Zynq UltraScale+ MPSoC：硬核 ARM（PS）与 FPGA 逻辑阵列（PL）在同一片裸片上，
由 AXI 相连。密钥由 Cortex-A53 安全世界里的 OP-TEE 可信应用（TA）管理。PL 做全部
密码运算和真随机数，不长期保存任何密钥。普通世界的 Linux 运行 daemon，由它串行化
对硬件的访问，并转交它自己打不开的密钥 blob。

```
 ┌──────────────────────────── PS · Cortex-A53 ────────────────────────────┐
 │ application ─▶ libsdfe / hsmctl / hsm_httpd ─▶ pqchsm_fpgad (daemon)    │
 │                                   op sequences │          │ key blobs   │
 │ ···························· normal world ·····│··········│·············│
 │                                     /dev/secmmio     OP-TEE TA (S-EL0)  │
 │                                                │     gen / wrap / unwrap│
 │                                                │     PTA (S-EL1)        │
 │                                                ▼          ▼             │
 │              BL31 SiP (EL3): whitelisted MMIO · seed / sym-key loading  │
 └────────────────────────────────────────┬────────────────────────────────┘
                 M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                          ▼
 ┌──────────────────────────── PL · FPGA fabric ───────────────────────────┐
 │                axi4lite_xbar  (full decode, one address per reg)        │
 │ ┌───────┬──────────┬─────────┬──────────┬─────────┬─────────┬─────────┐ │
 │ │ slot0 │  slot1   │  slot2  │  slot3   │  slot4  │  slot5  │  slot6  │ │
 │ │ trng  │ sym key  │   sym   │  mlkem   │ canary  │   fan   │  mldsa  │ │
 │ │       │ registers│AES/SM4/ │ 512/768/ │ same as │ observe │44/65/87 │ │
 │ │       │          │   SM3   │   1024   │ slot 1  │  only   │         │ │
 │ └───────┴────┬─────┴────▲────┴──────────┴─────────┴─────────┴─────────┘ │
 │              └──────────┘  key goes to the cipher over a direct wire    │
 │  slots 0-4 and 6 sit behind axi4lite_firewall (AxPROT gate);            │
 │  the fan port (slot 5) is outside the boundary, no firewall             │
 │                                                                         │
 │  SYSMONE4 ──▶ fan_ctrl ──▶ pin AA11   (no AXI in this path)             │
 └─────────────────────────────────────────────────────────────────────────┘
```

进入 PL 的有两条路，带的东西不同。普通运算（对密码核的寄存器序列）由 daemon 经
`/dev/secmmio` 交给 BL31 的 SiP，在 EL3 按白名单发出访问。密钥材料由 TA 经伪 TA
（PTA，S-EL1）走另外几个 SiP：`0x8200ff16` 装对称密钥，`0x8200ff15` 写后量子种子字，
`0x8200ff17` 取 TRNG 字。白名单把槽 1 的装载口和种子口排除在外，所以只有 TA 用得了
它们。SiP 清单见 [boot/README.md](../boot/README.md)。

远程调用时，`libsdfe` 到 `pqchsm_fpgad` 这一跳走 mTLS（TLS 1.3，双向证书，TCP 9797）。
本机那一跳是 0600 的 UNIX socket，不套 TLS（理由见 [SECURITY.zh-CN.md](SECURITY.zh-CN.md)
的「远程口」一节）。两条路的帧格式相同。

有两条通路有意绕开总线：

- `use_key`：对称密钥寄存器经私有连线把密钥交给对称密码核。总线上没有读取密钥材料的
  通路。见[对称密钥寄存器与防火墙](#对称密钥寄存器与防火墙)。
- 风扇控制：结温经 DRP 从 PL 自带的 SYSMONE4 读出，直接驱动风扇引脚。Linux 不工作时
  散热也要工作（上电头几秒、U-Boot 停在提示符、内核挂死）。AXI 槽 5 的接口只做观测和
  覆盖，去掉它风扇照样转。

## 地址映射

基址 `0x8000_0000`，槽位由 `addr[18:16]` 选中，每槽 64 KB。

| 地址 | 从机 | `SECURE_ONLY` | 用途 |
|---|---|---|---|
| `0x8000_0000` | `trng_axi` | 随位流 | 熵源。读 `0x08` 弹出一个 32 位字 |
| `0x8001_0000` | `key_vault_axi` | 随位流 | 对称密钥寄存器：8 个槽位，每个 256 bit（0–6 给应用，7 只装公开的 KAT 向量密钥）。装载口（`SLOT_SEL`、`KEY_IN`、`SLOT_CTRL`）无论 `SECURE_ONLY` 取值都只接受安全事务；被拒的写丢弃并计入 `LOAD_REJ`（`0x34`）。没有读出通路 |
| `0x8002_0000` | `sym_axi` | 随位流 | AES-128/256、SM4、SM3 |
| `0x8003_0000` | `mlkem_axi` | 随位流 | ML-KEM 512/768/1024，参数集由 `MODE[3:2]` 决定；种子暂存口（仅安全世界）和每次运算用的展开区 |
| `0x8004_0000` | 金丝雀（`key_vault_axi`） | 1 | 与槽 1 同一个模块，`SECURE_ONLY` 固定为 1，`use` 口不接任何东西 |
| `0x8005_0000` | `fan_ctrl_axi` | 无防火墙 | 风扇温度、占空比与覆盖。在密码边界之外 |
| `0x8006_0000` | `mldsa_axi` | 随位流 | ML-DSA-44/65/87，参数集由 `MODE[3:2]` 决定；种子暂存口（仅安全世界）和每次运算用的展开区 |

「随位流」指默认位流 `zu3eg_hsm.bit` 里 `SECURE_ONLY=1`，开发位流 `zu3eg_hsm_dev.bit`
里 `SECURE_ONLY=0`（见下）。PL 里没有签名密钥仓：ML-DSA 私钥每次运算时由种子展开，
算完由硬件清零。

译码是一一对应的：一个寄存器有且只有一个地址能够到它。`axi4lite_xbar` 检查孔径高位、
槽位号、槽内偏移的高位和 32 位对齐，任何一项不成立，事务就地被拒（读回 0，写被丢弃），
从机什么都看不到。

PS 侧没有任何保护单元覆盖这个窗口。UG1085 v2.5 把 `0x8000_0000` 排在 XPPU 孔径表之外，
也不在 FPD_XMPU 的通路上，所以 PL 里的译码加防火墙是这条路径上唯一的强制手段。镜像
地址会给这道防线增加等价的入口，每个都返回 OKAY 且不留痕迹。`test_xbar` 逐字扫描一个
完整的 64 KB 槽位：恰好 `0x00` 到 `0xFC` 这 64 个地址可达，其余 16,320 个读回 0。

**默认位流与开发位流。** 两者由同一份 RTL 生成。默认位流里五个功能从机（槽 0、1、2、3、6）
是 `SECURE_ONLY=1`，Linux 发出的事务 `AxPROT[1]=1`，在每个寄存器上都读回 0；daemon 的
所有操作经 `/dev/secmmio` 和 BL31 白名单（`boot/atf/patch_atf_secmmio.py`）完成。
`PQC_DEV_OPEN=1` 生成 `zu3eg_hsm_dev.bit`，功能从机为 `SECURE_ONLY=0`，Linux 可经
`/dev/mem` 直接驱动。开发位流里对称密钥装载口和种子口仍然只认安全事务。板子现在启动的
是开发位流：`board/scripts/hsm-boot.sh` 装载 `zu3eg_hsm_dev.bit`，默认位流用于核对边界，
运行时用 `pl_deadman.sh` 手动载入。两种位流下 daemon 走的是同一条路径（`/dev/secmmio`）。

槽 4 的金丝雀是对照组。功能从机开着的时候演示不了门控，而关着的金丝雀既能演示门控，
又不会掩盖核本身是否正常：`VERSION` 是非零常量，所以读到 0 就是防火墙拒绝了访问。

新增的从机也要加进 BL31 白名单。没有那一行，核在默认位流里够不到：位流装好了，核也在，
读出来却什么都没有。

## 时钟与复位

```
pl_clk0 (150 MHz, from PS)
   └─ BUFGCE_DIV ÷2 ──▶ clk_sys (75 MHz) ──▶ all crypto cores + fan + xbar
                                          └─▶ PS maxihpm0_lpd_aclk

pl_resetn0 (async) ──▶ 2-stage synchroniser ──┐
                                              ├─ & ──▶ rst_n
fabric power-on reset (count to 255 after GSR)┘
```

- 用 75 MHz 而不是 150。最慢的核 `mlkem_decaps` 在 out-of-context 下收敛于 108.5 MHz，
  75 MHz 给它留出 45 % 余量，供时钟树和总线集成使用。
- 用 `BUFGCE_DIV` 而不是 MMCM。MMCM 要等锁定，复位次序也要摆对；分频器从上电起就是对的。
- 另有一路由逻辑阵列自己产生的上电复位。配置完成后每个触发器都是 0（GSR），所以计数器
  从 0 开始。即使运行时重配置之后 `pl_resetn0` 没有干净释放，设计也能起来。

## 密码核

全部是可推断的纯 Verilog-2001：任何密码模块里都没有例化厂商原语，同一份源码可以面向
Xilinx、Intel 或 Lattice。厂商原语只出现在板级顶层，`hardware/tb/lint/vendor_stubs.v`
提供空壳，使顶层仍能通过 lint。

| 目录 | 模块数 | 内容 |
|---|---|---|
| `hardware/rtl/mlkem/` | 27 | NTT、基乘、Montgomery／Barrett 约简、采样、压缩、编码／解码，以及完整的 KeyGen / Encaps / Decaps 核 |
| `hardware/rtl/mldsa/` | 29 | NTT、舍入、提示、采样，以及共用 `mldsa_engine` 的完整 KeyGen / Sign / Verify 核（44/65/87 运行时选择） |
| `hardware/rtl/keccak/` | 2 | `keccak_f1600` 置换与 `sha3_core` 海绵 |
| `hardware/rtl/sym/` | 6 | AES-128/256、SM4、SM3 |
| `hardware/rtl/trng/` | 6 | 环形振荡器源、RCT／APT 健康测试、SHA-3 调理、可擦除 FIFO、AXI 封装 |
| `hardware/rtl/bus/` | 9 | AXI4-Lite 防火墙、对称密钥寄存器（`key_vault`、`key_vault_axi`、`sym_vault_top`）、各核的 AXI 封装 |
| `hardware/rtl/common/` | 2 | 双口 RAM、同步 FIFO |
| `hardware/rtl/board/` | 2 | `axi4lite_xbar` 交叉开关与板级顶层 `zu3eg_hsm_top` |
| `hardware/platform/fan_ctrl/` | 4 | 风扇控制。不属于密码部分，单独放一棵树，只共用时钟与复位 |

算法清单、参数集与验证状态见
[reference/security-policy.zh-CN.md](reference/security-policy.zh-CN.md)。

ML-KEM 用一个 `sha3_core` 承担全部四种 Keccak 用途（G、PRF、XOF、H）。`mldsa_axi` 自带
一个 `sha3_core`，所以对其中一个从机做 zeroize 或 tamper 不会动到另一个的海绵状态。
Decaps 的重加密步骤直接例化 Encaps 核，这是 FIPS 203 §7.3 对该运算的定义。

PL 里恒定时间只有一处要紧：Decaps 的隐式拒绝比较会比完每一个字节再选择，不提前返回。

## 对称密钥寄存器与防火墙

`key_vault_axi` 是一组 8 个对称密钥寄存器，只在一次会话内保存密钥。它不是存储：PL 里
没有任何东西能跨断电保留，密钥的长期副本是 PL 之外的、KEK 包裹的 blob。

- 装载。TA 经 PTA 和 SiP `0x8200ff16` 写入密钥，序列是 `SLOT_SEL`、`BEGIN`、8 个 `KEY_IN`
  字、`COMMIT`。三个装载寄存器只接受 `AxPROT[1]==0`，与 `SECURE_ONLY` 无关。普通世界对它们
  的写被丢弃，响应为 OKAY，并计入 `LOAD_REJ`。`COMMIT` 之后槽位才可用，写了一半的用不了。
- 使用。密钥只经 `use_key` 连线进入对称密码核。RTL 里没有对密钥寄存器的总线侧读取：
  `KEY_IN` 读回 0，其余寄存器都是元数据。`test_key_vault` 逐字扫描 256 字节地址空间，
  断言写入的密钥字一个都不出现。daemon 也取不回密钥。
- 清零。`CTRL.ZEROIZE`（`sym_axi` 的 `CTRL.ZEROIZE` 也经 `ext_zeroize` 直通到这里）在一个
  时钟内清空全部寄存器，不存在半擦除的状态。会话结束时 daemon 会发出它。`tamper` 输入同样
  会清空，而且是单向的，不会反擦除。
- 违规会计数（`VIOL_CNT`、`VIOL_INFO`、`LOAD_REJ`），安全世界可读，所以被拒的访问和从未发生的
  访问可以区分。
- `tamper` 输入由 SYSMONE4 的过温输出驱动：它连续高电平 2^16 个周期（约 0.87 ms）且 ADC 已
  出过结果才算数。没有接别的信号，过温这条路径也没有在板上实际触发过。

`axi4lite_firewall` 给每个从机做门控。`SECURE_ONLY=1` 时它要求 `AxPROT[1] == 0`，否则读回 0、
写被丢弃，不产生总线错误（RAZ/WI）。有两点要注意：

- 被拒的读绝不弹出 FIFO。否则一次拿不到数据的非安全读仍会消耗随机字，等于让普通世界抽干
  熵池。改成 RAZ/WI 之后响应码不再区分放行和拒绝，「FIFO 没动」就是事务没往下走的证据。
- 拒绝用 RAZ/WI，不用 DECERR。被拒的写是 posted 的，DECERR 会以 SError 回来，内核只能
  panic，代价是一次断电；板子曾因为内核自己的 GPIO 探测挨过一次。拒绝仍然可见：每个核的
  `VERSION` 是非零常量，读到 0 就是信号，违规计数器留有记录。放弃的是打错地址时的可诊断性，
  换来的是任何用户态程序都搞不垮这块板。

防火墙挡的是普通世界自己发起的事务，挡不住 root 经通用的 EL3 寄存器 SiP 转一手。所以 BL31
白名单把槽 1 整个和种子口排除在外，两部分合起来才成立。

## 熵源

```
ring-osc array ──sample/decimate──┬──▶ continuous health tests (RCT + APT)
                                  └──▶ Keccak sponge conditioning ──▶ FIFO ──▶ AXI
```

八个长度不同的环形振荡器（13/15/.../27 级），两级同步器，然后抽取。

- 调理用 Keccak 海绵。SP 800-90B §3.1.5.1.2 列出了经审定的调理构造，所以输出熵可以直接
  按 `min(output length, input entropy)` 计。它复用 `keccak_f1600`，不额外占面积。
- 健康测试处理的是抽取之后的流，与调理器用的是同一条流。
- 报警同时清空熵池：`ready` 拉低，FIFO 擦除，调理器与海绵复位，重新跑启动测试。标准只要求
  停止输出，但报警意味着源可能已经劣化了一段时间，所以直接丢弃池子。
- 门限来自实测熵。见 [TESTING.zh-CN.md](TESTING.zh-CN.md)。

TA 取密钥熵时，把 OP-TEE PRNG 与这个 TRNG 的字（SiP `0x8200ff17`）混合；TRNG 健康检测报警时
SiP 返回错误。

## 密钥管理

密钥归 TA 所有。下表是各类密钥所在位置的权威汇总。

| 密钥 | 静态存放 | 使用时 |
|---|---|---|
| AES / SM4 | KEK 包裹的 blob（`PWRP`，AES-256-GCM），放在普通世界的 keystore 文件里 | TA 解开 blob，经 PTA 和 SiP `0x8200ff16` 装进一个密钥寄存器槽。槽位保持到会话结束，然后清零 |
| ML-KEM `dk`、ML-DSA `sk` | keystore 文件里存 TA 包裹的种子 blob 和公钥，任何地方都不存私钥 | 每次运算时 TA 解开种子，写入 PL 暂存口（SiP `0x8200ff15`）。PL 展开私钥（`MODE[4]=CHAIN`）、运算，并自己清零展开区。软件在下一次运算前等 `KEYSTAT[0]`（`EXP_WIPING`）落下 |
| KEK | 不存储，在 TA 里由 HUK 派生 | 不出 TA |

daemon `pqchsm_fpgad` 只在应用和 TA 之间传 blob，看不到明文密钥，也解不开 blob。KEK 绑定在
这块板上，所以 keystore 文件拷到别的板子上解不开。

由软件提供 `d‖z`（ML-KEM）或 `ξ`（ML-DSA）的 KeyGen 路径是为 ACVP 保留的，仍会经 `OUT_DATA`
返回私钥。`CTRL.SEED_LOCK` 可以把 KeyGen 限制为只走种子口，它是只有断电才能清除的闩锁，daemon
不设置它。

这套做法没有提供的东西：HUK 是 `SHA-256(Device DNA)`，没有烧 eFUSE（见[密钥层次](#密钥层次)），
所以 KEK 与芯片绑定，但对能读到 DNA 的人并不保密。普通世界内核仍可以让 TA 装载它手里 blob
对应的密钥，也可以使用已经装载的密钥。

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

每一层只依赖它下面一层的接口，`pqc_backend_t` 虚表以上的一切都只操作不透明句柄。
`include/pqchsm/` 里声明的函数没有一个返回私钥材料：密钥生成返回句柄，签名接收句柄和消息，
解封装接收句柄和密文。句柄编码了槽位及其世代计数，销毁一个对象会让为它发出的所有句柄失效。

`accel.h` 固定了寄存器契约（`CTRL`、`STATUS`、`MODE`、`PARAM`、`IN_LEN`、`OUT_LEN`、`ERRCODE`）。
有四种 transport 实现它：软件桩、Verilator 仿真的 RTL、经 AXI4-Lite 和 AXI4-Stream 驱动的同一批
RTL，以及板上的 `/dev/mem` + `mmap`。桩与仿真 RTL 必须给出逐字节相同的结果，由
`tests/unit/test_accel.c` 断言。`accel_shake()` 用 C 实现海绵，只在做置换时调用 transport。

板上实际运行的路径是 `service/`：应用、`libsdfe`、`pqchsm_fpgad`、`/dev/secmmio`、BL31、PL，
密钥由旁边的 TA 处理。ACVP 结果、对称算法向量、熵采集和边界测量来自 `board/src/` 下的独立程序
以及 `service/`。`accel_transport_t` 之上的 `src/` 栈（PKCS#11、槽位管理、keystore）没有在真实
PL 上跑过。

## 密钥层次

板上：

```
Device DNA  （只读，不保密）
 └── HUK = SHA-256(Device DNA)        ZynqMP 的 OP-TEE 分支，不烧 eFUSE
      └── TA 唯一密钥                 PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY，extra data "pqchsm-kdr-v1"
           └── KEK = KMAC256(K = TA 唯一密钥, X = salt, S = "pqc-hsm/storage-kek")
                └── PWRP blob = AES-256-GCM(KEK, 密钥材料)
```

salt 是 daemon 持有的、每台设备固定的常量，启动时发给 TA（`CMD_KEK_SET`）。它不是秘密；改动它
会让已有的 blob 打不开。KEK 缓存在 TA 里，没有出口。blob 的格式是
`"PWRP" ‖ 版本 ‖ 算法 ‖ 长度 ‖ nonce ‖ 密文 ‖ tag`（`tee/ta/ta_wrap.h`）。

Device DNA 可以通过 JTAG 读到，拿到 DNA 和 blob 的人可以推出 KEK。这层绑定能防止把 keystore
搬到别的板子上，防不了拿到本板 DNA 的攻击者。HUK 要做到保密，需要基于 eFUSE 的认证启动，
而本项目不烧 eFUSE。

`src/` 里的主机栈（PKCS#11 前端、keystore、备份）有自己的层次：

```
KDR  （32 字节；DEV 构建：固定桩常量；PQCHSM_KDR=device-dna：绑定 Device DNA）
 └── KEK        包裹 keystore 文件里的密钥材料
RMK  （恢复主密钥，随机生成）
 └── BEK        包裹备份 blob；可移植到替换设备
      └── GF(256) 上的 Shamir M-of-N 分片
```

两把包裹密钥目的不同：受设备绑定密钥保护的材料很安全，但设备坏了就找不回；受可携带密钥保护
的材料能找回，但保护强度不超过分片的保管强度。每个槽位带一个策略位，决定它的密钥是否参与备份。
keystore 文件由逐槽位的 blob 和整文件 MAC 构成：

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

元数据只认证不加密。写入是原子的（临时文件、`fsync`、`rename`、`fsync(dir)`）。每次状态迁移追加
一条审计记录，其 SHA3-256 哈希覆盖前一条记录的哈希，链头用 ML-DSA 设备身份密钥签名。

`hsmctl backup` 是另一套更简单的机制：把 TA 包裹的 blob 连同每项的 SM3 摘要打成一个包，只能在
同一块板上恢复。

密钥注入在不暴露密钥的前提下完成灌装：灌装主机对设备的 ML-KEM 公钥做封装，并用得到的共享秘密
包裹一个种子。FIPS 203 与 204 的密钥完全由种子决定，所以这样搬动的敏感字节更少，设备也可以自己
验证展开结果。

## 集成注意事项

`hardware/rtl/board/zu3eg_hsm_top.v` 在一个文件里把所有东西接起来。下面三件事是在真实硬件上发现的：

`M_AXI_HPM0_LPD` 是 AXI4，不是 AXI4-Lite。当成 Lite 处理，`bid`、`rid` 和 `rlast`（都由 PL 驱动、
都要回给 PS）就悬空了，PS 会一直等 `rlast`，CPU 挂在那条 load 指令上。综合、布局布线、时序和位流
生成都干净，板子还会报告「运行中」。AXI4-Lite 就是突发长度固定为 1 的 AXI4，所以 `rlast = rvalid`，
`bid`／`rid` 回显 `awid`／`arid`。实现流程里有一条综合后断言检查这件事。

声明顺序有影响。在 Vivado 里引用后面才声明的 `wire` 不会报错，而是在该端口上新建一根同名、无驱动的
网线。Icarus 会拒绝同样的代码，这是 lint 流程要有厂商空壳的原因：把板级顶层排除在 lint 之外，就
没有任何自动检查能抓到这件事。

风扇安全一律偏向多吹风。最小占空比 25 %；80 °C 及以上强制 100 %，降到 74 °C 才释放；温度陈旧强制
100 %；读数一个比特都不变也强制 100 %。最后一条规则是一次上板之后加的：第一版 SYSMON 配置让 ADC
没有在转换，DRP 却照样应答，寄存器里是看着合理的 32.5 °C。真实结温总在抖动，所以完美的稳定本身
就是故障信号。
