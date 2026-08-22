[English](TESTING.md) · **中文**

# 测试

检查了什么、用什么手段检查，以及本仓库中引用的每一个数字如何复现。

- [概览](#概览)
- [原则](#原则)
- [RTL 验证](#rtl-验证)
- [实现流程断言](#实现流程断言)
- [真硅结果](#真硅结果)
- [熵](#熵)
- [主机软件测试](#主机软件测试)
- [复现](#复现)

## 概览

| 检查项 | 结果 | 位置 |
|---|---|---|
| cocotb RTL 回归 | 200 个测试，0 失败 | `./tools/rtl_sim.sh` |
| Verilator `-Wall` + Icarus lint | 70 个模块，0 警告 | `./tools/rtl_lint.sh` |
| Yosys 可综合性 | 68 个模块，全部可综合 | `./tools/rtl_synth_check.sh` |
| ML-KEM 512/768/1024 对照 NIST ACVP，真硅上 | 20 / 20 逐字节一致 | `board/logs/RESULT_seckem3.txt` |
| 板级自测 | 24 / 24 | `board/logs/RESULT_hwtest.txt` |
| 审计后板级重跑 | PASS 25 / FAIL 0 / SKIP 6 | `board/logs/RESULT_audit.txt` |
| 边界反证 | 48 个读得到的地址中 0 个密钥字 | `board/logs/RESULT_hwtest.txt` |
| AxPROT 门控，双向 | 闭合 | `board/logs/RESULT_secproof.txt`、`RESULT_secneg.txt` |
| TRNG 最小熵 | H = 0.871234 bit/sample | `tools/sp800_90b.py` |
| SP 800-90B 重启测试 | H_restart = 0.745427，通过 | `board/logs/RESULT_restart.txt` |
| Decaps 时序，合法 vs 隐式拒绝 | 中位差 0.000 % | `board/logs/RESULT_seckem3.txt` |
| 经 SDF 接口的端到端 | 通过，另加 6/6 反证（测于 RAZ/WI 之前，当时记录为 DECERR） | `board/logs/RESULT_service.txt` |
| `ctest`（主机软件） | 51 / 51 全过 | `ctest --test-dir build` |
| ASan + UBSan / TSan / `leaks` | 全过 · 0 竞争 · 0 泄漏 | `docs/USAGE.zh-CN.md` |
| libFuzzer | 138 万次执行，无崩溃 | `./tools/fuzz.sh` |
| aarch64 Linux（GCC 12） | 全过 | `./tools/aarch64_test.sh` |
| RAZ/WI 边界反证，真硅 | 6 / 6 —— 每个 `SECURE_ONLY=1` 的核在放着 `0x00010000` 的地址上读回 0；`SECURE_ONLY=0` 的对照读到真值 | `board/logs/RESULT_secneg.txt` |
| **任何用户态程序都崩不了板**，真硅 | 11 / 11 —— 九类地址各读写 2000 次（共 36,000 笔），板子活着 | `board/logs/RESULT_nocrash.txt` |
| 网络穿过 PL 重配 | eth1（PS GEM）在解绑驱动 + 装载密码位流全程不掉 | `board/logs/deadman_eth1.log` |
| 线协议畸形输入 | 32 / 32 —— 没有一条请求能让服务卡住、死掉、或被悄悄算成别的东西（现在跑在 mTLS 之上） | `service/wire_fuzz.py` |
| 远程调用（**mTLS**） | 另一台机器完成完整九节演示（KEM / 三个参数集的 ML-DSA / 四种分组模式 / SM3 / 句柄失效反证）；**别家 CA 签的客户端证书被拒**，实测于真板子 | `tools/demo_remote.sh`、`service/sdf_demo.c` |
| mTLS 层本身 | 4 / 4 —— 一条正例 + 三条否定（别家 CA / 不出示证书 / CN 不在 ACL）。**否定是主体**："能连上"只证明配置没写错 | `tools/tls_regress.sh` |
| 两条 P0 并发缺陷 | 各自在修复前**确实会红**：liboqs 随机源串扰 849/3000 次、自测闸门 85 万次不自洽 | `tests/unit/test_crypto_concurrent.c` |

> ⚠️ 那条"liboqs 随机源串扰"是**历史**：liboqs 依赖后来整个去掉了
> （改用仓内 vendored 的 mlkem-native / mldsa-native，见
> `src/crypto/pqc_native.c`），那个进程级全局随机源与它的锁一起消失。
> 用例保留 —— 它测的"并发下两条路互不串扰"这条性质仍然要成立。
| 会话 close 的 ABA | 修复前同一句柄被 4 线程 close 成功 311 次（应为 300），且旁观线程的会话被误抹 2 次 | `tests/unit/test_slot_concurrent.c` |
| 密钥库 fail-closed | 库损坏/读不了时 daemon 拒绝启动，**且原文件一字节不改** | `tools/daemon_failclosed.sh` |
| 安全状态抗掉电 | PIN 失败计数、锁定、解锁**不经任何 save 也在盘上**（模拟拔电后重载） | `tests/unit/test_keystore.c` |
| 防回滚锚点两种强度 | 文件锚点**确实能被"两个文件一起换"绕过**（把弱点钉住）；硬件单调锚点同样的攻击不成立 | `tests/unit/test_keystore.c` |
| PRODUCTION 里没有桩根密钥 | 目标文件里搜不到那段字面量，**且带空对照**（DEV 里必须搜得到） | `tools/check_profile.sh` |
| eMMC RPMB | 硬件够（4 MB、内核支持），认证密钥在使用前先密码学确证（正确密钥 3/3 通过，错误密钥 0/8） | `board/src/rpmb_probe.c`、`rpmb_tool.c`、`rpmb_verify.c` |

## 原则

有三个习惯贯穿本仓库中的每一项测试。

**独立的判据。** 一个结果不会因为它与本项目自己的模型对得上就被信任。Keccak 对照
公开的全零置换向量以及 `hashlib`/OpenSSL 检查。NTT 对照一个从不碰旋转因子表的
教科书式负循环卷积检查，并通过重建 ML-KEM 密钥生成、逐字节复现 ACVP 的 `ek`/`dk`
来检查。KMAC 对照 NIST 文档、OpenSSL 以及另一份独立的 Keccak 检查。

**结构性检查优于习惯。** 那些功能测试观察不到的性质——密钥派生根没有读回接口、
`src/` 中没有依赖秘密的分支或索引、每一个密钥材料字段都被其析构函数擦除——被写成
扫描器并接入 `ctest`。每个扫描器**先在合成样本上自测**：一个什么都找不到的扫描器，
在报告"干净"时什么也证明不了。

**阴性对照。** 断言要靠弄坏一样东西、确认测试确实失败来验证——扰动一个旋转因子、
去掉一层 NTT、翻转 Keccak 轮常数中的一个比特、在 TSan 下移除一把锁、加入一个假的
密钥读回函数、对一个故意提前返回的比较计时、探测一个从未被擦除的栈帧。

同样的纪律也管着结果该怎么读。边界反证在两个从机上各扫 256 字节，**同时**报告
48 个读得到的地址中没有出现任何密钥字，**以及**这些密钥算出的密文是正确的。
任何一半单独拿出来都证明不了什么：沉默也可能意味着密钥压根没被装载。

## RTL 验证

跨 26 个顶层的 251 个 cocotb 测试，在 Icarus Verilog 下运行（`tools/rtl_sim.sh`）：

| 组 | 测试 |
|---|---|
| ML-KEM 算子与数据通路 | `mont_reduce`、`butterfly`、`basemul`、`ntt_core`、`compress`/`decompress`（5 种位宽）、`sample`、`bitpack`，以及完整的 KeyGen/Encaps/Decaps |
| ML-DSA 算子 | `tb_mldsa_units`（8）、`mldsa_ntt_core`（5）、`tb_mldsa_keygen`（8） |
| Keccak | `keccak_f1600`（5）、`sha3_core`（9） |
| 总线 | `axi4lite_xbar`（8，另按板级 NS=7 再跑 8）、`axi4lite_firewall`（6）、`pqc_accel_axi`（16）、`key_vault`（4）、`key_vault_axi`（6）、`mlkem_axi`（13）、`mldsa_axi`（24） |
| 对称与商密 | `aes_core`（5）、`sm4_core`（6）、`sm3_core`（6）、`sym_vault_top`（5） |
| 风扇 | `fan_ctrl`（7）、`sysmon_drp`（3） |
| TRNG | `trng_health`（8）、`trng_source`（3）、`trng_cond`（4）、`trng_top`（4 + 2 不丢弃 + 1 丢弃）、`trng_axi`（6）、告警路径（4）、原始抽头（3） |

⚠️ **`mldsa_axi` 那 24 条是整条链路的端到端对拍，不是"总线层"而已。**
它接的是真 `mldsa_engine`（行为级替身已删），判据是 **ACVP 官方向量**
（KeyGen 的 pk/sk、Sign 的 σ 逐字节；Verify 的 pass 与 fail 两种判定）加
`hardware/model/mldsa_oracle.py`（用在官方向量给不出期望值的地方，主要是
**按槽签名** —— 金库里的 sk 只能来自一次真 KeyGen，而 90 条 ACVP siggen 的 sk
没有一条出现在 keygen 向量里）。同一条 σ 因此被两条独立的路钉住：自送 sk 的
那条对官方 ACVP，按槽签的那条对 oracle，且两条必须逐字节相同。

上面这一轮只覆盖 **ML-DSA-44**（不带参数就是它）。三个参数集的全量矩阵是
`tools/mldsa_grid.sh` 的**十二格**（KeyGen / Sign / Verify / **AXI** × 44 / 65 / 87，
共 134 条）：Verilator 约 15 分钟做开发内环，Icarus 做合并前的收尾关卡。
⚠️ Verilator 是二值仿真、**不传播 X**，而本项目坑表第 1 条正是"空敏感列表 →
输出 X"那一类 —— 所以**不要只跑 Verilator**。

Lint 把每一个模块都当作自己的顶层来跑，而不只是跑集成后的顶层：否则那些没有被
任何地方例化的模块——组合算子、采样器——永远不会被展开，也永远不会被检查。真正
出于设计意图的告警在 `hardware/rtl/lint_waivers.vlt` 中逐条豁免，按文件名与信号名
匹配，这样同类的新告警不会被一并吞掉。用 Icarus 再跑一遍能抓到另一类问题；两个
仿真器在位宽截断语义上的差异本身就是一次交叉检查。

## 实现流程断言

`hardware/syn/impl_bitstream.tcl` 在综合之后立即检查并在失败时中止，这样缺陷就不会
拖到三十五分钟之后才被发现，更不会拖到板子上。

| 断言 | 学到它的代价 |
|---|---|
| PS 的 `aclk` / `rlast` / `bid` / `rid` 必须被驱动 | 两次板子挂死，两次断电重启 |
| `fan` 必须落在 `PACKAGE_PIN AA11` / LVCMOS33 上 | 风扇引脚接错在运行时没有任何症状；它只是悄悄地把芯片烤热 |
| `SYSMONE4 SIM_DEVICE = ZYNQ_ULTRASCALE` | 一次跑满 35 分钟的实现在最后的 DRC 上失败 |
| WNS < 0 就不出 bitstream | 一份不收敛的 bitstream 在板上产生的失败看起来像算法 bug，而且追查代价极高 |
| 有效保持余量 ≥ 0.050 ns | 三份 bitstream 停在刀锋边上（见下文） |

保持余量底线值得单独记一笔。三份 bitstream 实测的 WHS 分别是
+0.001 / +0.010 / +0.013 ns——全为正，全部被 Vivado 签核通过，全都实质上等于零，
并且随 RTL 漂移。保持违例不能靠降低时钟频率来修。做法是把要求抬高，让布线器去
满足它：

```tcl
set_clock_uncertainty -hold 0.100 [get_clocks -of_objects [get_pins u_div/O]]
```

布线过程中 WHS 从 −0.150 → −0.027 → +0.010，买到大约 0.16 ns 的真实余量，代价是
在可用的 3.5 ns 中花掉 0.14 ns 的建立余量。有效保持余量从 0.010 ns 变成
**0.110 ns**。这条底线断言在第一次运行时就抓住了先前那三份 bitstream；没有它，
它们每一份都会悄无声息地发出去。

## 真硅结果

**ML-DSA，两种位流形态各验一遍。** 三个参数集的
KeyGen/Sign/Verify 逐字节对上 ACVP，加上片内签名金库与运行时切参数集：

| 判据 | 演示形态 `SECURE_ONLY=0` | 送检形态 `SECURE_ONLY=1` |
|---|---|---|
| 普通世界 `/dev/mem` 直连 | ACVP 自检 **32/32**（`RESULT_mldsa_demoform.txt`） | 五个槽全读回 0，直连 KAT 程序自己停下、一个字节都不写，板子不崩（`RESULT_secform_mldsa.txt`） |
| daemon 经 `/dev/secmmio` → EL3 白名单 | 三参数集端到端通 | 三参数集端到端通（同上日志 ④ 段） |
| 孤立单验（跨参数集、跨判定）| 61/61 与 ACVP 一致（`RESULT_ctbprobe_demoform.txt`） | — |

金库那三条同时钉住"私钥不出硬件"：`OUT_LEN` 恰好只到 pk 的长度，把读游标
seek 到 sk 那一段读回全 0，而按槽签出来的 σ 与自送 sk 逐字节相同。

> **送检形态下 ML-DSA 可达，前提是 BL31 的 SiP 白名单里有槽 6**
> （`0x8006_0000`）。那张表一度只到槽 5，于是"位流装好了、核也在，却什么都
> 读不出来"，而排查方向会被引向位流与 RTL。补一行、重建 BL31、重打 BOOT.BIN
> 之后才第一次可达。**加从机就要同时加白名单那一行。**

> ⚠️ 上板同时抓到一个**仿真看不见**的 Verify 缺陷：c̃ 的判定是整 512 位比较，
> 而寄存器每次只写低 ctb 字节，高位是上一次运算的残留。仿真里复位把高位清成
> 0，而板上装载之后再也不复位。已修，回归见 `mldsa_axi` 那 24 条与坑表 V8。

**算法。** ML-KEM 512/768/1024 的 KeyGen/Encaps/Decaps 对照 NIST ACVP 的
prompt/expectedResults 配对：20/20 逐字节一致。长度由 RTL 从 `pset` 字段推导，
所以软件报不出一个错的长度。AES-128/256 对照 FIPS 197 C.1/C.3，SM4 对照
GB/T 32907 A.1，SM3 对照 GB/T 32905 A.1。

**审计后复验**（`RESULT_audit.txt`），在改动了译码、擦除状态机与 TRNG 采样 FIFO
之后：

- *地址混叠*，13/13——五次正向读成功；八个镜像地址（+0x110、+0x100、+0x1100、
  +0xFF00、+0x8000、孔径之外、槽位 6、槽位 7）全部被拒。

  > **这次测量早于 RAZ/WI 改动**，所以它当时记录到的是那八个地址回 DECERR/SIGBUS。
  > 用例断言的"被拒"本身没变，变的只是观测量（现在是读回 0）。
  > **在 RAZ/WI 位流上重跑这一条还欠着** —— 板子的 SSH key 没扛过上次断电。
  > 新观测量由仿真覆盖（`test_xbar` 8/8，含 64 KB 全扫）。
- *清零*，4/4——`STATUS.WIPING` 置起，持续 112.5 µs，对照理论值 109.2 µs
  *（这次测量早于片内私钥金库；擦除现在要多盖一块 16 KB，共 16384 拍 ≈ 218 µs，待重测）*
  > **⚠️ 这两个数都过时了。** 金库是 16 槽 / 64 KB，擦除计数器 16 位，本层跑
  > **65536 拍**（@75 MHz ≈ 874 µs）；批 1 之后三个 ML-KEM 核还各有自己的擦除机。
  > 这个数字**以 `REGISTERS.zh-CN.md` 为唯一口径**。上面那些实测值保留，因为它们
  > 记录的是当时**在更早那版 bitstream 上**真的量到了什么 —— 重测待上板。
  （8192 个周期 @ 75 MHz；差值是轮询开销），之后 `OUT_LEN = IN_PTR = 0`，且相同
  输入重现出 2432 个完全一致的字节。
- *TRNG*，3/3——在无人读取期间吸收了 `BLOCKS = 17892` 而 `DROPS = 0`。修复之前
  这个计数器读出来是 65535，已经饱和。
- *非法参数*，4/4——`mode=3`、`pset=3`，以及两者同时：`PARAM_ERR` 置位，`BUSY`
  始终不抬起，先前的 `DONE`/`OUT_LEN` 被作废，之后一组合法参数能正常跑完。

算法回归在同一轮里跑了，结果没有变化，24/24——改了三个子系统，输出字节一个都
没变。

**吞吐** @ 75 MHz，每个操作 20 次：

| 参数集 | KeyGen | Encaps | Decaps |
|---|---|---|---|
| ML-KEM-512 | 1.08 ms (924/s) | 0.75 ms (1339/s) | 0.98 ms (1018/s) |
| ML-KEM-768 | 1.65 ms (605/s) | 1.12 ms (895/s) | 1.44 ms (694/s) |
| ML-KEM-1024 | 2.27 ms (440/s) | 1.57 ms (636/s) | 1.96 ms (510/s) |

这些数字包含逐字节的软件 AXI 传输，**不是**硬件核的耗时。把它们发布出来是为了那个
比值——跨参数集大致 1 : 1.5 : 2.1，与 k = 2/3/4 的工作量吻合——那才是有意义的部分。

**资源与时序**，在 `xazu3eg-sfvc784-1-i` 上完整布局布线：

| | |
|---|---|
| CLB LUT | 35,659 / 70,560 (50.54 %) |
| 寄存器 | 25,977 (18.41 %) |
| Block RAM | 15.5 / 216 (7.18 %) |
| DSP | 140 / 360 (38.89 %) |
| 外部引脚 | 1（风扇，`AA11`） |
| WNS / 有效保持余量 | +3.325 ns / +0.110 ns @ 75 MHz |

RAZ/WI 那次改动的增量：**+48 LUT、+61 寄存器，DSP 与 BRAM 不变。** 那是四个新增的
16 位饱和计数器（译码器读/写、TRNG 读/写）连同它们的寄存器读出通路 —— RAZ/WI 的
响应本身其实比 DECERR **更省**，它把一个响应码多路选择器换成了一个常量。
有效保持余量没变，仍是 +0.110 ns；建立余量从 +3.504 降到 +3.325 ns，
对 13.3 ns 的周期仍有 3.3 ns 富余。

这套流程在构建机上是确定性的：lint 清理（位宽修正与死代码删除）前后跑出来的资源
与时序结果逐比特一致，这本来就是应该的——Vivado 本来就会删死代码，而位宽修正不会
改变所实现的逻辑。

## 熵

用一份表征用 bitstream（`RAW_TAP=1`；在生产构建里这条路径根本不存在，而不是返回
零）导出了 1,048,576 个**调理前**样本。取调理前是必须的：SHA-3 海绵的输出无论进去
的熵有多少都看起来是均匀的，所以去评估 `RDATA` 只会得到一个漂亮而无意义的数字。

```
$ python3 tools/sp800_90b.py /tmp/trng_bits.bin
  MCV 0.996191 · Collision 0.871234 · Markov 0.996108 · Compression 1.000000
  t-Tuple 0.923437 · LRS 0.993807 · MultiMCW 0.999567 · Lag 0.987625
  MultiMMC 0.994768 · LZ78Y 0.993032
  min-entropy (minimum of the ten)    0.871234
```

配套统计：1 的比例 0.500064，最长游程 19（100 万比特下期望 ≈ 20），滞后 1–8 的
自相关全部低于 0.4 %，32 位字内无位置偏置，采集期间无健康告警。

> **这个工具不是 NIST 的参考实现。** 构建机没有外网，也没有 NIST
> EntropyAssessment 的副本。`tools/sp800_90b.py` 直接实现了 SP 800-90B
> （2018-01 终稿）§6.3.1–6.3.10，其可信度建立在能复现规范中每一节的算例上：
> `python3 tools/sp800_90b.py --selftest` 复现其中九个，精确到小数点后四位。
> 第十个 LZ78Y 在规范中没有算例，且它与另外四个预测器共用已被验证的尾部计算。

**这次测量在两个不同方向上作废了先前的门限**：

| 参数 | 旧取值在实测 H 下实际意味着什么 | 后果 |
|---|---|---|
| `RCT = 41` | 在约 9.4 M samples/s 下 α ≈ 2⁻³⁴·⁸ | 每 55 分钟一次误报——对一个常开的源来说太频繁 |
| `APT = 793` | 触发概率 5.5 × 10⁻⁵² | 这个测试根本不可能触发。此前每一条"无 APT 告警"的记录都毫无价值 |

按 α = 2⁻⁴⁰ 重算（在这个采样率下大约每 33 小时一次误报）：
**RCT 41 → 47，APT 793 → 672**。α 不是照抄规范里的 2⁻²⁰，那个值在 9.4 M
samples/s 下意味着每 0.11 s 一次误报——**α 必须按采样率来选**。作为对推导过程的
校验，这个公式先在 H = 0.5 下重新求值，精确重现了 RTL 里已有的 41/793，然后才被
用来往前推。

> 采集是**有间隙的**：抽头 FIFO 深 64 个字，满了就丢弃新样本，而不是对噪声源施加
> 反压，因为反压会改变正在被评估的那条流本身。这不影响最小熵估计，但重启测试的
> 数据必须单独采集（已经采了；H_restart = 0.745427，通过）。

## 主机软件测试

46 个 `ctest` 目标，4109 条断言：槽位状态机与并发、密钥库与包裹、备份/Shamir/注入、
审计链与锚定、PKCS#11、KAT 解析、常量时间计时、清零，以及各个加速器 transport
——包括"软件桩与 Verilator 仿真的 RTL 逐字节一致"这条断言。

390 条 NIST ACVP 向量在软件中做逐字节校验，其中 60 条被显式跳过并如实报告，而不是
悄悄地算作通过。

消毒器与平台运行：ASan + UBSan 全过；ThreadSanitizer 0 竞争（通过移除锁来验证，
那时报出 9 条）；macOS `leaks` 干净；libFuzzer 138 万次执行无崩溃；aarch64 Linux
在 GCC 12 下全过。

## 复现

本节中没有任何一项需要板子。

```bash
./tools/rtl_sim.sh                     # 251 个 cocotb 测试
sh tools/mldsa_grid.sh                 # ML-DSA 十二格矩阵（Verilator，约 15 分钟）
sh tools/mldsa_grid.sh icarus          # 同一套矩阵走 Icarus —— 合并前的收尾关卡
./tools/rtl_lint.sh                    # Verilator -Wall + Icarus
./tools/rtl_synth_check.sh             # Yosys 可综合性
python3 tools/sp800_90b.py --selftest  # 复现规范中的算例
python3 tools/ct_audit.py --self-test  # 常量时间扫描器，先跑对照
python3 tools/check_zeroize.py --self-test

./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

生成 bitstream，需要一台装有 Vivado 2020.1 的机器（约 35 分钟）：

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm.bit
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit  （低风扇门限 + TRNG 原始抽头；
#    这是表征用的构建，不是产品形态）
```

在板子上，每一个碰到 PL 的动作都必须走 harness——见
[USAGE.zh-CN.md](USAGE.zh-CN.md)。

## CI 门禁：哪些 job 应当设为 required（**需要在网页端开**）

`main` 目前**没有开分支保护，也没有必需检查** —— 也就是说 `.github/workflows/ci.yml`
跑不跑得过，都拦不住任何一次推送。这一节给出应当设成 required 的清单；
**设置本身只能在 GitHub 网页端做**（Settings → Branches → Add branch ruleset），
仓库里的文件改不了它，所以这里只能写清楚该怎么设。

| Job（`ci.yml` 里的 `name:`） | 覆盖什么 | 为什么必须拦 |
|---|---|---|
| `RTL — lint, simulate, synthesise` | Verilator/Icarus lint、cocotb 全量回归、Yosys 可综合性 | RTL 的错在仿真里是几秒，在板子上是一次断电 |
| `Host software — build and test` | `cmake` + `ctest`（51 项） | 两条 P0 并发缺陷的回归就在这里；它们**单线程跑一万遍也不会红** |
| `Static analysis` | 常量时间扫描、清零结构检查、SP 800-90B 自测 | 这三条测的是"看不见的性质"，没有它们功能测试全绿也说明不了什么 |
| `Service layer` | daemon/客户端构建 + `tls_regress.sh` | 远程口是 mTLS，四条用例里三条是否定用例 —— 漏配 `FAIL_IF_NO_PEER_CERT` 只有它们能发现 |

建议一并打开的两条：

- **Require a pull request before merging**，至少 1 个 review。理由不是流程洁癖：
  这个仓库里大量决定写在注释里（"别改回去"那类），而那些决定只有在 review 里
  才会被读到；直接 push 到 `main` 等于绕过唯一一次被读的机会。
- **Require branches to be up to date before merging**：RTL 与主机侧有共享的常量
  （`wire.h` 的 `PQCS_MAXPAY`、寄存器偏移），两条分支各自绿、合起来红是常见情形。

⚠️ 板上那几项（`--smoke` 九节、`wire_fuzz.py`、RPMB）**进不了 CI** —— 它们要真板子。
它们的位置是"合并前人工跑一遍"，清单见上面的表。别把它们写进 required：
一个永远拿不到 runner 的必需检查会把仓库锁死。
