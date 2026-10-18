# board/

Code, scripts and recorded output for the AXU3EGB board (XCZU3EG).

| Path | Contents |
|---|---|
| `src/` | Standalone C programs that run on the board (self-tests, probes, benchmarks), plus the generated KAT headers and their generator |
| `scripts/` | Boot and operations scripts, the PL harness `plharness.sh`, and the `pay_*.sh` payloads it runs |
| `demo/` | On-board demos. See [`demo/README.zh-CN.md`](demo/README.zh-CN.md) |
| `logs/` | Raw output of board runs (`RESULT_*.txt`) |
| `kmod/` | Kernel modules: `secmmio` (`/dev/secmmio`), `pmsec` (PM SMC caller), `protdump` (XPPU/XMPU dump through EL3), `xmpupoison` (XMPU_DDR poison switch) |
| `factory/` | Rebuild of the vendor design's bitstream with the fan logic replaced, and `make_boot_scr.sh` |
| `petalinux/` | PetaLinux recipe `fanquiet` (loads the fan-controlled bitstream at boot) |
| `recovery/` | JTAG rescue: `build_rescue.sh` builds the shim, device tree and initramfs that `jtag_rescue.tcl` loads |

## Boot

`scripts/hsm-boot.sh` lives on the SD card (`/media/sd-mmcblk1p2/hsm/`) and is called from an init script in the image. In order, it:

1. Moves the clock forward if it is earlier than the newest of `pki/hsm_device.crt`, `pki/hsm_ca.crt` and `CLOCK_STAMP` (mTLS certificates would otherwise not be valid yet).
2. Brings up `eth1` (the PS GEM) and waits for link, then asks for a DHCP lease (host name `pqchsm`; `udhcpc` stays in the background to renew). With no lease it falls back to `192.168.50.175`. It always adds the link-local address `169.254.50.175`, and prints the address in the serial login banner. Without link it sets the fallback addresses and stops before touching the PL.
   It also starts dropbear with an extra ECDSA host key kept in `hsm/ssh/` (the built-in dropbear 2019.78 otherwise offers only `ssh-rsa`, which current OpenSSH refuses), merges `hsm/ssh/authorized_keys` into root's keys, and installs `/usr/bin/hsmctl` (keystore `/media/sd-mmcblk1p2/keys`) and `/usr/bin/hsm-demo`.
3. Unbinds the PL drivers and loads `zu3eg_hsm_dev.bit` with `fpgautil`. If the PL does not reach `operating`, it reloads `factory.bit` and rebinds.
4. Loads `secmmio.ko`, copies `*.ta` from the SD card to `/lib/optee_armtz/` (`/lib` is an initramfs), and starts `tee-supplicant` if it is not running.
5. Starts `pqchsm_fpgad` and `hsm_httpd 8443 8080` with the keystore in `/media/sd-mmcblk1p2/keys`.

Progress is written to `hsm/HSM_STATUS` and `hsm/hsm-boot.log`. `READY=yes` is the last line of a complete boot. Without `zu3eg_hsm_dev.bit` on the card the PL stays on the vendor bitstream. `zu3eg_hsm.bit` (`SECURE_ONLY=1`) can be loaded at run time with `pl_deadman.sh <bit> <seconds>`, which rolls back unless someone logs in over `eth1` and touches `hsm/PL_KEEP` within the grace period.

Other scripts:

| Script | Purpose |
|---|---|
| `hsmreboot.sh` | Reboot through sysrq (`s`, `u`, `b`). `reboot` does not work on this board |
| `pl_deadman.sh` | Load a bitstream and roll back unless confirmed |
| `try_mldsa.sh`, `try_mldsa_dev.sh`, `try_fanquiet.sh` | Trial installs with a dead-man switch |
| `pay_*.sh` | Payloads for `plharness.sh`: D7/D11 checks, secure-form KAT, seed and audit checks, TA deployment, TRNG characterization and restart matrices, fan checks |
| `trng_restart_plreload.sh`, `verify_after_por.sh` | Restart matrix with PL reconfiguration as the restart; post-POR verification |
| `jtag_por.tcl`, `xmpu_poison_on.tcl`, `uboot_multiboot.py`, `serial_console.py` | JTAG power-on reset, XMPU poison over JTAG, setting `CSU_MULTI_BOOT` from U-Boot, serial console from the Mac |
| `fanquiet-init.sh`, `fan_minduty_sweep.sh`, `mk_image_hook.sh` | Fan control at boot, minimum-duty sweep, boot hook for `image.ub` |

## plharness.sh

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

⚠️ Reconfiguring the PL while PL drivers with live AXI masters are bound hangs the bus. After that even writing to sysrq fails, and only a power cycle recovers the board. `eth0` (`80000000.ethernet`) is part of the vendor PL design, so every reconfiguration has to unbind the PL drivers first.

`plharness.sh` detaches from the terminal, unbinds the PL drivers, runs the payload, then unbinds again unconditionally (a payload may have rebound them), reloads `factory.bit`, rebinds and restores `eth0`. It also starts a watchdog that triggers sysrq `b` after 480 seconds. The watchdog does not help if the bus is already hung.

Never run the unbind from a foreground SSH command. The first device unbound is `eth0`, the session dies, and the rebind never runs.

## Logs

