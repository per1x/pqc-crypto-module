# Reference material

Longer documents that are not part of the main set: plans and reports from
earlier stages, design notes, a security policy draft, a deployment procedure and
audit methods. New readers should start with [../ARCHITECTURE.md](../ARCHITECTURE.md)
and [../SECURITY.md](../SECURITY.md).

Several of these were written at a particular stage and describe the design as it
was then. The status column says which ones are history. Where one of them
disagrees with the code, the code and the main documents are correct.

| Document | Contents | Status |
|---|---|---|
| [FINAL-PLAN.zh-CN.md](FINAL-PLAN.zh-CN.md) (Chinese) | Architecture decision with the TEE as the main path: nine decisions with industry references, the order of landing batches, the red lines, and what the project may claim externally | Plan; the authority when the reference documents disagree |
| [BATCH1-REPORT.zh-CN.md](BATCH1-REPORT.zh-CN.md), [BATCH2-REPORT.zh-CN.md](BATCH2-REPORT.zh-CN.md) (Chinese) | Close-out reports for the first two landing batches: status of each item on the FINAL-PLAN checklist, and the final result of batch 2 (merged into `main`) | Report |
| [SYMKEY-TEE-REPORT.zh-CN.md](SYMKEY-TEE-REPORT.zh-CN.md) (Chinese) | Symmetric keys managed by the TEE (the key-loading half of D7): plan, results, and a list of checks that still need a board | Report |
| [STATELESS-PL-CONFLICTS.zh-CN.md](STATELESS-PL-CONFLICTS.zh-CN.md) (Chinese) | Inventory of conflicts with the model "PL holds no state, all keys held by the TEE". Analysis only | History |
| [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md) (Chinese) | Target architecture: where each asset lives (PS, PL, TEE, normal world) and why, threat model per attacker class, the fix for the software-seed path (CODE-1), and a landing order that respects the red lines | Design document, not a list of what is implemented |
| [HSM-COMPARISON.md](HSM-COMPARISON.md) (Chinese) | Comparison with Thales Luna, Marvell LiquidSecurity, YubiHSM 2, AWS/Azure cloud HSM and Chinese GM/T crypto machines on trust root, tamper protection, certification, algorithms, PQC, interfaces, performance and form factor, with sources | Reference |
| [security-policy.md](security-policy.md), [中文](security-policy.zh-CN.md) | Draft security policy in the style of FIPS 140-3 and GM/T 0028, with a list of what would block a submission | Draft. The body describes an earlier form of the implementation (see its note at the top) |
| [deployment.md](deployment.md), [中文](deployment.zh-CN.md) | Deploying the software-only part (C module, tests, PKCS#11 front end, Python and Java demos) on an isolated Linux host, including staging dependencies offline | Procedure |
| [constant-time.md](constant-time.md), [中文](constant-time.zh-CN.md) | Scope and method of the constant-time and zeroization audits, the exemptions, and what is not claimed | Audit record |
| [zynq-port.zh-CN.md](zynq-port.zh-CN.md) (Chinese) | Plan for porting to Zynq UltraScale+: stages, mapping the software boundary onto the device, irreversible steps | History; the hardware part has been done |
| [mldsa-engine-design.zh-CN.md](mldsa-engine-design.zh-CN.md) (Chinese) | ML-DSA shared engine: interface contract and buffer address layout shared by the RTL and the AXI slave | Design note |
| [mldsa-keygen-design.zh-CN.md](mldsa-keygen-design.zh-CN.md), [mldsa-sign-design.zh-CN.md](mldsa-sign-design.zh-CN.md), [mldsa-verify-design.zh-CN.md](mldsa-verify-design.zh-CN.md) (Chinese) | Design notes for the ML-DSA KeyGen, Sign and Verify cores: datapath stages, the golden model each stage is checked against, parameter-set generalisation | Design note |
| [tee-protocol.zh-CN.md](tee-protocol.zh-CN.md) (Chinese) | Early design of the OP-TEE trusted application. Its header describes the TEE as having a minimal role and the key vault in the PL, which no longer matches the project: the TA now generates and wraps all keys | History; read the TA sources under `tee/` instead |
