[English](README.md) · **中文**

# pqc-crypto-module

基于 Xilinx Zynq UltraScale+ XCZU3EG（ALINX AXU3EGB 开发板）的后量子 + 国密密码机原型。
密钥由 ARM 侧安全世界里的 OP-TEE 可信应用（TA）管理，密码运算和真随机数在 FPGA（PL）里完成，
普通世界的 Linux 只负责转发请求和保存加密后的密钥文件。

这是研究原型，没有经过任何认证，也没有做过加固，不能用于生产。已知的问题列在
[局限](#局限)一节。

## 功能

| 类别 | 内容 |
|---|---|
| 后量子密钥封装 | ML-KEM-512 / 768 / 1024（FIPS 203），KeyGen / Encaps / Decaps 在 PL 里实现，板上与 NIST ACVP 向量逐字节一致 |
| 后量子签名 | ML-DSA-44 / 65 / 87（FIPS 204），KeyGen / Sign / Verify 在 PL 里实现，同一份位流运行时切换参数集，板上与 ACVP 向量一致 |
| 对称与国密 | AES-128 / 256、SM4、SM3，按 FIPS 197、GB/T 32907、GB/T 32905 的测试向量校验 |
| 真随机数 | PL 里 8 路环形振荡器，带 SP 800-90B 健康检测（RCT / APT）和 SHA-3 调理；实测最小熵 0.871 bit/样本 |
| 密钥管理 | 在 OP-TEE TA 里生成、包裹、解包；对外只给出 KEK 加密的 blob（AES-256-GCM） |
| 接口 | SDF 风格（GM/T 0018）C 库、PKCS#11 v3.2 模块、`hsmctl` 命令行、HTTP（JSON）网关 |

## 分工

| 位置 | 做什么 | 不做什么 |
|---|---|---|
| OP-TEE TA（安全世界，S-EL0） | 生成密钥（熵 = OP-TEE PRNG 与 PL TRNG 混合），用 KEK 包裹 / 解包；需要用某把密钥时，经 PTA → BL31 SiP 把它写进 PL | 不做分组运算和后量子运算 |
| BL31（EL3） | 提供 SiP 调用：带白名单的寄存器读写、种子与对称密钥装载、取 TRNG 字 | 不保存密钥 |
| PL（FPGA） | 全部密码运算与真随机数；对称密钥在一次会话内放在密钥寄存器里，会话结束清零；后量子私钥每次运算时由种子展开，算完由硬件清零 | 不长期保存任何密钥；密钥寄存器没有读出通路 |
| 普通世界 daemon `pqchsm_fpgad` | 会话管理、串行化对硬件的访问、经 `/dev/secmmio` 发起运算、在应用和 TA 之间转交密钥 blob | 拿不到明文密钥，也解不开 blob |
| 应用（`libsdfe` / `hsmctl` / `hsm_httpd`） | 调接口；密钥以 blob 文件的形式保存在普通文件系统里 | 不做任何密码运算 |

KEK 只存在于 TA 里，所以密钥文件拷到别处没有用，只有生成它的那块板能解开。

## 架构

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
 └─────────────────────────────────────────────────────────────────────────┘
```

每个槽占 64 KB，地址为 `0x8000_0000 + 槽号 × 0x1_0000`。

| 槽 | 模块 | 说明 |
|---|---|---|
| 0 | `trng_axi` | 真随机数 |
| 1 | `key_vault_axi` | 对称密钥寄存器，8 个槽位（0–6 给业务用，7 只装公开的 KAT 向量密钥）。装载口只接受安全事务，普通世界的写被丢弃并计入 `LOAD_REJ`；寄存器与总线之间没有读出通路，密钥经专线送给槽 2 |
| 2 | `sym_axi` | AES / SM4 / SM3 |
| 3 | `mlkem_axi` | ML-KEM 三个参数集 |
| 4 | 对照实例 | 与槽 1 同一个模块，固定 `SECURE_ONLY=1`，用来说明"读回 0"是防火墙拒绝的结果，不是核坏了 |
| 5 | `fan_ctrl_axi` | 风扇状态与手动覆盖；不属于密码边界，没有防火墙 |
| 6 | `mldsa_axi` | ML-DSA 三个参数集 |

默认位流里五个功能从机（槽 0、1、2、3、6）都是 `SECURE_ONLY=1`，普通世界直接访问一律读回 0。
daemon 对密码核的访问经 `/dev/secmmio` 转给 BL31，由 EL3 按白名单发出；白名单不包含槽 1
的装载口和后量子种子口，这两条只有 TA 经 PTA 才能用。

用 `PQC_DEV_OPEN=1` 构建得到开发位流 `zu3eg_hsm_dev.bit`，功能从机改为 `SECURE_ONLY=0`，
Linux 可以直接驱动。两份位流来自同一份 RTL，只差这一个参数。即使在开发位流里，
对称密钥装载口和种子口也仍然只接受安全事务。目前板子开机由 `hsm-boot.sh` 装入的是开发位流，
默认（`SECURE_ONLY=1`）位流用于验证边界，需要手动装入。

被拒的访问读回 0、写入被丢弃、总线照常返回 OKAY（RAZ/WI），不会产生总线错误。这样任何
用户态程序都不会因为访问错地址把板子弄死机（板上对 9 个被拒地址各读写 2000 次，共 36,000 次，板子正常），代价是
地址写错时没有报错。判断通路是否正常，读各核的 `VERSION`（都是 `0x0001_0000`）。

详细说明：[docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md)；寄存器定义：
[docs/REGISTERS.zh-CN.md](docs/REGISTERS.zh-CN.md)。

当前默认位流（含 ML-DSA）布局布线后占用 55,366 LUT（78.5 %）、199 DSP、85 BRAM，
75 MHz 下 WNS +1.615 ns。

## 不接板子能做的

```bash
git clone https://github.com/per1x/pqc-crypto-module
cd pqc-crypto-module

python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator      # 或用发行版自带的包

./tools/rtl_sim_fast.sh     # 平时用：按模块跑，Verilator，几十秒到几分钟
./tools/rtl_sim.sh          # 全套 272 条 cocotb 用例，Icarus，约 40 分钟，合入前跑
./tools/rtl_lint.sh         # Verilator -Wall + Icarus，hardware/rtl 下逐个模块
./tools/rtl_synth_check.sh  # Yosys 可综合性检查
```

`rtl_lint.sh` 目前不通过：mldsa_axi、mldsa_engine、mldsa_keygen、mldsa_sign、mldsa_verify、mlkem_axi、zu3eg_hsm_top 七个模块还有 Verilator 告警（大多是常量位宽扩展和未使用的信号），其余模块无告警。

主机软件和 PKCS#11 模块（需要 CMake ≥ 3.20、OpenSSL 3）。后量子算法用仓库里自带的
mlkem-native / mldsa-native（`third_party/pqc-native/`，OP-TEE TA 也用这一份），不依赖 liboqs。

```bash
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

位流（Vivado 2020.1，约 35 分钟，器件 `xazu3eg-sfvc784-1-i`）：

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
```

## 使用

### C 接口（libsdfe）

`service/` 下是给应用用的接口。示例程序 `sdf_demo` 不包含任何密码算法实现（它链接的
OpenSSL 只用于远程口的 TLS），所以它打印出来的正确结果只可能来自板子。

```c
#include "sdfe.h"

SDFE_HANDLE dev, ses;
uint8_t ek[1600], ct[1600], ss[32];
uint8_t blob[SDFE_SYMKEY_BLOB_MAX], pt[16], out[16];
uint32_t ek_len = sizeof ek, ct_len = sizeof ct, ss_len = sizeof ss, kh, blen = sizeof blob;

SDFE_OpenDevice(&dev);
SDFE_OpenSession(dev, &ses);

SDFE_GenerateRandom(ses, 32, ss);                    /* PL 真随机数                         */
SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768,      /* 返回公钥和句柄，私钥不出来          */
                           ek, &ek_len, &kh);
SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
                       ss, &ss_len, ct, &ct_len);
SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss, &ss_len);   /* 按句柄解封装             */

SDFE_GenerateKey(ses, /*slot*/ 3, 16, blob, &blen);  /* TA 生成 SM4 密钥写进 PL，返回 blob */
SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, out);         /* 加密一个分组，在 PL 里算            */
SDFE_LoadKey(ses, 3, blob, blen);                    /* 以后的会话用 blob 把同一把密钥装回  */
```

`SDFE_ImportKey`（明文导入）仍在接口里，但总是返回 `SDR_NOTSUPPORT`，明文密钥不经过普通世界。

构建运行：`make -C service && ./service/sdf_demo`。接口说明：[docs/API.zh-CN.md](docs/API.zh-CN.md)。

从另一台机器连板子跑完整演示（远程口 TCP 9797，双向 TLS）：

```bash
./demo/remote/run.sh                           # 会问板子地址；仓库自带演示用证书
./tools/demo_remote.sh <板子IP> --provision    # 换成自己的 PKI（只需做一次）
```

从别的电脑 ssh 上板子：板上 dropbear 是 2019.78，主机密钥有 ssh-rsa 和 ECDSA 两把（ECDSA 那把在 SD 的 `hsm/ssh/`，开机由 `hsm-boot.sh` 带上），新版 OpenSSH 不用加参数就能连。登录可以用口令 `root`/`root`，也可以用公钥：ECDSA 或 RSA 公钥（RSA 要加 `-o PubkeyAcceptedAlgorithms=+ssh-rsa`），ed25519 这版 dropbear 不认。公钥追加到 SD 上的 `hsm/ssh/authorized_keys`，开机会并进 `/home/root/.ssh/authorized_keys`，重启不丢。板子没有 RTC，要走 9797 / 8443 的 mTLS 先对表：`ssh root@<板子IP> "date -u -s @$(date -u +%s)"`（`demo/remote/full_demo.sh` 会自动做）。

### 命令行：hsmctl

源码 [service/hsmctl.c](service/hsmctl.c)，业务逻辑在 [service/hsmops.c](service/hsmops.c)，
和 HTTP 网关共用。它只读写文件、做 PKCS#7 填充，加密、签名、杂凑、随机数都交给密码机。

- 构建：`make -C service hsmctl`。板上的版本在构建机上交叉编译（[boot/d7/build-apps-d7.sh](boot/d7/build-apps-d7.sh)），放在 SD 卡的 `hsm/hsmctl`。
- 运行位置：通过本机 UNIX socket 连 `pqchsm_fpgad`，所以要在板子上运行。`hsm/HSM_STATUS` 里有 `READY=yes` 才能用。开机脚本会把它装成 `/usr/bin/hsmctl`，ssh 上去直接敲 `hsmctl` 即可；`hsm-demo` 跑整套板上演示。
- 密钥库：环境变量 `HSM_KEYSTORE` 指定的目录。直接敲 `hsmctl` 时默认是 SD 上的 `/media/sd-mmcblk1p2/keys`（与 HTTP 网关共用，重启不丢）；直接跑 `hsm/hsmctl` 二进制时默认 `./keystore`。不存在会自动创建。每把密钥一个 `<名字>.key` 文件（权限 0600）。对称密钥文件里是 TA 包裹的 blob（60 或 76 字节），后量子密钥文件里是公钥加 TA 包裹的种子 blob，都没有明文密钥。

| 命令 | 作用 | 执行位置 |
|---|---|---|
| `hsmctl info` | 各密码核版本、密钥库位置 | — |
| `hsmctl keygen <算法> <名字>` | 生成密钥，写入 `<名字>.key` | TA 生成并包裹，写进 PL |
| `hsmctl list` | 列出密钥：名字、算法、公钥长度、blob 长度、创建时间 | 本地 |
| `hsmctl show <名字>` | 查看密钥文件：公钥摘要、SM3 指纹、blob 开头几个字节 | SM3 在 PL |
| `hsmctl encrypt <名字> <明文> <密文>` | CBC + PKCS#7 加密，IV 取自 PL 真随机数；输出 = 16 字节 IV + 密文 | TA 解包并写进 PL，PL 逐分组加密 |
| `hsmctl decrypt <名字> <密文> <明文>` | 解密并检查填充 | 同上 |
| `hsmctl sign <名字> <文件> <签名>` | ML-DSA 签名，文件不超过 8 KB | TA 解开种子，PL 展开私钥并签名，算完清零 |
| `hsmctl verify <名字> <文件> <签名>` | ML-DSA 验签，只用公钥 | PL |
| `hsmctl encap <名字> <封装密文>` | ML-KEM 封装（发送方，只用公钥），打印共享密钥 | PL |
| `hsmctl decap <名字> <封装密文>` | ML-KEM 解封装（接收方），打印共享密钥 | TA 解开种子，PL 计算 |
| `hsmctl hash <文件>` | SM3，文件不超过 16 KB | PL |
| `hsmctl random <字节数>` | 真随机数，1–256 字节 | PL |
| `hsmctl backup <备份包>` | 把整个密钥库打包，每项附 SM3 | SM3 在 PL |
| `hsmctl restore <备份包>` | 先校验全部条目，都通过才写回；有一项不符就一个文件也不写 | SM3 在 PL |
| `hsmctl destroy <名字>` | 覆写后删除密钥文件 | 本地 |

| `<算法>` | 算法 | 用途 | 公钥 / blob 长度 |
|---|---|---|---|
| `sm4` | SM4 | 加解密 | — / 60 字节 |
| `aes128` / `aes256` | AES-128 / AES-256 | 加解密 | — / 60、76 字节 |
| `mldsa44` / `mldsa65` / `mldsa87` | ML-DSA | 签名 | 1312 / 1952 / 2592 字节；blob 76 字节 |
| `mlkem512` / `mlkem768` / `mlkem1024` | ML-KEM | 密钥封装 | 800 / 1184 / 1568 字节；blob 108 字节 |

一个完整流程：

```bash
export HSM_KEYSTORE=/media/sd-mmcblk1p2/keys     # 放在 SD 卡上，重启后还在

