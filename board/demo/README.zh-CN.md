# 后量子密码机原型 —— 怎么连、怎么跑

一页纸。上电、插网线、跑一条命令。

---

## 这台机器是什么

一块 Zynq UltraScale+ 板子（XCZU3EG），分工是：

- **密钥管理在 TEE**：OP-TEE 里的 TA 生成后量子私钥的种子与对称密钥，用 KEK
  包成 blob 交出（KEK 不出 TA），需要时解开、经安全通路装进 FPGA；
- **运算在 FPGA**：ML-KEM 512/768/1024、ML-DSA 44/65/87、AES-128/256、SM4、SM3，
  以及一个环振噪声源做的真随机数发生器。私钥每次从种子现展开、算完即擦；
- **Linux 不做任何密码运算**，手里也没有明文密钥 —— 它只调度请求、搬运句柄和 blob。

对外是一套 SDF 风格的接口（`SDFE_*`），本机和远程调用完全一样。

---

## 一、接线

板子上有**两个网口**，请**两个都插**：

| 网口 | 接口名 | 在哪儿 | 地址 | 说明 |
|---|---|---|---|---|
| PL 侧 | `eth0` | FPGA 逻辑里 | `192.168.50.174` | **装入密码位流后这个口会消失**，这是正常的 |
| PS 侧 | `eth1` | 处理器硬核里 | `192.168.50.175` | 演示用这个。它不随 FPGA 重配而消失 |

> **为什么 `eth0` 会消失**：出厂位流里的网卡是用 FPGA 逻辑搭的。换成密码
> 位流之后，那块网卡的硬件就不存在了。`eth1` 在处理器里，与 FPGA 无关，
> 所以它是演示形态唯一可靠的入口。

串口（备用）：Micro-USB，**115200 8N1**。

---

## 二、上电

插电，等 **约 35 秒**。不需要按任何东西、不需要登录、不需要手动加载。

上电后会自动完成（`hsm/hsm-boot.sh`）：校时 → 配好 `eth1` → 装入密码位流 →
打开安全世界通道 → 把 TA 放到 OP-TEE 找得到的地方 → 起密码机服务（含 mTLS 远程端口）→
起 HTTP 网关（本机明文口 + 对外 HTTPS）。

确认就绪：

```
ssh root@192.168.50.175 cat /media/sd-mmcblk1p2/hsm/HSM_STATUS
```

期望看到：

```
CLOCK=ok
NET=ok eth1=192.168.50.175
PL=crypto
SECMMIO=ok
TA=ok
DAEMON=ok
TCP=9797 远程可用（mTLS）
HTTPS=8443 网关可用（mTLS）
HTTP=127.0.0.1:8080 本机接口可用
READY=yes
```

（`CLOCK=corrected` 表示开机时发现系统时间比 SD 上的凭据还早、已往前拨正；
不拨的话 mTLS 会因证书尚未生效而被拒。）

**`READY=yes` 之外的任何结果都不要当成就绪。** 每一行都是实测的，不是假定的——
比如 `DAEMON=ok` 的前提是服务启动时真的经安全世界读到了密码核的版本号，
读不到它会拒绝启动，而不是装作好了。

---

## 三、跑演示

```
ssh root@192.168.50.175
sh /media/sd-mmcblk1p2/demo/run.sh            # 业务流程演示（命令 + 输出 + 分层日志）
sh /media/sd-mmcblk1p2/demo/run_classic.sh    # 原来那套（sdf_demo + 反证 + 管理功能面）
```

**业务流程演示**（[business_demo.sh](business_demo.sh)，部署为 `demo/run.sh`）用命令行工具
`hsmctl` 一步一步走，共八部分：〇 设备 → 一 密钥管理（生成 SM4 / ML-DSA-65 / ML-KEM-768、
列出、查看）→ 二 SM4-CBC 加解密 → 三 ML-DSA 签名验签（含改一位被拒）→ 四 ML-KEM
封装/解封装 → 五 SM3 与随机数 → 六 备份、销毁、恢复（含篡改备份被拒）→ 七 安全性检查
（篡改密钥被 TA 拒、普通世界绕过 TEE 装不进密钥、会话后金库为空）→ 八 应用经 HTTP
接口调用（板上用 `nc` 向 `127.0.0.1:8080` 发原样 HTTP 请求，调用 `hsm_httpd` 的各条路由）。
每条命令后面跟着 daemon 日志（`hsm/hsm-boot.log`）里**这条命令**产生的分层记录：
`[TEE]` TA 里做的、`[TEE→PL]` 安全通路写进 FPGA、`[PL]` FPGA 里算的（带时延）、
`[NW]` daemon 搬密文；第八部分还有 `[HTTP]` 网关收到请求的记录。

