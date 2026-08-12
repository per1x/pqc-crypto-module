**English** · [中文](constant-time.zh-CN.md)

# Constant-time behaviour and memory zeroization

This document records what has been audited, by what method, what the audit found, and
where the guarantee stops. For the layering it applies to see
[architecture.md](architecture.md).

Two properties are covered:

- **Constant time** — the execution time of code that touches key material must not
  depend on the value of that material.
- **Zeroization** — key material must not survive in memory past the point where it is
  no longer needed.

Neither property can be maintained by care alone. Both are invisible to functional
tests: a `memcmp` on a MAC tag, an early `return` that skips a wipe, or a table lookup
indexed by a secret byte all pass every correctness test in the suite. Both are
therefore expressed as checks that run on every regression.

## Scope

| In scope | Out of scope |
|---|---|
| Everything under `src/` | liboqs internals |
| The comparison primitive `pqc_ct_equal` | OpenSSL internals |
| The GF(256) arithmetic in `src/backup/shamir.c` | `hardware/` RTL and its Python model |
| Key-material lifetimes in `src/` and `include/` | `cli/`, `tests/`, `demo/` |

The excluded items are excluded for different reasons, stated honestly:

- **liboqs and OpenSSL** implement every cryptographic primitive on the live path. Their
  timing behaviour is a property of those projects, not of this one. This project makes
  no claim about it and cannot verify it from the outside. Where liboqs uses hand-written
  assembly, source-level auditing would not be meaningful in any case.
- **`hardware/`** is register-transfer logic, where the relevant question is cycle
  counts, not instruction timing. `ntt_core` and `keccak_f1600` both run a fixed number
  of cycles per transform (2305 and 24 respectively), independent of the data — but that
  is verified by the cocotb regression, not by these tools.
- **`cli/` and `tests/`** do not hold long-lived key material; the daemon passes PINs
  straight through to the slot manager and wipes its stack buffers.

## Method

### Source-level audit — `tools/ct_audit.py`

The tool strips comments and string literals, then applies four rules to what remains:

| Rule | What it flags |
|---|---|
| R1 comparison | `memcmp` / `strcmp` / `strncmp` / `bcmp` applied to secret data |
| R2 branch | `if` / `while` / `switch` / `?:` whose controlling expression reads secret data |
| R3 index | an array subscript expression derived from secret data |
| R4 division | a divisor derived from secret data |

"Secret" is decided by identifier naming (`sk`, `seed`, `pin`, `kek`, `bek`, `rmk`,
`cek`, `ss`, `verifier`, `tag`, …), with an exclusion list for public quantities whose
names contain the same words (`sk_len`, `key_type`, `pin_flags`, …). Naming-based
classification over-reports rather than under-reports, which is the right bias for an
audit tool.

Two refinements keep the output actionable rather than noisy:

- **Passing a secret into a function is not a branch on it.** `if (pqc_wrap(kek, …) != 0)`
  branches on a return code. The tool collapses call expressions before analysing a
  condition, so only values read directly in the condition count.
- **An access chain is classified by its last member.** `g_secrets[i].in_use` yields
  `in_use`, not `g_secrets`; `s->sk` yields `sk`.

A pointer-validity test (`if (!sk)`, `sk == NULL`) is not a data-dependent branch and is
not flagged.

### Whitelist

A finding can be declared reviewed with a comment marker, on the same line or in the
comment block immediately above it:

```c
/* tag is the TLV type field here, not a MAC. The frame structure is visible to an
 * attacker during parsing anyway.
 * 常量时间: the condition is a public protocol tag, unrelated to any key material */
if (t == tag) {
```

A reason after the marker is mandatory. Whitelisted entries are printed in their own
section of the report rather than disappearing, so every "this one is fine" carries a
visible justification.

### Statistical timing test — `tests/unit/test_ct_timing.c`

A source-level audit says nothing about what the compiler and the pipeline actually do.
The timing test measures it directly, following the dudect approach: two input classes —
one fixed, one random — sampled in randomly interleaved order, then Welch's t-test on
the two samples. If execution time is independent of the data, the two samples are drawn
from the same distribution and |t| stays in the noise.

The test is built so that it cannot pass vacuously:

- **Negative control.** The same sampling and test are applied to `leaky_equal()`, an
  early-returning comparison. It **must** be classified as data-dependent. If it is not,
  the apparatus has no discriminating power and the test fails rather than reporting
  success.
- **Null control.** Both classes are drawn at random. Under that condition even
  `leaky_equal()` must not be flagged. This rules out the opposite failure — an
  apparatus that reports a difference between anything.

Stability on loaded machines comes from a conservative threshold (|t| ≤ 25, against
dudect's usual 10), cropping the slowest 10 % of samples in each round (long tails come
from scheduling, not from the code under test), and taking the median over 7 rounds.

### Zeroization check — `tools/check_zeroize.py`

