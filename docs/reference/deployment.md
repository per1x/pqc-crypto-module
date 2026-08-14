**English** · [中文](deployment.zh-CN.md)

# Deployment on an intranet Linux host

This document covers deploying the software-only part — the C module, its tests,
the PKCS#11 front end, and the Python and Java demos — onto a Linux virtual
machine on an isolated network. The emphasis is on obtaining dependencies without
internet access. RTL and boards are out of scope.

Every command here was executed in a **network-disabled** x86_64 Debian 12
container: the build, `ctest` at 45/45, and both demos completed using only
pre-staged files, with no network access at any point.

## 1. Target environment

| Item | Requirement |
|---|---|
| Architecture | x86_64 |
| Distribution | Debian 12 / Ubuntu 22.04 or newer; RHEL 9 family (including Kylin V10, UOS) — differences noted below |
| CPU | 2 cores (build parallelism is the only thing that benefits) |
| Memory | 2 GiB |
| Disk | 5 GiB free, including the JDK and build outputs |
| Privileges | An ordinary user suffices; only installing liboqs into `/usr/local` needs one `sudo` |

There is no minimum `glibc` version. Nothing in the build requires root, and no
network port is opened — the module is a library plus a few command-line tools,
and it does not listen on anything.

## 2. Getting the code onto the intranet host

### Case A: the host can reach GitHub (usually through a proxy)

```bash
export https_proxy=http://<proxy>:<port>
export http_proxy=$https_proxy
git clone -b main https://github.com/per1x/pqc-crypto-module.git
```

### Case B: no route to the internet

Package on an internet-connected machine and carry it across. Either form works.

`git bundle`, which preserves history and allows incremental updates later:

```bash
# internet-connected machine
git clone -b main https://github.com/per1x/pqc-crypto-module.git
cd pqc-crypto-module
git bundle create pqc-crypto-module.bundle main
```

```bash
# intranet host
git clone -b main pqc-crypto-module.bundle pqc-hsm
cd pqc-hsm && git log --oneline -1
```

`-b main` is **not optional**. A bundle records no HEAD branch, so `git clone`
without `-b` lands on an empty `master` and reports
`your current branch 'master' does not have any commits yet`.

A tarball, when only the files are wanted:

```bash
# internet-connected machine
git archive --format=tar.gz -o pqc-hsm-src.tar.gz --prefix=pqc-hsm/ main
# intranet host
tar xzf pqc-hsm-src.tar.gz && cd pqc-hsm
```

## 3. System dependencies

If the intranet has an internal apt/dnf mirror, install normally. For a host with
no package repository at all, see the end of section 4.

### Debian / Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libssl-dev \
    python3 python3-venv python3-pip git file
```

`ninja-build` is optional: with it, add `-GNinja` to `cmake` for a faster build;
without it, CMake's default Makefile generator is used. Both paths are verified.

### RHEL 9 family (including Kylin V10, UOS)

```bash
sudo dnf install -y gcc gcc-c++ make cmake openssl-devel \
    python3 python3-pip git file
```

Three differences:

- **No `ninja-build`.** It is not in the RHEL 9 base repositories; it requires
  CRB (`sudo dnf config-manager --set-enabled crb`) or EPEL. It is not needed —
  use the default generator.
- **`openssl-devel` *is* OpenSSL 3** (3.0 at the RHEL 9 baseline, 3.5 after
  updates), which satisfies the requirement.
- **Python is 3.9.** This affects the PyKCS11 wheel in section 4: wheels are tied
  to the CPython minor version — `cp311` on Debian 12, `cp39` on RHEL 9 — so the
  wheel must be produced on a machine running the **same** Python version.

Minimum versions: CMake ≥ 3.20, a C11 compiler, OpenSSL ≥ 3.0. Both Debian 12
(CMake 3.25, GCC 12) and RHEL 9 (CMake 3.31, GCC 11.5) satisfy them.

## 4. Four artefacts to stage offline

Prepare these on an internet-connected machine and carry them across. About
160 MiB in total, dominated by the JDK.

| File | Size | Purpose |
|---|---|---|
| `liboqs-0.16.0.tar.gz` | 9.4 MiB | ML-KEM / ML-DSA implementation |
| `vectors.tar.gz` | 10 MiB | NIST ACVP test vectors |
| `pykcs11-*.whl` | 1.2 MiB | Python demo |
| `jdk-linux-x64.tar.gz` | 139 MiB | Java demo (Debian/Ubuntu only) |

### 4.1 liboqs 0.16

Distribution repositories generally do not carry liboqs. Build it from source.

```bash
# internet-connected machine
curl -fsSL -o liboqs-0.16.0.tar.gz \
  https://github.com/open-quantum-safe/liboqs/archive/refs/tags/0.16.0.tar.gz