输出可以用 `tools/demo_to_html.py` 转成一页 HTML 报告（样例：
[docs/demo/business_demo.html](../../docs/demo/business_demo.html)）。

### 原来那套

`run_classic.sh`（仓库里是 [board_demo.sh](board_demo.sh)）：先打 `HSM_STATUS`，再依次跑
`hsm/run_demo.sh`（[run_demo.sh](run_demo.sh)：① + ② 用 `sdf_demo` 走 SDF 接口，
③ 绕过接口直接扫地址，④ 往被拒地址狂写）和 `functions.sh`（⑤ 管理功能面，仓库里是
[functions_board.sh](functions_board.sh)）。

### 在板子上

```
ssh root@192.168.50.175
sh /media/sd-mmcblk1p2/demo/run.sh
```

### 从另一台机器（同一内网）

**第一步：在那台机器上构建客户端。** 客户端不做任何密码运算，所以
**不需要 liboqs、不需要 OpenSSL**，一条命令、只用系统的 `cc`：

```
sh board/demo/build_client.sh
```

> **只是想演示的话不必读下去** —— SSH 上板一条命令：
> ```
> sh /media/sd-mmcblk1p2/demo/run.sh
> ```
> 那个目录只放入口脚本（`run.sh`、`run_classic.sh`、`functions.sh`；`hsm/` 里有六十多个文件，
> 不该让人在里面找）。`run.sh` 在仓库里是 [business_demo.sh](business_demo.sh)。

它会产出两个东西：`sdf_demo`（SDF 接口）和 `p11_hw_demo`（标准 PKCS#11 接口）。

**第二步：拿凭据**（只需一次）。远程口是 mTLS，不是口令 —— 需要
`hsm_ca.crt` / `client.crt` / `client.key` 三个文件。仓库里带了一份演示用的
（`demo/remote/creds`），要自己那套就跑 `tools/demo_remote.sh <板子IP> --provision`。

**第三步：连**。

SDF 接口：

```
./sdf_demo <板子IP> <凭据目录> [端口] [设备CN]
```

不想记这些参数，仓库根下有一条一键的：

```
./demo/remote/run.sh <板子IP>
```

想把「板上命令行 + 本机 curl 经 HTTPS（mTLS）调用」连同 HTML 报告一起跑完，用
`./demo/remote/full_demo.sh <板子IP> [凭据目录]`（前一半经 ssh 跑 `demo/run.sh`，
后一半是 `demo/remote/http_demo.sh`，结果合成 `demo-out/demo.html`）。

标准 PKCS#11 接口（这一条最能说明问题——一个不认识这块板的标准应用，
拿标准 `C_*` 调用过来，密钥就在 FPGA 里生成、私钥永不离开硬件）：

```
PQCHSM_BACKEND=sdfe PQCHSM_SDFE_HOST=<板子IP> \
  PQCHSM_SDFE_PKI=~/.config/pqchsm/pki PQCHSM_SDFE_DEVICE_CN=axu3egb-hsm-01 \
    ./p11_hw_demo <PKCS#11 模块.so>
```

`sdf_demo` 是普通的用户程序，链接 `libsdfe`，**不含任何硬件细节**。
本机版和远程版是同一个程序、同一段代码，只有"打开设备"那一行不同。
`p11_hw_demo` 更彻底：它只认 OASIS 的 `pkcs11.h`，连 `libsdfe` 都不直接调 ——
后端是不是 FPGA，由环境变量决定，代码一个字不用改。

---

## 四、演示证明了什么

| # | 展示 | 判据 |
|---|---|---|
| ① | 远程调用 → FPGA 算 → 结果对 | ML-KEM 两端共享密钥一致；SM4 密文与 **GB/T 32907 A.1** 逐字节相同 |
| ② | 密钥在 TEE 里管 | 应用只拿到句柄与密文 blob；明文导入被拒；blob 重装同密文；后量子私钥由 TA 保管种子、FPGA 现展开算完即擦 |
| ③ | 绕过接口也够不着 | 演示形态：普通世界直接往对称金库装载口写密钥，一笔不生效且被计数；送检形态：五个功能核的版本号**全部读回 0**（真值 `0x00010000`） |
| ④ | 怎么打都崩不了板 | 往被拒或不存在的地址读写（10 个地点、各读写 2000 次），板子照常运行 |
| ⑤ | **标准 PKCS#11 接口** → FPGA | `p11_hw_demo`：`C_GenerateKeyPair` 私钥留片内、读 `CKA_VALUE` 被拒（`CKR_ATTRIBUTE_SENSITIVE`）、`C_Encapsulate`/`C_Decapsulate` 两端一致、`C_Encrypt`/`C_Decrypt`（`CKM_AES_GCM`）公钥加密任意数据（KEM-DEM，KEM 在 FPGA、私钥留片内，DEM 带认证） |

