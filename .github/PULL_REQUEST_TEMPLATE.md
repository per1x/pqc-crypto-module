## What this changes

<!-- One paragraph. Why, not just what. -->

## Checks run

- [ ] `./tools/rtl_lint.sh` — zero warnings
- [ ] `./tools/rtl_sim.sh` — all cocotb tests pass
- [ ] `./tools/rtl_synth_check.sh` — every module synthesises
- [ ] `ctest --test-dir build --output-on-failure`

## Hardware

- [ ] This change was validated on the board
- [ ] This change does not need the board
- [ ] This change needs the board and has **not** been validated there yet

## If this touches the security boundary

The firewall, the crossbar decode, the key vault, or the EL3 SiP:

- [ ] The claim in `docs/SECURITY.md` still holds, or has been updated
- [ ] There is a test that fails when the property is broken (negative control)

## Documentation

- [ ] English and `*.zh-CN.md` are both updated, or neither needed changing
