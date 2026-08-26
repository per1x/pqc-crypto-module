# 批 2 结果报告

分支 `feature/tee-mainline-batch2`（基于 batch1）。`main` 全程未动。

---

## 0. 一句话

**批 2 的"前置死结"不成立，而且核心那一条做完了：种子保管已上移 OP-TEE TA，
EL3 退成纯通路，普通世界连"触发换一份种子"的能力都没有了。**

A 组（删 PL 金库与槽位 ABI）与 C 组的 `src/` 改造**没有做**，原因见 §5 ——
不是受阻，是工程量与风险评估后的取舍，如实标。

---

## 1. 那条"前置死结"：查明不成立

`ARCHITECTURE-TARGET §9`、`FINAL-PLAN §4`、`STATELESS-PL-CONFLICTS §5` 三处都写着
**"OP-TEE TA 从未上板跑通（卡在 BL32 入口），在这解决之前整条主线是空中楼阁"**。

实测：**不成立**。板上（槽 6）现状：

```
optee: revision 3.8 (af141c61)
optee: initialized driver
/dev/tee0  /dev/teepriv0
tee-supplicant 开机就在跑（rootfs 自带 /usr/sbin/tee-supplicant）
```

为什么之前认为卡住：那是**打包漏 24 字节 `boot_embdata`** 那个坑还没修时的状态
（`bif` 里直接写 `tee.elf` 会丢掉追加在 `tee.bin` 尾巴上的 embdata，表现是
约 6% 成功率的偶发挂死）。那个坑修好之后就通了，但结论文档没有跟着更新。
本轮已把三处过时说法改掉（保留原文并注明当时的判断依据）。

⚠️ **"驱动起来了"不是 TA 跑通的证据。** `optee: initialized driver` 只说明内核侧
驱动起来了，`/dev/tee0` 也只是驱动创建了设备节点 —— 两者都不检查 TA 能不能加载。
所以新增 `tee/host/ta_probe.c`，判据分两半、两半都必须成立：

| | 判据 | 结果 |
|---|---|---|
| [2a] | 匿名登录（`LOGIN_PUBLIC`）被拒，**且 origin = TA** | ✅ `ACCESS_DENIED`，origin=TA |
| [2b] | `LOGIN_USER` 会话建立 | ✅ |
| [3] | `CMD_GET_INFO` 返回的版本与 host 期望一致 | ✅ `0x00010000` |

`origin = TA` 才是要害：它意味着 TA **已经被找到、加载、并执行到了自己的
open-session 入口**，是它主动拒的 —— 而不是"找不到 TA / 签名不对 / ABI 不匹配"。
顺带这也在真硅上确认了批 1 那道 PS-22 的门（TA 不接受匿名登录）。

只测"能开会话"是不够的：一个把门拆掉的 TA 也能通过。

---

## 2. 核心：种子保管上移 TA，EL3 退成纯通路

### 批 1 留下的那半个问题

批 1 的形态是"EL3 自己取熵、自己写 PL"。种子明文确实没经过普通世界 ——
但**触发权**还在普通世界：有 root 的人可以随时让板子换一份种子。

### 批 2 的三处改动（是一套，缺一不可）

| 层 | 改动 |
|---|---|
| **EL3**（`boot/atf/patch_atf_secmmio.py`） | `PQCHSM_SEED_NS_ALLOWED` → **0**；新增 `PQC_SEED_WORD`（`0x8200ff15`）：只把一个 32 位字写进 PL 的种子暂存口，自己不产生、不保存、不回读。**无条件只认安全世界，没有过渡开关** |
| **OP-TEE**（`boot/optee/patch_optee_seedpta.py`，新增） | 加一个极窄的伪 TA。TA 跑在 S-EL0 发不了 SMC，必须有 S-EL1 中转。它只有一条命令、两个整数参数、一条固定的 SMC |
| **TA**（`CMD_SEED_TO_PL`） | 自己 `TEE_GenerateRandom` 生成 `d‖z` / `ξ`，逐字送出去；本地那份**成功失败都当场 memzero** |

**为什么 `PQC_SEED_WORD` 不给过渡开关**：这条路上流的是**种子明文**（不像批 1
那条只是"触发"），放行普通世界等于把 CODE-1 原样搬回来。

