[English](deployment.md) · **中文**

# 内网 Linux 部署指南

本文覆盖把纯软件部分（C 模块、测试、PKCS#11 前端、Python 与 Java 演示）部署到
内网 Linux 虚拟机的完整过程，重点解决离线环境下的依赖获取。不涉及 RTL 与开发板。

文中每条命令都在一台**断网**的 x86_64 Debian 12 容器里实测通过：仅凭事先拷入的
文件完成构建、`ctest` 45/45、两个演示各自全过。构建期间容器没有任何网络访问。

## 1. 目标环境

| 项目 | 要求 |
|---|---|
| 架构 | x86_64 |
| 发行版 | Debian 12 / Ubuntu 22.04 及以上；RHEL 9 系（含麒麟 V10、统信 UOS 等）见下方差异说明 |
| CPU | 2 核（构建 liboqs 与本项目时并行度越高越快） |
| 内存 | 2 GiB |
| 磁盘 | 5 GiB 空闲（含 JDK 与构建产物） |
| 权限 | 普通用户即可；只有把 liboqs 安装到 `/usr/local` 需要一次 `sudo` |

`glibc` 版本没有下限要求。构建全程不需要 root，也不需要打开任何网络端口 ——
本模块是一个库加几个命令行工具，不监听端口。

## 2. 把代码弄进内网

### 情况一：内网能访问 GitHub（通常要经代理）

```bash
export https_proxy=http://<代理地址>:<端口>
export http_proxy=$https_proxy
git clone -b main https://github.com/per1x/pqc-crypto-module.git
```

### 情况二：内网不通外网

在**有网的机器**上打包，再用可移动介质或跳板机拷进去。两种包任选其一。

`git bundle`（保留提交历史，便于以后增量更新）：

```bash
# 有网机器
git clone -b main https://github.com/per1x/pqc-crypto-module.git
cd pqc-crypto-module
git bundle create pqc-crypto-module.bundle main
```

```bash
# 内网机器
git clone -b main pqc-crypto-module.bundle pqc-hsm
cd pqc-hsm && git log --oneline -1
```

`-b main` **不能省**。bundle 里没有记录 HEAD 指向哪个分支，`git clone` 不带
`-b` 会落到一个空的 `master` 上并报
`your current branch 'master' does not have any commits yet`。

只要文件、不要历史时用 tar 包：

```bash
# 有网机器
git archive --format=tar.gz -o pqc-hsm-src.tar.gz --prefix=pqc-hsm/ main
# 内网机器
tar xzf pqc-hsm-src.tar.gz && cd pqc-hsm
```

## 3. 系统依赖

内网如果有单位自建的 apt/dnf 镜像源，直接装即可；完全离线的情况见第 4 节末尾。

### Debian / Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libssl-dev \
    python3 python3-venv python3-pip git file
```

`ninja-build` 是可选的：装了就可以在 `cmake` 时加 `-GNinja` 让构建快一些，
不装则用 CMake 默认的 Makefile 生成器，两条路都验证过。

### RHEL 9 系（含麒麟 V10、统信 UOS 等）

```bash
sudo dnf install -y gcc gcc-c++ make cmake openssl-devel \
    python3 python3-pip git file
```

三点差异：

- **没有 `ninja-build`**。它不在 RHEL 9 的基础仓库里，需要启用 CRB
  （`sudo dnf config-manager --set-enabled crb`）或 EPEL。不装也行，
  按上面的默认生成器构建即可。
- **`openssl-devel` 就是 OpenSSL 3**（RHEL 9 基线是 3.0，更新后为 3.5），
  满足要求。
- **Python 是 3.9**。这会影响第 4 节 PyKCS11 的 wheel 版本：wheel 与
  CPython 小版本绑定，Debian 12 上是 `cp311`，RHEL 9 上是 `cp39`，
  必须在与内网**同版本 Python** 的机器上生成。

版本下限：CMake ≥ 3.20、支持 C11 的编译器、OpenSSL ≥ 3.0。Debian 12
（CMake 3.25、GCC 12）与 RHEL 9（CMake 3.31、GCC 11.5）都满足。

## 4. 离线要预取的四样东西

在有网机器上准备好，一起拷进内网。总量约 160 MiB，其中 JDK 占大头。

| 文件 | 大小 | 用途 |
|---|---|---|
| `liboqs-0.16.0.tar.gz` | 9.4 MiB | ML-KEM / ML-DSA 实现 |
| `vectors.tar.gz` | 10 MiB | NIST ACVP 测试向量 |
| `pykcs11-*.whl` | 1.2 MiB | Python 演示 |
| `jdk-linux-x64.tar.gz` | 139 MiB | Java 演示（仅 Debian/Ubuntu 需要） |

### 4.1 liboqs 0.16

内网的发行版仓库里通常没有 liboqs，从源码编译。

```bash
# 有网机器
curl -fsSL -o liboqs-0.16.0.tar.gz \
  https://github.com/open-quantum-safe/liboqs/archive/refs/tags/0.16.0.tar.gz
