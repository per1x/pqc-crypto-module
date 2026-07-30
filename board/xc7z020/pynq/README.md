**English** · [中文](README.zh-CN.md)

# PYNQ path

Driving the accelerator from Python on a PYNQ image. This is the fastest way to
answer the three questions that only a real board can answer — is the address map
right, is the PL clock running, are the results correct — because changing
anything here means editing one Python file, not cross-compiling a firmware image.

The PetaLinux path in [`../petalinux/`](../petalinux/README.md) is the product
form. Both paths implement the same register contract from
[`docs/register-map.md`](../../../docs/register-map.md), so a timing sequence
validated here holds on the C side as well.

> Everything in the "On the board" sections below **must be run on the board**.
> None of it has been executed: this repository's development machine has no
> Zynq hardware and no PYNQ installation.

## Files

| File | Purpose |
|---|---|
| `pqc_accel.py` | `PqcAccel` class: loads the overlay, drives AXI4-Lite registers and the AXI-DMA channels |
| `selftest.py` | Four on-board checks plus a hardware-independent reference-value self-check |

## Placing the bitstream

`build_bitstream.tcl` leaves three artefacts in the Vivado project's `outputs/`:

| Artefact | Consumer |
|---|---|
| `pqc_accel_bd_wrapper.bit` | PYNQ `Overlay` |
| `pqc_accel_bd.hwh` | PYNQ `Overlay` |
| `pqc_accel_bd_wrapper.xsa` | PetaLinux |

**PYNQ requires the `.hwh` to sit next to the `.bit` and share its base name.**
Vivado names the handoff file after the block design (`pqc_accel_bd.hwh`) and the
bitstream after the wrapper (`pqc_accel_bd_wrapper.bit`), so one of them has to be
renamed:

```sh
# on the board, in whatever directory you keep the overlay
cp pqc_accel_bd_wrapper.bit  .
cp pqc_accel_bd.hwh          pqc_accel_bd_wrapper.hwh
```

If the `.hwh` is missing, `Overlay()` raises; if it is stale — a `.hwh` from a
different build than the `.bit` — `Overlay()` succeeds and the address map is
silently wrong. `pqc_accel.py` guards against the second case by comparing the
base address it gets from the `.hwh` against the constant in
[`../include/pqc_accel_zynq.h`](../include/pqc_accel_zynq.h) and printing a
warning on mismatch. Keeping the two files as one pair, copied together, is
cheaper than diagnosing that warning.

## Loading the overlay

```python
from pqc_accel import PqcAccel

with PqcAccel("pqc_accel_bd_wrapper.bit") as accel:
    print(f"VERSION = 0x{accel.version:08X}")          # expect 0x00010000

    out = accel.keccak_f1600(bytes(200))               # one permutation
    print(out[:8].hex())                               # e7dde140798f25f1 = first lane, little-endian

    coeffs = [0] * 256
    coeffs[0] = 1
    print(accel.ntt(coeffs)[:4])                       # forward NTT
    print(accel.ntt(coeffs, inverse=True)[:4])         # inverse NTT
```

The class follows the command sequence in `docs/register-map.md` literally: arm
the receive channel, stream the input packet, write `MODE`, write `IN_LEN`, write
`CTRL = START`, poll `STATUS` until `DONE`, check `ERR`, read `OUT_LEN`, drain the
result. Two details are worth knowing before changing that order:

* **The receive channel is armed before `START`.** The accelerator pushes its
  result out as soon as it finishes; if S2MM is not running yet, those beats stall
  inside the PL because `TREADY` is low. `accel_zynq.c` arms S2MM before starting
  MM2S for the same reason.
* **The input packet is fully delivered before `START` is written.** The
  accelerator's semantics are "packet arrives, `START` is written, computation
  begins". A `START` that races the data computes on the previous packet's
  residue.

**Cache coherency needs no action here.** `pynq.allocate` returns a
dma-coherent CMA buffer, so CPU accesses do not go through a cache that could
alias the DMA's view, and `pqc_accel.py` never calls `flush()` or `invalidate()`
— on a coherent buffer those methods do nothing. This is the one place where the
two paths differ in mechanism rather than in sequence: the C path maps reserved
memory through `/dev/mem` with `O_SYNC` to get the same uncached property.

Every wait in `pqc_accel.py` is bounded by the `timeout` constructor argument
(1 s by default). PYNQ's own `DMA.wait()` spins forever; on a board where the
stream handshake never completes that turns into a hung script with no
information in it, which is the worst possible outcome for a first power-on.

## Dependencies

* A PYNQ image for a Zynq-7000 board. The reference board for this port is
  PYNQ-Z2.
* `pynq` new enough to provide `pynq.allocate` and `pynq.lib.dma`. Check what the
  image actually ships rather than trusting a version number from documentation:

  ```sh
  python3 -c "import pynq; print(pynq.__version__)"
  python3 -c "from pynq import allocate; from pynq.lib.dma import DMA; print('ok')"
  ```

* `numpy`, which every PYNQ image includes.
* `hardware/model/ref_model.py` from this repository, for `selftest.py`. If the
  board does not have the full checkout, copy that single file next to
  `selftest.py`, or point `--ref-model` at its directory.
* Root privileges: loading a bitstream writes to devices that are root-only.

## Running the self-test

On the board:

```sh
sudo python3 selftest.py --bitstream pqc_accel_bd_wrapper.bit
```

On a development machine, the reference values can be checked without any
hardware — this is the part that verifies the checker rather than the accelerator:

```sh
python3 selftest.py --refs-only
```

