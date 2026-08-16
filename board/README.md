# board/

Everything that runs on the AXU3EGB board, plus the raw output of every run.

| Path | Contents |
|---|---|
| `src/` | Standalone C programs that `mmap` `/dev/mem` at `0x8000_0000` and drive the cores directly, plus the generated KAT headers and their generator |
| `scripts/` | The PL harness and the payload scripts it runs (`pay_*.sh`) |
| `logs/` | Raw output of board runs, kept verbatim as evidence |
| `kmod/` | Kernel modules: `/dev/secmmio`, the PM/SiP caller, the protection-unit dump |
| `factory/` | Vendor-project bitstream build for the fan-only image |
| `petalinux/` | PetaLinux recipe for the fan-quiet init |

## The harness is not optional

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

**`eth0` is inside the PL** — `80000000.ethernet` is the vendor design's AXI
Ethernet — so every PL reconfiguration has to unbind the PL drivers first and
rebind afterwards. Reconfiguring the fabric with a live AXI master hangs the
bus, and once AXI is down even sysrq cannot be written, so the watchdog cannot
recover it either. Only a power cycle can.

`scripts/plharness.sh` therefore detaches from the terminal, restores the
network unconditionally on exit, and arms a sysrq watchdog. Two rules inside it
were each paid for with a power cycle:

- **Never put the unbind in a foreground SSH command.** The first device
  unbound is `eth0`; the session dies on the spot and the rebind never runs.
- **Teardown does not trust the payload's state.** It unbinds unconditionally
  before reconfiguring, because some payloads rebind the drivers themselves.
  Unbinding an already-unbound device is one ignored error.

## Logs

`logs/` is evidence, not documentation. The files are the unedited output of
real runs and should stay that way — the numbers quoted in
[docs/TESTING.md](../docs/TESTING.md) are traceable to them.

| File | Run |
|---|---|
| `RESULT_hwtest.txt` | 24-item board self-test: ML-KEM-512 ACVP, symmetric and SM vectors, key vault boundary scan, canary, TRNG health |
| `RESULT_hwtest_after_audit.txt` | The same suite re-run after the RTL audit fixes |
| `RESULT_audit.txt` | Post-audit verification: address aliasing, zeroize, TRNG counters, illegal parameters |
| `RESULT_seckem3.txt` | ML-KEM 512/768/1024 ACVP, throughput, constant-time sampling, from the secure world |
| `RESULT_sechwtest.txt` | Board self-test driven through EL3 against `SECURE_ONLY=1` cores |
| `RESULT_secproof.txt`, `RESULT_secneg.txt` | The AxPROT gate closed in both directions |
| `RESULT_restart.txt` | SP 800-90B restart test |
| `RESULT_service.txt` | End-to-end through the SDF interface, with the direct-access counter-proof |
| `RESULT_harness.log` | Harness log from the corresponding run |
| `RESULT_ctbprobe_before.txt`, `RESULT_ctbprobe_after.txt` | The c̃-comparison defect on silicon: 61 ordered Verify steps before and after the `verify.v` fix |
| `RESULT_mldsa_after.txt`, `RESULT_mldsa_demoform.txt` | ML-DSA board self-test, 32/32: three parameter sets against ACVP, the on-chip vault, runtime parameter-set switching |
| `RESULT_ctbprobe_demoform.txt` | The same 61-step probe re-run from the promoted default boot |
| `RESULT_sdf_modes.txt` | Full SDF-style session: hardware RNG, ML-KEM, SM4 block, ML-DSA (three sets), the four cipher modes against SP 800-38A, and the counter-proof that a handle dies with its session |
| `RESULT_secform_mldsa.txt` | Secure form (`SECURE_ONLY=1`): normal world reads all zero and the direct KAT program refuses to write, while the daemon reaches ML-DSA through the EL3 whitelist and signs end to end |

## Programs

| File | Purpose |
|---|---|
| `src/hsm_hwtest.c` | The 24-item self-test |
| `src/hsm_kem3.c` | ML-KEM three-parameter-set KAT, throughput, constant-time sampling |
| `src/hsm_secneg.c` | Counter-proof: read the cores from the normal world and expect DECERR |
| `src/hsm_audit.c` | Post-audit checks (aliasing, zeroize, illegal parameters) |
| `src/trngraw.c` | Export pre-conditioning noise bits |
| `src/trngrestart.c`, `src/restart_mcv.c` | SP 800-90B restart-test collection and on-board MCV computation |
| `src/xmpu_probe.c` | Probe the PS-side protection units from the normal world |
| `src/plprobe.c`, `src/rdreg.c` | Minimal PL and register probes |
| `src/mldsa_hwtest.c` | ML-DSA on silicon: KeyGen/Sign/Verify against ACVP for all three parameter sets, the on-chip key vault, and runtime parameter-set switching |
| `src/mldsa_ctb_probe.c` | Attribution experiment for the isolated-Verify rejection: an ordered sequence that separates "runtime parameter set not applied" from "c̃ comparison reads bytes left by the previous run" |
| `src/gen_kat_mlkem_all.py` | Generates `kat_mlkem_all.h` from the ACVP vectors |

Build them for the board with an aarch64 cross compiler and copy them across;
they are static and have no runtime dependencies.