```

```bash
# 内网机器
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

`OQS_MINIMAL_BUILD` 只编本项目用到的六个参数集，构建时间与体积都显著下降。
装成静态库（`BUILD_SHARED_LIBS=OFF`）可以免掉运行时的 `LD_LIBRARY_PATH` 配置。

**本项目怎么找到它**：`CMakeLists.txt` 在 `/usr/local/include` 与
`/usr/local/lib` 里找，所以按上面装到 `/usr/local` 之后无需任何额外参数。
装到别处时用

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<liboqs 安装前缀>
```

装好后应能看到这两个文件：

```
/usr/local/include/oqs/oqs.h
/usr/local/lib/liboqs.a
```

### 4.2 测试向量

`tools/fetch_vectors.sh` 会从 GitHub 拉 NIST ACVP 的 JSON 再展平成
`vectors/*.kat`，内网跑不了。在有网机器上跑一次，把整个目录打包带走：

```bash
# 有网机器（在仓库根目录）
./tools/fetch_vectors.sh
tar czf vectors.tar.gz vectors
```

```bash
# 内网机器（在仓库根目录）
tar xzf vectors.tar.gz
ls vectors/*.kat | wc -l      # 应为 7
```

`vectors/` 不入库（可复现的派生产物，且有 18 MiB 的原始 JSON），所以必须单独带。

**缺向量的后果**：6 个 `kat_*` 用例会**失败**，不是跳过 —— `ctest` 变成
39/45。其余 39 个用例仍然验证模块自身的全部逻辑（槽位状态机、密钥库、包裹、
备份恢复、审计链、常量时间、清零、上电自测、PKCS#11 前端）。也就是说没有向量
仍可确认模块能用，但**拿不到"算法与 NIST 向量逐字节一致"这条证据**，
而那正是合规材料里最关键的一项。

### 4.3 PyKCS11

不要在内网 `pip install`（要联网，且需要编译器与 SWIG）。在有网机器上先做成
wheel，内网只需安装现成的二进制：

```bash
# 有网机器，架构与 Python 小版本都要与内网一致
sudo apt-get install -y python3-dev build-essential swig libssl-dev
pip3 wheel --no-cache-dir -w pykcs11 PyKCS11
ls pykcs11        # pykcs11-1.5.18-cp311-cp311-linux_x86_64.whl
```

```bash
# 内网机器（在仓库根目录）
python3 -m venv .venv-p11
./.venv-p11/bin/pip install --no-index --find-links /path/to/pykcs11 PyKCS11
```

wheel 名字里的 `cp311` 是 CPython 3.11。内网 Python 版本不同就装不上，
必须在同版本的机器上重新生成（Debian 12 → `cp311`，RHEL 9 → `cp39`）。

### 4.4 JDK

Java 演示用 JDK 22 起才定型的 Foreign Function & Memory API。

- **RHEL 9 系**：仓库里有 `java-25-openjdk-devel`，直接
  `sudo dnf install -y java-25-openjdk-devel` 即可，不必额外下载。
- **Debian 12 / Ubuntu 22.04**：仓库里最高是 OpenJDK 17 / 21，**都不够**。
  在有网机器上取一个 Temurin 的 tarball 带进去：

```bash
# 有网机器
curl -fsSL -o jdk-linux-x64.tar.gz \
  "https://api.adoptium.net/v3/binary/latest/24/ga/linux/x64/jdk/hotspot/normal/eclipse"
```

```bash
# 内网机器
sudo mkdir -p /opt/jdk && sudo tar xzf jdk-linux-x64.tar.gz -C /opt/jdk --strip-components=1
/opt/jdk/bin/java -version
```

只想跑 C 模块与 `ctest` 时可以完全跳过 JDK。

### 4.5 连 apt/dnf 都不通的情况

如果内网连内部镜像源都没有，系统依赖有两条路：

- 用发行版安装 ISO 做本地仓库（Debian 系 `apt-cdrom add`，RHEL 系把 ISO 挂载后
  写一个指向它的 `.repo` 文件），基础编译工具链与 `libssl-dev`/`openssl-devel`
  都在 ISO 里；
- 在有网的**同版本同架构**机器上 `apt-get download <包名>`（或
  `dnf download --resolve`）把 `.deb`/`.rpm` 连依赖一起下好，拷进去
  `sudo dpkg -i *.deb`（或 `sudo rpm -Uvh *.rpm`）。

## 5. 构建与测试

```bash
cd pqc-hsm
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

装了 ninja 的话把第一条换成 `cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release`。
`Debug`（项目默认）与 `Release` 两种构建类型都验证过，结果一致。

配置阶段应看到：

```
-- liboqs: /usr/local/lib/liboqs.a
-- Verilator: 未找到 —— RTL 仿真后端不编入
```

第二行是**正常的**：Verilator 只用于 RTL 仿真后端，内网部署不需要。
可选组件是被检测而不是被要求的，缺了就不编进去。

预期结果：

```
100% tests passed, 0 tests failed out of 45
```

其中 `p11_smoke` 会因为没装 `pkcs11-tool` 自行跳过并打印说明，`rtl_sim` 同理
（需要 cocotb 与 iverilog）。跳过不计为失败。

## 6. 运行

### PKCS#11 模块

构建产物是 `build/pqchsm-pkcs11.so`（Linux 上是 `.so`，macOS 上是 `.dylib`）。
两个环境变量控制它的行为：

| 变量 | 默认 | 说明 |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | 密钥库文件路径 |
| `PQCHSM_SLOTS` | `4` | 槽位数，必须与密钥库一致 |

### Python 演示

```bash
cmake --build build --target pqchsm-p11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

预期 `通过 25，失败 0`。它自己建一个临时密钥库，可以反复运行。

### Java 演示

```bash
export LANG=C.UTF-8 LC_ALL=C.UTF-8
PQCHSM_KEYSTORE=$(mktemp -d)/keystore.bin \
  /opt/jdk/bin/java --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java
```

预期 `通过 30，失败 0`。两处细节：

- **每次用一个新的 `PQCHSM_KEYSTORE`**。这个演示从 `C_InitToken` 开始，
  对着一个已经初始化过的密钥库跑会得到 `通过 18，失败 12`。
  Python 演示自己建临时库，没有这个问题。
- **设 `LANG`**。最小化安装的系统常常没有 UTF-8 locale，Java 的标准输出编码
  会跟着退化，中文全部显示成 `?`。这只影响显示，不影响结果。

模块路径可以作为第一个参数显式传入；不传则在 `build/` 下按平台探测
`.so` 与 `.dylib`。

### 管理工具 pqchsm-admin

它直接操作密钥库文件，不经过 PKCS#11 会话。密钥库**必须已经存在**：
先用演示或 PKCS#11 前端建好，再用它管理。

```bash
./build/pqchsm-admin -k <密钥库> [-n <槽位数>] <命令>
```

| 命令 | 用途 |
|---|---|
| `list` | 列出各槽位状态 |
| `backup <备份文件> <分片前缀> <M> <N> <slot> <so-pin>` | 导出 M-of-N 备份 |
| `restore <备份文件> <分片文件...>` | 用给定分片恢复 |
| `zeroize-all` | 所有槽位清零（设备级，不可逆） |
| `audit-verify <日志> [锚点 <公钥文件>]` | 验证审计链，给了锚点就一并验签名 |

实际输出：

```
$ ./build/pqchsm-admin -k /tmp/demo-ks.bin -n 4 list
slot   label              state     alg            usage    policy
0      javaDemo           LOADED    ML-DSA-65      0x4      0x2
1      javaKem            LOADED    ML-KEM-768     0x2      0x2
2      -                  UNINIT    -              0x0      0x0
3      -                  UNINIT    -              0x0      0x0
```

`-n` 必须与建库时的槽位数一致，否则会因为元数据完整性校验失败而拒绝装载 ——
这是设计如此，不是缺陷：槽位数参与元数据的 MAC。

### 守护进程与客户端

`build/pqchsmd` 与 `build/pqchsm-cli` 是 TLV 协议的服务端与客户端，用于把模块
放在单独进程里。内网只跑演示与测试时不需要它们。

## 7. 常见问题

**`找不到 liboqs。macOS: brew install liboqs`（配置阶段直接失败）**
liboqs 没装、或没装到 `/usr/local`。确认 `/usr/local/lib/liboqs.a` 与
`/usr/local/include/oqs/oqs.h` 都在；装在别处时加
`-DCMAKE_PREFIX_PATH=<前缀>`。改完要删掉 `build/` 重新配置，CMake 会缓存查找
结果。

**6 个 `kat_*` 用例失败**
`vectors/` 缺失或不完整。按 4.2 节带一份进来，`ls vectors/*.kat | wc -l` 应为 7。

**Java 报 `--enable-native-access` 不认识，或 FFM 相关的类找不到**
JDK 版本低于 22。`java -version` 确认；Debian/Ubuntu 仓库里的 17/21 都不够，
按 4.4 节换 Temurin。

**Java 演示第二次跑就有 12 条失败**
密钥库已经初始化过。每次换一个新的 `PQCHSM_KEYSTORE`，见第 6 节。

**Java 输出全是 `?`**
系统没有 UTF-8 locale。`export LANG=C.UTF-8`。

**`pip install PyKCS11` 卡住或报找不到包**
在内网直接 pip 会去连公网。按 4.3 节改成 `--no-index --find-links`。
若报 wheel 不兼容，是 CPython 小版本对不上，要在同版本机器上重新生成 wheel。

**`装载密钥库失败: metadata integrity failure —— 槽位数对不上？`**
`pqchsm-admin` 的 `-n` 与建库时的槽位数不一致。

**权限与 SELinux**
模块是普通用户态共享库，SELinux 的默认策略不会拦。实际会踩到的是另外两件事：
拷贝进来的目录挂载带 `noexec`（构建产物无法执行，换到 home 下重建即可）；
以及密钥库所在目录不可写。密钥库本身按 `0600` 创建。

**`mlock` 相关**
敏感缓冲会尽力 `mlock`，但 `RLIMIT_MEMLOCK` 不足时静默跳过，不作为错误 ——
功能不受影响，只是页可能被换出。想启用就调高 `ulimit -l`。
真正不可妥协的是释放时的清零，那条不依赖任何权限。

## 8. 本文验证到什么程度

| 项目 | 实测环境 | 结果 |
|---|---|---|
| 断网构建 | Debian 12 x86_64，容器 `--network none` | 通过 |
| `ctest` | 同上，Release 与 Debug | 45 / 45 |
| 无 `vectors/` 时的行为 | 同上 | 39 / 45，6 个 `kat_*` 失败 |
| 不装 ninja（默认生成器） | 同上 | 45 / 45 |
| Python 演示 | 同上，PyKCS11 由 wheel 离线安装 | 通过 25，失败 0 |
| Java 演示 | 同上，Temurin 24 | 通过 30，失败 0 |
| `pqchsm-admin list` | 同上 | 正常列出槽位 |
| RHEL 9 系包名 | Rocky Linux 9 x86_64 | 除 `ninja-build` 外均可直接安装 |

未实测的部分：麒麟、统信等具体发行版的仓库内容（按 RHEL 9 系推断，包名与
可用性以手上镜像为准）；使用安装 ISO 作本地仓库的步骤。
