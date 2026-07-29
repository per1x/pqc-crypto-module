# Level A / Level B 完成报告

> 日期 2026-07-29。ctest **36/36**，断言 **2690**，24 个提交。

## 一句话回答

**无硬件条件下，整条链能纯软件跑通，也能从 Java / Python 当 provider 调用；
密码路径能走"寄存器接口"这条将来上板的同一条代码路径，但 RTL 侧目前只有 NTT 核 ——
完整的 ML-KEM/ML-DSA 硬件核是路线图 Phase 1–4 的工作，不在本轮范围。**

---

## Level A：done

### A1. 端到端集成（C 层）—— `tests/integration/test_e2e.c`，67 断言

```
建库 → 生成 ML-DSA-65 / ML-KEM-768 / 设备身份钥
     → sign/verify + encaps/decaps
     → 密钥库落盘 → 备份导出(3-of-5) → 审计锚点固化
     → 整机清零 → 确认密钥与 PIN 都没了
     → 用备份 + 3 份分片恢复
     → ★ 恢复出来的私钥签出的签名，被**清零之前导出的公钥**验过
     → 审计链验证 + 锚点验证 + 操作序列核对（generate 3 / backup 1 / zeroize 3 / restore 2）
```

判据一律是"能不能继续用"，不是"有没有报错"。
另含红线：整条链跑完，日志/备份/密钥库里都搜不到 PIN 明文。

### A2. 从 PKCS#11 层驱动 —— `tools/e2e_p11.sh`，8 项

```
[应用面 · PKCS#11]  init token → 生成 ML-DSA → 签名 → 导出公钥
[运维面 · admin  ]  备份(3-of-5) → 整机清零
[应用面 · PKCS#11]  确认登录失败（密钥与 PIN 都没了）
[运维面 · admin  ]  恢复（并验证只给 2 份必失败）
[应用面 · PKCS#11]  ★ 再签一次：公钥与清零前**逐字节相同**，验签通过
```

PKCS#11 里没有备份/恢复/清零 —— 它是应用面接口。所以新增了
`cli/pqchsm_admin.c` 作为运维面工具，两边共用同一个密钥库文件、顺序访问。
真实 HSM 也是这么分的（应用走 P11，运维走厂商工具）。

### A3. Python / Java provider demo —— 已在上一轮补齐

- `demo/python/pqchsm_demo.py`（PyKCS11，25 断言）
- `demo/java/PqcHsmDemo.java`（JDK 22+ FFM，零外部依赖，30 断言）
- 兼容性结论见 `demo/README.md`

### 本步抓到的真 bug

P11 前端生成密钥时 policy 恒为 0 ⇒ **从 PKCS#11 建的密钥永远进不了备份**
（e2e 里表现为"已备份 0 个槽位"）。PKCS#11 没有"可否被 KEK 包裹备份"的标准属性
（`CKA_EXTRACTABLE` 说的是明文导出，不是一回事），故按规范用
`CKA_VENDOR_DEFINED` 区段加了 `CKA_PQCHSM_BACKUPABLE` / `CKA_PQCHSM_SEED_STORAGE`，
默认可备份。

---

## Level B：done（骨架 + 真 RTL 的 NTT 那一段）

### 做出来的

| 层 | 文件 | 说明 |
|----|------|------|
| 寄存器表 | `include/pqchsm/accel.h` | §5.8.2 要求"写 RTL 之前定死"的那张表 |
| 软件桩 | `src/hal/accel_stub.c` | 暴露与真 PL 完全相同的寄存器语义，内部调 liboqs（§5.7.1） |
| 后端 | `src/hal/pqc_accel.c` | 用寄存器接口实现 `pqc_backend_t`，与 liboqs 后端可互换 |
| RTL | `rtl/mlkem/ntt_core.v` | 256 点 NTT/INTT，1 蝶形/周期，**1154 cycles** |
| 仿真接入 | `src/hal/verilator/ntt_sim.cpp` + `accel_verilator.c` | Verilate 出来的真 RTL 挂到同一个 transport 上 |

三种 transport 实现同一张 `accel_transport_t`：
`stub`（现在就能跑）/ `verilator`（真 RTL，仅 NTT）/ `mmap`（Phase 3+，待板子）。

### 关键断言（`test_accel`，78 条）

- **两个后端逐字节等价**：6 个参数集的 `keygen_from_seed`、`encaps/decaps`、
  deterministic 签名 —— 不是"能跑"，是"结果一模一样"。
- **整条 HSM 链跑在 accel 后端上**（生成→签名→备份→清零→恢复→验签）。
- ★ **RTL(Verilator) 与软件桩的 NTT 结果逐字节相同** —— 同一个寄存器接口。
- **RTL 后端对没实现的模式明确报 `PQC_ERR_UNSUPPORTED`，不偷偷回落软件**。
  这一条是有意的：偷偷回落会让"跑通了"变成假象。

### 修 RTL 位宽（值得记）

Verilator 是 2-state，隐式位宽截断会**真的算错**，而 Icarus 的 4-state 可能"碰巧"对上。
把 `mont`/`barr`/蝶形里每一步的位宽显式写出来之后，两个仿真器都绿。
跑两个仿真器的意义就在这里。

### Level B 没做到的（明说）

**RTL 侧只有 NTT 核**。完整的 ML-KEM/ML-DSA 硬件核需要 Keccak 核、采样器、
控制流水线，是路线图 **Phase 1–4** 的工作（按 §5.2 的 Amdahl 结论，
若目标是端到端性能，下一个该做的是 Keccak 核而不是继续堆 NTT）。

所以现在的状态是：
- **密码路径的"接线"已经是将来上板的同一条**（寄存器语义 + vtable 缝）；
- **密码路径的"运算"目前仍由软件桩完成**，只有 NTT 这一段能真正走 RTL。

---

## 其余无硬件项：上一轮已全部完成

3.1 审计锚点 / 3.2 KDR provider / 3.3 PKCS#11 前端 / 3.4 TCP+TLV 命令接口 /
3.5 安全密钥注入 / 3.6 profiling+Amdahl（符号级归因如实标注失败）/
3.7 cocotb 纯仿真 / 3.8 综合脚本+cycle 预算（Vivado 未装，标注未验证）/
3.9 fuzz / 3.10 aarch64 Linux 回归 —— 见 `doc/无硬件推进报告.md`。

---

## 可运行命令

```bash
# 全量回归（36 个用例）
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# Level A：从 PKCS#11 驱动的整条链
./tools/e2e_p11.sh

# Python / Java provider demo
./tools/demo_p11.sh

# RTL 对拍（cocotb + Icarus）
./tools/rtl_sim.sh

# aarch64 Linux 回归（容器，原生执行）
./tools/aarch64_test.sh
```
