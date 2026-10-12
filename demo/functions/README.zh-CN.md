[English](README.md) · **中文**

# 密码机功能演示

依次演示主机侧密码模块（`pqchsmd`）的管理功能：算法支持性、熵源、槽位管理、密钥管理、备份恢复（M-of-N）、安全存储。

```bash
cmake -S . -B build && cmake --build build -j
./demo/functions/run.sh
```

脚本自己起 daemon、用临时密钥库，退出时一并删除。不需要板子，也不碰已有的密钥库。需要 `cc`（编译 `mechs` 和 `roundtrip`）和 `python3`（第六节改一个 bit）。

环境变量：`BUILD=`（默认 `build`）、`PORT=`（默认 19099）、`SO_PIN=`、`USER_PIN=`。

## 演示的是哪一个模块

仓库里有两个密码模块，功能面不同：

| | 板上 `pqchsm_fpgad` | 主机侧 `pqchsmd`（本演示） |
|---|---|---|
| 槽位管理 | 没有。密钥归 OP-TEE 里的 TA，交出去的是句柄和包裹后的 blob。对称密钥由 TA 装进 PL 的对称密钥寄存器（0 到 6 号给业务密钥，7 号只放公开的 KAT 密钥） | 状态机、标签、策略 |
| PIN、会话、角色 | 没有 | SO 和 User、登录、失败计数 |
| 密钥库 | 没有带 PIN 的密钥库。`hsmctl` / `hsm_httpd` 的密钥库目录里只有 TA 包裹的 blob | AES-GCM 包裹、KDR 派生 KEK、全文件 MAC、epoch 防回滚 |
| 备份恢复 | `hsmctl backup` / `restore`：整个密钥库打成一个包，每项带 SM3，没有 M-of-N | M-of-N Shamir 分片 |
| 密码运算 | 在 PL 里 | liboqs，或转发给板子 |

所以本演示里的槽位、备份、存储管理都是主机侧的功能。板子那一半见 [demo/remote/](../remote/) 和 [board/demo/](../../board/demo/README.zh-CN.md)。

两者可以组合。设 `PQCHSM_BACKEND=sdfe` 后，ML-KEM 的密钥生成在板上完成：TA 把种子包成 blob 保管，PL 每次运算时展开。主机侧的槽位只保存公钥和一个句柄，密钥库里不为它写入任何密钥材料。

## 各节内容

| 节 | 内容 |
|---|---|
| 一、算法支持性 | `C_GetMechanismList` 和 `C_GetMechanismInfo`，向模块本身查询（5 个机制） |
| 二、熵源 | 主机侧的随机数来自 OpenSSL。板上 TRNG（8 个环振，H = 0.871 bit/sample，见 [docs/SECURITY.zh-CN.md](../../docs/SECURITY.zh-CN.md)）只有设 `PQCHSM_HWRNG=sdfe` 才会用到 |
| 三、槽位管理 | 列出四个槽位，`UNINIT`、`EMPTY`、`LOADED` 状态，SO 和 User 角色。没登录就生成密钥会被拒（`not authorized`） |
| 四、密钥管理 | 生成、导出公钥、签名、销毁。没有导出私钥的命令。长度对照 FIPS 204 检查（ML-DSA-44：公钥 1312 字节，签名 2420 字节） |
| 五、备份恢复 | 只备份带 `BACKUPABLE` 策略的槽位。2-of-3 的备份可以用第 1、3 片或第 1、2 片恢复，只给 1 片被拒 |
| 六、安全存储 | 改一个 bit，密钥库拒绝装载，文件不被改写。换回旧快照被 epoch 锚点拒绝。`rotate-kek` 只换包裹密钥，不动密钥本身 |
| 七、算法调用 | `roundtrip` 走 `C_GenerateKeyPair`、`C_Sign`、`C_Verify`，再分别改消息一个 bit、改签名一个 bit 各验一次，都必须失败。`board/demo/p11_hw_demo.c` 走 ML-KEM 封装、解封装和 AES-GCM（KEM-DEM），并检查私钥读不出来 |

## 两个容易读反的地方

`backup` 的 `<slot>` 参数决定用哪个槽位的 SO PIN 授权，不决定备份什么：所有带 `BACKUPABLE` 策略的槽位都会导出。`pqchsm-admin` 的帮助串在这一点上有歧义。

在这个密钥库里，能备份的密钥和留在板上的密钥互相排斥。主机侧的备份包含种子；在板上生成的密钥只以 TA 包裹的 blob 存在，只有本板的 TA 打得开，主机侧的密钥库导不出来。板上自己的备份包（`hsmctl backup`）只能恢复回同一块板子。

## 默认的密钥派生根

默认 KDR 是编译进二进制的公开常量（daemon 启动时打印 `DEV` 警告），所以这个配置下密钥库的机密性为零。PRODUCTION 构建在没有硬件派生根时拒绝启动。见 [docs/SECURITY.zh-CN.md](../../docs/SECURITY.zh-CN.md) 的构建形态一节。
