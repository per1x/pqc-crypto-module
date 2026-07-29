**English** · [中文](README.zh-CN.md)

# Hardware

RTL sources, verification, the reference model that produces golden vectors, and
synthesis scripts. Everything here runs in simulation — no board has been involved.

```
hardware/
├── rtl/
│   ├── mlkem/      mont_reduce.v, butterfly.v, ntt_core.v, basemul.v,
│   │               compress.v, sample.v, pack.v
│   ├── mldsa/      mont_reduce.v, reduce.v, butterfly.v, ntt_core.v,
│   │               rounding.v, sample.v
│   ├── keccak/     keccak_f1600.v
│   └── bus/        axi4lite_regs.v, pqc_accel_axi.v
├── tb/cocotb/      cocotb testbenches, simulation-only top levels, Makefile
├── model/          Python reference model, vector export, independent oracles
└── syn/            Vivado out-of-context synthesis scripts and constraints
```

Every module is plain inferrable Verilog-2001. No vendor primitive is instantiated
anywhere, so the same sources target Xilinx, Intel, or Lattice unchanged.

## What exists

| Module | Structure | Cycles |
|---|---|---|
| `mont_reduce`, `barrett_reduce` | combinational | — |
| `butterfly_ct`, `butterfly_gs` | combinational | — |
| `ntt_core` | single butterfly unit, 7-layer ML-KEM NTT, instantiates the above | 1153 per transform |
| `mlkem_basemul` | combinational, five Montgomery multiplies | — |
| `mlkem_compress`, `mlkem_decompress` | combinational, parameterised by `D` | — |
| `mlkem_cbd2`, `mlkem_cbd3` | combinational, bit-parallel binomial sampling | — |
| `mlkem_rej_pair` | combinational candidate extraction | — |
| `mlkem_rej_uniform` | collector, one 3-byte group per cycle | ~430 per polynomial |
| `mlkem_encode12`, `mlkem_decode12` | combinational | — |
| `mldsa_mont_reduce`, `mldsa_reduce32`, `mldsa_caddq` | combinational | — |
| `mldsa_butterfly_ct`, `mldsa_butterfly_gs` | combinational | — |
| `mldsa_ntt_core` | single butterfly unit, full 8-layer ML-DSA NTT | 1025 forward, 1281 inverse |
| `mldsa_power2round`, `mldsa_decompose` | combinational, parameterised by `MODE` | — |
| `mldsa_make_hint`, `mldsa_use_hint` | combinational, parameterised by `MODE` | — |
| `mldsa_rej_uniform`, `mldsa_rej_eta` | combinational | — |
| `mldsa_rej_uniform_buf` | collector, one 3-byte group per cycle | ~340 per polynomial |
| `keccak_f1600` | single-round iterative, `round_cnt` over 24 rounds | 24 per permutation |
| `axi4lite_regs` | AXI4-Lite slave, control and status registers | — |
| `pqc_accel_axi` | accelerator top level: AXI4-Lite + AXI4-Stream + cores | — |

ML-KEM and ML-DSA use different arithmetic — different modulus (3329 against 8380417),
different Montgomery base (2^16 against 2^32), different coefficient width — so the two
sets of modules are separate and share nothing. The ML-DSA NTT runs the full eight
layers, which is why multiplication in the transform domain is a plain pointwise product
rather than the 2x2 base multiplication ML-KEM needs after seven layers.

`mldsa_decompose` and `mldsa_reduce32` replace division by a constant with a
multiply-and-shift in the same way `mlkem_compress` does. Both `MODE` values are
instantiated side by side in the testbench, so a constant that is wrong in only one
parameter set cannot pass unnoticed.

`mlkem_compress` replaces the division by `q` with a multiply by `ceil(2^33/q)` followed
by a shift. The two are equal over the whole input domain, which is only 3329 values, so
the testbench verifies the substitution **exhaustively** rather than by sampling.

`pqc_accel_axi`'s data buffer is sized to the largest operation the hardware actually
implements — 512 bytes for the NTT — rather than to `ACCEL_BUF_MAX`, which is the
software-side bound of 16 KiB. The buffer has two read ports and a write port, so no
synthesis tool can map it to block RAM; at 16 KiB it becomes 131072 flip-flops, more
than an XC7Z020 has in total. Sizing it to what the implemented operation codes need
brings it to 4096 bits. `tools/rtl_synth_check.sh` reports every memory that gets
spread into registers for exactly this reason.

`ByteEncode_d` for `d < 12` is omitted deliberately: packing coefficients that already
fit in `d` bits is pure wiring, with no logic to get wrong. Only the 12-bit path, which
additionally folds signed coefficients back into `[0, q)`, exists as a module.

`keccak_f1600` is deliberately **not** a 24-round unrolled design. Unrolling multiplies
the round logic by 24 for no benefit at the call rates involved: 24 cycles at 100 MHz is
240 ns per permutation, and an ML-KEM-768 key generation needs 43 of them — about 1000
cycles in total, against roughly 6900 for the NTTs. The bottleneck is elsewhere.

`pqc_accel_axi` is the accelerator as a system would see it: AXI4-Lite for the control
and status registers, AXI4-Stream for bulk data, and the algorithm cores underneath. The
register semantics it implements are the ones `include/pqchsm/accel.h` was written
against — START self-clearing, DONE latched as a level, status registers written by
hardware and read-only to software. [docs/register-map.md](../docs/register-map.md) is
the contract; `test_axi.py` verifies it clause by clause with a hand-written bus
functional model and no third-party AXI library.

