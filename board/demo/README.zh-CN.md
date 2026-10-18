# 板上演示

AXU3EGB 板子（XCZU3EG）上的演示：怎么连、怎么跑、每段演示看什么。

## 系统分工

- OP-TEE 里的 TA 管密钥：生成对称密钥和后量子私钥的种子，用 KEK 包成 blob；需要时解开，经 PTA、BL31 的 SiP 写进 PL。KEK 不出 TA。
- PL（FPGA）做全部密码运算：ML-KEM 512/768/1024、ML-DSA 44/65/87、AES-128/256、SM4、SM3，以及环振真随机数发生器。PL 不长期保存密钥。对称密钥只在会话期间放在对称密钥寄存器（`key_vault_axi`）里，会话结束即擦。后量子私钥每次运算由 TA 把种子写进 PL，在 PL 内展开，运算后由硬件擦除。
- Linux 里的 `pqchsm_fpgad` 只调度请求、搬运句柄和 blob，看不到明文密钥，自己也不做密码运算。

对外是 SDF 风格接口（`SDFE_*`），本机和远程调用方式相同。另有 HTTP 网关 `hsm_httpd` 和 PKCS#11 模块。

## 接线

板子有两个网口，两个都插：

| 网口 | 接口名 | 位置 | 地址 | 说明 |
|---|---|---|---|---|
| PL 侧 | `eth0` | PL 里的 AXI Ethernet | `192.168.50.174` | 装入密码位流后消失 |
| PS 侧 | `eth1` | 处理器里的 GEM | DHCP；没有 DHCP 时 `192.168.50.175`；另常挂 `169.254.50.175` | 演示用这个，不随 PL 重配消失 |

串口（备用）：Micro-USB，115200 8N1。

## 上电

插电，约 35 秒后就绪（等 DHCP 最多再多十来秒），不需要登录或手工加载。`hsm/hsm-boot.sh` 依次做这些事：校时、配 `eth1`（DHCP，兜底与链路本地地址见上表）、给 dropbear 加 ECDSA 主机密钥、装 `/usr/bin/hsmctl` 和 `/usr/bin/hsm-demo`、装入密码位流、加载 `secmmio.ko`、把 TA 复制到 OP-TEE 找得到的位置、起 `pqchsm_fpgad`（mTLS 端口 9797）、起 `hsm_httpd`（本机 `127.0.0.1:8080` 明文，对外 `:8443` mTLS）。

查看状态：

```
ssh root@<板子IP> cat /media/sd-mmcblk1p2/hsm/HSM_STATUS
```

正常输出：

```
CLOCK=ok
NET=ok eth1=192.168.50.185 dhcp
NET_LINKLOCAL=169.254.50.175
SSH=ok rsa+ecdsa
PL=crypto
SECMMIO=ok
TA=ok
DAEMON=ok
TCP=9797 远程可用（mTLS）
HTTPS=8443 网关可用（mTLS）
HTTP=127.0.0.1:8080 本机接口可用
READY=yes
```

- `CLOCK=corrected` 表示开机时系统时间早于 SD 卡上凭据的修改时间，脚本把时钟拨到了那个时间。不拨的话证书尚未生效，mTLS 会被拒。
- `NET=` 行是板子当前的地址，`dhcp` 表示从 DHCP 拿到，`fallback` 表示网里没有 DHCP、用的兜底地址。不知道板子 IP 时，在电脑上跑 `python3 tools/find_board.py`，或看串口登录提示。
- `PL=crypto` 表示 `zu3eg_hsm_dev.bit` 已装入。
- `DAEMON=ok` 表示 daemon 已启动并建立了 socket。
- `pki/` 下的 `hsm_ca.crt`、`hsm_device.crt`、`hsm_device.key` 缺一个，就不监听 9797 和 8443（`TCP=off`），本机 8080 不受影响。
- 只有 `READY=yes` 才算就绪。失败原因写在 `hsm/hsm-boot.log`。

## 演示脚本

SD 卡上的演示入口在 `/media/sd-mmcblk1p2/demo/`，对应仓库里的文件如下：

