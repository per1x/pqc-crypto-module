# pqc-hsm — PQC 密码机（HSM）原型：纯软件轨

对应 [`FPGA_PQC_实现路线图.md`](../FPGA_PQC_实现路线图.md) 的 **§5「零硬件阶段」** 与 **Phase 5/6**：
软件槽位管理器、密钥库、KEK 包裹、备份恢复、审计日志。

目标不是"KAT 过了"，而是路线图 §0.1 的验收标准：
**任何时刻明文私钥不出安全边界，且密钥可管理、可恢复、可销毁。**
本阶段安全边界暂时放宽到"主机进程内存"（Phase 7 收紧到 PL），但**接口按最终形态设计**。

---

## 纯软件启动清单

| # | 事项 | 状态 |
|---|------|------|
| 0 | 工具链：CMake ≥3.20、C11 编译器、Python ≥3.11 | ✅ |
| 1 | liboqs 0.16.0（ML-KEM / ML-DSA 最终版 FIPS 203/204） | ✅ `brew install liboqs` |
| 2 | OpenSSL 3（AES-256-GCM 包裹、SHA3/KMAC256 派生与哈希链） | ✅ `brew install openssl@3` |
| 3 | ACVP 最终版向量（**非 round-3**）→ 扁平化黄金向量 | ✅ `vectors/` |
| 4 | crypto 后端抽象（liboqs ↔ 未来 FPGA 核可替换） | ✅ `include/pqchsm/pqc.h` |
| 5 | 槽位管理器 / 密钥库 / 包裹 / 恢复 / 审计 | ✅ 见 `doc/STATUS.md` |

### 一键跑通

```bash
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

消毒器构建（内存/未定义行为/数据竞争）：

```bash
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan -j && ctest --test-dir build-asan
```

---

## 技术栈选择理由

### 编排层：C11（不是 Python）

路线图本身已经给了答案——§5.7.1 的桩加速器写作 `fw/hal/accel_stub.c`，Phase 5 写的是
"槽位管理器 **C 库**"。在此之上有三条独立理由：

1. **零化（zeroize）在 Python 里不可验证。** Python 的 `bytes` 不可变、赋值即拷贝，
   解释器、GC 和字符串驻留会在你不知道的地方留下副本——路线图 §8.7 红线
   "恢复过程中间值用后即清"、Phase 7 "内存 dump 中搜不到 KEK 明文" 在 Python 里
   **根本无法写出成立的测试**。C 里 `OPENSSL_cleanse` + `mlock` 才有意义，
   而且可以写"清零后扫描整块缓冲断言全 0"的真实负测试。
2. **PKCS#11 是 C ABI。** 第 4 步的对外接口是一个导出 `C_GetFunctionList` 的动态库，
   用 Python 写等于将来整层重写。
3. **PS 侧固件直接复用。** 这套代码最终跑在 Zynq 的 Cortex-A 上（裸机或 Linux），
   C11 + 无第三方运行时可以直接交叉编译过去。

代价是迭代比 Python 慢。用两个办法补偿：小步提交 + 每步配单测；把**数据加工**
（ACVP JSON 解析、向量导出）放到 Python，C 只读极简扁平格式（见下）。

### 密码运算：liboqs 0.16.0（不自己写算法）

- 直接对齐**最终版** FIPS 203/204，不是 round-3（路线图 §5.1.1 反复强调的版本陷阱）。
- ML-KEM 提供完整**去随机化 API**（`keypair_derand` / `encaps_derand`），
  ACVP 的 keyGen / encapDecap 向量可以逐条驱动。
- ML-DSA **没有**去随机化 API — 本项目通过 `OQS_randombytes_custom_algorithm`
  注入确定性随机源来实现（见 `src/crypto/oqs_rng.c`）。这不只是测试手段：
  路线图 §7.6 的**种子存储优化**（每槽只存 32B ξ，用时重展开）在生产路径上就需要
  "从种子确定性生成 ML-DSA 密钥对"。

### 对称原语：OpenSSL 3

AES-256-GCM（§8.2 包裹）、SHA3-256（§8.6 审计哈希链）、KMAC256（§8.1 KDF）
全都现成且经过 FIPS 验证路径，无需引第二个库。liboqs 本身也依赖 openssl@3。

### 向量流水线：Python 加工 → C 消费扁平格式

ACVP 是嵌套 JSON，在 C 里解析要引 JSON 库、且没有复用价值。做法：
`tools/acvp_to_kat.py` 把 ACVP 的 `prompt.json` + `expectedResults.json` 合并、
展平成 `key = hex` 的文本格式落到 `vectors/`。

这不是权宜之计——路线图 §5.1.3 要求的正是 `vectors/` 下一批
"`$readmemh` 可直接读"的分层黄金向量，将来 cocotb testbench 和 RTL 仿真
消费的是同一批文件。C 侧因此只需要一个 60 行的 key=hex 解析器。

---

## 目录结构

```
pqc-hsm/
├── include/pqchsm/     公共头：pqc.h(算法后端抽象) util.h kat.h
├── src/
│   ├── crypto/         pqc_liboqs.c(后端) oqs_rng.c(确定性RNG) kdf.c(KMAC/SHA3) kdr.c(根密钥桩)
│   ├── hal/            (Phase 7) 桩加速器 / AXI mmap —— 与真 PL 二选一
│   ├── slot/           fsm.c(状态机) slot.c(主体) meta.c(元数据KMAC) persist.c(槽位↔密文)
│   ├── store/          wrap.c(AES-GCM 包裹) keystore.c(密钥库文件、原子写)
│   ├── backup/         shamir.c(GF(256) 门限) backup.c(RMK→BEK 备份恢复)
│   ├── audit/          audit.c —— append-only 哈希链日志
│   └── util/           hex、安全清零/分配、KAT 解析
├── tests/
│   ├── unit/           各模块单测
│   └── kat/            ACVP 黄金向量回归
├── tools/              acvp_to_kat.py 等 Python 加工脚本
├── vectors/
│   ├── acvp/           上游原始 JSON（不改动）
│   └── *.kat           展平后的黄金向量（C 与未来 cocotb 共用）
└── doc/
```

## 当前状态 / 下一步

见 [`doc/STATUS.md`](doc/STATUS.md)。