The cores are also driven from C through the same register interface, which means the
same RTL is exercised by cocotb and by the C test suite.

## Verification

```bash
./tools/rtl_sim.sh                        # full cocotb regression, Icarus Verilog
./tools/rtl_lint.sh                       # Verilator -Wall + Icarus, every module as top
./tools/rtl_synth_check.sh                # Yosys: synthesisability, vendor-neutral
python3 hardware/model/ntt_oracle.py      # the two independent NTT oracles
python3 hardware/model/mlkem_oracle.py    # oracles for the rest of the ML-KEM datapath
python3 hardware/model/mldsa_oracle.py    # oracles for the ML-DSA datapath
```

The C-side build also verilates both cores and asserts that the simulated RTL and the
software stub agree byte-for-byte (`tests/unit/test_accel.c`).

### Independent oracles

A result is not trusted because it matches this project's own reference model — a
self-consistent but wrong twiddle table would pass such a check. Each core is therefore
pinned to something external:

- **NTT, oracle A** — a schoolbook negacyclic convolution in `Z_q[x]/(x^256+1)` that
  never touches the twiddle table, used to verify that
  `invntt(basemul(ntt(a), ntt(b))) == schoolbook(a, b)`. This tests the *semantics* of
  the transform, not its self-consistency.
- **NTT, oracle B** — FIPS 203 K-PKE key generation reimplemented in Python calling the
  reference model's `ntt()`, reproducing NIST ACVP `ek`/`dk` byte-for-byte. This pins
  the transform to the one ML-KEM actually uses.
- **Compression** — `Compress_d` and `Decompress_d` computed from the FIPS 203 §4.2.1
  definition in exact rational arithmetic, compared against the integer implementation
  over every one of the 3329 possible inputs, for every `d` ML-KEM uses. The round-trip
  error is separately checked against the `round(q/2^(d+1))` bound the standard states.
- **Binomial sampling** — the FIPS 203 Alg 8 definition (count the Hamming weight of two
  bit runs) against the bit-parallel implementation, exhaustively over each coefficient's
  bit group with the surrounding bits held at zero, at one, and at random. This covers
  both "each group is right" and "groups do not interfere".
- **Rejection sampling** — a full `SampleNTT` run over a real SHAKE128 stream, compared
  coefficient by coefficient with an independent implementation written straight from
  FIPS 203 Alg 7.
- **The datapath as a whole** — ML-KEM key generation rebuilt from `rej_pair`, `cbd2`,
  `cbd3`, `basemul`, and `encode12`, reproducing NIST ACVP `ek`/`dk` byte-for-byte. This
  pins every one of those operators to the algorithm as standardised, not merely to a
  self-consistent set of formulas.
- **ML-DSA NTT** — the same schoolbook negacyclic convolution argument, in
  `Z_q[x]/(x^256+1)` with `q = 8380417`. Because the ML-DSA transform runs all eight
  layers, the check is `invntt(ntt(a) * ntt(b)) == schoolbook(a, b)` with an ordinary
  pointwise product in the middle.
- **Rounding and hints** — `Power2Round` and `Decompose` checked against their defining
  decompositions and value ranges across representatives of the whole coefficient field;
  the hint bits checked against the property FIPS 204 actually depends on, namely that
  `UseHint(r+e, MakeHint(r0+e, r1)) == r1` for every perturbation with `|e| <= gamma2`.
- **The ML-DSA datapath as a whole** — ML-DSA key generation rebuilt from
  `rej_uniform_coeff`, `rej_eta_coeff`, `ntt`, `invntt_tomont`, `montgomery_reduce`, and
  `power2round`, reproducing NIST ACVP `pk`/`sk` byte-for-byte.
- **Keccak, oracle 1** — the published all-zero-input Keccak-f[1600] permutation output,
  hard-coded as a constant and not generated by any code in this repository.
- **Keccak, oracle 2** — SHAKE128/256 and SHA3-256 built as a sponge *on top of the RTL
  core* and compared byte-for-byte with `hashlib` (and with OpenSSL on the C side). This
  covers padding, rate, lane endianness, and multi-block absorb/squeeze, not just the
  permutation.

Each oracle has been validated with a negative control — perturbing a twiddle factor,
dropping an NTT layer, flipping one bit of a Keccak round constant, shifting the
compression rounding constant by one, inverting the sign convention of the binomial
sampler, offsetting a base-multiplication twiddle — and confirmed to fail in each case.
`hardware/model/mlkem_oracle.py` runs its own negative controls as part of every
invocation, so a check that has stopped being able to fail reports itself.

## Simulator choice

Self-contained algorithm cores are verified with Verilator and Icarus Verilog under
cocotb; cocotb does not support Vivado `xsim`, so any future top level containing vendor
IP would be simulated separately. The side effect is useful: it forces the algorithm
cores to stay free of vendor primitives, which is also what makes them portable.

Verilator's 2-state semantics and Icarus's 4-state semantics disagree on width
truncation, and that disagreement has caught real bugs. Both are run.

A third tool answers a question neither simulator can: whether the source means the same
thing to a synthesiser. Yosys rejects, for instance, an asynchronous reset branch that
also tests a signal absent from the sensitivity list — a construct both simulators
execute exactly as written while synthesis produces different hardware.
`tools/rtl_synth_check.sh` runs it over every module.

## Synthesis

Scripts are in `syn/` and have **never been executed** — Vivado is not installed on the
development machine. See [syn/README.md](syn/README.md).