```

```bash
# intranet host
mkdir -p ~/liboqs && tar xzf liboqs-0.16.0.tar.gz -C ~/liboqs --strip-components=1
cmake -S ~/liboqs -B ~/liboqs/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DOQS_BUILD_ONLY_LIB=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DOQS_MINIMAL_BUILD="KEM_ml_kem_512;KEM_ml_kem_768;KEM_ml_kem_1024;SIG_ml_dsa_44;SIG_ml_dsa_65;SIG_ml_dsa_87"
cmake --build ~/liboqs/build -j"$(nproc)"
sudo cmake --install ~/liboqs/build
```

`OQS_MINIMAL_BUILD` restricts the build to the six parameter sets this project
uses, cutting build time and size substantially. A static library
(`BUILD_SHARED_LIBS=OFF`) avoids any runtime `LD_LIBRARY_PATH` configuration.

**How this project finds it**: `CMakeLists.txt` searches
`/usr/local/include` and `/usr/local/lib`, so installing to `/usr/local` as above
needs no extra arguments. For another location:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<liboqs install prefix>
```

Afterwards these two files must exist:

```
/usr/local/include/oqs/oqs.h
/usr/local/lib/liboqs.a
```

### 4.2 Test vectors

`tools/fetch_vectors.sh` downloads NIST ACVP JSON from GitHub and flattens it
into `vectors/*.kat`, which cannot run on an isolated host. Run it once on an
internet-connected machine and carry the whole directory:

```bash
# internet-connected machine, at the repository root
./tools/fetch_vectors.sh
tar czf vectors.tar.gz vectors
```

```bash
# intranet host, at the repository root
tar xzf vectors.tar.gz
ls vectors/*.kat | wc -l      # must be 7
```

`vectors/` is not tracked in git — it is a reproducible derivative and carries
18 MiB of raw JSON — so it has to travel separately.

**Consequence of missing vectors**: the six `kat_*` cases **fail**; they do not
skip. `ctest` becomes 39/45. The other 39 cases still exercise all of the
module's own logic (slot state machine, keystore, wrapping, backup and recovery,
audit chain, constant time, zeroisation, pre-operational self-tests, the PKCS#11
front end). In other words the module can still be shown to work, but **the
evidence that the algorithms match NIST vectors byte-for-byte is exactly what is
lost** — and that is the most important item in a compliance package.

### 4.3 PyKCS11

Do not `pip install` on the intranet host: that needs network access, a compiler,
and SWIG. Build a wheel on the internet-connected machine so the host only
installs a prebuilt binary:

```bash
# internet-connected machine; architecture and Python minor version must match the host
sudo apt-get install -y python3-dev build-essential swig libssl-dev
pip3 wheel --no-cache-dir -w pykcs11 PyKCS11
ls pykcs11        # pykcs11-1.5.18-cp311-cp311-linux_x86_64.whl
```

```bash
# intranet host, at the repository root
python3 -m venv .venv-p11
./.venv-p11/bin/pip install --no-index --find-links /path/to/pykcs11 PyKCS11
```

The `cp311` in the wheel name is CPython 3.11. A host with a different Python
version cannot install it, and the wheel must be regenerated on a machine with
the matching version (Debian 12 → `cp311`, RHEL 9 → `cp39`).

### 4.4 JDK

