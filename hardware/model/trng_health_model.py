#!/usr/bin/env python3
"""噪声源连续健康检测的参考模型（NIST SP 800-90B §4.4）

阈值在这里**按定义现算**，而不是抄一张表：抄表最容易出的问题是取错一行，
而现算之后，RTL 的参数取值就可以被测试台反过来验证。

  重复计数检测 RCT：C = 1 + ceil(−log2(α) / H)
  自适应比例检测 APT：C = 满足 P(X ≥ C) ≤ α 的最小整数，X ~ B(W, 2⁻ᴴ)

其中 H 是每样本最小熵，α 是误报率（标准建议 2⁻²⁰），W 是窗口长度
（二元源 1024，其它 512）。

用法：python3 hardware/model/trng_health_model.py
"""
from __future__ import annotations

from math import ceil, comb, log2

ALPHA = 2.0 ** -20


def rct_cutoff(min_entropy: float, alpha: float = ALPHA) -> int:
    """重复计数检测的阈值：连续这么多个相同样本即告警"""
    return 1 + ceil(-log2(alpha) / min_entropy)


def apt_cutoff(min_entropy: float, window: int, alpha: float = ALPHA) -> int:
    """自适应比例检测的阈值：窗口内等于参考值的样本数超过它即告警

    从分布的上尾往下累加，找到第一个使尾概率超过 α 的取值，其上一个即为阈值。
    """
    p = 2.0 ** (-min_entropy)
    tail = 0.0
    for k in range(window, -1, -1):
        tail += comb(window, k) * (p ** k) * ((1.0 - p) ** (window - k))
        if tail > alpha:
            return k + 1
    return 0


def apt_window(sample_bits: int) -> int:
    """SP 800-90B 规定的窗口长度：二元源 1024，其它 512"""
    return 1024 if sample_bits == 1 else 512


class HealthTests:
    """把两项检测串起来的参考实现，逐样本推进，与 RTL 的行为一一对应"""

    def __init__(self, rct_c: int, apt_w: int, apt_c: int):
        self.rct_c = rct_c
        self.apt_w = apt_w
        self.apt_c = apt_c
        self.rct_alarm = False
        self.apt_alarm = False
        self.rct_run = 0
        self.apt_count = 0
        self.apt_index = 0
        self._prev = None
        self._ref = None

    def clear(self) -> None:
        self.rct_alarm = False
        self.apt_alarm = False

    def feed(self, sample: int) -> None:
        # 重复计数检测
        if self._prev is None or sample != self._prev:
            self._prev = sample
            self.rct_run = 1
        else:
            self.rct_run += 1
            if self.rct_run >= self.rct_c:
                self.rct_alarm = True

        # 自适应比例检测
        if self._ref is None or self.apt_index == self.apt_w:
            self._ref = sample
            self.apt_count = 1
            self.apt_index = 1
        else:
            self.apt_index += 1
            if sample == self._ref:
                self.apt_count += 1
                if self.apt_count > self.apt_c:
                    self.apt_alarm = True

    @property
    def alarm(self) -> bool:
        return self.rct_alarm or self.apt_alarm


# 本工程 RTL 默认参数对应的噪声源假设：1 位样本，每样本最小熵 0.5
DEFAULT_SAMPLE_BITS = 1
DEFAULT_MIN_ENTROPY = 0.5


def default_parameters() -> tuple[int, int, int]:
    w = apt_window(DEFAULT_SAMPLE_BITS)
    return rct_cutoff(DEFAULT_MIN_ENTROPY), w, apt_cutoff(DEFAULT_MIN_ENTROPY, w)


def _self_test() -> None:
    import random

    rct_c, apt_w, apt_c = default_parameters()
    assert (rct_c, apt_w, apt_c) == (41, 1024, 793), \
        f"默认参数与定义式不符：{(rct_c, apt_w, apt_c)}"

    # 与标准中广为引用的取值一致：H = 1 位/样本、W = 1024 时 APT 阈值为 589
    assert apt_cutoff(1.0, 1024) == 589

    # 卡死的噪声源：第 C 个样本上 RCT 必须告警，第 C−1 个上必须还没有
    h = HealthTests(rct_c, apt_w, apt_c)
    for i in range(rct_c - 1):
        h.feed(1)
        assert not h.rct_alarm, f"第 {i + 1} 个相同样本就告警了"
    h.feed(1)
    assert h.rct_alarm, "连续 C 个相同样本没有触发 RCT"

    # 理想的均匀源：不应告警
    rng = random.Random(20260730)
    h = HealthTests(rct_c, apt_w, apt_c)
    for _ in range(20000):
        h.feed(rng.getrandbits(1))
    assert not h.alarm, "均匀随机源上出现了误报"

    # 严重偏置的源：APT 必须在一个窗口内抓到
    h = HealthTests(rct_c, apt_w, apt_c)
    for _ in range(apt_w):
        h.feed(0 if rng.random() < 0.95 else 1)
    assert h.apt_alarm, "95% 偏置的源没有触发 APT"

    print(f"trng_health_model 自检通过：RCT C={rct_c}，APT W={apt_w} C={apt_c}；"
          f"卡死、偏置、均匀三种源的判定均正确")


if __name__ == "__main__":
    _self_test()
