**English** · [中文](cross-build.zh-CN.md)

# Cross-compiling for Zynq-7000 (XC7Z020)

The XC7Z020 processing system is a dual-core Cortex-A9: 32-bit ARMv7-A with
NEON and VFPv3, running a hard-float Linux userland (`arm-linux-gnueabihf`).
The development host for this project is 64-bit arm64. The two differ in data
model, and that difference is the reason this document exists:

| | Development host (aarch64) | Target (armv7l) |
|---|---|---|
| Data model | LP64 | ILP32 |
| `long`, `unsigned long` | 8 bytes | **4 bytes** |
| pointer, `size_t`, `ptrdiff_t` | 8 bytes | **4 bytes** |
| `off_t` (glibc default) | 8 bytes | **4 bytes** |
| `time_t` (glibc default) | 8 bytes | **4 bytes** |
| `long long`, `uint64_t` | 8 bytes | 8 bytes |

Any code that assumes `long` or a pointer can hold a 64-bit value builds and
passes its tests on the host, and only breaks on the target. `board/xc7z020/tools/armv7_test.sh`
exists to close that gap before the board is in the loop.

## Toolchain and dependencies

Debian multiarch supplies everything except liboqs:

```sh
dpkg --add-architecture armhf
apt-get update
apt-get install -y \
    crossbuild-essential-armhf \   # arm-linux-gnueabihf gcc/g++ and armhf libc
    qemu-user-static \             # qemu-arm-static, to execute the output
    libssl-dev:armhf \             # OpenSSL 3 for the target
    cmake ninja-build curl python3
```

`libssl-dev:armhf` installs its libraries into `/usr/lib/arm-linux-gnueabihf`
and shares `/usr/include` with the host packages. The sysroot is therefore `/`,
not a separate tree — which is why the toolchain file sets
`CMAKE_LIBRARY_ARCHITECTURE` rather than `CMAKE_SYSROOT`.

liboqs has no armhf Debian package and must be built from source with the same
toolchain file.

## CMake toolchain file

`board/xc7z020/cmake/armv7-linux-gnueabihf.cmake`. Three points in it are not
boilerplate:

- **`CMAKE_SYSTEM_PROCESSOR` is `armv7l`, not `arm`.** liboqs matches this
  string against `armel|armhf|armv7|arm32v7` to select its 32-bit ARM
  architecture branch; the broader spelling `arm` makes it fail configuration
  with "Unknown or unsupported processor".
- **`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard`.** Not `neon-vfpv4` — that
  floating-point unit belongs to Cortex-A7/A15 and does not exist on the A9.
- **`CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER`, the rest `BOTH`.** Build-time
  tools (`cmake`, `python3`, `verilator`) must be host binaries, while libraries
  and headers legitimately live in host paths under multiarch.

It also sets `CMAKE_CROSSCOMPILING_EMULATOR` to `qemu-arm-static` when that
binary is present, so `ctest` can run the cross-built executables directly.

The CPU, FPU and triple are cache variables, so a PetaLinux or Xilinx SDK
toolchain can be substituted without editing the file:

```sh
cmake -S . -B build-armv7 \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DPQCHSM_ARMV7_TRIPLE=arm-xilinx-linux-gnueabi \
      -DCMAKE_PREFIX_PATH=/path/to/liboqs-armv7
```

## Cross-compiling liboqs

Only the six parameter sets this project uses are built, which keeps the build
to well under a minute:

```sh
cmake -S liboqs -B liboqs/build -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release -DOQS_BUILD_ONLY_LIB=ON -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/oqs-armv7 \
  -DOQS_MINIMAL_BUILD="KEM_ml_kem_512;KEM_ml_kem_768;KEM_ml_kem_1024;SIG_ml_dsa_44;SIG_ml_dsa_65;SIG_ml_dsa_87"
ninja -C liboqs/build install
```

liboqs selects its `arm32v7` architecture branch and builds the portable C
reference implementations; there is no ARM32 assembly path in liboqs 0.16.0 for
ML-KEM or ML-DSA, so the target gets the same code the host reference build uses.

## Building and testing the project

```sh
cmake -S . -B build-armv7 -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DCMAKE_PREFIX_PATH=/opt/oqs-armv7
ninja -C build-armv7
ctest --test-dir build-armv7 --output-on-failure
```

`board/xc7z020/tools/armv7_test.sh` performs all of the above inside a
`linux/arm64` Debian container, from a clean slate, and reports the type widths
it actually built against. It skips cleanly (exit 0) when Docker is unavailable,
and it forwards the host HTTP proxy into the container when
`127.0.0.1:$PQCHSM_PROXY_PORT` (default 6152) is listening.

### Why cross-compile instead of an emulated container

`tools/aarch64_test.sh` can use a native `linux/arm64` container because the
development host is itself arm64. That option does not exist for armv7: Apple
Silicon does not implement the AArch32 execution state, so 32-bit ARM code can
only be emulated. Compiling the project and liboqs inside an emulated container
is an order of magnitude slower than cross-compiling and emulating only the
tests, which is what the script does.

### QEMU coverage and its limits

