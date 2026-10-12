**English** · [中文](README.zh-CN.md)

# Hardware

RTL for the PL (FPGA) side of the HSM, the cocotb testbenches, the Python reference
models that produce golden vectors, and the Vivado scripts. The PL does the
cryptographic computation (ML-KEM, ML-DSA, SHA-3, AES, SM4, SM3) and hosts the TRNG.
It stores no long-term keys: the OP-TEE trusted application holds keys as wrapped blobs
and loads them into the PL only for the duration of an operation (see
[../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) and [../docs/SECURITY.md](../docs/SECURITY.md)).

The sources are built into the bitstream that runs on the XCZU3EG board. The simulation
suite described below is the first check; ACVP and the on-board tests are the second
(see [../docs/TESTING.md](../docs/TESTING.md)).

```
hardware/
├── rtl/
│   ├── mlkem/      ML-KEM: NTT, base multiply, reduction, sampling, compression,
│   │               packing, and the KeyGen / Encaps / Decaps cores
│   ├── mldsa/      ML-DSA: NTT, rounding, hints, sampling, packing, parameter table,
│   │               the KeyGen / Sign / Verify cores, and the mldsa_engine adapter
│   ├── keccak/     keccak_f1600.v, sha3_core.v
│   ├── sym/        aes_core.v, sbox.v, sm4_core.v, sm3_core.v
│   ├── trng/       ring oscillators, health tests (RCT/APT), conditioning, AXI slave
│   ├── bus/        AXI4-Lite firewall, symmetric key registers, per-core AXI slaves
│   ├── board/      axi4lite_xbar.v, zu3eg_hsm_top.v
│   ├── common/     ram_dp.v, sync_fifo.v
│   └── lint_waivers.vlt
├── platform/       Fan control (not cryptographic), see platform/README.md
├── tb/cocotb/      cocotb testbenches, simulation-only top levels, Makefile
├── tb/lint/        Vendor-primitive stubs for lint (not synthesised)
├── model/          Python reference models, vector export, independent oracles
└── syn/            Vivado out-of-context synthesis and the bitstream flow
```

The algorithm cores are plain Verilog-2001 with no vendor primitive. The vendor
primitives and IP are `BUFGCE_DIV` and the PS instance in `rtl/board/zu3eg_hsm_top.v`
and `SYSMONE4` in `platform/fan_ctrl/fan_sysmon.v`.

## Board top level

`zu3eg_hsm_top` connects the PS port `M_AXI_HPM0_LPD` (AXI4, treated as AXI4-Lite with
`rlast = rvalid` and the IDs echoed back) to `axi4lite_xbar`, which decodes
64 KB slots starting at `0x8000_0000`. The crypto cores run at 75 MHz (`pl_clk0` at
150 MHz through `BUFGCE_DIV` /2). A miss in the decoder, and any access rejected by a
firewall, is answered RAZ/WI (reads 0, write dropped, response OKAY), so a stray access
cannot raise an AXI error.

| Slot | Address | Module | Contents |
|---|---|---|---|
| 0 | `0x8000_0000` | `trng_axi` | TRNG |
| 1 | `0x8001_0000` | `key_vault_axi` | symmetric key registers (8 slots x 256 bit) |
| 2 | `0x8002_0000` | `sym_axi` | AES / SM4 / SM3 (slots 1 and 2 are the two slaves of `sym_vault_top`) |
| 3 | `0x8003_0000` | `mlkem_axi` | ML-KEM-512/768/1024 |
| 4 | `0x8004_0000` | `key_vault_axi` | canary, a copy of slot 1 with `SECURE_ONLY` fixed at 1 and no use port connected |
| 5 | `0x8005_0000` | `fan_ctrl_axi` | fan observation port (no AxPROT gating) |
| 6 | `0x8006_0000` | `mldsa_axi` | ML-DSA-44/65/87 |

In the default build the functional slaves (slots 0, 1, 2, 3, 6) are built with
`SECURE_ONLY=1`: only transactions with `AxPROT[1]=0` (secure) reach them. Building with
`PQC_DEV_OPEN=1` gives `zu3eg_hsm_dev.bit`, where these slaves use `SECURE_ONLY=0` so
Linux can drive them directly for KAT runs. Even then the key-register load port
(`SLOT_SEL`, `KEY_IN`, `SLOT_CTRL`) and the seed-staging ports of `mlkem_axi` and
`mldsa_axi` accept secure writes only. Register layouts are in
[../docs/REGISTERS.md](../docs/REGISTERS.md).

