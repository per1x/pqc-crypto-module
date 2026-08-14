**English** · [中文](USAGE.zh-CN.md)

# Usage

> ### ⚠️ Read this before writing any register
>
> **The default bitstream (`zu3eg_hsm.bit`) gives the normal world *zero*
> reachability.** All four functional slaves are built with `SECURE_ONLY=1`, so
> every access from Linux — which always carries `AxPROT[1]=1` — is refused at
> the bus. Only the secure world (EL3, via the BL31 SiP) can drive them.
>
> **Never issue a write you expect to be refused.** A refused read returns
> DECERR synchronously and Linux turns it into `SIGBUS`, which a program can
> catch. A refused *write* is different: AXI writes are posted, so the error
> comes back later as an **SError**, which belongs to no instruction and which
> the kernel can only answer with a panic. **The cost is a power cycle**, and on
> this board a power cycle clears `CSU_MULTI_BOOT`.
>
> Programs that write registers (`hsm_hwtest`, `hsm_kem3`) must therefore run
> against the **development** bitstream, not the default one:
>
> ```
> PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
> ```
>
> That build sets `SECURE_ONLY=0` on the functional slaves and is named
> `zu3eg_hsm_dev.bit` so the two can never be confused. It is a debug form, not
> the shipping one.

- [Simulation and static checks](#simulation-and-static-checks)
- [Host software](#host-software)
- [Bitstream](#bitstream)
- [The service layer](#the-service-layer)
- [Running the SDF demo](#running-the-sdf-demo)
- [PKCS#11 demos](#pkcs11-demos)
- [On the board](#on-the-board)
- [Offline and intranet installation](#offline-and-intranet-installation)

## Simulation and static checks

Nothing here needs a board or a licensed toolchain.

```bash
python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator yosys     # or your distro's packages

./tools/rtl_sim.sh          # 197 cocotb tests
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, 70 modules
./tools/rtl_synth_check.sh  # Yosys synthesisability, 68 modules
```

`rtl_sim.sh` generates the golden vectors on first run
(`python3 hardware/model/export_vectors.py`). Each script skips with a message
rather than failing when a tool is absent.

## Host software

Requirements: CMake ≥ 3.20, a C11 compiler, OpenSSL 3, liboqs.

```bash
brew install liboqs openssl@3 cmake        # macOS
# Debian/Ubuntu: libssl-dev, cmake, ninja-build; liboqs from source
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
message instead of failing.

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
# → hardware/syn/impl/zu3eg_hsm.bit
```

A characterisation build lowers the fan thresholds and exposes the raw TRNG tap.
**This is not the product form** — in the product build that tap does not exist
in the fabric at all, rather than reading as zero:

```bash
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit
```

The flow aborts on any of its post-synthesis assertions — driven PS handshake
signals, the fan pin, `SYSMONE4 SIM_DEVICE`, negative setup slack, and an
effective hold margin below 0.050 ns. Each is documented in
[TESTING.md](TESTING.md#implementation-flow-assertions), with what it cost to
learn.

Out-of-context synthesis of a single module, for area and Fmax:

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs mlkem_decaps
```

## The service layer

`service/` builds the daemon, the SDF-style client library and the demo:

```bash
make -C service                              # host build
make -C service CROSS=aarch64-linux-gnu-     # for the board
```

Three artefacts: `pqchsm_fpgad` (sessions, handles, serialisation),
`libsdfe.a` (the client library), and `sdf_demo`. They are statically linked, so
they can be copied to the board without worrying about its libc.

The daemon needs `/dev/secmmio`, which is provided by the kernel module in
`board/kmod/`, which in turn needs a BL31 carrying the SiP from `boot/atf/`.

## Running the SDF demo

```bash
./service/pqchsm_fpgad &
./service/sdf_demo
```

`sdf_demo` links **only** `libsdfe` — no crypto library at all — so it cannot
compute anything itself. Every correct value it prints came out of the FPGA.

```
[device] pqchsm_fpgad on FPGA  mlkem=0x00010000 sym=0x00010000

[1] SDFE_GenerateRandom             32 bytes from the PL ring-oscillator source
[2] SDFE_GenerateKeyPair_MLKEM(768) ek 1184 bytes, private key handle = 0
                                    ← the application never receives dk
[3] SDFE_Encapsulate_MLKEM          K 32 + c 1088 bytes
[4] SDFE_Decapsulate_MLKEM          K recovered by handle matches [3]
[5] SDFE_ImportKey → key_vault slot 3; SDFE_Encrypt(SM4)
    ciphertext 681edf34d206965e86b3e94f536e4246
    byte-exact against GB/T 32907 A.1; decryption returns the plaintext
```

In the same run, a counter-proof program reads the five cores directly from the
normal world and gets **6/6 DECERR**. The application works; bypassing the
service layer to touch the hardware does not.

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
objects. Both use the low-level binding directly, because higher-level provider
frameworks do not yet support these mechanisms — see
[demo/README.md](../demo/README.md), which includes a probe demonstrating it.

## On the board

**Every action that touches the PL must go through the harness.**

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

`board/scripts/plharness.sh` detaches from the terminal, restores the network
unconditionally on exit, and arms a sysrq watchdog. This is not ceremony:

- **`eth0` is inside the PL.** `80000000.ethernet` is the vendor design's AXI
  Ethernet, so every PL reconfiguration must unbind the PL drivers first and
  rebind afterwards. Reconfiguring the fabric with a live AXI master hangs the
  bus — and once AXI is down, even sysrq cannot be written, so the watchdog
  cannot save it either. Only a power cycle can.
- **Never put the unbind in a foreground SSH command.** The first device
  unbound is `eth0`; the session dies on the spot and nothing after it ever
  runs. Three disconnections and three power cycles came from exactly this.
- **Teardown does not trust the payload's state.** It unbinds unconditionally
  before reconfiguring, because some payloads rebind the drivers themselves.
  Unbinding an already-unbound device is one ignored error; the alternative was
  a hung bus.

Payload scripts live in `board/scripts/` (`pay_*.sh`); the programs they run are
in `board/src/`. Raw output from every board run is kept verbatim in
`board/logs/`.

Fan control is fully working but is not yet persistent across reboots — one
command quiets the board:

```sh
sh /media/sd-mmcblk1p2/hsm/fanquiet-init.sh
```

Making it survive a reboot requires writing the PL section of the golden
`BOOT.BIN`, which needs JTAG to be recoverable. See
[SECURITY.md](SECURITY.md#limitations).

## Offline and intranet installation

Staging every dependency onto a host with no route to the internet — liboqs,
ACVP vectors, PyKCS11, a JDK, and the fallbacks for when even `apt`/`dnf` is
unavailable — is covered step by step in
[reference/deployment.md](reference/deployment.md).