The Java demo uses the Foreign Function & Memory API, finalised in JDK 22.

- **RHEL 9 family**: `java-25-openjdk-devel` is in the repositories, so
  `sudo dnf install -y java-25-openjdk-devel` is enough — no separate download.
- **Debian 12 / Ubuntu 22.04**: the repositories top out at OpenJDK 17 / 21,
  **neither of which is sufficient**. Fetch a Temurin tarball:

```bash
# internet-connected machine
curl -fsSL -o jdk-linux-x64.tar.gz \
  "https://api.adoptium.net/v3/binary/latest/24/ga/linux/x64/jdk/hotspot/normal/eclipse"
```

```bash
# intranet host
sudo mkdir -p /opt/jdk && sudo tar xzf jdk-linux-x64.tar.gz -C /opt/jdk --strip-components=1
/opt/jdk/bin/java -version
```

The JDK can be skipped entirely if only the C module and `ctest` are needed.

### 4.5 When even apt/dnf is unavailable

If the host has no package repository at all, there are two routes for system
dependencies:

- Use the distribution's installation ISO as a local repository (`apt-cdrom add`
  on Debian, or mount the ISO and write a `.repo` file pointing at it on RHEL).
  The base toolchain and `libssl-dev`/`openssl-devel` are on the ISO.
- On an internet-connected machine of the **same distribution version and
  architecture**, run `apt-get download <pkg>` (or `dnf download --resolve`) to
  fetch the packages with their dependencies, carry them across, and install with
  `sudo dpkg -i *.deb` (or `sudo rpm -Uvh *.rpm`).

## 5. Build and test

```bash
cd pqc-hsm
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

With ninja installed, make the first command
`cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release`. Both `Debug` (the
project default) and `Release` are verified and agree.

Configuration should report:

```
-- liboqs: /usr/local/lib/liboqs.a
-- Verilator: 未找到 —— RTL 仿真后端不编入
```

The second line is **expected**: Verilator is only used by the RTL simulation
backend, which an intranet deployment does not need. Optional components are
detected, not required, and are simply left out when absent.

Expected result:

```
100% tests passed, 0 tests failed out of 45
```

Within that, `p11_smoke` skips itself with a message because `pkcs11-tool` is not
installed, and `rtl_sim` does the same for cocotb and iverilog. A skip is not a
failure.

## 6. Running

### The PKCS#11 module

The build produces `build/pqchsm-pkcs11.so` (`.so` on Linux, `.dylib` on macOS).
Two environment variables control it:

| Variable | Default | Meaning |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | keystore file path |
| `PQCHSM_SLOTS` | `4` | slot count, must match the keystore |

### Python demo

```bash
cmake --build build --target pqchsm-p11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

Expected: `通过 25，失败 0`. It creates its own temporary keystore and can be run
repeatedly.

### Java demo

```bash
export LANG=C.UTF-8 LC_ALL=C.UTF-8
PQCHSM_KEYSTORE=$(mktemp -d)/keystore.bin \
  /opt/jdk/bin/java --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java
```

Expected: `通过 30，失败 0`. Two details:

- **Use a fresh `PQCHSM_KEYSTORE` each run.** This demo starts from
  `C_InitToken`; run against an already-initialised keystore it reports
  `通过 18，失败 12`. The Python demo creates its own temporary keystore and does
  not have this property.
- **Set `LANG`.** Minimal installations frequently lack a UTF-8 locale, which
  degrades Java's stdout encoding and renders all Chinese output as `?`. This
  affects display only, not results.

The module path can be passed as the first argument; without it, `build/` is
probed for `.so` and `.dylib` according to platform.

### The pqchsm-admin tool

It operates directly on the keystore file rather than through a PKCS#11 session.
The keystore **must already exist**: create it with a demo or through the PKCS#11
front end first.

```bash
./build/pqchsm-admin -k <keystore> [-n <slots>] <command>
```