## Key handling in the PL

- `key_vault` (wrapped by `key_vault_axi`) is a set of 8 key registers, 256 bit each.
  A key is written through the load port (`SLOT_SEL`, `SLOT_CTRL` BEGIN, eight `KEY_IN`
  words, `SLOT_CTRL` COMMIT) and read only by the symmetric cores through the use port.
  No register address returns key material.
- Loads from the normal world are dropped and counted in `LOAD_REJ` (`0x34`).
  `CTRL.ZEROIZE` is not gated. `sym_axi` also drives `ext_zeroize` into the key registers.
- The registers are wiped in one cycle on `tamper`; after that, loads are refused until
  reset. On the board `tamper` comes from the `SYSMONE4` over-temperature output.
- For ML-KEM and ML-DSA, `MODE[4]=CHAIN` makes the PL expand the private key from a
  staged seed, run the operation, and wipe the expanded key. Software waits for
  `KEYSTAT[0]` (`EXP_WIPING`) to clear after each operation. `CTRL.SEED_LOCK` is a latch
  that only a power cycle clears. The ACVP KeyGen path, where software supplies `d‖z`,
  still returns `dk` through `OUT_DATA`.

## Modules

### `rtl/mlkem/`

| File | Modules |
|---|---|
| `mont_reduce.v` | `mont_reduce` (combinational) |
| `butterfly.v` | `barrett_reduce`, `butterfly_ct`, `butterfly_gs`, each built from `_head` + `_tail` |
| `ntt_core.v` | `ntt_core`: one butterfly unit, 7-layer NTT, coefficients in `ram_dp`; 3457 cycles per transform |
| `basemul.v` | `mlkem_basemul` (five Montgomery multiplies, `_head` + `_tail`), `mlkem_bmzeta` (the ζ table for base multiply) |
| `compress.v` | `mlkem_compress`, `mlkem_decompress`, parameterised by `D` |
| `sample.v` | `mlkem_cbd2`, `mlkem_cbd3`, `mlkem_rej_pair`, `mlkem_rej_uniform` |
| `cbd_stream.v` | `mlkem_cbd_stream`: SHAKE256 byte stream to 256 CBD coefficients |
| `pack.v` | `mlkem_encode12`, `mlkem_decode12` |
| `bitpack.v` | `mlkem_bitpack`, `mlkem_bitunpack`: ByteEncode_d / ByteDecode_d with `d` as a runtime input |
| `keygen.v`, `encaps.v`, `decaps.v` | `mlkem_keygen`, `mlkem_encaps`, `mlkem_decaps`: the complete FIPS 203 internal algorithms. Decaps re-encrypts by instantiating `mlkem_encaps`, compares ciphertexts in constant time, and uses implicit rejection J(z‖c) |

### `rtl/mldsa/`

| File | Modules |
|---|---|
| `mont_reduce.v` | `mldsa_mont_reduce`, `mldsa_mont_mul_pipe` (5-stage `mont(x·y)`, shared by the NTT and pointwise loops) |
| `butterfly.v` | `mldsa_butterfly_ct`, `mldsa_butterfly_gs` (reference forms), `mldsa_butterfly_pipe` (CT / GS / scaling on one multiplier) |
| `ntt_core.v` | `mldsa_ntt_core`: pipelined, 8 layers, ping-pong `ram_dp`; 1081 cycles forward, 1344 inverse |
| `reduce.v`, `rounding.v` | `mldsa_reduce32`, `mldsa_caddq`, `mldsa_power2round`, `mldsa_decompose`, `mldsa_make_hint`, `mldsa_use_hint` |
| `sample.v` | `mldsa_rej_uniform`, `mldsa_rej_eta`, `mldsa_rej_uniform_buf` |
| `sampler.v`, `expand_mask.v`, `sample_in_ball.v` | `mldsa_poly_uniform`, `mldsa_poly_eta`, `mldsa_expand_mask`, `mldsa_sample_in_ball` (drive the shared `sha3_core`) |
| `pack.v`, `unpack.v` | `mldsa_bitpack`, `mldsa_polyt1_pack`, `mldsa_polyt0_pack`, `mldsa_polyeta_pack`, `mldsa_bitunpack` |
| `params.v` | `mldsa_params`: the FIPS 204 parameter table, indexed by parameter set |
| `keygen.v`, `sign.v`, `verify.v` | `mldsa_keygen`, `mldsa_sign`, `mldsa_verify` |
| `mldsa_engine.v` | `mldsa_engine`: one byte-stream write port in front of the three cores. The parameter set (44/65/87) is chosen per operation at run time |

