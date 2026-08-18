[English](README.md) · **中文**

# 一条命令跑完远程演示

在任何一台够得着板子的机器上驱动板上的密码机，不需要 SSH、不需要配置、
不需要摆弄凭据。

```bash
git clone https://github.com/per1x/pqc-crypto-module && cd pqc-crypto-module
./demo/remote/run.sh
```

**什么都不带跑，它会问你**，所以不必先去查用法：

```
这台密码机在哪？
  板子的 IP 或主机名，例如 192.168.1.50 或 hsm.local

  地址： 192.168.1.50

跑哪一个？
    1) 完整九节演示        默认
    2) 只打关键结论        --smoke
    3) 只连一下看在不在    --status

  选 [1-3，回车= 1]:
```

跑通之后它会问要不要记住这个地址，之后再跑就什么都不用给了。当然也可以一次
性全写在命令行上：

| | |
|---|---|
| `./demo/remote/run.sh 192.168.1.50` | 完整九节演示 |
| `./demo/remote/run.sh 192.168.1.50 --smoke` | 只打关键结论 |
| `./demo/remote/run.sh 192.168.1.50 --status` | 只连一下，打印设备横幅 |
| `./demo/remote/run.sh 192.168.1.50 --save` | 不用问就记住这个地址 |

**地址在任何情况下都不是写死的常量** —— 你的板子不在我们的网段上。取值顺序是
命令行参数 → `$BOARD` → `demo/remote/board.conf`（不进仓库），最后才问人。
而且**只在真的有人在敲键盘时才问**：从管道、CI、cron 里跑一律打印用法并退出
（退出码 2），不会挂在那儿等一个永远不来的回车。它也从不猜地址 —— 猜错的表现
是"连不上"，比"你没给地址"难查得多。

其余可覆盖：`PORT=… DEVICE_CN=… CREDS=… ./demo/remote/run.sh <板子IP>`

客户端在第一次运行时由三个文件现编（`sdf_demo.c`、`libsdfe.c`、`pqcs_tls.c`）。
它**不链接任何密码算法库** —— 链的 OpenSSL 只做 mTLS 传输 —— 所以它打印出的
任何正确结果都只可能来自 FPGA。

**前置**：一个 C 编译器与 **OpenSSL 3**。macOS 自带的是 LibreSSL，缺 TLS 1.3
那套 API，需要 `brew install openssl@3`；脚本会自己找，找不到就当场说清楚。

## ⚠️ `creds/` 里的凭据是**公开的**，这是有意的

`creds/` 里放着一张客户端证书**和它的私钥**，就提交在这个公开仓库里。这样做
是为了让上面那条命令零配置可用，但代价必须说清楚，不能含糊过去：

- **任何够得着板子 9797 口的人都能驱动这台密码机。** 它成立的前提只有一条 ——
  板子在内网，外面路由不进来。
- **它不是远程口的安全论证。** mTLS 买到的是"持有私钥"而不是"知道一个字符串"；
  把私钥公开发布，等于在这一条链路上自愿放弃那个区别。真实部署要自己生成 CA
  （`tools/mkpki.sh`）、凭据带外分发，必要时用板上的 `hsm_acl` 按 CN 限制。
  见 [docs/SECURITY.zh-CN.md — 远程口](../../docs/SECURITY.zh-CN.md#远程口)。
- **CA 私钥与设备私钥不在这里，也永远不该进来。** 有它们就能签发新证书、
  冒充这台设备，那是另一个量级的事。

想用自己的凭据，把 `CREDS` 指过去即可：

```bash
CREDS=~/.config/pqchsm/pki ./demo/remote/run.sh
```

给一块板子装一套全新的、私有的 PKI，用 `tools/demo_remote.sh <板子IP> --provision`。