When the kernel has a `binfmt_misc` handler registered for `qemu-arm`, armv7
binaries execute transparently and the entire `ctest` suite runs, including the
cases driven by shell scripts (`cli_smoke`, `e2e_p11`, `p11_demos`) and the one
that `dlopen`s the PKCS#11 shared object. Without `binfmt_misc`,
`CMAKE_CROSSCOMPILING_EMULATOR` still covers every case whose command is an
executable target, but a shell script that execs a binary itself does not
inherit the emulator prefix; the script excludes those cases and says so.

Two categories of result do not transfer from QEMU to the board:

- **Timing.** `ct_timing` is a Welch t-test over measured execution times. Under
  emulation it verifies that the statistical machinery and the control cases
  behave correctly, not that the target hardware is constant-time. A
  constant-time claim for the Cortex-A9 requires measurement on the board.
- **Performance.** `pqchsm-bench` numbers under QEMU say nothing about the A9.

Everything else — data model, struct layout, glibc behaviour, syscall
semantics, `dlopen` and symbol export — is exercised faithfully.

## Deployment on the target

### Type widths must match the build

The project defines `_FILE_OFFSET_BITS=64` and, when the C library supports it,
`_TIME_BITS=64` on 32-bit targets (see `CMakeLists.txt`). Both macros change the
size of types that appear in function signatures, so they must be defined
consistently for every translation unit — including any out-of-tree code that
links against `libpqchsm`. Mixing them produces an ABI mismatch that no
compiler warning catches.

### PYNQ

The PYNQ image for the PYNQ-Z2 is Ubuntu-based armhf. Install `libssl-dev` and a
cross-built or natively built liboqs, then build on the board or copy the
cross-built tree over. The DMA buffer comes from `pynq.allocate`, which returns
a `dma-coherent` (uncached) buffer whose physical address is known only at run
time — that path goes through Python and does not use `board/xc7z020/src/accel_zynq.c`.

### PetaLinux

Use the SDK's `arm-*-linux-gnueabihf` toolchain by overriding
`PQCHSM_ARMV7_TRIPLE`. Two device-tree-dependent facts must line up with
`board/xc7z020/include/pqc_accel_zynq.h`:

- the PL address ranges bound to `generic-uio`, so `/sys/class/uio/uioN/maps`
  reports the addresses the header expects;
- the `reserved-memory` node backing `PQC_ZYNQ_DMA_BUF_PHYS`, which must be
  physically contiguous because AXI-DMA sees physical addresses.

### Cache coherency

The PS7 HP ports are not coherent with the Cortex-A9 L1/L2 caches. The DMA
buffer must be an uncached mapping; `accel_zynq.c` opens `/dev/mem` with
`O_SYNC` for exactly this reason. Getting this wrong produces intermittent
wrong results rather than a clean failure.

### Root filesystem

`ctest` on the target additionally needs `python3` for the structural checks
(`ct_audit`, `zeroize_check`, `prim_count`, `kdr_no_readback`). Those checks
scan source text and are architecture-independent, so they can equally be run on
the host; a minimal target rootfs does not need Python.

## 32-bit versus 64-bit behaviour: findings

### Scan scope and method

All C sources under `src/`, `cli/`, `tests/`, `tools/`, `include/`,
`board/xc7z020/src/` and `board/xc7z020/tests/` were examined for:

- assumptions about `sizeof(long)`, `sizeof(size_t)`, `sizeof(void *)`;
- `printf`/`scanf` conversion specifiers against their argument types;
- pointer/integer round trips;
- `time_t` and `off_t` use;
- struct padding and alignment assumptions in serialisation;
- alignment requirements of 64-bit integers;
- `CK_ULONG` width in the PKCS#11 front end.

Mechanically, the cross build was repeated with
`-Wformat=2 -Wconversion -Wsign-conversion -Wpointer-to-int-cast -Wint-to-pointer-cast -Wshift-count-overflow -Wshift-overflow=2 -Wpadded`
and every diagnostic triaged. Those flags are deliberately **not** enabled in
`CMakeLists.txt`: `-Wconversion` and `-Wpadded` fire on a large volume of
correct, deliberate code, and a warning set nobody can keep clean stops being
read. They are a periodic audit tool, and the command line above is the record
of how to reproduce the audit.

### Confirmed defect: PKCS#11 object handles

`CK_OBJECT_HANDLE` is `CK_ULONG`, and `CK_ULONG` is `unsigned long`
(`third_party/pkcs11-v3.2/pkcs11t.h`). It is 4 bytes on armv7l. The handle
encoding assumed 8:

```c
#define PUB_BIT (1ULL << 63)
return ((CK_OBJECT_HANDLE)m.generation << 32) | (CK_OBJECT_HANDLE)(slot + 1);
```

On the target the shift is undefined behaviour (`-Wshift-count-overflow` reports
it), and both flag bits are truncated away on assignment to a 4-byte handle.
Observable consequences, all reproduced under QEMU before the fix:

- `pub == priv` — the public and private key objects became the same handle, so
  `C_GetAttributeValue` reported `CKO_PRIVATE_KEY` for a public key object and
  refused to return `CKA_VALUE`;
