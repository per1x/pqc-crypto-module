[English](README.md) · **中文**

# 远程演示

在任何能连到板子的机器上，经 mTLS 端口（9797）运行 SDF 接口演示（`service/sdf_demo.c`）。不需要 SSH。

```bash
git clone https://github.com/per1x/pqc-crypto-module && cd pqc-crypto-module
./demo/remote/run.sh
```

不带参数运行时，脚本会询问板子地址和运行模式：

```
这台密码机在哪？
  板子的 IP 或主机名，例如 192.168.1.50
  （板子走 DHCP，不知道地址就跑 python3 tools/find_board.py；上电约一分钟后就绪；Ctrl-C 退出）

  地址： 192.168.1.50

跑哪一个？
    1) 完整九节演示        默认
    2) 只打关键结论        --smoke
    3) 只连一下看在不在    --status

  选 [1-3，回车= 1]：
```

运行成功后脚本会提示是否记住地址。也可以直接在命令行给出：

| 命令 | 作用 |
|---|---|
| `./demo/remote/run.sh 192.168.1.50` | 完整演示，九节（`sdf_demo` 的 `[1]` 到 `[9]`：硬件随机数、ML-KEM、TA 保管的 SM4 密钥、ML-DSA、分组模式、SM3、重连后句柄失效） |
| `./demo/remote/run.sh 192.168.1.50 --smoke` | 只打关键结论 |
| `./demo/remote/run.sh 192.168.1.50 --status` | 只连接并打印设备信息 |
| `./demo/remote/run.sh 192.168.1.50 --save` | 把地址写入 `demo/remote/board.conf`（不进仓库） |

地址的取值顺序是：命令行、`$BOARD`、`demo/remote/board.conf`、交互询问。只有标准输入是终端时才询问，否则打印用法并以状态 2 退出。没有默认地址。

可用环境变量覆盖：`PORT=`（默认 9797）、`DEVICE_CN=`（默认 `axu3egb-hsm-01`）、`CREDS=`（默认 `demo/remote/creds`）。

首次运行时，脚本用 `service/sdf_demo.c`、`service/libsdfe.c`、`service/pqcs_tls.c` 编译出客户端 `build-demo/sdf_demo`。客户端不含任何密码算法实现，链接的 OpenSSL 只用于 mTLS 传输。需要 C 编译器和 OpenSSL 3。macOS 自带的 LibreSSL 缺 TLS 1.3 的 API，需要用 Homebrew 装 `openssl@3`。

## `creds/` 里的凭据是公开的

⚠️ `creds/` 里有一份客户端证书和它的私钥，提交在这个公开仓库中，目的是让上面的命令不用任何配置就能运行。任何能连到板子 9797 端口（或同一个 CA 的 HTTPS 网关 8443 端口）的人，都可以用它驱动密码机。只有板子处在私有局域网内，这样才可以接受。在这条链路上，mTLS 因此不再能防止冒充。

CA 私钥和设备私钥不在仓库里。正式部署应该用 `tools/mkpki.sh` 生成自己的 CA，带外分发凭据，并可以用板上的 `pki/hsm_acl` 按 CN 限制客户端。见 [docs/SECURITY.zh-CN.md](../../docs/SECURITY.zh-CN.md#远程口)。

使用自己的凭据：

```bash
CREDS=~/.config/pqchsm/pki ./demo/remote/run.sh
```

`tools/demo_remote.sh <板子IP> --provision` 会生成一套私有 PKI 并装到板子上。

## 板上命令行与 HTTPS

| 脚本 | 作用 |
|---|---|
| `http_demo.sh <板子IP> [凭据目录]` | 在本机用 `curl` 经 HTTPS 和 mTLS 调用板上 8443 端口的网关，走一遍业务流程。凭据目录默认 `~/.config/pqchsm/pki`；需要 `curl` 和 `python3`。输出格式与板上演示相同 |
| `full_demo.sh <板子IP> [凭据目录]` | 经 ssh 在板上运行 `/media/sd-mmcblk1p2/demo/run.sh`，再运行 `http_demo.sh`，用 `tools/demo_to_html.py` 把两份输出合成 `demo-out/demo.html`。这个脚本用 SSH |