第 ⑤ 条是给"这东西能不能接进现有系统"这个问题的答案：不用任何私有 SDK，
一个只认 OASIS `pkcs11.h` 的应用就能用它，而私钥从生成到使用一步都不出 FPGA。

### 关于 ③：为什么"读回 0"是个强证据

密码核对普通世界是关死的（AXI 防火墙按安全属性门控）。被拒的访问
**读回 0**，而每个核真实的版本号是非零常量 `0x0001_0000`。

所以"读到 0"同时说明了两件事：**事务确实到达了那个核**（路是通的、核在那儿），
**并且值被扣下了**。作为对照，演示里还会读一个故意不设防的观测口 ——
它读到真值，证明这条路本身是通的，"全是 0"不是因为什么都没发生。

### 关于 ④：这条比听起来重要

早先的版本里，被拒的访问会在总线上报错。而 ARM64 的写是"发出去就不管"的，
错误要晚一些才以一种内核接不住的形式回来 —— 结果就是**一次写错地址 = 一次
断电重启**。触发它甚至不需要有人捣乱：内核自己的驱动探测一个不存在的外设
就打穿过一次。

现在被拒的访问静悄悄地什么也不做（读返 0、写丢弃），所以**现场随便谁乱敲
都弄不坏这台机器**。代价是打错地址不再报错，因此内部留了违规计数器，
且**只有安全世界读得到**。

---

## 五、出问题时

| 现象 | 怎么办 |
|---|---|
| `192.168.50.174` 连不上 | 正常。装入密码位流后 `eth0` 就没了，用 `.175` |
| `.175` 也连不上 | 网线是不是只插了一个口？串口 115200 进去看 `HSM_STATUS` |
| `READY=no` | 看 `/media/sd-mmcblk1p2/hsm/hsm-boot.log`，每一步失败都写了原因 |
| 想回到出厂状态 | 删掉 `/media/sd-mmcblk1p2/hsm/zu3eg_hsm_dev.bit`（开机默认装的演示形态位流）再重启，就不装密码位流了 |
| 远程连不上 | 凭据对不对？板上 `pki/` 三件套（`hsm_ca.crt` / `hsm_device.crt` / `hsm_device.key`）缺一就**不会**监听 9797 与 8443（有意为之；本机 `127.0.0.1:8080` 不受影响） |
| `uptime` 显示 load 约 1.0，但 CPU 是 100% 空闲 | **正常，不用管。** 是一个 USB 集线器的内核工作线程常驻在不可中断等待里 —— 它不占 CPU，但按 Linux 的算法会永久计入 load。开机 5 秒时 USB 枚举就已经这样了，早于密码机的任何脚本，与本项目无关。别在演示时去追它 |

**重启**用 `sh /media/sd-mmcblk1p2/hsm/hsmreboot.sh`（这块板子的 `reboot`
命令不生效，脚本里走的是内核的 sysrq 通道）。

---

## 六、还没做的，说清楚

- 远程接口是 **mTLS**（TLS 1.3 双向证书），传输加密与身份都有了，但**没有权限
  分级** —— 握手过了就等于拿到全部操作，也没有吊销。而且仓库里那份演示凭据是
  **公开的**（`demo/remote/creds`），凭它任何够得着这个口的人都能驱动这台机器。
  内网演示够用，不是可以直接对外的形态；边界写在
  [docs/SECURITY.zh-CN.md](../../docs/SECURITY.zh-CN.md#远程口)。
- 后量子私钥的**权威副本是 TA 包裹的种子 blob**：PL 里不按槽常驻私钥，每次运算
  由 TA 把种子送进 FPGA、现展开、算完由硬件无条件擦。对称密钥同理，由 TA 装进
  FPGA 金库，会话结束即擦。服务进程手里只有句柄与 blob。
- TA 的 KEK 派生自 SHA-256(Device DNA)，而 DNA 可读 —— 在这块板上，blob 对
  "能读 DNA 又拿到 blob"的人不是真正的密文。真 HUK 依赖认证启动（见下一条）。
- 没有烧写任何一次性熔丝，**所以不存在硬件信任根**。当前的边界由 FPGA 里的
  防火墙 + 安全世界的受限通道构成，可以被有物理访问权的人换掉启动镜像绕过。