hsmctl keygen sm4      data-key
hsmctl keygen mldsa65  signer
hsmctl keygen mlkem768 kem-key
hsmctl list

hsmctl encrypt data-key contract.txt contract.enc
hsmctl decrypt data-key contract.enc contract.dec

hsmctl sign   signer contract.txt contract.sig
hsmctl verify signer contract.txt contract.sig   # 文件改动一个字节就验不过

hsmctl encap kem-key kem.ct                      # 发送方得到共享密钥
hsmctl decap kem-key kem.ct                      # 接收方得到同一个共享密钥

hsmctl backup keys.pqb
hsmctl destroy signer
hsmctl restore keys.pqb                          # 恢复后，之前的签名照样能验过
```

退出码：0 成功；1 出错（参数错误、文件读写失败、密码机拒绝，比如 blob 被改过或不是本机生成的）；
2 表示结果为"否"（验签不通过，或备份包校验不符拒绝恢复）。

常见报错：`TA 拒绝装载这把密钥` 表示密钥文件被改过，或者是别的板子生成的；`连不上密码机`
表示 daemon 没有启动，看 `hsm/HSM_STATUS` 和 `hsm/hsm-boot.log`。

daemon 在每一层调用成功返回后，往 `/media/sd-mmcblk1p2/hsm/hsm-boot.log` 写一行带标签的日志，
可以看出每一步在哪里执行：

| 标签 | 含义 |
|---|---|
| `[TEE]` | TA 里做的：生成、包裹、解包密钥 |
| `[TEE→PL]` | TA 经 PTA 和 EL3 把密钥材料写进 PL |
| `[PL]` | PL 里的运算，带耗时 |
| `[NW]` | daemon 自己做的，只涉及转交密文 blob |
| `[HTTP]` | HTTP 网关收到的请求 |

```bash
hsmctl sign signer contract.txt contract.sig; tail -6 /media/sd-mmcblk1p2/hsm/hsm-boot.log
```

限制：

- 备份包只能在同一块板上恢复。blob 是这块板的 KEK 包裹的，跨设备迁移还没有实现。
- 每次对称加解密命令都要经 TEE 把密钥写进 PL 一次（实测约 1.3 ms），之后的分组运算不再经过 TEE。
- 每次寄存器访问都要进一次 EL3，吞吐不高，AES-256-CBC 实测约 0.9 MB/s。适合密钥类业务和小数据量，不适合大批量数据加密。

### HTTP 接口：hsm_httpd

源码 [service/hsm_httpd.c](service/hsm_httpd.c)，和 `hsmctl` 共用 `hsmops.c`，行为相同。
开机时由 `hsm-boot.sh` 启动，密钥库在 `/media/sd-mmcblk1p2/keys`。

| 端口 | 协议 | 访问范围 |
|---|---|---|
| `127.0.0.1:8080` | HTTP | 只监听回环地址，只有板上的进程能连；不做认证，信任范围与能在板上运行 `hsmctl` 相同 |
| `:8443` | HTTPS，双向证书 | 客户端证书须由 `hsm/pki/hsm_ca.crt` 签发；证书文件不齐时不监听 |

启动参数：`hsm_httpd [HTTPS端口 [本机HTTP端口]]`，默认 8443 和 8080，填 0 表示不开该端口。

| 方法 | 路径 | 请求体 | 响应 |
|---|---|---|---|
| GET | `/v1/info` | — | 设备信息 |
| GET | `/v1/keys` | — | 密钥列表 |
| POST | `/v1/keys` | `{"name","alg"}`，`alg` 取值同 `hsmctl keygen` | 201，密钥信息（含公钥） |
| GET / DELETE | `/v1/keys/{name}` | — | 查看 / 删除一把密钥 |
| POST | `/v1/keys/{name}/encrypt` | `{"plaintext"}` 或 `{"plaintext_b64"}` | `ciphertext_b64`（IV + CBC 密文） |
| POST | `/v1/keys/{name}/decrypt` | `{"ciphertext_b64"}` | `plaintext_b64`，内容是 UTF-8 时另给 `plaintext` |
| POST | `/v1/keys/{name}/sign` | `{"message"}` 或 `{"message_b64"}` | `signature_b64` |
| POST | `/v1/keys/{name}/verify` | 同上，加 `"signature_b64"` | `{"valid": true/false}` |
| POST | `/v1/keys/{name}/encapsulate` | `{}` | `ciphertext_b64`、`shared_secret_hex` |
| POST | `/v1/keys/{name}/decapsulate` | `{"ciphertext_b64"}` | `shared_secret_hex` |
| POST | `/v1/sm3` | `{"data"}` 或 `{"data_b64"}` | `sm3_hex` |
| POST | `/v1/random` | `{"length"}`，不超过 1024 | `random_hex` |

状态码：密钥不存在 404，重名 409，参数错误 400，TA 拒绝装载 422。请求路径后加 `?trace=1`，
响应里会多一个 `trace` 数组，内容就是这次请求产生的分层日志。

板上没有 curl，busybox 的 `wget` 也不能发 POST，可以用 `nc`：

```bash
printf 'POST /v1/sm3 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 14\r\nConnection: close\r\n\r\n{"data":"abc"}' | nc 127.0.0.1 8080
```

### 板上演示

`sh /media/sd-mmcblk1p2/demo/run.sh` 按业务流程依次执行上面的命令（设备信息、密钥管理、
加解密、签名验签、密钥封装、杂凑与随机数、备份恢复、安全性检查），最后一部分改用 HTTP
接口把主要操作再做一遍。每条命令后面附上它产生的分层日志。

输出转成网页：`python3 tools/demo_to_html.py <演示输出.txt> <输出.html>`。一次实测结果见
[docs/demo/business_demo.html](docs/demo/business_demo.html)。

## 实测结果

以下在板子上实测，配置见 [docs/SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md)。

| 项目 | 结果 |
|---|---|
| ML-KEM 512 / 768 / 1024 对照 NIST ACVP | 20 / 20 逐字节一致 |
| ML-DSA 44 / 65 / 87 对照 ACVP（含同一位流内切换参数集） | 32 / 32 |
| 板上自检（对称、国密、边界、AxPROT、TRNG） | 24 / 24 |
| 对称密钥寄存器没有读出通路：两个实例各扫一遍 256 字节地址空间 | 密钥字出现 0 次，密文正确 |
| AxPROT 门控（在开发位流上测，便于同时对照一个 `SECURE_ONLY=0` 的核） | EL3 读 `SECURE_ONLY=1` 的核得到 `0x0001_0000`；EL1 非安全读同一地址被拒；同一个 EL1 能读 `SECURE_ONLY=0` 的核 |
| 普通世界直接写对称密钥装载口（开发位流，开机形态） | 11 笔全部丢弃，`LOAD_REJ` 0 → 11，槽位仍为空 |
| daemon 线协议模糊测试 | 41 / 41 |
| TRNG 最小熵，1,048,576 个调理前样本 | 0.871234 bit/样本，RCT 截止 47、APT 截止 672 |
| Decaps 时序，有效密文与隐式拒绝各 200 次 | 中位数差 0.000 % |
| ML-KEM-512 吞吐，75 MHz | 924 / 1339 / 1018 次/秒（KeyGen / Encaps / Decaps） |
| cocotb 回归 | 272 条用例 |

测试方法见 [docs/TESTING.zh-CN.md](docs/TESTING.zh-CN.md)，板上的原始输出在
[board/logs/](board/logs/)。

## 局限

- **ACVP 用的 KeyGen 通路仍会输出私钥。** 业务路径上私钥不离开 PL：种子由 TA 生成、包裹，
  经安全世界写进 PL 的种子口，每次运算（`MODE.CHAIN`）现展开、算完由硬件清零。但由软件
  提供 `d‖z` 的 KeyGen 还保留着，ACVP 需要按给定种子复现并核对私钥，这条通路的 `dk`
  可以从 `OUT_DATA` 读出。`CTRL.SEED_LOCK` 可以让 KeyGen 只走种子口，但它掉电就失效，
  daemon 目前也不设置它。
- **root 仍能让硬件做运算。** daemon 在普通世界里组织运算序列，经 EL3 的白名单发出。
  密钥装载已经只能由安全世界完成，普通世界读不到也写不进密钥；但它可以使用已经装好的
  密钥，而且任何能以 `LOGIN_USER` 身份打开 TA 会话的进程，都能让 TA 装载一把由它自己的
  blob 解出的密钥。按调用者身份授权还没有做。
- **KEK 的根可以被推算出来。** TA 的 HUK 目前是 SHA-256(Device DNA)，没有烧 eFUSE。
  Device DNA 通过 JTAG 等方式可以读到，同时拿到 DNA 和 blob 的人能解开 blob。真正的
  HUK 依赖认证启动，见下面"没有加密启动"一条。主机侧 `src/crypto/kdr.c` 在 DEV 构建里是
  固定的桩常量（PRODUCTION 构建不编译进来，未提供根密钥时直接失败）；设置
  `PQCHSM_KDR=device-dna` 可以改用绑定 Device DNA 的派生根，它能防克隆，但不提供保密性。
- **没有 SM2。** 有 SM4 和 SM3，没有 SM2。
- **性能不是目标。** 吞吐数字包含软件逐字节访问 AXI 的开销，列出来是为了比较各参数集之间
  的比例（约 1 : 1.5 : 2.1），不代表这套硬件的性能上限。
- **不防功耗和电磁侧信道。** 常数时间的工作只针对时序。没有侧信道测试平台就无法验证掩码，
  所以没有做。
- **没有加密启动。** BBRAM 方案受板子硬件限制（`VCC_BATT` 没有接电池）；PUF 黑密钥在 BootROM
  使用 PUF 之前就被拒绝。RSA 认证启动在不烧 eFUSE 的情况下能跑通，但它不构成信任根：
  `bh_auth_enable` 的含义就是跳过 eFUSE 里 PPK 摘要的校验。要防止镜像被替换只能烧 eFUSE，
  本项目不做任何不可逆的烧写。详见 [docs/SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md)。
- **PS 侧的 XMPU / XPPU 管不到 PL 的地址窗口。** 按 UG1085，PS 的保护单元都不覆盖
  `0x8000_0000`，所以 PL 里的防火墙是这条路径上唯一的访问控制点，地址译码也因此一一对应、
  没有镜像地址。
- **DDR 侧的 XMPU 只能防读。** XMPU 本身不拦截事务，只给事务打毒化标记，由终点拒绝
  （XAPP1320 v3.0 第 10 页）。BL31 启动时配置好它，普通世界读 OP-TEE 的安全内存会得到
  总线错误。它防不住的是：root 仍然可以通过 SiP 让硬件做运算，SiP 不检查调用者是谁。

## 目录

```
hardware/rtl/       Verilog-2001 密码核：mlkem/ mldsa/ keccak/ sym/ trng/ bus/ board/
hardware/platform/  与密码无关的 PL 逻辑（风扇控制），同一份位流，不共用信号
hardware/tb/        cocotb 测试平台、仿真顶层、只用于 lint 的厂商原语替身
hardware/model/     Python 参考模型、独立校验、向量导出
hardware/syn/       Vivado 综合与出位流脚本
service/            daemon、libsdfe、hsmctl、hsm_httpd、sdf_demo
tee/                OP-TEE TA 与普通世界客户端：密钥生成、包裹、种子和对称密钥装载
boot/               BL31 补丁（SiP）、OP-TEE 补丁、当前启动镜像的构建脚本（boot/d7/）
board/              板上程序、启动脚本、内核模块、演示脚本、原始日志
demo/               在另一台机器上跑的演示（远程 SDF、HTTP），以及管理功能演示
include/ src/ cli/  主机软件：keystore、备份、审计、PKCS#11 前端
tests/              主机软件的单元、集成、KAT、模糊测试
tools/              回归脚本、SP 800-90B 估计器、静态检查
docs/               架构、接口、安全模型、测试、寄存器定义
```

## 文档

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md) | 分层、地址映射、时钟、密钥层级、软硬件接口 |
| [API.zh-CN.md](docs/API.zh-CN.md) | SDF 风格接口和 PKCS#11 v3.2 前端 |
| [SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md) | 信任边界、威胁模型、已验证与未验证的部分 |
| [REGISTERS.zh-CN.md](docs/REGISTERS.zh-CN.md) | 各 AXI 从机的寄存器定义 |
| [TESTING.zh-CN.md](docs/TESTING.zh-CN.md) | 测试内容、方法和复现步骤 |
| [USAGE.zh-CN.md](docs/USAGE.zh-CN.md) | 构建、运行、部署和板子操作 |
| [reference/](docs/reference/) | 各阶段报告、安全策略草稿、离线部署、常数时间审计 |

英文版与中文版放在一起，文件名去掉 `.zh-CN`。`./tools/pdf/build-pdf.sh` 可以把架构、接口、
寄存器、安全、测试五份文档合成一个 PDF。

## 参与

见 [CONTRIBUTING.md](CONTRIBUTING.md)。要操作板子，先读
[不可逆操作红线](docs/SECURITY.zh-CN.md#-不可逆操作红线)。报告漏洞见 [SECURITY.md](SECURITY.md)，
上面"局限"里列出的问题已知，不需要再报。

## 许可证

[Apache-2.0](LICENSE)。选它而不是 MIT，是因为它有明确的专利授权和专利反制条款。

`third_party/pkcs11-v3.2/` 里的三个 OASIS 头文件按其原有条款原样收录。
