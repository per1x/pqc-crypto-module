**English** · [中文](README.zh-CN.md)

# One-command remote demo

Drives the crypto module on the board over its remote port from any machine that
can reach it. No SSH, no configuration, no credential wrangling.

```bash
git clone https://github.com/per1x/pqc-crypto-module && cd pqc-crypto-module
./demo/remote/run.sh
```

**Run it with no arguments and it asks**, so there is nothing to look up first:

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

Once it works it offers to remember the address, so later runs need nothing at
all. Everything can also be given up front:

| | |
|---|---|
| `./demo/remote/run.sh 192.168.1.50` | the full nine-section demo |
| `./demo/remote/run.sh 192.168.1.50 --smoke` | headline results only |
| `./demo/remote/run.sh 192.168.1.50 --status` | just connect and print the device banner |
| `./demo/remote/run.sh 192.168.1.50 --save` | remember this address without being asked |

**The address is never a baked-in constant** — your board is not on our network.
It is resolved from the command line, then `$BOARD`, then `demo/remote/board.conf`
(not tracked), and only then by asking. The script **only prompts when a person is
actually at the keyboard**; from a pipe, CI or cron it prints usage and exits 2
rather than blocking on a keystroke that will never come. It never guesses an
address: a wrong guess surfaces as "cannot connect", which is far harder to
diagnose than "you did not give me an address".

Other overrides: `PORT=… DEVICE_CN=… CREDS=… ./demo/remote/run.sh <board-ip>`

The client is built from three files (`sdf_demo.c`, `libsdfe.c`, `pqcs_tls.c`) on
first run. It links **no cryptographic algorithm library** — the OpenSSL it links
serves the mTLS transport only — so every correct answer it prints can only have
come from the FPGA.

**Requirements**: a C compiler and **OpenSSL 3**. macOS ships LibreSSL, which
lacks the TLS 1.3 API, so `brew install openssl@3` is needed there; the script
finds it and says so plainly if it is missing.

## ⚠️ The credentials in `creds/` are public, on purpose

`creds/` holds a client certificate **and its private key**, committed to this
public repository. That is deliberate — it is what makes the command above work
with no setup — and the cost has to be stated rather than glossed over:

- **Anyone who can reach the board on port 9797 can drive this crypto module.**
  The only thing making that acceptable is that the board sits on a private LAN
  with no route in from outside.
- **This is not the security argument for the remote port.** mTLS buys "holds a
  private key" instead of "knows a string"; publishing the private key gives that
  distinction away on this one link. A real deployment generates its own CA
  (`tools/mkpki.sh`), distributes credentials out of band, and restricts by CN
  through the board's `hsm_acl`. See
  [docs/SECURITY.md — The remote port](../../docs/SECURITY.md#the-remote-port).
- **The CA private key and the device private key are not here and never will
  be.** Those would let anyone issue new certificates or impersonate the device,
  which is a different order of problem.

To use your own credentials instead, point `CREDS` at them:

```bash
CREDS=~/.config/pqchsm/pki ./demo/remote/run.sh
```

Provisioning a board with a fresh, private PKI is
`tools/demo_remote.sh <board-ip> --provision`.