Two structural checks, sharing the secret-naming classifier with the audit tool so that
both reports agree on what counts as key material:

| Check | What it requires |
|---|---|
| A structure coverage | every key-material field of a structure is wiped by its destructor |
| B return paths | a local key buffer is wiped before every `return` that follows a write to it |

Check A resolves the destructor set transitively: `slot_wipe_key_material()` and
`slot_wipe_pins()` together cover `slot_t`, and a whole-structure
`pqc_secure_zero(x, sizeof(*x))` counts for every field. A structure whose *type name*
denotes a secret (`p11_secret_t`) is required to be wiped whole rather than field by
field, so that adding a field later cannot silently escape.

Check B is a linear approximation — it advances through the function body in textual
order and performs no control-flow analysis — so it over-reports where the write and
the early return lie on mutually exclusive paths. Both checks use the marker
`无需清零:` with a mandatory reason, placed either on the buffer's declaration (exempting
it entirely) or on a specific `return`.

### Zeroization test — `tests/unit/test_zeroize.c`

Five groups of runtime assertions. Three deserve mention:

- **Structural struct coverage.** After a slot is zeroized, the test copies the whole
  `slot_t`, punches out the handful of fields that are legitimately non-zero (the mutex,
  the metadata, its KMAC tag, the unlock target state), and asserts every remaining byte
  is zero. A field added later falls into "every remaining byte" by default, so
  forgetting to wipe it fails here instead of slipping past a list of per-field
  assertions.
- **Dead-store elimination.** `pqc_secure_zero` is only useful if the store survives
  optimisation. A `noinline` function fills a stack frame with a sentinel, wipes it,
  publishes the frame address through a `volatile` pointer, and returns; the caller then
  reads the dead frame back. The translation unit is compiled at `-O2` specifically so
  the compiler has a motive to remove the store. The negative control — the same probe
  with no wipe at all — must find the sentinel; if it does not, the probe never read the
  right memory and the conclusion would be worthless.
- **Post-free residue.** A pool of blocks is filled with a sentinel, released, and a
  fresh pool is allocated with plain `malloc` and scanned. This check is gated by its own
  negative control: plain `malloc`/`free` must leave the sentinel visible, otherwise the
  allocator itself clears blocks on release and the observation has no discriminating
  power on that platform. When the control fails the test says so and skips, rather than
  recording a pass.

## Findings

Running the audit over `src/` produced four real findings. All four are fixed.

### `src/hal/accel_stub.c` — branch on the signing randomness

The register interface has no way to pass a null pointer, so an all-zero `rnd` field is
the agreed sentinel for "let the backend take its own randomness". The sentinel was
detected with a per-byte branch:

```c
int all_zero = 1;
for (int z = 0; z < 32; z++) {
    if (rnd[z]) {
        all_zero = 0;
    }
}
```

`rnd` is the ML-DSA signing randomness — secret. The branch leaks which byte is first
non-zero. Replaced with a branchless OR-accumulation over a fixed 32 iterations.

### `src/hal/accel_stub.c` — private key left in a buffer on an error path

In the signing path the private key is copied into a local buffer before the context
length is validated. The validation failure returned without wiping it, leaving a full
ML-DSA private key in a `static` buffer for the lifetime of the process. A
`pqc_secure_zero` was added on that path.

### `src/store/keystore.c` — partial key left on a derivation failure

`keystore_save_impl()` derives the KEK and the file-MAC key in a single condition. If
the second derivation fails the first has already written a complete KEK into the
buffer, and the function returned without wiping it. Both buffers and the salt are now
wiped on that path.

### `src/crypto/kdr.c`, `src/hal/pqc_accel.c`, `src/slot/slot.c` — unwiped buffers on failure paths

Three further paths returned without wiping a seed or root-key buffer that a failed KDF
or a failed `RAND_bytes` may already have partially written. All three now wipe first.

### Findings that were naming collisions, not defects

Four flagged sites were false positives caused by generic names, resolved at the source
rather than by whitelisting, because the names were genuinely misleading:

- `accel_stub.c` used `rnd` as the Keccak round counter, colliding with `rnd` meaning
  signing randomness elsewhere in the same file. Renamed to `round`.
- `p11_module.c` used `want` for the requested object class and `slot.c` used `want` for
  the requested state, colliding with `want` meaning "the recomputed expected tag" in the
  crypto sources. Renamed to `want_class` and `want_state`.

One site is whitelisted: `tlv_find()` in `src/proto/proto.c` branches on `tag`, which is
the TLV type field — part of the frame structure, visible to anyone who can see the
wire, and unrelated to any MAC.

## Exemptions in the zeroization check

Eight entries are declared exempt with reasons. They fall into two groups.

**Public integrity tags.** `keystore_save_impl().tag`, `hsm_keystore_load().want`,
`slot_meta_verify().want`, `slot_t.meta_tag`, and the audit chain hashes in
`audit_verify_file()` and `audit_read()` are MACs and hashes that are written to disk or
published in the anchor file. Wiping a value that the module itself publishes would be
theatre; what has to be wiped is the key that computed it, and that is wiped. `meta_tag`
additionally *must not* be cleared: zeroization resets the metadata and re-seals it, so
the slot remains integrity-checkable after being emptied.

