# 为什么这台机器拉 GitHub 那么慢，以及怎么绕过去

> 2026-07-30。起因：`tools/aarch64_test.sh` 在容器里拉 liboqs 时长时间卡住，
> CPU 只有 1.7%——不是脚本死循环，是网络。

## 结论先行

| | 直连 | 走本机代理 | 倍数 |
|---|---:|---:|---:|
| 宿主 curl `codeload.github.com` | **36.8 KB/s** | **3.86 MB/s** | **105×** |
| **容器内** curl 同一 URL | **14.2 KB/s** | **8.4 MB/s** | **592×** |
| **容器内** `apt-get update` | **>150 s 超时** | **3 s** | **>50×** |
| `git clone` liboqs（79 MB） | 25 s 拉不完 | **2.26 s** | — |
| `git clone` 本仓库（SSH） | 21.2 s | **5.0 s** | 4.2× |
| 实际 `git push` 本仓库 | 20 s | **3.7 s** | 5.4× |

**根因有两层，容器那层才是真正卡住 CI 的：**

1. **宿主上直连 GitHub 本身就慢**（36.8 KB/s）。宿主已配了系统代理
   （Surge，`127.0.0.1:6152`），`HTTP_PROXY`/`HTTPS_PROXY` 环境变量也有，
   所以**交互式用 curl / git-over-https 是走代理的、不慢**。
2. **容器完全不受这些影响**。Docker 容器不继承宿主的 `HTTP_PROXY`，
   而且容器里的 `127.0.0.1` 是容器自己 —— 所以 `aarch64_test.sh` 里那句
   `git clone https://github.com/open-quantum-safe/liboqs.git` 是**彻底直连**，
   14 KB/s 拉 9.4 MB 的源码包，十几分钟且经常断。**这就是卡住的直接原因。**
3. **SSH 是第三条独立的路**。`git@github.com:` 走 SSH，而 SSH **不读**
   `http_proxy`/`HTTPS_PROXY`，所以 push/fetch 一直是直连的慢路径
   （克隆本仓库这么小的东西都要 21 秒）。

## 诊断记录

### DNS：没有被污染，但被代理接管了

| 主机 | 系统解析器 | `dig @1.1.1.1` | `dig @8.8.8.8` |
|---|---|---|---|
| `github.com` | 20.205.243.166 | 140.82.116.4 | **198.18.31.107** |
| `codeload.github.com` | 20.205.243.165 | 140.82.116.9 | **198.18.61.12** |
| `raw.githubusercontent.com` | 185.199.108.133… | 185.199.108.133… | **198.18.32.36** |

`198.18.0.0/15` 是 RFC 2544 的基准测试保留网段，**不是真实路由地址** ——
这是 Surge 的 **fake-IP**：查询被本地代理截获并返回一个占位地址，
真实连接由 Surge 按规则代发。所以 `dig @8.8.8.8` 那一列**不是 DNS 污染**，
是本机代理在工作。系统解析器与 `@1.1.1.1` 给的都是 GitHub 的真实 IP。

> 注意 `github.com` 系统解析到 20.205.243.166（GitHub 的亚洲入口，ping 92 ms、
> 丢包 33%），而 `@1.1.1.1` 给的是 140.82.116.4（美国，TLS 握手 1.65 s vs 0.21 s）。
> 两个都通，快慢差别主要在丢包与拥塞，不在解析。

### 一个容易误读的现象

直连 `curl` 的 `time_connect` 是 **0.0003 s**，而该 IP 的 ping 是 **92 ms**。
TCP 握手不可能比 RTT 还快 —— 说明 Surge 的 TUN/增强模式在**本地就终结了 SYN**，
再按规则决定这条连接是代发还是直出。所以"直连慢"准确的说法是：
**Surge 对这些流量命中了 DIRECT 规则，走物理链路出去，然后就慢。**
这不是本机配置错了，是这条物理链路到 GitHub 本身就差。

### 各条路径到底走没走代理

| 通道 | 读 `http_proxy`？ | 读系统代理？ | 结论 |
|---|---|---|---|
| `curl`（交互式 shell） | ✅ | — | 走代理，快 |
| `git` over HTTPS | ✅（libcurl） | ❌ | 走代理，快 |
| `git` over **SSH** | ❌ | ❌ | **一直直连，慢** |
| **Docker 容器内一切** | ❌（不继承） | ❌ | **一直直连，最慢** |

## 做了哪些改动（全部用户级、可逆）

### 1. git 对 GitHub 走代理（**按 URL 限定，不是全局**）