The header comments of `keygen.v`, `sign.v` and `verify.v` still describe the first
implementation stage; the files contain the complete algorithms.

### Other directories

| File | Module | Notes |
|---|---|---|
| `keccak/keccak_f1600.v` | `keccak_f1600` | one round per cycle, 24 cycles per permutation |
| `keccak/sha3_core.v` | `sha3_core` | sponge (padding, absorb, squeeze) around the permutation |
| `sym/aes_core.v` | `aes_core` | AES-128/256, encrypt and decrypt; loading a key takes 41 (AES-128) or 53 (AES-256) cycles, a block 11 or 15 |
| `sym/sm4_core.v` | `sm4_core` | 32 cycles for the key schedule, 33 per block |
| `sym/sm3_core.v` | `sm3_core` | SM3 with padding inside the core |
| `sym/sbox.v` | `aes_sbox`, `aes_inv_sbox`, `sm4_sbox` | lookup tables generated by `sym_oracle.py --emit-sbox` |
| `trng/` | `ring_osc`, `trng_source`, `trng_health`, `trng_cond`, `trng_top`, `trng_axi` | multi-ring-oscillator source, RCT/APT health tests (SP 800-90B), Keccak conditioner, FIFO, AXI slave |
| `common/ram_dp.v` | `ram_dp` | true dual-port synchronous RAM, inferred as block RAM, one cycle read latency |
| `common/sync_fifo.v` | `sync_fifo` | first-word-fall-through FIFO; `flush` overwrites the contents with zeros |
| `bus/axi4lite_firewall.v` | `axi4lite_firewall` | gates on `AxPROT`, address window and `tamper`; a rejected transaction does not reach the downstream port. Policy is set by parameters, not registers. Counts violations |
| `bus/key_vault.v`, `bus/key_vault_axi.v` | `key_vault`, `key_vault_axi` | symmetric key registers and their AXI slave (see above) |
| `bus/sym_axi.v`, `bus/sym_vault_top.v` | `sym_axi`, `sym_vault_top` | AES/SM4/SM3 slave with no key register (it selects a key slot instead), and the wrapper that joins it to the key registers |
| `bus/mlkem_axi.v` | `mlkem_axi` | slave for the three ML-KEM cores, with 8 KB input and output buffers |
| `bus/mldsa_axi.v` | `mldsa_axi` | slave for `mldsa_engine` |
| `bus/axi4lite_regs.v`, `bus/pqc_accel_axi.v` | `axi4lite_regs`, `pqc_accel_axi` | register block and accelerator top (AXI4-Lite control, AXI4-Stream data) used by the C simulation backend and cocotb. Not in the board image |
| `board/axi4lite_xbar.v` | `axi4lite_xbar` | one master, N slaves, 64 KB per slot |
| `board/zu3eg_hsm_top.v` | `zu3eg_hsm_top` | board top level |

## Design notes

- ML-KEM (q = 3329, Montgomery base 2^16) and ML-DSA (q = 8380417, base 2^32) share no
  arithmetic modules. The ML-DSA NTT runs all eight layers, so its transform-domain
  product is a pointwise multiply; ML-KEM stops after seven layers and needs the 2x2
  base multiply.
- `mlkem_compress` replaces division by q with a multiply by `ceil(2^33/q)` and a shift.
  The input domain has only 3329 values, and the testbench checks all of them.
  `mldsa_decompose` and `mldsa_reduce32` use the same technique, and the testbench
  instantiates both `MODE` values so a constant that is wrong in one parameter set fails.
- `ByteEncode_d` for `d < 12` is not a module: the coefficients already fit in `d` bits,
  so packing is wiring. `mlkem_encode12` exists because it also folds signed
  coefficients back into `[0, q)`.
- `keccak_f1600` is one round per cycle rather than a 24-round unrolled datapath. An
  ML-KEM-768 key generation needs 43 permutations, about 1000 cycles, against roughly
  20700 for the NTTs.
- Large memories are written as `ram_dp` so they map to block RAM. A register array with
  several combinational read ports is spread over LUTs (the `pqc_accel_axi` data buffer
  took about 30000 LUTs that way). `tools/rtl_synth_check.sh` lists every memory that
  Yosys spreads into registers.