- `C_SignInit` **accepted a public key handle for signing** instead of returning
  `CKR_KEY_TYPE_INCONSISTENT`, because the test that rejects it is `hKey & PUB_BIT`;
- `SECRET_BIT` truncated to zero, so KEM session key objects were unreachable;
- the generation field vanished from every handle, so stale-handle rejection
  after `C_DestroyObject` no longer worked;
- the test binary ended in a segmentation fault.

The encoding is now width-independent and fits in 32 bits: bit 31 public,
bit 30 session-secret, bits 12–29 the low 18 bits of the slot generation, bits
0–11 the object index. Conversion to and from the 64-bit core handle
(`hsm_handle_t`) is explicit, in `p11_handle_of_core()` and `core_handle_of()`;
the core layer still performs the full 64-bit generation comparison, so
truncating the generation to 18 bits only affects the module's own early check.

### Confirmed hazard: default `off_t` and `time_t`

On armv7l glibc, both default to 32-bit signed. Two consequences:

- a physical address at or above `0x8000_0000` becomes negative when cast to
  `off_t`, and `mmap` fails with `EINVAL`. `accel_zynq.c` maps PL addresses
  below that boundary today, but `map_devmem()` accepts any `uint32_t`, and the
  PS peripheral and OCM regions are above it;
- `time()` returns a negative value after 2038-01-19, and the audit and slot
  metadata timestamps cast that to `uint64_t`, producing a very large
  meaningless number.

`CMakeLists.txt` now defines `_FILE_OFFSET_BITS=64` on 32-bit targets, and
`_TIME_BITS=64` after a compile probe confirms the C library supports it
(glibc 2.34 or newer). Verified widths with those macros: `off_t` 8, `time_t` 8.

Note that `ftell`/`fseek` return and take `long` and stay 32-bit regardless.
The keystore, backup and anchor readers use them, which caps those files at
2 GiB. They check for a short or negative result and fail closed, so the
limitation is a bounded capacity limit rather than a correctness bug.

### Width-fragile parse, corrected

`uio_find_by_phys()` read the UIO map address with `fscanf(f, "%lx", &addr)`
into an `unsigned long`. The same sysfs text overflows differently depending on
whether `unsigned long` is 4 or 8 bytes. It now parses into `unsigned long long`
with `%llx` and compares after an explicit narrowing, so an LPAE kernel
reporting an address above 4 GiB simply fails to match instead of matching a
truncated value.

### Test harness narrowing, corrected

`CHECK_EQ_INT` in `tests/testlib.h` compared through `long`, which truncates
64-bit values on the target: two values differing only above bit 31 would have
compared equal — a silently passing assertion. It now compares through
`long long`. No current call site was affected (handle comparisons use `CHECK`
with native types), so this is a latent hazard removed, not a defect fixed.

`tests/unit/test_p11.c` hard-coded `1ULL << 63` as the public-key flag; it now
uses a 32-bit-safe constant matching the module.

### Examined and found correct

- **Serialisation.** No whole-struct `memcpy`, `fwrite` or `pwrite` anywhere;
  every on-disk and on-wire format is encoded field by field, byte at a time.
  Struct padding (84 `-Wpadded` reports) is therefore confined to in-memory
  layout and does not reach any format. The same property means there are no
  endianness assumptions and no unaligned wide accesses: a grep for casts to
  `uint16_t *`, `uint32_t *`, `uint64_t *` and friends returns nothing outside
  the `volatile uint32_t *` MMIO register windows, which mmap guarantees to be
  page-aligned.
- **`CK_ULONG` as a length type.** The PKCS#11 API passes almost every length
  through `CK_ULONG`, so its narrowing to 4 bytes was checked separately from
  the handle defect. `attr_ulong()` gates on `a->ulValueLen == sizeof(CK_ULONG)`
  rather than a literal 8, `fill_attr()` is width-agnostic, and every
  `*pulXxxLen = (CK_ULONG)` assignment carries a value bounded by an ML-KEM or
  ML-DSA artefact size (at most a few kilobytes). Module and application are
  compiled for the same ABI, so the shared `CK_ULONG` width is consistent
  across the boundary.
- **String-to-integer parsing.** `cli/pqchsm_cli.c` uses `strtoull` for the
  64-bit session and object handles and `strtoul` only for `uint32_t` fields,
  which is correct on both widths.
- **Remaining `-Wconversion` reports.** `tests/fuzz/fuzz_targets.c` narrows
  `rnd() % n` from `uint64_t` to `size_t`; the result is bounded by the modulus,
  so no value is lost. `src/hal/pqc_accel.c` converts a range-checked positive
  `int` to `size_t`. Both are correct as written on either width.
- **`%llu` formatting.** Every 64-bit print site already casts to
  `unsigned long long` and uses `%llu`, which is the width-independent choice;
  no site prints a `size_t` or `uint64_t` with `%lu`.
- **`accel_zynq.c` register and buffer arithmetic.** Offsets, lengths and
  physical addresses are explicitly `uint32_t`, register access goes through
  `volatile uint32_t *` with word indexing, and buffer bounds are checked in
  `uint32_t`/`size_t` — all of which behave identically at both widths.
