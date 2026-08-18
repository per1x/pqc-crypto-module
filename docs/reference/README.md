# Reference material

Longer documents that do not belong in the main set: a validation-style security
policy, deployment procedure, audit method, and forward-looking plans. Start with
[../ARCHITECTURE.md](../ARCHITECTURE.md) and [../SECURITY.md](../SECURITY.md)
instead if you are new to the project.

| Document | Contents |
|---|---|
| [security-policy.md](security-policy.md) · [中文](security-policy.zh-CN.md) | FIPS 140-3 / GM/T 0028 style security policy draft, with an explicit list of what would block a submission |
| [deployment.md](deployment.md) · [中文](deployment.zh-CN.md) | Deploying on an intranet Linux host, including staging every dependency offline |
| [constant-time.md](constant-time.md) · [中文](constant-time.zh-CN.md) | Scope and method of the constant-time and zeroization audits, the exemptions, and what is not claimed |
| [zynq-port.zh-CN.md](zynq-port.zh-CN.md) (中文) | The porting plan onto Zynq UltraScale+: staging, mapping the software boundary onto silicon, and the irreversible steps |
| [mldsa-engine-design.zh-CN.md](mldsa-engine-design.zh-CN.md) (中文) | The ML-DSA shared engine: interface contract and buffer address layout, shared by the RTL and the AXI slave |
| [mldsa-keygen-design.zh-CN.md](mldsa-keygen-design.zh-CN.md) · [sign](mldsa-sign-design.zh-CN.md) · [verify](mldsa-verify-design.zh-CN.md) (中文) | Per-core design notes for the ML-DSA hardware: datapath stages, the golden model each stage is checked against, and the parameter-set generalisation |
| [tee-protocol.zh-CN.md](tee-protocol.zh-CN.md) (中文) | The OP-TEE trusted application — a separate line of work that puts key operations in secure-world *software* |
</content>