```
== 参考值自检（不依赖硬件）==
[ 通过 ] 参考值：全零态置换的首个 lane 等于公开常量 —— 得到 0xF1258F7940E1DDE7，期望 0xF1258F7940E1DDE7
[ 通过 ] 参考值：用同一个置换手工算的 SHAKE128("") 与 hashlib 一致 —— 前 8 字节 7f9c2ba4e88f827d / 7f9c2ba4e88f827d
[ 通过 ] 参考值：系数打包/解包往返恒等 —— 512 字节
[ 通过 ] 参考值：正变换结果在 16 位有符号范围内 —— 极值 -1632 .. 1653
[ 通过 ] 参考值：正逆变换往返满足 invntt(ntt(x)) ≡ x·2^16 (mod q)
```

The exit code equals the number of failed checks.

### The four on-board checks

The order is deliberate: if a check fails, the ones after it produce meaningless
diagnostics, so `selftest.py` stops after `VERSION`.

1. **`VERSION` reads `0x00010000`.** Control plane only.
2. **All-zero-state Keccak-f[1600].** The 200-byte datapath. All 200 bytes are
   compared, not just the published first lane `0xF1258F7940E1DDE7`: a check on
   one lane passes even when the other 24 are wrong.
3. **256-point forward NTT.** The 512-byte datapath and the "two 16-bit
   coefficients per 32-bit word" byte order.
4. **An unimplemented operation code returns `ERRCODE = 3`.** The failure path.
   Only codes 7, 8 and 9 exist in hardware; the rest must report the failure
   rather than compute something. A pass here is what distinguishes a partially
   implemented accelerator from one that merely looks complete.

The reference values for checks 2 and 3 come from
`hardware/model/ref_model.py` and are themselves verified first: the Keccak
permutation is cross-checked against `hashlib.shake_128` by walking the FIPS 202
sponge by hand (168 output bytes = 21 lanes compared byte for byte), and the NTT
is checked through the forward/inverse round-trip relation
`invntt(ntt(x)) ≡ x·2^16 (mod q)`.

## Triaging failures

The three failure classes point at disjoint sets of causes. Identifying which one
you are in is most of the diagnosis.

### `VERSION` is wrong (`0x00000000`, `0xFFFFFFFF`, or garbage)

The control plane is not reaching the accelerator's registers. Nothing about the
datapath can be concluded yet.

* Bitstream not actually loaded. `Overlay()` returning without an exception is not
  proof; check `overlay.bitfile_name` and the PL state.
* `.hwh` and `.bit` from different builds, so the address in the `.hwh` points at
  something else. `pqc_accel.py` prints a warning when the resolved base differs
  from `0x43C00000`.
* Address map changed in `create_project.tcl` without updating
  `include/pqc_accel_zynq.h` and `ACCEL_BASE` in `pqc_accel.py`. All three have to
  agree.
* `FCLK_CLK0` not enabled or `aresetn` held low: with no clock, AXI4-Lite reads
  never complete their handshake and either hang or return whatever the
  interconnect substitutes.
* A hang rather than a wrong value means the read never got a response — that is
  the clock/reset case, not the address case.

### `VERSION` is right but `DONE` never gets set

The registers work, so this is the command path: either `START` never took effect
or the accelerator never got its input.

* `PqcAccelTimeout` naming the **input (MM2S)** channel: the accelerator is not
  accepting data. `TREADY` stays low while `BUSY` is set, so a previous command
  left the accelerator busy — try `accel.reset()` first. If it persists, the
  stream connection between `axi_dma_0/M_AXIS_MM2S` and `pqc_accel_0/s_axis` is
  suspect.
* `PqcAccelTimeout` naming `STATUS.DONE`: the input arrived but the datapath is
  not running. At 100 MHz the longest command takes about 13 µs, so a 1 s timeout
  is four orders of magnitude of margin — this is never "still computing".
  Suspect the accelerator's `aclk`/`aresetn`, or a `MODE`/`IN_LEN` pair the
  hardware rejects (that case sets `ERR`, so check `accel.status` and
  `accel.errcode` by hand).
* `PqcAccelTimeout` naming the **output (S2MM)** channel: the accelerator
  finished and set `DONE`, but no result came back. The `pqc_accel_0/m_axis` →
  `axi_dma_0/S_AXIS_S2MM` connection, or the HP0 path from the DMA to DDR.
* `accel.last_poll_count` tells the two "never" cases apart from a slow one: a
  large count means the loop really ran against a live register.

### `VERSION` is right, `DONE` is set, but the result is wrong

The plumbing works and the computation does not. `selftest.py` prints the offset
or index of the first difference, which localises this immediately:

* **Every byte differs**: byte order or packing. The NTT payload is 256 signed
  16-bit little-endian coefficients, two per 32-bit word, low half first.
* **The result is the previous command's output**: the input did not reach the
  PL before `START`, or the output was drained twice. A result can be taken
  exactly once; taking it again requires reissuing the command.
* **A few scattered coefficients differ**: an arithmetic bug in the RTL, or an
  input outside the range the cores are specified for. Reproduce it in
  `hardware/tb/cocotb/test_axi.py`, which compares against the same reference
  model without any board in the loop, and bisect there — a simulator gives you
  waveforms, a board gives you a wrong number.
* **The result changes between runs with identical input**: not a logic bug.
  Timing closure at 100 MHz (read `timing.rpt` from the Vivado build) or, on the
  C path only, a cache-coherency mistake. It cannot be coherency on the PYNQ
  path, because `allocate` buffers are coherent.