| SD 卡上 | 仓库里 | 内容 |
|---|---|---|
| `demo/run.sh` | `business_demo.sh` | 业务流程演示，八部分 |
| `demo/run_classic.sh` | `board_demo.sh` | 先打 `HSM_STATUS`，再依次跑 `hsm/run_demo.sh` 和 `demo/functions.sh` |
| `demo/functions.sh` | `functions_board.sh` | 管理功能面演示（槽位、备份、安全存储），用的是主机侧软件密码机 |
| `demo/bin/` | (交叉编译产物) | `functions.sh` 用的 `pqchsmd`、`pqchsm-cli`、`pqchsm-admin`、`mechs`、`roundtrip`、PKCS#11 模块 |
| `hsm/run_demo.sh` | `run_demo.sh` | `sdf_demo`、绕过接口的反证、`hsm_nocrash` |

```
ssh root@<板子IP>                              # 口令 root
hsm-demo                                       # 业务流程演示，同 sh /media/sd-mmcblk1p2/demo/run.sh
sh /media/sd-mmcblk1p2/demo/run_classic.sh    # sdf_demo + 反证 + 管理功能面
```

### 业务流程演示

`run.sh` 用命令行工具 `hsmctl` 逐步演示，共八部分：

| 部分 | 内容 |
|---|---|
| 〇 设备 | `hsmctl info` |
| 一 密钥管理 | 生成 SM4、ML-DSA-65、ML-KEM-768 密钥，列出、查看 |
| 二 数据加解密 | SM4-CBC |
| 三 签名与验签 | ML-DSA-65；改一位的消息验签被拒 |
| 四 密钥封装 | ML-KEM-768 封装、解封装 |
| 五 杂凑与随机数 | SM3、随机数 |
| 六 备份与恢复 | 备份、销毁、恢复；篡改过的备份被拒 |
| 七 安全性检查 | 篡改密钥文件里的 blob，TA 拒绝装载；普通世界直接往密钥装载口写密钥（`d7probe`）；会话结束后对称密钥寄存器为空（`d7probe vmap`） |
| 八 HTTP 接口 | 板上没有 curl，用 `nc` 向 `127.0.0.1:8080` 发原样 HTTP 请求，调用 `hsm_httpd` 的路由：`/v1/info`、`/v1/keys`（生成、列出、销毁）、加解密、签名验签、封装解封装、`/v1/sm3`、`/v1/random` |

每条命令后面会附上 daemon 日志里这条命令产生的分层记录：

| 标签 | 含义 |
|---|---|
| `[TEE]` | TA 里完成的操作 |
| `[TEE→PL]` | TA 经安全通路把密钥材料写进 PL |
| `[PL]` | PL 里的运算（带时延） |
| `[NW]` | 普通世界 daemon 搬运 blob |
| `[HTTP]` | 网关收到请求（第八部分） |

演示用的密钥库在 `/tmp/hsm-demo`，结束后删除。第八部分用网关自己的密钥库（`/media/sd-mmcblk1p2/keys`），演示中创建的密钥最后都会删掉。

输出可以用 `tools/demo_to_html.py` 转成 HTML 报告，样例见 [docs/demo/business_demo.html](../../docs/demo/business_demo.html)。

### 经典演示

`run_classic.sh` 的内容：

| 编号 | 内容 | 判据 |
|---|---|---|
| ① ② | `sdf_demo` 经 SDF 接口调用 | ML-KEM 两端共享密钥一致；SM4 密文与 GB/T 32907 附录 A.1 逐字节相同；明文导入对称密钥被拒；blob 重装后密文相同；会话结束后句柄失效 |
| ③ | 绕过接口，从普通世界直接访问 PL | 见下 |
| ④ | `hsm_nocrash` 往被拒或不存在的地址读写 | 板子照常运行 |
| ⑤ | `functions.sh` 管理功能面 | 见 [demo/functions/README.zh-CN.md](../../demo/functions/README.zh-CN.md) |

③ 先读风扇观测口和 `trng_axi` 的 VERSION 判断当前位流：

- 默认的演示形态（`zu3eg_hsm_dev.bit`，`SECURE_ONLY=0`）：功能核对普通世界开放，不能用读回 0 作判据。演示改为运行 `d7probe`，从普通世界往对称密钥寄存器的装载口写一把密钥，该口只接受安全世界的事务，写入全部被丢弃，`LOAD_REJ` 计数增加 11。
- 送检形态（`zu3eg_hsm.bit`，`SECURE_ONLY=1`）：`hsm_secneg` 从普通世界读五个功能核的 VERSION，全部读回 0，而真值是 `0x00010000`。风扇观测口始终开放，读到真值，用来确认这条访问路径本身是通的。

