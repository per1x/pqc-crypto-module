**English** · [中文](README.zh-CN.md)

# One-command remote demo

Drives the crypto module on the board over its remote port from any machine that
can reach it. No SSH, no configuration, no credential wrangling.

```bash
git clone https://github.com/per1x/pqc-crypto-module && cd pqc-crypto-module
./demo/remote/run.sh <board-ip>
```

| | |
|---|---|
| `./demo/remote/run.sh 192.168.1.50` | the full nine-section demo |
| `./demo/remote/run.sh 192.168.1.50 --smoke` | headline results only |
| `./demo/remote/run.sh 192.168.1.50 --status` | just connect and print the device banner |
| `./demo/remote/run.sh 192.168.1.50 --save` | remember this address; omit it from then on |

**The address is an argument, not a baked-in constant** — your board is not on
our network. It is resolved from the command line, then `$BOARD`, then
`demo/remote/board.conf` (what `--save` writes; it is not tracked). If none of
the three is set the script prints usage and stops rather than guessing: a wrong
guess shows up as "cannot connect", which is far harder to diagnose than "you
did not give me an address".

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
