**English** · [中文](testing.zh-CN.md)

# Test description

What is tested, by what means, and how to reproduce each result. The intent is
that every number quoted anywhere in this repository can be regenerated from a
clean checkout by running the command in the last column.

## Summary

| Check | Result | Command |
|---|---|---|
| Unit, integration, and KAT tests | 45 / 45, 4059 assertions | `ctest --test-dir build` |
| NIST ACVP vectors | 390 byte-exact, 60 explicitly skipped | `./tools/kat_evidence.sh` |
| AddressSanitizer + UndefinedBehaviorSanitizer | 45 / 45 | `ctest --test-dir build-asan` |
| ThreadSanitizer | 0 races | `ctest --test-dir build-tsan` |
| aarch64 Linux (GCC 12) | 45 / 45 | `./tools/aarch64_test.sh` |
| libFuzzer | 1.38 M executions, no crashes | `./tools/fuzz.sh` |
| cocotb RTL regression | 156 tests across 26 top levels | `./tools/rtl_sim.sh` |
| RTL lint | 31 modules, 0 warnings | `./tools/rtl_lint.sh` |
| RTL synthesisability (Yosys) | 31 modules, all synthesise | `./tools/rtl_synth_check.sh` |
| Constant-time source audit | 0 unannotated findings, 1 justified waiver | `python3 tools/ct_audit.py` |
| Zeroisation structure check | 0 gaps, 9 justified waivers | `python3 tools/check_zeroize.py` |
| Independent oracles | all pass, each with its own negative control | `python3 hardware/model/*_oracle.py` |

## Method

Three habits run through the test sources and account for most of their length.

**Independent oracles.** A result is not trusted because it matches this
project's own model. Where the model and the vectors share an origin, that is
stated and a genuinely independent check is added: a schoolbook convolution that
never touches the twiddle table, the FIPS definition computed in exact rational
arithmetic, a reconstruction of key generation from the individual operators
compared against NIST vectors byte-for-byte.

**Negative controls.** Every assertion that is supposed to be able to fail is
demonstrated failing. A twiddle factor is perturbed, an NTT layer dropped, one
bit of a Keccak round constant flipped, a lock removed under ThreadSanitizer, a
key-readback function added, a deliberately early-returning comparison timed, a
health-test threshold raised beyond reach. A check that cannot fail is not
evidence of anything, and the controls are what distinguish the two cases.

**Structural checks.** Properties no functional test can observe — that no
secret-dependent branch exists, that every key field is wiped by its destructor,
that the key derivation root has no read-back interface — are expressed as
scanners wired into `ctest`. Each scanner self-tests on synthetic samples
*before* it is permitted to report a clean scan of the real tree.

## Software tests

`ctest` runs 45 cases. Beyond the per-module unit tests, the ones worth calling
out individually:

| Case | What it establishes |
|---|---|
| `selftest` | The five known-answer tests pass, and the error state genuinely blocks every cryptographic entry point — including when the arguments are invalid, so an illegal call cannot bypass the gate |
| `accel` | The register-interface backend and liboqs produce byte-identical output for every algorithm and parameter set |
| `accel_axi` | The same, driven over real AXI4-Lite and AXI4-Stream transactions against the simulated RTL, plus the register contract as seen from software |
| `zeroize` | Key structures are wiped whole, the zeroising primitive survives `-O2`, and a plain `memset` in the same position does not |
| `ct_timing` | Welch t-test on the constant-time comparison, with a leaky control that must be flagged and a null control that must not |
| `slot_concurrent` | The slot manager under concurrent sessions; validated by removing a lock and observing ThreadSanitizer report races |
| `audit_integration` | Tampering with any record in the audit chain is detected, including rewriting the whole file, which the ML-DSA anchor catches |
| `kat_*` | 390 NIST ACVP vectors, byte-exact |

## Hardware tests

`./tools/rtl_sim.sh` runs 156 cocotb tests across 26 top levels under Icarus
Verilog. The same RTL is separately compiled by Verilator and driven from the C
test suite, so any disagreement between the two simulators' width-truncation
semantics surfaces as a test failure rather than as a silent difference.

| Top level | Coverage |
|---|---|
| `mont_reduce`, `butterfly_ct`, `butterfly_gs` | ML-KEM operators against vectors and the defining identities |
| `ntt_core` | 7-layer ML-KEM NTT, level-latched `done`, clean reset, round-trip property |
| `mlkem_basemul` | Base multiplication against the ring definition |
| `mlkem_compress`, `mlkem_decompress` | Exhaustive over the whole input domain, for every `d` |
| `tb_mlkem_units` | Binomial sampling, rejection sampling and its collector, 12-bit encoding |
| `tb_mldsa_units` | ML-DSA operators, rounding, hints, both sampling paths, both parameter sets |
| `mldsa_ntt_core` | Full 8-layer ML-DSA NTT, 2049 cycles forward, 2561 inverse |
| `keccak_f1600` | Published permutation vector, sponge against `hashlib` |
| `pqc_accel_axi` | Register map contract clause by clause, both datapaths, stream handshake gaps |
| `tb_trng_health` | SP 800-90B continuous health tests, with a null-control instance |

## Reproducing the evidence

```bash
./tools/fetch_vectors.sh                   # NIST ACVP vectors, pinned commit
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./tools/kat_evidence.sh                    # regenerates the ACVP evidence table
./tools/rtl_sim.sh                         # cocotb regression
./tools/rtl_lint.sh                        # Verilator + Icarus lint
./tools/rtl_synth_check.sh                 # Yosys synthesisability check
python3 tools/ct_audit.py --self-test && python3 tools/ct_audit.py
python3 tools/check_zeroize.py --self-test && python3 tools/check_zeroize.py
python3 hardware/model/ntt_oracle.py
python3 hardware/model/mlkem_oracle.py
python3 hardware/model/mldsa_oracle.py
python3 hardware/model/trng_health_model.py
```

Optional components are detected, not required: without Verilator the simulated
RTL backends are not compiled in and the corresponding transports return `NULL`;
without `cocotb`, `iverilog`, or `pkcs11-tool` the affected tests skip with a
message rather than failing.

## What the tests do not establish

- Nothing has run on hardware. There is no synthesis result, no timing closure,
  no power measurement, and no entropy assessment of a physical noise source.
- The algorithm implementations inside liboqs and OpenSSL are used as-is; the
  ACVP vectors demonstrate that the module drives them correctly, not that those
  libraries are themselves free of defects.
- The constant-time audit is lexical, not compiler-level: it analyses the
  sources, not the emitted instructions.
- No side-channel measurement of any kind has been performed.