切换到送检形态：`sh plharness.sh pay_d7_sec.sh`（装送检位流，从普通世界扫描，经安全世界跑完整 KAT，最后装回厂家位流），跑完用 `hsmreboot.sh` 重启回到演示形态。

④ 的记录在 `hsm/RESULT_nocrash.txt`：9 个被拒或不存在的地址，每个读 2000 次、写 2000 次，共 36,000 次访问。被拒的访问读回 0、写入被丢弃，不产生总线错误。如果被拒访问回 SLVERR，写是 posted 的，错误会以 SError 回到内核，内核无法处理，只能断电重启。违规计数器只有安全世界读得到。

## 从另一台机器连

**构建客户端。** 客户端不做密码运算，不需要 liboqs，只用系统的 `cc`：

```
sh board/demo/build_client.sh
```

产物是 `sdf_demo`（SDF 接口）和 `p11_hw_demo`（PKCS#11 接口）。

**凭据。** 远程口是 mTLS，需要 `hsm_ca.crt`、`client.crt`、`client.key`。仓库里的 `demo/remote/creds` 是公开的演示凭据。使用自己的 PKI 时运行 `tools/demo_remote.sh <板子IP> --provision`。

**连接。**

```
./sdf_demo <板子IP> <凭据目录> [端口] [设备CN]
./demo/remote/run.sh <板子IP>                 # 自动编译客户端并带默认参数
./demo/remote/full_demo.sh <板子IP> [凭据目录]  # 板上 run.sh（经 ssh）+ 本机 curl 调 HTTPS，合成 demo-out/demo.html
```

PKCS#11 接口：

```
PQCHSM_BACKEND=sdfe PQCHSM_SDFE_HOST=<板子IP> \
  PQCHSM_SDFE_PKI=~/.config/pqchsm/pki PQCHSM_SDFE_DEVICE_CN=axu3egb-hsm-01 \
    ./p11_hw_demo <PKCS#11 模块.so>
```

`p11_hw_demo` 只用 OASIS 的 `pkcs11.h`。后端是不是板子由环境变量决定。它检查四件事：`C_GenerateKeyPair` 生成 ML-KEM 密钥对；读私钥的 `CKA_VALUE` 被拒；`C_EncapsulateKey` / `C_DecapsulateKey` 两端共享密钥一致；`C_Encrypt` / `C_Decrypt`（`CKM_AES_GCM`）对任意长度数据做 KEM-DEM 公钥加密。

## 出问题时

| 现象 | 处理 |
|---|---|
| `192.168.50.174` 连不上 | 装入密码位流后 `eth0` 就消失了，用 `eth1` 的地址 |
| 不知道板子地址 | 电脑上跑 `python3 tools/find_board.py`；或看路由器里的 `pqchsm`、串口登录提示 |
| 找不到板子 | 检查两个网口是否都插了线；网络可能隔离了设备，网线直插电脑用 `169.254.50.175`；用串口（115200）进去看 `HSM_STATUS` |
| `READY=no` | 看 `/media/sd-mmcblk1p2/hsm/hsm-boot.log` |
| 想回到厂家位流 | 删掉 `/media/sd-mmcblk1p2/hsm/zu3eg_hsm_dev.bit` 再重启，开机脚本不再装密码位流 |
| 远程连不上 | 检查凭据；板上 `pki/` 三个文件是否齐全；两端时钟差太大会导致证书未生效 |
| `uptime` 的 load 约 1.0 而 CPU 空闲 | 正常。一个 USB 集线器的内核线程常驻不可中断等待，计入 load。开机 5 秒时就已如此，与本项目无关 |

重启用 `sh /media/sd-mmcblk1p2/hsm/hsmreboot.sh`。这块板上 `reboot` 不生效，脚本走内核 sysrq。

## 限制

- 远程接口是 mTLS（TLS 1.3 双向证书）。握手通过后可以执行全部操作，没有分级授权；可选的 `pki/hsm_acl` 按证书 CN 限制，文件不存在时 CA 签发的任何证书都放行；没有吊销。仓库里的演示凭据是公开的，任何能连到这个口的人都能用它驱动密码机，只适合内网演示。详见 [docs/SECURITY.zh-CN.md](../../docs/SECURITY.zh-CN.md#远程口)。
- TA 的 KEK 派生自 SHA-256(Device DNA)。DNA 不是秘密，能读 DNA 又拿到 blob 的人可以解开 blob。
- 没有烧写 eFUSE，没有硬件信任根。能物理访问板子的人可以替换 SD 卡上的启动镜像，绕过当前的边界。
