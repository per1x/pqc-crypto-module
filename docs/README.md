# Documentation

Design notes and investigation write-ups. These documents are written in Chinese; the
English entry point for the project is the [top-level README](../README.md).

## design/

| Document | Contents |
|---|---|
| [status-and-roadmap.md](design/status-and-roadmap.md) | The main reference: what is complete, what is deferred, what can still be done without hardware, and what requires a board. Includes the technology-stack rationale. |
| [development-log.md](design/development-log.md) | Step-by-step progress with the decisions taken at each point. |

## reports/

| Document | Contents |
|---|---|
| [profiling.md](reports/profiling.md) | Performance baseline, the failure of symbol-level attribution on this platform, deterministic primitive counting as a substitute, and the Amdahl analysis driving the hardware partitioning decision. |
| [rtl-review.md](reports/rtl-review.md) | RTL review findings and their resolutions. |
| [level-a-b.md](reports/level-a-b.md) | End-to-end integration work and the RTL-as-replaceable-backend milestone. |
| [software-only-progress.md](reports/software-only-progress.md) | Everything completed without hardware, in two rounds, with the reasoning behind each design trade-off. |
| [network-proxy-diagnosis.md](reports/network-proxy-diagnosis.md) | Why this machine could not fetch dependencies, and the user-level workarounds applied. Not about the cryptographic module itself; kept because the diagnosis is reusable. |
