# Contributing

Thanks for looking. This is a research prototype for a hardware security module
on a Zynq UltraScale+ device, so a few things work differently from a typical
software project.

## Before you start

- **The board is not required**, and most of the repository can be worked on
  without it: all RTL simulation, lint, synthesisability checking, the host
  software and its tests run on a laptop. See
  [docs/USAGE.md](docs/USAGE.md#simulation-and-static-checks).
- **There is exactly one board.** Anything that touches real hardware is
  serialised by hand, so changes that can only be validated on silicon will
  take a while to land.
- ⛔ **Read the red lines first:
  [docs/SECURITY.md — Irreversible-operation red lines](docs/SECURITY.md#-irreversible-operation-red-lines)**
  (中文：[docs/SECURITY.zh-CN.md — 不可逆操作红线](docs/SECURITY.zh-CN.md#-不可逆操作红线)).
  That section is normative; the summary below is only a pointer to it.

- **Nothing in this repository performs one-time / irreversible programming.**
  Not eFUSEs, not the eMMC RPMB authentication key, not BBRAM latch bits, not
  one-time latches that cannot be cleared. Do not add a code path that can, and
  do not re-enable an existing one without saying so in the commit message. The
  one remaining path (`provision` in `board/src/rpmb_tool.c`) is disabled at
  compile time and needs `-DRPMB_ALLOW_PROVISION=1` plus a rebuild.

- **If an irreversible decision rests on a device-state reading, prove the
  reading first.** RPMB responses are validated on `req_resp` and the nonce
  echo, never on `result` (the field a stale frame corrupts), and I/O failure
  must return a different code from "wrong key". The red-lines section explains
  why this is not optional.

## What a change has to pass

```bash
./tools/rtl_lint.sh          # Verilator -Wall + Icarus — zero warnings, 70 modules
./tools/rtl_sim.sh           # 197 cocotb tests
./tools/rtl_synth_check.sh   # Yosys — every module must synthesise
ctest --test-dir build --output-on-failure   # host software, 45 targets
```

All four must be clean. There is no "known failure" list.

## RTL

- **Plain inferrable Verilog-2001.** No vendor primitive in any crypto module —
  the same sources must target Xilinx, Intel and Lattice unchanged. Vendor
  primitives are allowed only in the board top level, where
  `hardware/tb/lint/vendor_stubs.v` supplies shells so it still passes lint.
- **Lint waivers are per-file, per-signal, with a reason.** Add them to
  `hardware/rtl/lint_waivers.vlt`. A blanket waiver that also hides the next
  warning of the same class is not acceptable.
- **Every module must lint as its own top level.** Modules that nothing
  instantiates are still checked; that is why the lint script iterates.
- **Declare before use in the board top level.** Vivado silently creates an
  undriven net for a forward reference; Icarus rejects it. This has hung the
  board twice.

## Tests

Three properties are expected of a new test:

- **An independent oracle.** Matching this project's own reference model is not
  evidence. Check against a published vector, a different implementation, or a
  construction that shares no code with the thing under test.
- **A negative control.** Show the test fails when the property is broken —
  perturb a constant, drop a stage, remove a lock. A test that has never failed
  has not been shown to be able to.
- **Self-test before reporting clean.** A scanner or structural check must
  demonstrate on synthetic samples that it can find what it looks for, before a
  clean result from it means anything.

## Security claims

Every claim in the documentation is attached to reproducible evidence. If you
add a claim, add the evidence with it; if you cannot measure it, say so plainly
rather than phrasing it carefully. Both halves of a counter-proof are required
— "no key bytes were found" means nothing without "and the cipher produced the
right answer".

Read [docs/SECURITY.md](docs/SECURITY.md) before touching anything on the
boundary: the firewall, the crossbar decode, the key vault, or the EL3 SiP.

## Documentation

- English is primary; `*.zh-CN.md` files are faithful mirrors, not variants. If
  you change one, change the other.
- Document what the design *is*, not how it came to be. Engineering narrative —
  what broke, what was tried — belongs in commit messages. The exception is a
  rationale that stops someone reintroducing a bug; those are kept, stated as
  the reason for the current design.
- Raw board logs in `board/logs/` are evidence and are kept verbatim. Do not
  tidy them.

## Commits and pull requests

- Branch off `zu3eg-fpga-crypto`.
- One logical change per commit; explain *why* in the body.
- Say in the PR description which of the four checks above you ran, and whether
  anything needed the board.

## License

Contributions are accepted under [Apache-2.0](LICENSE).