- `pqc_accel_axi` implements the register semantics of `include/pqchsm/accel.h`: START
  self-clears, DONE is a latched level, status registers are written by hardware and
  read-only to software. [../docs/REGISTERS.md](../docs/REGISTERS.md) is the contract and
  `test_axi.py` checks it clause by clause with a hand-written bus functional model.
  The C build verilates `ntt_core`, `keccak_f1600` and `pqc_accel_axi` and
  `tests/unit/test_accel.c` compares the simulated RTL with the software stub.

## Verification

```bash
./tools/rtl_sim_fast.sh                   # per-module cocotb on Verilator (default set)
./tools/rtl_sim_fast.sh --list            # names it accepts
./tools/rtl_sim.sh                        # full cocotb regression on Icarus Verilog
./tools/rtl_lint.sh                       # Verilator -Wall + Icarus, every module as top
./tools/rtl_synth_check.sh                # Yosys synthesisability
python3 hardware/model/ntt_oracle.py      # NTT oracles
python3 hardware/model/mlkem_oracle.py    # ML-KEM datapath oracles
python3 hardware/model/mldsa_oracle.py    # ML-DSA datapath oracles
```

There are 272 `@cocotb.test` cases in `tb/cocotb`. `rtl_sim_fast.sh` takes a few
minutes for its default set. `rtl_sim.sh` takes about 40 minutes and should be run
before merging, because Verilator is two-valued and does not propagate X: three bugs
were found only under Icarus (an unconnected new port, an undriven input, a read of
a RAM that was never written). cocotb does not support Vivado `xsim`, so the sources
under test stay free of vendor primitives; `rtl_lint.sh` uses the stubs in `tb/lint`.

Yosys checks something neither simulator does: whether the source means the same to a
synthesiser. For example, an asynchronous reset branch that also tests a signal missing
from the sensitivity list simulates as written but synthesises differently, and Yosys
rejects it. Run `tools/rtl_synth_check.sh` after changing RTL; a reg driven from two
`always @(posedge clk)` blocks passes lint and simulation and is rejected only by
synthesis.

### Independent oracles

The golden vectors come from `ref_model.py` and `mldsa_model.py`, so agreement with them
only shows that the RTL matches the model. The oracles check the model's operators
against something independent. `mlkem_oracle.py` and `mldsa_oracle.py` also run negative
controls (a perturbed constant or table must make the oracle fail) on every invocation.

- NTT, oracle A: a schoolbook negacyclic convolution in `Z_q[x]/(x^256+1)`, which does
  not use the twiddle table, compared with `invntt(basemul(ntt(a), ntt(b)))`.
- NTT, oracle B: K-PKE key generation from FIPS 203 reimplemented in Python on top of the
  reference model's `ntt()`, reproducing NIST ACVP `ek` and `dk`.
- ML-KEM compression: `Compress_d` / `Decompress_d` from the FIPS 203 §4.2.1 definition
  in exact rational arithmetic, over all 3329 inputs for every `d` used, plus the
  `round(q/2^(d+1))` error bound.
- Binomial sampling: the FIPS 203 Alg 8 definition against the bit-parallel
  implementation, exhaustive per coefficient bit group with the other bits at zero, one
  and random.
- Rebuilding whole algorithms from the individual operators and comparing with ACVP:
  KeyGen (`ek`/`dk`), Encaps (`c`, `K`) and Decaps (`K`, including implicit rejection)
  for ML-KEM; KeyGen (`pk`/`sk`), siggen and sigver for ML-DSA.
- ML-DSA NTT: `invntt(ntt(a) * ntt(b)) == schoolbook(a, b)` modulo 8380417.
- Rounding and hints: `Power2Round` and `Decompose` against their defining
  decompositions; `UseHint(r+e, MakeHint(r0+e, r1)) == r1` for `|e| <= gamma2`.
- Keccak: the published Keccak-f[1600] output for an all-zero input, hard-coded in
  `test_keccak.py`; and SHAKE128/256 and SHA3-256 built as a sponge on the RTL
  permutation and compared with `hashlib` (the C side compares with OpenSSL).
- AES, SM4, SM3: `sym_oracle.py`, written from the standards without a crypto library.
- TRNG: `trng_health_model.py` computes the RCT/APT thresholds from their definitions,
  and `trng_cond_model.py` models the conditioner bit for bit.

## Synthesis

Scripts are in `syn/`. Vivado 2020.1 runs on the build machine only; see
[syn/README.md](syn/README.md).