```bash
git config --global http.https://github.com/.proxy      http://127.0.0.1:6152
git config --global http.https://gist.github.com/.proxy http://127.0.0.1:6152
```

刻意**没有**用全局 `http.proxy` —— 那会把国内/内网的 https remote（gitee、
公司 GitLab）也一并塞进代理。验证：

```
git config --get-urlmatch http.proxy https://github.com/foo  → http://127.0.0.1:6152
git config --get-urlmatch http.proxy https://gitee.com/foo   → （空，不走代理）
```

**撤销**：`git config --global --unset-all http.https://github.com/.proxy`

### 2. SSH 经代理的 CONNECT 隧道（带自动回退）

`~/.ssh/config` 的 `Host github.com` 段里加了：

```
ProxyCommand sh -c 'nc -z 127.0.0.1 6152 2>/dev/null && exec nc -X connect -x 127.0.0.1:6152 %h %p || exec nc %h %p'
```

前半段先探测代理端口在不在，**不在就回退直连** —— Surge 关掉时 push 仍然能用，
只是慢，不会直接失败。已用"把探测端口指到无人监听的 59999"做过反证：
克隆照样成功。

**撤销**：`~/.ssh/config.bak-<时间戳>` 是改动前的备份，覆盖回去即可。

### 3. `tools/aarch64_test.sh`：把宿主代理透进容器

```bash
PROXY_PORT="${PQCHSM_PROXY_PORT:-6152}"
if nc -z 127.0.0.1 "$PROXY_PORT"; then
  PROXY_ARGS=(--add-host=host.docker.internal:host-gateway
              -e http_proxy=http://host.docker.internal:6152
              -e https_proxy=... -e HTTP_PROXY=... -e HTTPS_PROXY=...)
fi
```

同样是**探测到才加**，没有代理的机器上脚本原样直连、不会因为这段而失败。
端口可用 `PQCHSM_PROXY_PORT` 覆盖。

顺带把 `git clone --depth 1` 换成 `curl codeload .../tar.gz | tar xz`：
git 的 smart-http 是多次往返，在高延迟链路上比单个 HTTP 流吃亏。

#### 中间走过一次弯路，值得记下来

第一版把 `*_proxy` 传给整个容器，跑起来卡在 `apt-get update` 上。
我当时判断是"Debian 源直连本来很快，绕代理反而慢"，于是改成**只**给拉 liboqs
的那条 `curl` 加 `--proxy` —— 结果还是卡在同一个地方。

问题出在判断依据：那次观察时有两个容器在抢同一条链路，数据是脏的。
清场后单跑一次带上限的对比，结论正好相反：

```
apt-get update  直连        TIMEOUT  用时 150s（上限）
apt-get update  经宿主代理  OK       用时 3s
```

**apt 直连一样慢**，需要代理的不只是 GitHub。所以最终版回到"整个容器走代理"。
教训是普通的那一条：**在有竞争的环境里测出来的数不能用来下结论**，
而一个听起来合理的解释（"Debian 源在国内有镜像所以快"）如果没量过，
就只是猜测。

## 修复后的验证

`tools/aarch64_test.sh` 完整跑一次（GCC 12 / glibc / 原生 aarch64 容器）：

```
宿主 127.0.0.1:6152 在监听 -> 容器整体走宿主代理
aarch64 / Linux / gcc (Debian 12.2.0-14+deb12u1) / sizeof(long)=8
100% tests passed, 0 tests failed out of 38
aarch64 Linux 回归通过

real 25.766s      ← 修复前：20 分钟仍卡在拉 liboqs，从未跑完
```

**从"跑不完"变成 26 秒**，且 GCC 全警告集下 0 warning。

## 只诊断、没有动的（按要求留给你定）

- **没有改 `/etc/hosts`**。网上常见的"给 github.com 手工写 IP"的做法在这里
  没有意义 —— 解析本来就是对的，慢在链路不在解析；而且写死 IP 会在
  GitHub 调度变更时静默失效。
- **没有动 Surge 的规则或系统网络设置**。如果想从根上解决，方向是在 Surge 里
  把 `github.com` / `*.githubusercontent.com` / `codeload.github.com`
  的策略从 DIRECT 改成走代理节点 —— 那样上面三条用户级绕行都可以撤掉。
  这是你的代理配置，我不碰。
- **没有引入 ghproxy 之类的第三方镜像**。本机既然有可用代理，多引一个
  第三方中间人对一个密码学项目不是好主意（供应链面）。