The files in `logs/` are unedited output of real runs. The numbers in [docs/TESTING.md](../docs/TESTING.md) come from them.

| File | Run |
|---|---|
| `RESULT_hwtest.txt` | 24-item self-test (`hsm_hwtest`): ML-KEM-512 ACVP, symmetric and SM vectors, key-register boundary scan, canary, TRNG health |
| `RESULT_hwtest_after_audit.txt`, `RESULT_audit.txt` | The same suite and the audit checks (aliasing, zeroize, TRNG counters, illegal parameters) after the RTL audit fixes |
| `RESULT_seckem3.txt` | ML-KEM 512/768/1024 ACVP, throughput, constant-time sampling, run from the secure world |
| `RESULT_sechwtest.txt` | `hsm_hwtest` driven through EL3 against `SECURE_ONLY=1` cores |
| `RESULT_secproof.txt`, `RESULT_secneg.txt` | Normal-world accesses to `SECURE_ONLY=1` cores (bus error / reads return 0) |
| `RESULT_nocrash.txt` | `hsm_nocrash`: 9 refused or nonexistent addresses, 2000 reads and 2000 writes each (36,000 accesses), board stays up |
| `RESULT_restart.txt` | SP 800-90B restart matrix, restart = TRNG zeroize (1000 rows) |
| `RESULT_restart_plreload.txt` | Same matrix with restart = PL reconfiguration (200 rows). Not a power-on reset, and fewer rows than the 1000 the standard suggests; both are stated in the file header |
| `RESULT_service.txt` | End-to-end through the SDF interface |
| `RESULT_harness.log` | `plharness.sh` log of a run |
| `RESULT_ctbprobe_before.txt`, `RESULT_ctbprobe_after.txt`, `RESULT_ctbprobe_demoform.txt` | `mldsa_ctb_probe`: 61 ordered Verify steps before and after the `verify.v` fix, and again from the default boot |
| `RESULT_mldsa_after.txt`, `RESULT_mldsa_demoform.txt` | `mldsa_hwtest`, 32/32: three parameter sets against ACVP and runtime parameter-set switching. These runs predate the removal of the key-slot ABI |
| `RESULT_sdf_modes.txt` | SDF session: hardware RNG, ML-KEM, SM4, ML-DSA, the four cipher modes against SP 800-38A, handle invalid after session end |
| `RESULT_secform_mldsa.txt` | `SECURE_ONLY=1`: normal world reads zero, the daemon reaches ML-DSA through the EL3 whitelist |
| `RESULT_b1_*`, `RESULT_b2_*`, `RESULT_d7_*`, `RESULT_business_demo*.txt`, others | Seed staging, TA key operations, symmetric-key loading and benchmarks, wire fuzzing, the business demo |

## Programs

Build with an aarch64 cross compiler and copy to the board; they are static. `make CROSS=aarch64-linux-gnu- -C src` builds `mldsa_hwtest` and `d7probe` only; the other programs carry their own compile line in the file header (`d7bench` links `libsdfe` and OpenSSL, see `boot/d7/build-apps-d7.sh`).

| File | Purpose |
|---|---|
| `src/hsm_hwtest.c` | 24-item self-test |
| `src/hsm_kem3.c` | ML-KEM three-parameter-set KAT, throughput, constant-time sampling |
| `src/mldsa_hwtest.c` | ML-DSA KeyGen/Sign/Verify against ACVP for all parameter sets, runtime parameter-set switching |
| `src/mldsa_ctb_probe.c` | Ordered Verify sequence that separates "parameter set not applied" from "c̃ comparison reads bytes left by the previous run" |
| `src/hsm_secneg.c` | Reads the cores from the normal world and expects zeros (RAZ) |
| `src/hsm_nocrash.c` | Reads and writes refused addresses 2000 times each. The current source has 10 sites (it includes `mldsa_axi`); the recorded run has 9 |
| `src/hsm_audit.c` | Post-audit checks |
| `src/d7probe.c` | Writes a key to the key-register load port from the normal world and checks that `LOAD_REJ` counts the 11 rejected writes; `d7probe vmap` reads `VALID_MAP` |
| `src/d7bench.c` | Loads the public AES-256 KAT key into slot 7, reads the whole key-register and `sym_axi` windows through `/dev/mem` and checks that no key word appears, then measures load latency and AES-CBC throughput |
| `src/seedprobe.c`, `src/seedel3.c` | Seed staging port checks (demo bitstream) and the EL3 seed path |
| `src/iopack_bench.c` | A/B of the 4-byte packed I/O port against byte-wise I/O |
| `src/dna_probe.c` | Reads the Device DNA window (`0xFFCA0050`), `-w` tries a write to confirm EL3 refuses it |
| `src/trngraw.c` | Exports pre-conditioning noise bits |
| `src/trngrestart.c`, `src/restart_mcv.c` | Restart-matrix collection and on-board MCV |
| `src/xmpu_probe.c`, `src/protunit_probe.c` | XPPU/XMPU configuration, from the normal world and through the EL3 read window |
| `src/plprobe.c`, `src/rdreg.c` | Minimal PL and register probes |
| `src/rpmb*.c` | eMMC RPMB probe and tool. `rpmb_tool provision` is disabled because it burns an irreversible key |
| `src/gen_kat_mlkem_all.py` | Generates `kat_mlkem_all.h` from the ACVP vectors |
