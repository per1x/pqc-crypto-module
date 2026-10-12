**English** · [中文](USAGE.zh-CN.md)

# Usage

> ### ⚠️ A refused access reads back 0 and raises no error
>
> In the default bitstream (`zu3eg_hsm.bit`) the five functional slaves (slots 0, 1,
> 2, 3, 6) are built with `SECURE_ONLY=1`. Every access from Linux carries
> `AxPROT[1]=1` and is refused at the bus. Only the secure world (EL3, through the
> BL31 SiP) can drive them. The fan slave (slot 5) has no firewall.
>
> Refusal is RAZ/WI: the read returns 0, the write is discarded, the response is
> OKAY, and no bus error is raised.
>
> * `VERSION` reading 0 is how you tell. It is `0x0001_0000` on every core, so 0
>   means refused (or no bitstream loaded). There is no error code to look for.
> * Nothing written from user space can take the board down. `hsm_nocrash` issued
>   36,000 refused accesses on the board and it stayed up. An earlier design
>   answered DECERR; because AXI writes are posted, that came back as an SError and
>   the kernel panicked, so one wrong address cost a power cycle.
> * A mistyped address is silent. The violation counters, readable only from the
>   secure world, are the record.
>
> The board currently boots the development bitstream `zu3eg_hsm_dev.bit`
> (`PQC_DEV_OPEN=1`, functional slaves at `SECURE_ONLY=0`), so Linux can also reach
> the cores through `/dev/mem`. The symmetric-key load port and the seed ports are
> secure-only in both bitstreams.

