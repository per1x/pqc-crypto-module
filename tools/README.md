# tools/

Development and verification scripts. None of them needs the board.

## Regression

| Script | What it does |
|---|---|
| `rtl_sim.sh` | 197 cocotb tests under Icarus Verilog. Generates the golden vectors on first run |
| `rtl_lint.sh` | Verilator `-Wall` plus Icarus, every module as its own top level, zero warnings |
| `rtl_synth_check.sh` | Yosys synthesisability for every module |
| `aarch64_test.sh` | Full rebuild and host regression inside an aarch64 Linux container |
| `fuzz.sh` | libFuzzer targets (needs LLVM clang) |
| `e2e_p11.sh`, `p11_smoke.sh`, `demo_p11.sh`, `cli_smoke.sh` | PKCS#11 and CLI end-to-end checks |
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
| `profile.sh` | Sampling profile |

## Support

| Script | What it does |
|---|---|
| `fetch_vectors.sh` | Downloads and flattens NIST ACVP vectors, pinned to a specific ACVP-Server commit |
| `acvp_to_kat.py` | Converts ACVP JSON into the KAT format the tests read |
| `bench.c`, `prim_bench.c` | Benchmark sources built by CMake |
| `pdf/` | Builds the design-and-validation reference PDF (pandoc + mermaid + headless Chrome) |
| `fsbl_xmpu/` | An FSBL XMPU configuration draft, kept for reference — see the note below |

> `fsbl_xmpu/` was written before UG1085 settled the question. No PS-side
> protection unit covers the `0x8000_0000` PL window, so this code would not
> have protected the crypto cores. It is retained as a record of the
> investigation, not as something to enable. See
> [../docs/SECURITY.md](../docs/SECURITY.md#the-boundary).
