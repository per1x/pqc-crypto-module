# Security policy

## This is a prototype

`pqc-crypto-module` is a research prototype. It is **not certified, not
hardened, and not suitable for protecting anything real.** Please read
[docs/SECURITY.md](docs/SECURITY.md) before reporting anything: the known gaps
are documented there in detail, including that ML-KEM private keys leave the
hardware, that root can still drive the cores, that the key derivation root is a
stub, and that power and electromagnetic side channels are deliberately out of
scope.

A report describing one of those documented limitations is not a vulnerability
report — but a report showing that one of the *stated guarantees* does not hold
very much is.

## Reporting

Report privately, not as a public issue:

- GitHub → **Security** → **Report a vulnerability** (private advisory), or
- email the maintainer listed in the repository metadata.

Please include the commit hash, whether you reproduced it in simulation or on
silicon, and the smallest reproduction you have — a cocotb test or a payload
script under `board/scripts/` is ideal.

Expect an acknowledgement within a week. Since this is a prototype with a single
board, a fix may take considerably longer than that, and the honest outcome is
sometimes to document the finding as a known limitation rather than to fix it.

## Supported versions

Only the tip of `zu3eg-fpga-crypto` is maintained. There are no released
versions and no backports.