| Command | Purpose |
|---|---|
| `list` | list slot states |
| `backup <file> <share-prefix> <M> <N> <slot> <so-pin>` | export an M-of-N backup |
| `restore <file> <share-files...>` | restore from the given shares |
| `zeroize-all` | zeroise every slot (device level, irreversible) |
| `audit-verify <log> [anchor <pubkey>]` | verify the audit chain, and the signature if an anchor is given |

Actual output:

```
$ ./build/pqchsm-admin -k /tmp/demo-ks.bin -n 4 list
slot   label              state     alg            usage    policy
0      javaDemo           LOADED    ML-DSA-65      0x4      0x2
1      javaKem            LOADED    ML-KEM-768     0x2      0x2
2      -                  UNINIT    -              0x0      0x0
3      -                  UNINIT    -              0x0      0x0
```

`-n` must match the slot count the keystore was created with, or loading is
refused with a metadata integrity failure. That is by design, not a defect: the
slot count is covered by the metadata MAC.

### Daemon and client

`build/pqchsmd` and `build/pqchsm-cli` are the server and client for the TLV
protocol, used to host the module in a separate process. They are not needed to
run the demos or the test suite.

## 7. Troubleshooting

**`找不到 liboqs。macOS: brew install liboqs` — configuration fails immediately**
liboqs is missing, or not in `/usr/local`. Confirm both
`/usr/local/lib/liboqs.a` and `/usr/local/include/oqs/oqs.h` exist; for another
location add `-DCMAKE_PREFIX_PATH=<prefix>`. Delete `build/` and reconfigure
afterwards — CMake caches the search result.

**Six `kat_*` cases fail**
`vectors/` is missing or incomplete. Stage it per section 4.2;
`ls vectors/*.kat | wc -l` must be 7.

**Java rejects `--enable-native-access`, or FFM classes are not found**
The JDK is older than 22. Check `java -version`; the 17/21 packages in
Debian/Ubuntu repositories are not sufficient — use Temurin per section 4.4.

**The Java demo fails 12 assertions on a second run**
The keystore is already initialised. Use a fresh `PQCHSM_KEYSTORE` each time, per
section 6.

**Java output is all `?`**
The system has no UTF-8 locale. `export LANG=C.UTF-8`.

**`pip install PyKCS11` hangs or reports the package cannot be found**
pip is trying to reach the public index. Use `--no-index --find-links` per
section 4.3. An "incompatible wheel" error means the CPython minor version does
not match; regenerate the wheel on a machine with the same version.

**`装载密钥库失败: metadata integrity failure —— 槽位数对不上？`**
The `-n` given to `pqchsm-admin` does not match the keystore's slot count.

**Permissions and SELinux**
The module is an ordinary user-space shared library and default SELinux policy
does not interfere. Two things do bite in practice: a transfer directory mounted
`noexec`, so build outputs cannot execute (rebuild under `$HOME`); and a keystore
directory that is not writable. The keystore itself is created `0600`.

**`mlock`**
Sensitive buffers are `mlock`-ed on a best-effort basis; when `RLIMIT_MEMLOCK` is
too low it is skipped silently and is not treated as an error — functionality is
unaffected, pages may simply be swappable. Raise `ulimit -l` to enable it. What
is not negotiable is zeroisation on free, and that depends on no privilege.

## 8. What this document was verified against

| Item | Environment | Result |
|---|---|---|
| Offline build | Debian 12 x86_64, container with `--network none` | pass |
| `ctest` | as above, Release and Debug | 45 / 45 |
| Behaviour without `vectors/` | as above | 39 / 45, six `kat_*` fail |
| Without ninja (default generator) | as above | 45 / 45 |
| Python demo | as above, PyKCS11 installed offline from a wheel | 通过 25，失败 0 |
| Java demo | as above, Temurin 24 | 通过 30，失败 0 |
| `pqchsm-admin list` | as above | slots listed correctly |
| RHEL 9 family package names | Rocky Linux 9 x86_64 | all install directly except `ninja-build` |

Not verified: the repository contents of specific distributions such as Kylin and
UOS (inferred from the RHEL 9 family; check package names and availability
against the image in hand), and the installation-ISO-as-local-repository route.