**为什么 PTA 不做成通用 SMC 转发口**：那等于把 EL3 的整个 SiP 表暴露给任何 TA。
它另有两道门：调用方为空（= 普通世界经 `/dev/tee0` 直接开 PTA 会话）拒；
不是用户 TA 也拒。加上 EL3 那道，**三处各自成立、不互相依赖**。

### daemon 与自检

daemon 不再发 `SECMMIO_SEED`，改为请 TA 装种子。启动自检**两面都验**：

```
自检：普通世界触发 EL3 种子装载已被拒（errno=5）—— 这是批 2 期望的形态
自检通过：TA → PTA → EL3 → PL 这条种子路通了（world=1）
```

反面那条尤其要紧：**成功反而是故障** —— 那说明装的是批 1 的 BL31，
"触发权已经收走"这句话在这台机器上是假的。fail-closed，不启动。

### 上板结果

- 槽 6 = `BOOT_B2.BIN`（`f045a64b…`），重启后 `CSU_MULTI_BOOT` **仍是 6**，29 秒回来
- 演示九节全绿 —— 那一轮**每一次 ML-KEM KeyGen 的种子都走的这条新路**
- 重启后 TA 由 `hsm-boot.sh` 从 SD 自动复制到 `/lib/optee_armtz/`，自检自动通过

⚠️ 踩到并记下的一条：**`/lib` 是 initramfs，重启就没了**。TA 的持久副本必须放 SD。
漏掉的症状是 daemon 自检 fail-closed 不启动，而板子其余部分完全正常 ——
很容易误以为是 OP-TEE 或 BL31 的问题。备份一律存 `*.ta.bak`：OP-TEE 按**文件名**
查 TA，备份名放进去加载不到，却会出现在"TA 已就位"那行里让人以为有两个可用的 TA。

---

## 3. TA 的密钥操作在真硅上验通

`tee/host/ta_keyops.c`，两条链，判据都是拿另一侧独立算出来的东西对上：

| 链 | 结果 |
|---|---|
| ML-DSA-44：TA KeyGen → TA Sign → 普通世界独立验签 | ✅ pk 1312 / blob 2604 / σ 2420 字节，验得过 |
| **反证**：改一个字节必须验不过 | ✅ |
| ML-KEM-768：TA KeyGen → 普通世界 Encaps → TA Decaps | ✅ pk 1184 / blob 2444，两边共享密钥逐字节一致 |

私钥在普通世界**只以 PWRP blob 存在**（KEK 在 TA 内，没有出口）。这个程序从头到尾
没有一个能放明文私钥的缓冲区 —— 那是判据的一部分，不是疏忽。

⚠️ **边界如实说**：普通世界那一侧用的是**同一份 vendored 源码**，所以它验不出
"算法实现本身有共同的错"。它验的是另一件同样要紧的事 —— TA 交出来的 pk 与它内部
那把私钥确实是一对、输入输出接得上。算法本身由 ACVP 向量钉着
（`tee/tests` 的 `test_pqc` 与板上 `hsm_kem3` 都在跑）。

---

## 4. 回归与板子状态

- **主机 `ctest` 55/55 通过**（`-E rtl_sim`）。批 2 **没有动一行 RTL**，
  所以没有重跑那 33 分钟的 Icarus 全套 —— 批 1 定版那次（`rtl_sim` 通过，
  2007.74 秒）的结果仍然有效。
- 演示：远程 mTLS `--smoke` 16 个 ✅、"全部完成"。
- **golden 三份全程逐字节未变**：`0ea6e443…` / `bb3402ea…` / `8d42d1a5…`
- 槽 7 仍是已知 10/10 能启动的安全网（`bb3402ea…`）
- 板子当前：multiboot=6、演示形态、TA 就位、daemon 在跑

**怎么演示**：板子上电约 35 秒自动就绪；从够得着 eth1 的机器上
`./demo/remote/run.sh <板子IP> --smoke`。

⚠️ 板子**没有 RTC**，重启后时钟回到镜像里那个日期，会让客户端证书"尚未生效"、
TLS 报 `bad certificate` —— **先 `date -u -s` 对表，再查凭据**。

⚠️ 掉电会把 multiboot 清零、自动回 golden，那时跑的是**没有批 2 那套的旧 BL31**，
daemon 会因为自检两面都不对而 fail-closed 不启动。要回到批 2 形态：
`insmod pmsec.ko set_multiboot=6` + `hsmreboot.sh`。