**Object handles.** `p11_session_t.sign_key` and `.verify_key` are PKCS#11 object
handles — a slot number and a generation counter. The key material they refer to never
leaves the slot.

## Constant-time constructions already in place

The audit confirmed rather than introduced these. They are listed because their reasons
are not obvious from the code alone:

- **`pqc_ct_equal`** (`src/util/util.c`) accumulates XOR differences over the full
  length and compares once at the end. It is used for every PIN verifier, wrap tag,
  metadata tag, share checksum, backup MAC, and audit chain hash comparison in the
  codebase. The audit found no `memcmp` on secret data anywhere in `src/`: the six
  `memcmp` calls that exist all compare file-format magic numbers.
- **GF(256) multiplication** (`src/backup/shamir.c`) uses the bitwise Russian-peasant
  method rather than log/antilog tables. Table lookups would be indexed by secret data
  and would leave a trace in the D-cache. The loop runs a fixed eight iterations with no
  data-dependent memory access; coefficient selection is done entirely with 0x00/0xFF
  masks.
- **GF(256) inversion** computes `a^254` through a fixed addition chain rather than the
  extended Euclidean algorithm, so the chain length does not depend on `a`.
- **Lagrange interpolation** at x=0 depends only on the share indices, which are public;
  the per-byte accumulation is XOR with no branches.

## What is not claimed

- **Nothing is claimed about liboqs or OpenSSL.** Every ML-KEM and ML-DSA operation on
  the live path runs inside liboqs. If liboqs has a timing side channel, this module has
  it too, and no check here would detect it.
- **The audit is a source-level lexical analysis, not a compiler-level one.** It cannot
  see a branch the compiler introduces, a `cmov` the compiler declines to emit, or a
  secret spilled to a stack slot that is later reloaded. The timing test covers the
  comparison primitive empirically; the rest of the codebase is covered by the source
  audit only.
- **The zeroization check is a static approximation.** Check B has no control-flow
  analysis, so it can miss a path where a buffer is written inside one branch and
  returned from another in a shape the linear scan does not model.
- **Nothing is claimed about memory outside the process address space.** CPU registers,
  swapped-out pages, and DRAM contents after power-off are not covered. `mlock` is
  attempted for key buffers but is best-effort and unverified under memory pressure.
- **The post-free residue observation is platform-dependent.** On glibc the control
  finds the sentinel in 32 of 32 reallocated blocks, so the check runs with full
  discriminating power on the Linux regression. On macOS the system allocator clears
  blocks on release; the control detects that, and the test reports that it skipped
  rather than recording a pass.

## Reproducing

```bash
# Source-level audit. Self-test first: a scanner that cannot find anything
# proves nothing when it reports "no findings".
python3 tools/ct_audit.py --self-test
python3 tools/ct_audit.py

# Zeroization structure check, same order.
python3 tools/check_zeroize.py --self-test
python3 tools/check_zeroize.py

# Runtime checks.
cmake --build build --target test_ct_timing test_zeroize
./build/test_ct_timing
./build/test_zeroize

# All five are wired into ctest.
ctest --test-dir build -R 'ct_audit|ct_timing|zeroize'
```

Both tools exit non-zero when an unannotated finding exists, so they fail the build
rather than printing a warning nobody reads.

Observed output of the timing test on the development machine (Apple M-series, macOS,
Apple clang):

```
    比较长度 4096 字节，每类每轮 20000 次采样，7 轮取中位数，裁掉最慢的 10%
    反证   leaky_equal（提前退出）     |t| =   1444.4  门限 25
    被测   pqc_ct_equal                |t| =      0.7  门限 25
    空对照 两类均随机：leaky |t| =    0.0   ct |t| =    0.4
```

and in the aarch64 Linux container (Debian, GCC 12, glibc):

```
    反证   leaky_equal（提前退出）     |t| =    366.8  门限 25
    被测   pqc_ct_equal                |t| =      1.5  门限 25
    空对照 两类均随机：leaky |t| =    1.0   ct |t| =    1.1
```

The two to three orders of magnitude between the negative control and the primitive
under test are the point: the apparatus demonstrably separates a data-dependent
comparison from a constant-time one, `pqc_ct_equal` lands on the constant-time side, and
the margin to the threshold is wide enough on both toolchains that scheduling noise does
not move the verdict.

`test_zeroize` reports 110 assertions on GCC 12 / glibc against 108 on macOS: the two
extra ones are the post-free residue check, which runs there because its negative
control succeeds. Both toolchains observe the plain `memset` being eliminated as a dead
store while the `pqc_secure_zero` in the same position survives — which is the whole
reason `pqc_secure_zero` exists.
