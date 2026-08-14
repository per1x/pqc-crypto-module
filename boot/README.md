# boot/

Boot-firmware patches. Everything here modifies ARM Trusted Firmware's BL31 so
that the secure world has a path to the programmable logic — which is what makes
`SECURE_ONLY=1` cores usable at all, since every transaction the normal world
issues carries `AxPROT[1] = 1` and is refused at the firewall.

| File | Purpose |
|---|---|
| `0001-sip-pl-secure-read.patch` | The SiP service itself: an EL3 read of the PL window |
| `patch_atf_secread.py` | Applies the read-only SiP |
| `patch_atf_secmmio.py` | Applies the whitelisted MMIO read/write SiP used by `/dev/secmmio` |
| `patch_atf_protread.py` | Applies the read-only protection-unit dump SiP |
| `patch_atf_plmap.py` | Adds the PL window to BL31's page tables |
| `build-bl31-*.sh` | Build a BL31 with the corresponding patch applied |

## `patch_atf_plmap.py` is not optional

Without the page-table entry, BL31 takes a translation fault on the first access
to the PL window — in EL3, where there is no exception handling. The core wedges
on the spot.

## The whitelist is the boundary

`/dev/secmmio` (see [`../board/kmod/`](../board/kmod/)) is **not** a trust
boundary; it is a courier. The whitelist that decides which slots and offsets
the normal world may reach lives entirely in BL31, because that is the only
place the normal world cannot rewrite.

The current SiP exposes MMIO reads and writes, with the operation sequence
assembled by a normal-world daemon. That is why root can still *use* the
hardware even though it cannot *read* key material — see
[../docs/SECURITY.md](../docs/SECURITY.md#threat-model) for what closing that
would require.