---

## 5. 没做的：A 组与 C 组的 `src/` 改造 —— 如实说明

### A 组（V-01…V-06、D-01、D-02）：删 PL 金库与槽位 ABI

**没做。** 这不是受阻，是取舍。它要同时改：

- RTL 三处（`mlkem_axi` 16 槽 dk 金库、`mldsa_axi` 8 槽 sk 金库、`MODE` 的
  `DK_TO_SLOT`/`SK_FROM_SLOT`/`SLOT[9:6]`、`KEYSTAT`/`KEYPSET`）；
- 对外 **ABI**（daemon、`libsdfe`、`board/src` 的一批宏与工具）；
- 两份位流重建 + 全套 Icarus 回归；
- 而 `-lock`（D-02）与 `dk_lock`/`sk_lock`（V-04）**恰好守在与模型相反的方向**，
  删掉它们会让"私钥不出硬件"这句话在演示形态下暂时不成立，直到 C 组接完。

也就是说 A 组与 C 组**必须一起落地**，中间态是"两头都不成立"。在剩余预算内
硬推的结果会是一棵半可用的树 —— 那比诚实地标"未做"更糟。

### C 组的 `src/` 改造（PS-01/02/06、D-01、PS-16/17）

**没做，但把它卡住的那件具体事查清楚了**：

`pqc_backend_t` 的私钥是**按字节的 `sk`**，缓冲区由上层按 `pqc_alg_info` 的
`sk_len` 分配。而 TA 的私钥只以 **PWRP blob** 出 TA，blob 比原始 sk **大**
（实测 ML-DSA-44：sk 2560 → blob 2604；ML-KEM-768：blob 2444）。
所以接 TA 后端不是"再写一个 backend"那么简单，要动 slot 层的缓冲尺寸口径 ——
`FINAL-PLAN` 自己把 PS-13 标成"工程量最大、推到批 2 之后"，说的就是这一片。

**前提已经备齐**：TA 的 KeyGen/Sign/Decaps 在真硅上验通了（§3），
`wrap.h` / `pqc.h` 的边界也在批 1 单一化过了（`pwrp_format.h` 单一源 +
`cross_world` 双向对拍）。剩下的是尺寸口径这件确定的工程。

### D7（bulk SM4）

按指示**保持文档里"方案 A + 待定"**，本轮未动。

---

## 6. 真正受阻 / 需产品化前置

| 项 | 状态 | 根因与代价 |
|---|---|---|
| **安全启动 / 镜像认证** | 受阻，**不做** | 认证根要锚在 eFUSE，而烧 eFUSE 是一次性不可逆。**能改 SD 的人可以把 BL32 换成"打印密钥"的恶意 OP-TEE，板子照跑** —— 所以"密钥在 TEE"对能改镜像的人**不提供任何保密**。这一条不因批 2 做完而改变。 |
| **真 HUK** | 受阻，**不做** | ZynqMP 的真 HUK 走 PUF/CSU AES，绑死在 `CSU_STATUS_AUTH` 后面 —— 没有认证启动就永远走不到。现在是 development HUK = SHA-256(Device DNA)，而 **DNA 是可读的**。所以 KEK 的根在本板上是**公开可推导**的。 |
| **RPMB 防回滚** | 能力边界，可做 | 密钥已找回、可正常用（读 / bump），但 `src/` 未接。接它**不需要**任何不可逆动作。 |
| **物理防篡改 / 认证资质** | 产品化前置 | 不属于本仓库范围。 |

⚠️ **这一节前两条合起来意味着**：批 2 做完之后，"种子 / 私钥由安全世界保管"在
**软件与总线层面**是真的（普通世界拿不到、也触发不了），但它**挡不住能改镜像的人**。
这个区别必须一直说清楚，别让"TEE 保管"升级成"硬件保护"。

---

## 7. 提交

```
d503365  批 2 门槛通过：OP-TEE TA 在这块板上跑起来了
3c04c6b  批 2 核心：种子保管上移 OP-TEE TA，EL3 退成纯通路
370ae4a  批 2：TA 的密钥操作在真硅上端到端验通
39fd56f  改掉三处已经变成假话的「卡 BL32 入口」
```

证据文件：`board/logs/RESULT_b2_ta_threshold.txt`、`RESULT_b2_seed_custody.txt`、
`RESULT_b2_ta_keyops.txt`。
