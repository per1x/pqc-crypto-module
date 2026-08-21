# Reference material

Longer documents that do not belong in the main set: a validation-style security
policy, deployment procedure, audit method, and forward-looking plans. Start with
[../ARCHITECTURE.md](../ARCHITECTURE.md) and [../SECURITY.md](../SECURITY.md)
instead if you are new to the project.

| Document | Contents |
|---|---|
| [FINAL-PLAN.zh-CN.md](FINAL-PLAN.zh-CN.md) (中文) | **决策文档,先读这份**：最终架构决定（TEE 主线）、九个拍板点（各附业界参照）、分批落地顺序、红线与对外宣称口径 |
| [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md) (中文) | Final target architecture: PS/PL/TEE/normal-world placement for every asset with rationale, the threat model per attacker class, the architectural fix for the software-seed path (CODE-1), and a red-line-respecting landing order |
| [HSM-COMPARISON.md](HSM-COMPARISON.md) (中文) | Evenhanded comparison against mature HSMs (Thales Luna, Marvell LiquidSecurity, YubiHSM 2, AWS/Azure cloud HSM, Chinese GM/T crypto machines) across trust root, tamper, certification, algorithm coverage, PQC, interfaces, performance, and form factor — with a sourced, no-hype positioning verdict |
| [security-policy.md](security-policy.md) · [中文](security-policy.zh-CN.md) | FIPS 140-3 / GM/T 0028 style security policy draft, with an explicit list of what would block a submission |
| [deployment.md](deployment.md) · [中文](deployment.zh-CN.md) | Deploying on an intranet Linux host, including staging every dependency offline |
| [constant-time.md](constant-time.md) · [中文](constant-time.zh-CN.md) | Scope and method of the constant-time and zeroization audits, the exemptions, and what is not claimed |
| [zynq-port.zh-CN.md](zynq-port.zh-CN.md) (中文) | The porting plan onto Zynq UltraScale+: staging, mapping the software boundary onto silicon, and the irreversible steps |
| [mldsa-engine-design.zh-CN.md](mldsa-engine-design.zh-CN.md) (中文) | The ML-DSA shared engine: interface contract and buffer address layout, shared by the RTL and the AXI slave |
| [mldsa-keygen-design.zh-CN.md](mldsa-keygen-design.zh-CN.md) · [sign](mldsa-sign-design.zh-CN.md) · [verify](mldsa-verify-design.zh-CN.md) (中文) | Per-core design notes for the ML-DSA hardware: datapath stages, the golden model each stage is checked against, and the parameter-set generalisation |
| [tee-protocol.zh-CN.md](tee-protocol.zh-CN.md) (中文) | The OP-TEE trusted application — a separate line of work that puts key operations in secure-world *software* |
</content>
