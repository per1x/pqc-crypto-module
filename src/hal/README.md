# src/hal —— 硬件抽象层（Phase 7 的落点，当前为空）

这个目录是**有意留空**的。它是路线图 §5.7.1 那个"软件桩加速器 / 真 PL 二选一"的位置。

将来这里会有一个 `pqc_accel.c`，实现与 `src/crypto/pqc_liboqs.c` **完全相同的
`pqc_backend_t` vtable**（定义见 `include/pqchsm/pqc.h`），内部通过 AXI-Lite 寄存器 +
AXI-DMA 驱动 PL 里的 ML-KEM / ML-DSA 算法核：

```c
/* 未来的 src/hal/pqc_accel.c */
static const pqc_backend_t g_accel = {
	.name              = "zynq-pl-accel",
	.keypair           = accel_keypair,
	.keypair_from_seed = accel_keypair_from_seed,
	/* ... 同一张表 ... */
};
```

换后端只需 `pqc_set_backend(pqc_backend_accel())`，
**槽位管理器、密钥库、包裹、备份恢复、审计一行都不用改**。

验收判据（§5.7.3 的"接口一致性"）：同一套上层测试与同一批 ACVP 黄金向量，
分别跑在 liboqs 后端和硬件后端上，**两边结果必须逐字节一致**。

详见 `doc/现状与后续计划.md` 第 4 节第 ② 步。
