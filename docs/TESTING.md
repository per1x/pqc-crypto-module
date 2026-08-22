**English** · [中文](TESTING.zh-CN.md)

# Testing

What is checked, by what means, and how to reproduce every number quoted in this
repository.

- [Summary](#summary)
- [Principles](#principles)
- [RTL verification](#rtl-verification)
- [Implementation-flow assertions](#implementation-flow-assertions)
- [On-silicon results](#on-silicon-results)
- [Entropy](#entropy)
- [Host software tests](#host-software-tests)
- [Reproducing](#reproducing)

## Summary

| Check | Result | Where |
|---|---|---|
| cocotb RTL regression | 200 tests, 0 failures | `./tools/rtl_sim.sh` |
| Verilator `-Wall` + Icarus lint | 70 modules, 0 warnings | `./tools/rtl_lint.sh` |
| Yosys synthesisability | 68 modules, all synthesise | `./tools/rtl_synth_check.sh` |
| ML-KEM 512/768/1024 vs NIST ACVP, on silicon | 20 / 20 byte-exact | `board/logs/RESULT_seckem3.txt` |
| Board self-test | 24 / 24 | `board/logs/RESULT_hwtest.txt` |
| Post-audit board re-run | PASS 25 / FAIL 0 / SKIP 6 | `board/logs/RESULT_audit.txt` |
| Boundary counter-proof | 0 key words in 48 readable addresses | `board/logs/RESULT_hwtest.txt` |
| AxPROT gate, both directions | closed | `board/logs/RESULT_secproof.txt`, `RESULT_secneg.txt` |
| TRNG min-entropy | H = 0.871234 bit/sample | `tools/sp800_90b.py` |
| SP 800-90B restart test | H_restart = 0.745427, pass | `board/logs/RESULT_restart.txt` |
| Decaps timing, valid vs implicit-reject | median difference 0.000 % | `board/logs/RESULT_seckem3.txt` |
| End-to-end through the SDF interface | pass, plus 6/6 counter-proof (measured pre-RAZ/WI, so recorded as DECERR) | `board/logs/RESULT_service.txt` |
| `ctest` (host software) | 46 / 46, 4109 assertions | `ctest --test-dir build` |
| ASan + UBSan / TSan / `leaks` | clean · 0 races · 0 leaks | `docs/USAGE.md` |
| libFuzzer | 1.38 M executions, no crashes | `./tools/fuzz.sh` |
| aarch64 Linux (GCC 12) | clean | `./tools/aarch64_test.sh` |
| RAZ/WI boundary counter-proof, on silicon | 6 / 6 — every `SECURE_ONLY=1` core reads back 0 where `0x00010000` lives; the `SECURE_ONLY=0` control reads its real value | `board/logs/RESULT_secneg.txt` |
| **No user-space program can crash the board**, on silicon | 11 / 11 — nine address classes, 2000 reads + 2000 writes each (36,000 accesses), board alive | `board/logs/RESULT_nocrash.txt` |
| Network survives PL reconfiguration | eth1 (PS GEM) stays up across driver unbind + crypto bitstream load | `board/logs/deadman_eth1.log` |
| Malformed wire requests | 26 / 26 — no request hangs the service, kills it, or is silently computed as something else | `service/wire_fuzz.py` |
| Remote call (**mTLS**) | full nine-section demo from another machine (KEM / ML-DSA at all three parameter sets / four block modes / SM3 / stale-handle counter-proof); **a client certificate from a different CA is refused**, measured against the real board | `tools/demo_remote.sh`, `service/sdf_demo.c` |
| The mTLS layer itself | 4 / 4 — one positive plus three negatives (foreign CA / no client certificate / CN not in the ACL). **The negatives are the point**; "it connects" only proves the configuration is not broken | `tools/tls_regress.sh` |
| Two P0 concurrency defects | each is demonstrably **red before the fix**: liboqs RNG cross-talk 849/3000 times, self-test gate inconsistent 852 862 times | `tests/unit/test_crypto_concurrent.c` |

> ⚠️ The "liboqs RNG cross-talk" figure is **historical**: the liboqs
> dependency has since been removed entirely (the vendored
> mlkem-native / mldsa-native is used instead, see
> `src/crypto/pqc_native.c`), taking the process-global RNG and its
> lock with it. The test stays — the property it checks (no cross-talk
> between the two paths under concurrency) must still hold.
| Session-close ABA | before the fix, four threads closing one handle succeeded 311 times (should be 300) and a bystander's session was wiped twice | `tests/unit/test_slot_concurrent.c` |
| Keystore fail-closed | a corrupt or unreadable keystore makes the daemon refuse to start **without touching the file** | `tools/daemon_failclosed.sh` |
| Security state survives power loss | PIN failure counts, lockout and unlock are **on disk without any save call** (simulated pull-the-plug, then reload) | `tests/unit/test_keystore.c` |
| Both rollback-anchor strengths | the file anchor **can indeed be bypassed by rolling both files back** (the weakness is pinned as a test); a hardware-monotonic anchor defeats the same attack | `tests/unit/test_keystore.c` |
| No stub root key in PRODUCTION | the literal is absent from the object file, **with a null control** (it must be present in DEV) | `tools/check_profile.sh` |
| eMMC RPMB | the hardware is sufficient (4 MB, kernel support) and the authentication key is verified cryptographically before use (correct key 3/3, wrong keys 0/8) | `board/src/rpmb_probe.c`, `rpmb_tool.c`, `rpmb_verify.c` |

## Principles

Three habits run through every test in this repository.

**Independent oracles.** A result is not trusted because it matches this
project's own model. Keccak is checked against the published all-zero
permutation vector and against `hashlib`/OpenSSL. The NTT is checked against a
schoolbook negacyclic convolution that never touches the twiddle table, and by
reconstructing ML-KEM key generation and reproducing ACVP `ek`/`dk`
byte-for-byte. KMAC is checked against the NIST document, OpenSSL, and a
separate Keccak.

**Structural checks over habits.** Properties no functional test can observe —
the key derivation root having no read-back interface, no secret-dependent
branch or index in `src/`, every key-material field wiped by its destructor —
are expressed as scanners wired into `ctest`. Each scanner **self-tests on
synthetic samples first**: a scanner that cannot find anything proves nothing
when it reports a clean result.

**Negative controls.** Assertions are validated by breaking something and
confirming the test fails — perturbing a twiddle factor, dropping an NTT layer,
flipping a bit in a Keccak round constant, removing a lock under TSan, adding a
fake key-readback function, timing a deliberately early-returning comparison,
probing a stack frame that was never wiped.

The same discipline governs how results are read. The boundary counter-proof
scans 256 bytes on each of two slaves and reports **both** that no key word
appears among the 48 readable addresses **and** that the ciphertext those keys
produced is correct. Either half alone proves nothing: silence could mean the
key was never loaded.

## RTL verification

251 cocotb tests across 26 top levels, run under Icarus Verilog (`tools/rtl_sim.sh`):

| Group | Tests |
|---|---|
| ML-KEM operators and datapath | `mont_reduce`, `butterfly`, `basemul`, `ntt_core`, `compress`/`decompress` (5 widths), `sample`, `bitpack`, whole KeyGen/Encaps/Decaps |
| ML-DSA operators | `tb_mldsa_units` (8), `mldsa_ntt_core` (5), `tb_mldsa_keygen` (8) |
| Keccak | `keccak_f1600` (5), `sha3_core` (9) |
| Bus | `axi4lite_xbar` (8, plus 8 more at the board's NS=7), `axi4lite_firewall` (6), `pqc_accel_axi` (16), `key_vault` (4), `key_vault_axi` (6), `mlkem_axi` (13), `mldsa_axi` (24) |
| Symmetric and SM | `aes_core` (5), `sm4_core` (6), `sm3_core` (6), `sym_vault_top` (5) |
| Fan | `fan_ctrl` (7), `sysmon_drp` (3) |
| TRNG | `trng_health` (8), `trng_source` (3), `trng_cond` (4), `trng_top` (4 + 2 no-drop + 1 drop), `trng_axi` (6), alarm path (4), raw tap (3) |

⚠️ **The 24 `mldsa_axi` tests are an end-to-end check of the whole chain, not
just a bus-layer one.** They drive the real `mldsa_engine` (the behavioural stub
is gone) and are judged against the **official ACVP vectors** (KeyGen's pk/sk and
Sign's σ byte for byte; Verify's pass *and* fail verdicts) plus
`hardware/model/mldsa_oracle.py`, which covers the cases ACVP cannot express —
chiefly **signing from a key slot**: an sk can only reach the vault through a real
KeyGen, and none of the 90 ACVP siggen secret keys appears in the keygen vectors.
The same σ is therefore pinned by two independent paths: the software-supplied-sk
path against official ACVP, the from-slot path against the oracle, and the two
must be byte-identical.

That run covers **ML-DSA-44 only** (the default when no parameters are passed).
The full matrix over all three parameter sets is the **twelve cells** of
`tools/mldsa_grid.sh` (KeyGen / Sign / Verify / **AXI** × 44 / 65 / 87, 134 tests):
Verilator for the ~15-minute development loop, Icarus as the pre-merge gate.
⚠️ Verilator is a two-state simulator and **does not propagate X**, which is
exactly the first entry in this project's defect table ("empty sensitivity list →
X on the output") — so never run Verilator alone.

Lint runs each module as its own top level, not only the integrated top:
otherwise modules that nothing instantiates — combinational operators, samplers
— are never elaborated and never checked. Genuine design intent is waived
individually in `hardware/rtl/lint_waivers.vlt`, matched by file and signal name
so that a new warning of the same class is not swallowed with it. A second pass
with Icarus catches a different class of problem; the two simulators' differing
width-truncation semantics are themselves a cross-check.

## Implementation-flow assertions

`hardware/syn/impl_bitstream.tcl` checks immediately after synthesis and aborts
on failure, so that a defect is not discovered thirty-five minutes later or,
worse, on the board.

| Assertion | What it cost to learn |
|---|---|
| PS `aclk` / `rlast` / `bid` / `rid` must be driven | Two board hangs, two power cycles |
| `fan` must land on `PACKAGE_PIN AA11` / LVCMOS33 | A wrong fan pin has no runtime symptom; it just quietly overheats the die |
| `SYSMONE4 SIM_DEVICE = ZYNQ_ULTRASCALE` | A full 35-minute implementation failing at the final DRC |
| No bitstream if WNS < 0 | A non-converging bitstream on the board produces failures that look like algorithm bugs, and are very expensive to chase |
| Effective hold margin ≥ 0.050 ns | Three bitstreams stopped at the blade's edge (see below) |

The hold-margin floor is worth its own note. Measured WHS across three
bitstreams was +0.001 / +0.010 / +0.013 ns — all positive, all signed off by
Vivado, all essentially zero, and drifting with the RTL. A hold violation cannot
be fixed by lowering the clock. The fix was to raise the requirement and let the
router meet it:

```tcl
set_clock_uncertainty -hold 0.100 [get_clocks -of_objects [get_pins u_div/O]]
```

WHS moved −0.150 → −0.027 → +0.010 during routing, buying about 0.16 ns of real
margin, for 0.14 ns of setup margin out of 3.5 ns available. Effective hold
margin went from 0.010 ns to **0.110 ns**. The floor assertion caught all three
of the earlier bitstreams on its first run; without it they would each have
shipped silently.

## On-silicon results

**ML-DSA, verified in both bitstream forms.** All three parameter
sets' KeyGen/Sign/Verify match ACVP byte for byte, plus the on-chip signing
vault and runtime parameter-set switching:

| Check | Demo form `SECURE_ONLY=0` | Secure form `SECURE_ONLY=1` |
|---|---|---|
| Normal world via `/dev/mem` | ACVP self-test **32/32** (`RESULT_mldsa_demoform.txt`) | All five slots read zero; the direct KAT program stops on its own without writing a byte; the board does not crash (`RESULT_secform_mldsa.txt`) |
| Daemon via `/dev/secmmio` → EL3 whitelist | End to end for all three sets | End to end for all three sets (same log, section ④) |
| Isolated verify, across sets and verdicts | 61/61 agreeing with ACVP (`RESULT_ctbprobe_demoform.txt`) | — |

Those vault checks pin "the private key never leaves the hardware": `OUT_LEN`
stops exactly at the public key's length, seeking the read cursor into the sk
region returns all zeros, and the signature produced from the slot is byte-identical
to the one produced when software supplies sk itself.

> ML-DSA is only reachable in the secure form because the BL31 SiP whitelist now
> covers slot 6 (`0x8006_0000`). That table used to stop at slot 5, so the core
> looked present but unreadable — which points debugging at the bitstream and the
> RTL instead of at one missing table row. **Adding a slave means adding that row.**

> A Verify defect that simulation could not see was caught here: the c̃ verdict
> compared all 512 bits while each run writes only the low `ctb` bytes, so the
> high bytes were whatever the previous run left. Reset clears them in simulation;
> on the board the PL is never reset after loading. Fixed; see pitfall V8.

**Algorithms.** ML-KEM 512/768/1024 KeyGen/Encaps/Decaps against NIST ACVP
prompt/expectedResults pairs: 20/20 byte-exact. Lengths are derived in RTL from
the `pset` field, so software cannot report a wrong one. AES-128/256 against
FIPS 197 C.1/C.3, SM4 against GB/T 32907 A.1, SM3 against GB/T 32905 A.1.

**Post-audit re-verification** (`RESULT_audit.txt`), after changes to the
decode, the erase machine and the TRNG sampling FIFO:

- *Address aliasing*, 13/13 — five positive reads succeed; eight mirror
  addresses (+0x110, +0x100, +0x1100, +0xFF00, +0x8000, outside the aperture,
  slot 6, slot 7) are all refused.

  > **This measurement predates the RAZ/WI change**, so what it actually
  > recorded was DECERR/SIGBUS on those eight. The refusal itself is what the
  > test asserts and that is unchanged; only the observable differs (they now
  > read back 0). **Re-running it on the RAZ/WI bitstream is pending** — the
  > board's SSH key did not survive the last power cycle. Simulation covers the
  > new observable (`test_xbar`, 8/8, including the exhaustive 64 KB sweep).
- *Zeroize*, 4/4 — `STATUS.WIPING` asserts, holds 112.5 µs against a theoretical
  109.2 µs (8192 cycles @ 75 MHz; the difference is polling overhead), and
  *(that measurement predates the on-chip key vault; the wipe now covers a
  third, 16 KB block and takes 16384 cycles ≈ 218 µs — re-measurement pending)*
  afterwards `OUT_LEN = IN_PTR = 0` with the same input reproducing 2432
  identical bytes.
- *TRNG*, 3/3 — `DROPS = 0` with `BLOCKS = 17892` absorbed while nobody was
  reading. Before the fix this counter read 65535, saturated.
- *Illegal parameters*, 4/4 — `mode=3`, `pset=3`, and both: `PARAM_ERR` sets,
  `BUSY` never rises, the previous `DONE`/`OUT_LEN` are invalidated, and a legal
  parameter set runs normally afterwards.

The algorithm regression ran in the same round and was unchanged, 24/24 — three
subsystems modified, not one output byte different.

**Throughput** @ 75 MHz, 20 runs per operation:

| Parameter set | KeyGen | Encaps | Decaps |
|---|---|---|---|
| ML-KEM-512 | 1.08 ms (924/s) | 0.75 ms (1339/s) | 0.98 ms (1018/s) |
| ML-KEM-768 | 1.65 ms (605/s) | 1.12 ms (895/s) | 1.44 ms (694/s) |
| ML-KEM-1024 | 2.27 ms (440/s) | 1.57 ms (636/s) | 1.96 ms (510/s) |

These include per-byte software AXI traffic and are **not** a hardware core
time. They are published for the ratio — roughly 1 : 1.5 : 2.1 across parameter
sets, matching the k = 2/3/4 workload — which is the part that means something.

**Resources and timing**, full place-and-route on `xazu3eg-sfvc784-1-i`:

| | |
|---|---|
| CLB LUTs | 35,659 / 70,560 (50.54 %) |
| Registers | 25,977 (18.41 %) |
| Block RAM | 15.5 / 216 (7.18 %) |
| DSP | 140 / 360 (38.89 %) |
| External pins | 1 (fan, `AA11`) |
| WNS / effective hold margin | +3.325 ns / +0.110 ns @ 75 MHz |

Delta from the RAZ/WI change: **+48 LUT, +61 registers, no change to DSP or
BRAM.** That is the four new saturating 16-bit counters (decoder read/write,
TRNG read/write) plus their register read paths — the RAZ/WI response itself is
*cheaper* than DECERR was, since it replaces a response-code multiplexer with a
constant. Effective hold margin is unchanged at +0.110 ns; setup fell from
+3.504 to +3.325 ns, still 3.3 ns of slack against a 13.3 ns period.

The flow is deterministic on the build machine: lint cleanup (width fixes and
dead-code removal) before and after produced bit-identical resource and timing
results, which is what one would expect — Vivado already removes dead code, and
width fixes do not change the logic implemented.

## Entropy

1,048,576 **pre-conditioning** samples were exported using a characterisation
bitstream (`RAW_TAP=1`; in the production build that path does not exist at all,
rather than returning zeros). Pre-conditioning is mandatory: a SHA-3 sponge's
output looks uniform regardless of how little entropy went in, so assessing
`RDATA` yields a beautiful and meaningless number.

```
$ python3 tools/sp800_90b.py /tmp/trng_bits.bin
  MCV 0.996191 · Collision 0.871234 · Markov 0.996108 · Compression 1.000000
  t-Tuple 0.923437 · LRS 0.993807 · MultiMCW 0.999567 · Lag 0.987625
  MultiMMC 0.994768 · LZ78Y 0.993032
  min-entropy (minimum of the ten)    0.871234
```

Supporting statistics: proportion of ones 0.500064, longest run 19 (expected
≈ 20 over 1 M bits), lag 1–8 autocorrelation all under 0.4 %, no positional bias
within the 32-bit words, and no health alarm during capture.

> **The tool is not the NIST reference implementation.** The build machine has
> no internet access and no copy of NIST's EntropyAssessment.
> `tools/sp800_90b.py` implements SP 800-90B (2018-01 final) §6.3.1–6.3.10
> directly, and its credibility rests on reproducing the worked example in each
> section of the specification: `python3 tools/sp800_90b.py --selftest`
> reproduces nine of them to four decimal places. The tenth, LZ78Y, has no
> worked example in the specification and shares its verified tail computation
> with the other four predictors.

**The measurement invalidated the previous cutoffs**, in two different
directions:

| Parameter | What the old value actually meant at the measured H | Consequence |
|---|---|---|
| `RCT = 41` | α ≈ 2⁻³⁴·⁸ at ~9.4 M samples/s | A false alarm every 55 minutes — too often for an always-on source |
| `APT = 793` | Trigger probability 5.5 × 10⁻⁵² | The test could never fire. Every previous "no APT alarm" record was worthless |

Recomputed at α = 2⁻⁴⁰ (about one false alarm per 33 hours at this sample rate):
**RCT 41 → 47, APT 793 → 672**. α is not copied from the specification's 2⁻²⁰,
which at 9.4 M samples/s would mean a false alarm every 0.11 s — **α has to be
chosen for the sample rate**. As a check on the derivation, the formula was
first re-evaluated at H = 0.5 and reproduced exactly the 41/793 already in the
RTL, before being used to move forward.

> Capture is **gapped**: the tap FIFO is 64 words deep and drops new samples
> when full rather than back-pressuring the noise source, because
> back-pressuring would alter the very stream being assessed. This does not
> affect min-entropy estimation, but restart-test data has to be collected
> separately (it was; H_restart = 0.745427, pass).

## Host software tests

46 `ctest` targets, 4109 assertions: slot FSM and concurrency, keystore and
wrapping, backup/Shamir/injection, audit chain and anchoring, PKCS#11, KAT
parsing, constant-time timing, zeroization, and the accelerator transports —
including the assertion that the software stub and the Verilator-simulated RTL
agree byte for byte.

390 NIST ACVP vectors are checked byte-exact in software, with 60 explicitly
skipped and reported rather than silently passed.

Sanitiser and platform runs: ASan + UBSan clean; ThreadSanitizer 0 races
(validated by removing locks, which reports 9); macOS `leaks` clean; libFuzzer
1.38 M executions with no crashes; aarch64 Linux under GCC 12, clean.

## Reproducing

Nothing in this section needs the board.

```bash
./tools/rtl_sim.sh                     # 251 cocotb tests
sh tools/mldsa_grid.sh                 # ML-DSA twelve-cell matrix (Verilator, ~15 min)
sh tools/mldsa_grid.sh icarus          # same matrix under Icarus — the pre-merge gate
./tools/rtl_lint.sh                    # Verilator -Wall + Icarus
./tools/rtl_synth_check.sh             # Yosys synthesisability
python3 tools/sp800_90b.py --selftest  # reproduce the specification's worked examples
python3 tools/ct_audit.py --self-test  # constant-time scanner, controls first
python3 tools/check_zeroize.py --self-test

./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Bitstream, on a machine with Vivado 2020.1 (~35 minutes):

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm.bit
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit  (low fan thresholds + raw TRNG tap;
#    a characterisation build, NOT the product form)
```

On the board, every action that touches the PL must go through the harness —
see [USAGE.md](USAGE.md#on-the-board).

## CI gating: which jobs should be required (**must be enabled in the web UI**)

`main` currently has **no branch protection and no required checks** — whether
`.github/workflows/ci.yml` passes does not block any push. This section lists what
should be marked required. **The setting itself can only be made in the GitHub web UI**
(Settings → Branches → Add branch ruleset); no file in the repository can do it, so all
this document can do is say what to configure.

| Job (`name:` in `ci.yml`) | Covers | Why it must block |
|---|---|---|
| `RTL — lint, simulate, synthesise` | Verilator/Icarus lint, full cocotb regression, Yosys synthesisability | an RTL mistake costs seconds in simulation and a power cycle on the board |
| `Host software — build and test` | `cmake` + `ctest` (51 tests) | the two P0 concurrency regressions live here, and they **stay green forever under single-threaded runs** |
| `Static analysis` | constant-time audit, zeroization structure check, SP 800-90B self-test | these test properties functional tests cannot observe |
| `Service layer` | daemon/client build + `tls_regress.sh` | the remote port is mTLS and three of its four cases are negatives — nothing else catches a missing `FAIL_IF_NO_PEER_CERT` |

Two settings worth enabling alongside:

- **Require a pull request before merging**, at least one review. Not process hygiene:
  a great many decisions in this repository live in comments ("do not change this
  back"), and those are only ever read during review; pushing straight to `main`
  bypasses the one moment they get read.
- **Require branches to be up to date before merging**: RTL and host share constants
  (`PQCS_MAXPAY` in `wire.h`, register offsets), and "both branches green, merge red"
  is the normal failure here.

⚠️ The on-board checks (the nine-section `--smoke`, `wire_fuzz.py`, RPMB) **cannot go
into CI** — they need the real board. They belong to a manual pre-merge pass; the list
is in the table above. Do not mark them required: a required check that can never get a
runner locks the repository.
