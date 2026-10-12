**English** · [中文](README.zh-CN.md)

# Remote demo

Runs the SDF-interface demo (`service/sdf_demo.c`) against the board over its mTLS port (9797) from any machine that can reach it. No SSH is needed.

```bash
git clone https://github.com/per1x/pqc-crypto-module && cd pqc-crypto-module
./demo/remote/run.sh
```

With no arguments the script asks for the board address and the mode (the prompts are in Chinese):

```
这台密码机在哪？
  板子的 IP 或主机名，例如 192.168.1.50 或 hsm.local
  （板子的 eth1 是静态地址，上电约 35 秒后就绪；Ctrl-C 退出）

  地址： 192.168.1.50

跑哪一个？
    1) 完整九节演示        默认
    2) 只打关键结论        --smoke
    3) 只连一下看在不在    --status

  选 [1-3，回车= 1]：
```

After a successful run it offers to save the address. Everything can also be given on the command line:

| Command | Effect |
|---|---|
| `./demo/remote/run.sh 192.168.1.50` | Full demo, nine sections (`[1]` to `[9]` in `sdf_demo`: hardware random, ML-KEM, TA-held SM4 keys, ML-DSA, cipher modes, SM3, handle invalid after reconnect) |
| `./demo/remote/run.sh 192.168.1.50 --smoke` | Headline results only |
| `./demo/remote/run.sh 192.168.1.50 --status` | Connect and print the device banner |
| `./demo/remote/run.sh 192.168.1.50 --save` | Save the address to `demo/remote/board.conf` (not tracked) |

The board address is looked up in this order: command line, `$BOARD`, `demo/remote/board.conf`, then a prompt. It is asked for only when stdin is a terminal; otherwise the script prints usage and exits with status 2. There is no default address.

Overrides: `PORT=` (default 9797), `DEVICE_CN=` (default `axu3egb-hsm-01`), `CREDS=` (default `demo/remote/creds`).

On first run the script builds the client from `service/sdf_demo.c`, `service/libsdfe.c` and `service/pqcs_tls.c` into `build-demo/sdf_demo`. The client contains no cryptographic algorithm implementation; the OpenSSL it links is used only for the mTLS transport. It needs a C compiler and OpenSSL 3. macOS ships LibreSSL, which lacks the TLS 1.3 API, so install `openssl@3` with Homebrew.

## The credentials in `creds/` are public

⚠️ `creds/` holds a client certificate and its private key, committed to this public repository so that the command above works without setup. Anyone who can reach the board on port 9797 (or the HTTPS gateway on 8443, which uses the same CA) can drive the crypto module with them. This is acceptable only because the board sits on a private LAN. mTLS on this link then no longer prevents impersonation.

The CA private key and the device private key are not in the repository. A real deployment generates its own CA with `tools/mkpki.sh`, distributes credentials out of band, and can restrict clients by CN with `pki/hsm_acl` on the board. See [docs/SECURITY.md](../../docs/SECURITY.md#the-remote-port).

To use your own credentials:

```bash
CREDS=~/.config/pqchsm/pki ./demo/remote/run.sh
```

`tools/demo_remote.sh <board-ip> --provision` generates a private PKI and installs it on the board.

## Board command line and HTTPS

| Script | What it does |
|---|---|
| `http_demo.sh <board-ip> [creds-dir]` | Runs the business flow from this machine with `curl` over HTTPS and mTLS to the gateway on port 8443. Default creds dir `~/.config/pqchsm/pki`; needs `curl` and `python3`. Output has the same format as the on-board demo |
| `full_demo.sh <board-ip> [creds-dir]` | Runs `/media/sd-mmcblk1p2/demo/run.sh` on the board over ssh, then `http_demo.sh`, and merges both outputs into `demo-out/demo.html` with `tools/demo_to_html.py`. This one uses SSH |
