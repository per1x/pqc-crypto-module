# boot/

Patches to ARM Trusted Firmware BL31 and optee_os, and the scripts that build the boot images from them. They give the secure world a path to the PL, and they limit what the normal world can reach there.

The PL cores are `SECURE_ONLY=1` in the default bitstream. Normal-world transactions carry `AxPROT[1]=1` and are not served, so everything the daemon does with the cores goes through BL31.

| Path | Purpose |
|---|---|
| `atf/patch_atf_secmmio.py` | The BL31 patch in use. Idempotent: it removes earlier secread/protread additions and leaves the final set of SiP services listed below. It also maps and configures XMPU_DDR (secure region over OP-TEE's core memory at `0x6000_0000`, size `0x1000_0000`) and turns on attribute poisoning before the handoff to BL33 |
| `atf/patch_atf_plmap.py` | Adds the PL window (`0x8000_0000`, 2 MB) to BL31's page tables. Required |
| `atf/build-bl31-secmmio.sh` | Builds BL31 with the secmmio patch into a boot image for a non-golden slot |
| `atf/patch_atf_secread.py`, `atf/0001-sip-pl-secure-read.patch`, `atf/patch_atf_protread.py`, `atf/build-bl31-secread.sh`, `atf/build-bl31-protread.sh` | Earlier read-only SiPs (PL read, protection-unit dump `0x8200ff11`). `patch_atf_secmmio.py` removes them. Kept for reference |
| `optee/patch_optee_seedpta.py` | Adds the pseudo-TA `pqchsm_seed.pta` (S-EL1) to optee_os. It has three commands: write one seed word (SiP `0x8200ff15`), symmetric-key load action (`0x8200ff16`), one TRNG word (`0x8200ff17`). It is not a general SMC forwarder, and it refuses callers that are not user-mode TAs |
| `d7/` | Build scripts for the current images on the build host: `build-bl31-d7.sh`, `build-optee-d7.sh`, `pack-boot-d7.sh` (`BOOT_D7.BIN`), `bitbuild-d7.sh` (default bitstream and `PQC_DEV_OPEN=1` bitstream), `build-apps-d7.sh` (daemon, `sdf_demo`, `d7bench`, `hsmctl`), `build-e1.sh` (`BOOT_E1.BIN`: TA key entropy mixes the OP-TEE PRNG with the PL TRNG) |
| `persist/` | `BOOT.BIN` variants: `build_j1_boot.sh` (crypto bitstream inside `BOOT.BIN`), `build_bbram_boot.sh` (BBRAM-key encryption), `build_puf_boot.sh` (PUF black key in the boot header), and `RSA-AUTH-BOOT-NOTE.zh-CN.md` |

## SiP services

Defined in `patch_atf_secmmio.py`.

| ID | Service | Caller |
|---|---|---|
| `0x8200ff12` / `0x8200ff13` | PL register read / write, restricted by a whitelist | Both worlds (used by `/dev/secmmio`) |
| `0x8200ff14` | EL3 draws a seed from the TRNG and writes it to the staging port of ML-KEM or ML-DSA | Secure world only (`PQCHSM_SEED_NS_ALLOWED=0`). The TA path uses `0x8200ff15`; this one is exercised by `board/src/seedel3.c` |
| `0x8200ff15` | One 32-bit seed word into the PQC seed staging port | Secure world only |
| `0x8200ff16` | Symmetric-key load into `key_vault_axi`: BEGIN, WORD, COMMIT, ERASE, ZEROIZE, STAT | Secure world only |
| `0x8200ff17` | One word from the PL TRNG, for the TA's key entropy. Returns an error if the TRNG health check alarms | Secure world only |

The TA runs in S-EL0 and cannot issue SMCs. It calls the PTA, which issues the SMC from S-EL1.

## Whitelist

The whitelist is in BL31, because the normal world cannot change it. `/dev/secmmio` (see [`../board/kmod/`](../board/kmod/)) only forwards requests. For each PL slot the whitelist lists the register offsets that exist and whether read and write are allowed:

| Slot | Core | Whitelist |
|---|---|---|
| 0 | `trng_axi` | read, write |
| 1 | `key_vault_axi` | excluded entirely |
| 2 | `sym_axi` | read, write |
| 3 | `mlkem_axi` | read, write (seed data register excluded) |
| 4 | canary | read only |
| 5 | fan | excluded |
| 6 | `mldsa_axi` | read, write (seed data register excluded) |

A new PL slave must be added here as well, otherwise the daemon cannot reach it in the secure form. Outside the PL, the whitelist allows reads (not writes) of the Device DNA words (`0xFFCA0050`..`5C`) and of the XPPU / XMPU registers.

Symmetric keys therefore reach the key registers only through `0x8200ff16`, driven by the TA. The daemon can still use the cores through the generic read/write SiP, but it cannot read or write key material. The remaining capabilities of that path are described in [../docs/SECURITY.md](../docs/SECURITY.md#threat-model).

## Pitfalls

- Without `patch_atf_plmap.py`, the first SiP access to the PL window raises a translation fault in EL3 and the calling core hangs. The other cores keep running, so the board looks alive.
- The SiP read/write must only be called after the PL is configured. `secmmio.ko` refuses SMCs until the caller has read `/sys/class/fpga_manager/fpga0/state` as `operating` and issued the `SECMMIO_ARM` ioctl, because a failed access in EL3 is not handled.
- The XMPU does not block transactions itself. It sets a poison attribute, and the DDR controller refuses the access. The patch writes the poison bit only (`ATTRIB`), not `POISONCFG`.
- The boot images are built with `LOG_LEVEL=20`. With `LOG_LEVEL=50` the page-table dump slowed boot enough for the FSBL watchdog to fire and select another multiboot slot (`board/logs/RESULT_slot_boot_wdt.txt`).
