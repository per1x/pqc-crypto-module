# boot/

Boot-firmware patches and image-build recipes. The core is a set of patches to ARM
Trusted Firmware's BL31 (and one to optee_os) that give the secure world a path to
the programmable logic — which is what makes `SECURE_ONLY=1` cores usable at all,
since every transaction the normal world issues carries `AxPROT[1] = 1` and is
refused at the firewall.

| Path | Purpose |
|---|---|
| `atf/patch_atf_secmmio.py` | **The BL31 patch that matters now.** Idempotent and builds the *final* state: whatever was applied before, afterwards the SiP holds only the services below. Whitelisted MMIO read/write (`0x8200ff12/13`, used by `/dev/secmmio`), seed and symmetric-key loading, and the TRNG word for the TA |
| `atf/patch_atf_plmap.py` | Adds the PL window to BL31's page tables (required) |
| `atf/build-bl31-secmmio.sh` | Builds a BL31 with the secmmio patch applied |
| `atf/patch_atf_secread.py`, `atf/0001-sip-pl-secure-read.patch`, `atf/patch_atf_protread.py` | Earlier, superseded steps: the read-only PL read SiP and the read-only protection-unit dump SiP (`0x8200ff11`). `patch_atf_secmmio.py` strips them from the tree again. Kept as history |
| `atf/build-bl31-secread.sh`, `atf/build-bl31-protread.sh` | Build scripts for those earlier steps |
| `optee/patch_optee_seedpta.py` | optee_os pseudo-TA (S-EL1) that forwards the TA's seed words, symmetric-key words and TRNG requests to the SiPs. It is deliberately narrow, not a general SMC forwarder |
| `d7/` | Build recipes for the current images on the build host: `build-bl31-d7.sh`, `build-optee-d7.sh`, `pack-boot-d7.sh` (`BOOT_D7.BIN`), `bitbuild-d7.sh` (bitstreams), `build-apps-d7.sh` (normal-world programs and the TA), and `build-e1.sh` (E1: TA key entropy mixed with the PL TRNG, `BOOT_E1.BIN`) |
| `persist/` | `BOOT.BIN` hardening variants: `build_j1_boot.sh` (cryptographic bitstream inside `BOOT.BIN`), `build_bbram_boot.sh`, `build_puf_boot.sh`, and the RSA-auth note |

The SiP function IDs, in `patch_atf_secmmio.py`:

| ID | Service | Callable from |
|---|---|---|
| `0x8200ff12` / `0x8200ff13` | Whitelisted PL register read / write | both worlds, within the whitelist |
| `0x8200ff14` | Seed generated inside EL3 (batch 1 transitional) | secure world only (`PQCHSM_SEED_NS_ALLOWED = 0`) |
| `0x8200ff15` | One seed word from the TA's PTA into the PL staging port | secure world only |
| `0x8200ff16` | Symmetric-key loading into the key vault (BEGIN / WORD / COMMIT / ERASE / ZEROIZE / STAT) | secure world only |
| `0x8200ff17` | One word from the PL TRNG, for the TA's key entropy | secure world only |

## `patch_atf_plmap.py` is not optional

Without the page-table entry, BL31 takes a translation fault on the first access
to the PL window — in EL3, where there is no exception handling. The core wedges
on the spot.

## The whitelist is the boundary

`/dev/secmmio` (see [`../board/kmod/`](../board/kmod/)) is **not** a trust
boundary; it is a courier. The whitelist that decides which slots and offsets
the normal world may reach lives entirely in BL31, because that is the only
place the normal world cannot rewrite.

The generic MMIO read/write SiP is still driven by a normal-world daemon, which
assembles the operation sequence. That is why root can still *use* the hardware
even though it cannot *read* key material. Slot 1 (the symmetric key vault) is
excluded from that whitelist entirely: symmetric keys can only be loaded through
`0x8200ff16`, which accepts only the secure world and is driven by the TA. What the remaining generic read/write path still allows is
described in [../docs/SECURITY.md](../docs/SECURITY.md#threat-model).
