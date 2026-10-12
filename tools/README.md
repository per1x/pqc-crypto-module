# tools/

Development and verification scripts. Everything under Regression and Analysis runs
on the development host; only the scripts under Board and remote use need a board or
an address of one.

## Regression

| Script | What it does |
|---|---|
| `rtl_sim_fast.sh` | Iteration loop for RTL: per-module cocotb testbenches under Verilator, minutes instead of tens of minutes. Verilator is two-valued and does not propagate X, so it cannot replace the full suite |
| `rtl_sim.sh` | The full cocotb suite under Icarus Verilog (about 40 minutes); run it as the merge gate, not in the edit loop. Generates the golden vectors on first run |
| `rtl_lint.sh` | Verilator `-Wall` plus Icarus, every module as its own top level, zero warnings |
| `rtl_synth_check.sh` | Yosys synthesisability for every module |
| `aarch64_test.sh` | Full rebuild and host regression inside an aarch64 Linux container |
| `fuzz.sh` | libFuzzer targets (needs LLVM clang) |
| `e2e_p11.sh`, `p11_smoke.sh`, `demo_p11.sh`, `cli_smoke.sh` | PKCS#11 and CLI end-to-end checks |
| `daemon_failclosed.sh` | `pqchsmd` keystore must fail closed: a load failure must not serve an empty token or overwrite the real keystore on exit |
| `tls_regress.sh` | mTLS regression for the remote port (right CA, wrong CA, no cert, CN not in ACL) with a throw-away PKI; no board |
| `tee_native_tests.sh` | Builds and runs `tee/tests` natively (TA-side primitives cross-checked against the normal-world implementation) |
| `mldsa_grid.sh` | ML-DSA gate: KeyGen / Sign / Verify / AXI x 44 / 65 / 87, each against the ACVP vectors |
| `check_profile.sh` | Structural check: a PRODUCTION build contains no stub root-key literal |
| `check_ta_surface.py` | Structural check: the removed TA oracle commands stay removed (`--self-test` first) |
| `check_atf_patch.py` | Compiles the BL31 patch generator's C fragments on the host (`boot/atf/patch_atf_secmmio.py`) |
| `check_shell_vars.py` | Rejects `$VAR` immediately followed by a multi-byte character in shell scripts |
| `kat_evidence.sh` | Regenerates the KAT evidence bundle |

## Analysis

| Script | What it does |
|---|---|
| `sp800_90b.py` | The ten SP 800-90B non-IID estimators. `--selftest` reproduces the specification's worked examples — run it before trusting a number from it |
| `restart_test.py` | SP 800-90B §3.1.4.3 restart-test analysis |
| `ct_audit.py` | Constant-time source audit. `--self-test` first: a scanner that cannot find anything proves nothing when it reports clean |
| `check_zeroize.py` | Zeroization structure check, same `--self-test` discipline |
| `check_no_readback.py` | Asserts the key derivation root has no read-back interface |
| `cycle_budget.py`, `prim_count.py`, `amdahl.py` | Cycle accounting and speed-up modelling |
| `profile.sh` | Sampling profile (macOS `sample(1)`) |

## Board and remote use

| Script | What it does |
|---|---|
| `mkpki.sh` | Generates the device PKI for the mTLS remote port on the host (the CA private key stays off the board) |
| `demo_remote.sh` | Calls the board's HSM from a host over mTLS (`<IP> --provision`, `--smoke`, `--status`); needs a reachable board |
| `dna_bind_check.c` (CMake target `dna-bind-check`) | Checks device-bound key derivation, locally or against a remote board over mTLS |

## Support

| Script | What it does |
|---|---|
| `fetch_vectors.sh` | Downloads and flattens NIST ACVP vectors, pinned to a specific ACVP-Server commit |
| `acvp_to_kat.py` | Converts ACVP JSON into the KAT format the tests read |
| `mldsa_kat_to_c.py` | Turns ACVP ML-DSA vectors into the C header the on-board test embeds (`board/src/kat_mldsa.h`) |
| `demo_to_html.py` | Turns the board business-demo output into a self-contained HTML report |
| `bench.c`, `prim_bench.c` | Benchmark sources built by CMake |
| `pdf/` | Builds the design-and-validation reference PDF (pandoc + mermaid + headless Chrome) |
| `fsbl_xmpu/` | An FSBL XMPU configuration draft, kept for reference — see the note below |

> `fsbl_xmpu/` was written before UG1085 settled the question. No PS-side
> protection unit covers the `0x8000_0000` PL window, so this code would not
> have protected the crypto cores. It is retained as a record of the
> investigation, not as something to enable. See
> [../docs/SECURITY.md](../docs/SECURITY.md#the-boundary).