- [Simulation and static checks](#simulation-and-static-checks)
- [Host software](#host-software)
- [Bitstream](#bitstream)
- [The service layer](#the-service-layer)
- [Running the SDF demo](#running-the-sdf-demo)
- [PKCS#11 demos](#pkcs11-demos)
- [On the board](#on-the-board)
- [Offline and intranet installation](#offline-and-intranet-installation)

> ## ⛔ Read this first: irreversible-operation red lines
>
> No one-time, irreversible programming is permitted on this board: not eFUSEs, not
> the eMMC RPMB authentication key, not BBRAM latch bits, not any OTP or
> `*_LOCK` / `*_DISABLE` / `*_EN` fuse. It requires the board owner's explicit
> consent first; the default is no, and evaluating something is never permission
> to execute it.
>
> The normative text is
> [SECURITY.md, Irreversible-operation red lines](SECURITY.md#-irreversible-operation-red-lines)
> (fuse list, reversible alternatives, the 2026-08-18 incident, and why the rule
> is enforced at compile time).

## Simulation and static checks

Nothing here needs a board or a licensed toolchain.

```bash
python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator yosys     # or your distro's packages

./tools/rtl_sim_fast.sh     # day-to-day: per module, Verilator
./tools/rtl_sim.sh          # full suite, 272 cocotb tests on Icarus, about 40 min
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, every module under hardware/rtl
./tools/rtl_synth_check.sh  # Yosys synthesisability
```

Use `rtl_sim_fast.sh` while developing (name the testbenches to run, or run the
default set) and run `rtl_sim.sh` before merging. Verilator is two-valued and does
not propagate X, so some bugs only show up on Icarus.

`rtl_lint.sh` currently fails: seven modules (`mldsa_axi`, `mldsa_engine`,
`mldsa_keygen`, `mldsa_sign`, `mldsa_verify`, `mlkem_axi`, `zu3eg_hsm_top`) still
have Verilator warnings, mostly constant width extension and unused signals. The
other modules are clean.

`rtl_sim.sh` generates the golden vectors on first run
(`python3 hardware/model/export_vectors.py`). Each script skips with a message
rather than failing when a tool is absent.

## Host software

Requirements: CMake ≥ 3.20, a C11 compiler, OpenSSL 3. The post-quantum
algorithms come from the bundled mlkem-native / mldsa-native
(`third_party/pqc-native/`, also used by the OP-TEE TA); liboqs is not needed.

```bash
brew install openssl@3 cmake               # macOS
# Debian/Ubuntu: libssl-dev, cmake, ninja-build
```

```bash
./tools/fetch_vectors.sh                   # NIST ACVP vectors (pinned commit)
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optional components are detected, not required. Without Verilator the simulated
RTL backend is not compiled in and `accel_transport_verilator()` returns `NULL`;
tests needing `cocotb`, `iverilog` or `pkcs11-tool` skip themselves with a
message.

Additional checks:

```bash
python3 tools/ct_audit.py       # constant-time source audit (--self-test first)
python3 tools/check_zeroize.py  # zeroization structure check (--self-test first)
./tools/aarch64_test.sh         # full rebuild and regression in an aarch64 container
./tools/fuzz.sh                 # libFuzzer targets (needs LLVM clang)
./tools/profile.sh              # sampling profile
./build/pqchsm-bench            # algorithm-level baseline
./build/pqchsm-prim-bench       # per-primitive cost and measured RTL cycle counts
```

## Bitstream

Vivado 2020.1, target `xazu3eg-sfvc784-1-i`, about 35 minutes.

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm.bit       default: functional slaves SECURE_ONLY=1

PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_dev.bit   functional slaves SECURE_ONLY=0
```

`boot/d7/bitbuild-d7.sh` builds both on the build host. The board boots the
development bitstream; the default one is loaded by hand to check the boundary.

A characterisation build lowers the fan thresholds and exposes the raw TRNG tap. It
is not a product form: in the product build that tap does not exist in the fabric.

```bash
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit
```

The flow aborts on any of its post-synthesis assertions: driven PS handshake
signals, the fan pin, `SYSMONE4 SIM_DEVICE`, negative setup slack, and an effective
hold margin below 0.050 ns. Each is documented in
[TESTING.md](TESTING.md#implementation-flow-assertions).

Out-of-context synthesis of a single module, for area and Fmax:

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs mlkem_decaps
```

## The service layer

`service/` builds the daemon, the SDF-style client library, the demo and the tools:

```bash
make -C service                              # host build
make -C service CROSS=aarch64-linux-gnu- SYSROOT=<target sysroot> SSLROOT=<cross-built openssl>   # for the board
```

Targets: `pqchsm_fpgad` (sessions, handles, serialisation), `libsdfe.a` (client
library), `sdf_demo`, `hsmctl`, `hsm_httpd`. The board binaries link OpenSSL
dynamically and expect `libssl.so.1.1` and `libcrypto.so.1.1` in
`/media/sd-mmcblk1p2/hsm/lib`; [service/README.md](../service/README.md) has the
details. `boot/d7/build-apps-d7.sh` does the cross build on the build host.

The daemon needs `/dev/secmmio`, provided by the kernel module in `board/kmod/`,
which needs a BL31 carrying the SiP from `boot/atf/`. Key operations also need the
TA (`tee/`) installed and `tee-supplicant` running; `hsm-boot.sh` takes care of both.
The services and whitelist are described in [boot/README.md](../boot/README.md).

## Running the SDF demo

On the board, with the daemon running (`READY=yes`, see [On the board](#on-the-board)):

```bash
./sdf_demo                                # local
```

### The function tour (no board needed)

To see the management surface of the host-side module (algorithm support, slots,
keys, M-of-N backup, secure storage) with nothing to set up:

```bash
./demo/functions/run.sh
```

It starts its own daemon with a temporary keystore and deletes both on exit. This is
the host module `pqchsmd`, not the board daemon; the differences are listed in
[demo/functions/README.md](../demo/functions/README.md).

### One command against the board

If the board is already provisioned, any machine that can reach it runs the whole
demo with no configuration:

```bash
./demo/remote/run.sh                     # asks for the board address, then what to run
./demo/remote/run.sh <board-ip> --smoke  # or give it up front; --status to just connect
```

With no arguments it prompts (and offers to remember the address once it works).
From a pipe or CI it prints usage and exits instead of blocking.

That path uses the public demo credentials committed under `demo/remote/creds/`.
Read [demo/remote/README.md](../demo/remote/README.md) for what that costs before
relying on it for anything but a LAN demo.

### Provisioning script (a board with its own private PKI)

```bash
./tools/demo_remote.sh <board-ip> --provision   # once: make a PKI, install it, keep the client creds
./tools/demo_remote.sh <board-ip>              # then purely local: build + mTLS call, no SSH
./tools/demo_remote.sh <board-ip> --smoke      # minimal smoke, key lines only
./tools/demo_remote.sh <board-ip> --status     # just check the device answers
```

No cryptographic operation goes through SSH. The client is a native local binary
that opens TCP to port 9797 on the board. `--provision` is the only action that
uses SSH; it generates a device PKI, installs the device certificate on the board,
keeps a client certificate locally (`~/.config/pqchsm/pki`, 0600) and syncs the
board clock.

The remote port is mTLS; there is no token path. The design and its limits are in
`service/pqcs_tls.h` and in "The remote port" in [SECURITY.md](SECURITY.md).

In a real deployment client certificates should be distributed out of band (or the
operator generates a CSR and has it signed). `--provision` is a convenience for
people who already have a board shell. The CA private key stays on your machine.

A large clock skew breaks the handshake. Certificates carry notBefore/notAfter and
the board's clock can fall behind them after a reboot. A TLS 1.3 rejection also
arrives after the client considers the handshake complete, so the symptom is
"device info prints garbage", not "authentication failed". `--provision` syncs the
clock, `hsm-boot.sh` moves the clock forward if it is older than the credentials on
the SD card, and the client does one OP_PING round trip at open to surface such
rejections. After a reboot, check the clock first when mTLS reports a bad
certificate.

Overrides: `BOARD=… PORT=… PQCHSM_PKI=… DEVICE_CN=… SSH_KEY=…`. Every failure
prints a next step.

### Doing the same by hand

The local and remote forms are the same program; only the device-open line
differs. It links `libsdfe` and OpenSSL, and OpenSSL is used for TLS transport
only: there is no ML-KEM, ML-DSA, SM4 or SM3 implementation in it. `make -C service`
builds it; the line below is the equivalent by hand.

```bash
cc -O2 -Iservice -o sdf_demo \
   service/sdf_demo.c service/libsdfe.c service/pqcs_tls.c -lssl -lcrypto
# macOS ships LibreSSL, which lacks the TLS 1.3 API; use Homebrew's openssl@3:
#   P=$(brew --prefix openssl@3); cc -O2 -I$P/include -Iservice ... -L$P/lib -lssl -lcrypto
```

```bash
# the credential directory must hold hsm_ca.crt / client.crt / client.key
./sdf_demo 192.168.50.175 ~/.config/pqchsm/pki 9797 axu3egb-hsm-01
#          └ board IP     └ credentials        └ port └ expected device CN (optional)
```

The remote board address is the PS-side `eth1`. Modern OpenSSH needs
`-o PubkeyAcceptedAlgorithms=+ssh-rsa` for this board (dropbear 2019.78), which only
matters for `--provision`; without it you get `Permission denied (publickey,password)`
although the key is installed.

The daemon does not listen on the remote port unless `hsm_ca.crt`, `hsm_device.crt`
and `hsm_device.key` are all present in the board's `pki/` directory.

`sdf_demo` contains no cryptographic algorithm, so every correct value it prints
came from the board. Its steps:

| Step | Call | What it shows |
|---|---|---|
| 1 | `SDFE_GenerateRandom` | 32 bytes from the PL TRNG |
| 2 | `SDFE_GenerateKeyPair_MLKEM` (768) | The public key and a handle. The application never receives `dk`; the seed is made and held by the TA |
| 3 | `SDFE_Encapsulate_MLKEM` | Shared secret and ciphertext from the public key |
| 4 | `SDFE_Decapsulate_MLKEM` | Decapsulation by handle gives the same shared secret; the PL expands the key for this call and clears it |
| 5 | `SDFE_ImportKey`, `SDFE_LoadKatKey`, `SDFE_GenerateKey`, `SDFE_LoadKey`, `SDFE_Encrypt` | Plaintext import is refused (`SDR_NOTSUPPORT`). The public GB/T 32907 SM4 vector is loaded into KAT slot 7 and checked. The TA generates an SM4 key into slot 3 and hands back a blob; reloading the blob into slot 4 gives the same ciphertext; the blob cannot be loaded into slot 7 |
| 6 | `SDFE_GenerateKeyPair_MLDSA`, `Sign`, `Verify` | ML-DSA 44/65/87 round trip, and a changed byte must fail verification |

For the business flow with `hsmctl` and `hsm_httpd`, see the [README](../README.md#usage).

## PKCS#11 demos

```bash
cmake --build build --target pqchsm-p11

# Python, via PyKCS11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py

# Java, via the JDK 22+ FFM API (no external dependency)
java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java \
     "$PWD/build/pqchsm-pkcs11.dylib"
```

Both drive the full lifecycle: initialise the token, set PINs, log in, generate
ML-DSA and ML-KEM keys into slots, sign and verify, read attributes, enumerate
objects. They use the host-side module with its software backend; no board is
needed. Both use the low-level binding directly, because higher-level provider
frameworks do not yet support these mechanisms; see
[demo/README.md](../demo/README.md), which includes a probe showing it.

## On the board

`board/scripts/hsm-boot.sh` runs at every boot from the SD card
(`/media/sd-mmcblk1p2/hsm/`). It corrects the clock, configures `eth1`
(`192.168.50.175`), loads `zu3eg_hsm_dev.bit`, loads `secmmio.ko`, installs the TA
and starts `tee-supplicant`, then starts `pqchsm_fpgad` and
`hsm_httpd 8443 8080`. `hsm/HSM_STATUS` ends with `READY=yes` when this has all
worked, about 35 seconds after power-on. [board/README.md](../board/README.md) lists
the steps and the other scripts. To switch to the default bitstream at run time:

```sh
sh /media/sd-mmcblk1p2/hsm/pl_deadman.sh /media/sd-mmcblk1p2/hsm/zu3eg_hsm.bit 300
```

It rolls back unless someone logs in over `eth1` and touches `hsm/PL_KEEP` within
the grace period.

Every action that reconfigures the PL must go through the harness:

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

`board/scripts/plharness.sh` detaches from the terminal, restores the network on
exit, and arms a sysrq watchdog.

- `eth0` is inside the PL. `80000000.ethernet` is the vendor design's AXI Ethernet,
  so every reconfiguration must unbind the PL drivers first and rebind afterwards.
  Reconfiguring with a live AXI master hangs the bus, after which even sysrq cannot
  be written and only a power cycle recovers the board.
- Do not put the unbind in a foreground SSH command. The first device unbound is
  `eth0`, the session dies, and nothing after it runs.
- Teardown does not trust the payload's state. It unbinds unconditionally before
  reconfiguring, because some payloads rebind the drivers themselves. Unbinding an
  already-unbound device is one ignored error.

Payload scripts live in `board/scripts/` (`pay_*.sh`); the programs they run are in
`board/src/`. Raw output from every board run is kept in `board/logs/`.

The fan controller is part of the crypto bitstream (`fan_ctrl`, with the AXI
observation port at slot 5), so it controls the fan once that bitstream is loaded.
Before that the board runs the vendor bitstream; `board/factory/` rebuilds it with
the fan logic replaced, and the `fanquiet` PetaLinux recipe loads a fan-controlled
bitstream at boot. See [board/README.md](../board/README.md).

## Offline and intranet installation

Staging every dependency onto a host with no route to the internet (ACVP vectors,
PyKCS11, a JDK, and the fallbacks for when even `apt`/`dnf` is unavailable) is
covered step by step in [reference/deployment.md](reference/deployment.md).
