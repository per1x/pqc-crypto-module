#!/usr/bin/env python3
"""TRNG 熵调理器的参考模型（Keccak 海绵）

与 hardware/rtl/trng/trng_cond.v 逐比特对应，cocotb 用它做对拍。

海绵的参数与 RTL 一致：
  rate     = RATE_LANES 个 lane（默认 17 → 1088 bit，SHA3-256 的 rate）
  capacity = 25 - RATE_LANES 个 lane（默认 8 → 512 bit）
  每 ABSORB_BLOCKS 个 rate 块挤出一次，每次 OUT_LANES 个 lane

两个容易写反、必须和 RTL 严格一致的约定：

  1. **比特进 lane 的顺序是 LSB 优先。** RTL 里是 `{bit_in, lane_sr[63:1]}`，
     移满 64 次之后第一个比特落在 bit 0。
  2. **挤出的 32 位字是先低后高。** lane0 的低 32 位、lane0 的高 32 位、
     lane1 的低 32 位……与 pqc_accel_axi.v 存 Keccak 结果的顺序一致。

用法：python3 hardware/model/trng_cond_model.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ref_model import keccak_f1600  # noqa: E402

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1


class SpongeConditioner:
    """逐比特推进的海绵调理器"""

    def __init__(self, rate_lanes: int = 17, absorb_blocks: int = 1,
                 out_lanes: int = 4):
        self.rate_lanes = rate_lanes
        self.absorb_blocks = absorb_blocks
        self.out_lanes = out_lanes

        # 海绵状态从不重新初始化（duplex 模式），只有 zeroize 才清零
        self.state = [0] * 25
        self.blocks_absorbed = 0

        self._lane = 0        # 正在拼装的 lane
        self._nbits = 0       # 已拼进去几个比特
        self._lane_idx = 0    # 本块吸收到第几个 lane
        self._blk = 0         # 本轮已吸收几个 rate 块

    def zeroize(self) -> None:
        self.state = [0] * 25
        self._lane = 0
        self._nbits = 0
        self._lane_idx = 0
        self._blk = 0

    def feed(self, bit: int) -> list[int]:
        """喂一个原始比特，返回本次挤出的 32 位字（通常是空表）"""
        self._lane |= (bit & 1) << self._nbits
        self._nbits += 1
        if self._nbits < 64:
            return []

        # 一个 lane 满了：读-改-写，异或注入状态
        self.state[self._lane_idx] ^= self._lane
        self._lane = 0
        self._nbits = 0
        self._lane_idx += 1
        if self._lane_idx < self.rate_lanes:
            return []

        # 一个 rate 块满了：置换
        self._lane_idx = 0
        self.blocks_absorbed += 1
        self.state = keccak_f1600(self.state)

        self._blk += 1
        if self._blk < self.absorb_blocks:
            return []

        # 吸收够了：挤出
        self._blk = 0
        out: list[int] = []
        for i in range(self.out_lanes):
            out.append(self.state[i] & MASK32)
            out.append((self.state[i] >> 32) & MASK32)
        return out

    def feed_bits(self, bits) -> list[int]:
        out: list[int] = []
        for b in bits:
            out.extend(self.feed(b))
        return out


def _self_test() -> None:
    import random

    # 1) 全零输入：吸收 1088 个 0 之后状态仍为全零，置换一次得到的是
    #    Keccak-f[1600] 作用在零状态上的结果 —— 这个值可以独立核对。
    c = SpongeConditioner()
    out = c.feed_bits([0] * 1088)
    assert len(out) == 8, f"应挤出 8 个字，实得 {len(out)}"
    ref = keccak_f1600([0] * 25)
    assert out[0] == ref[0] & MASK32 and out[1] == (ref[0] >> 32) & MASK32, \
        "全零输入的挤出结果与直接置换零状态不符"

    # 2) 比特序：只把第一个比特置 1，它必须落在 lane0 的 bit 0
    c = SpongeConditioner()
    bits = [1] + [0] * 1087
    c.feed_bits(bits)
    ref = keccak_f1600([1] + [0] * 24)
    assert c.state == ref, "LSB 优先的比特序没对上"

    # 3) 吸收够了才挤出：ABSORB_BLOCKS=2 时，第一个 rate 块不产出
    c = SpongeConditioner(absorb_blocks=2)
    assert c.feed_bits([1] * 1088) == [], "第一块就挤出了，ABSORB_BLOCKS 没生效"
    assert len(c.feed_bits([1] * 1088)) == 8, "第二块没有挤出"
    assert c.blocks_absorbed == 2

    # 4) 输出看起来是均匀的（不是证明，只是防低级错误：比如恒零、恒重复）
    rng = random.Random(20260812)
    c = SpongeConditioner()
    words = c.feed_bits([rng.getrandbits(1) for _ in range(1088 * 8)])
    assert len(words) == 64
    assert len(set(words)) == 64, "挤出的字里有重复，海绵状态可能没在推进"
    ones = sum(bin(w).count("1") for w in words)
    assert 0.45 < ones / (64 * 32) < 0.55, f"输出比特偏置异常：{ones / (64 * 32):.3f}"

    print("trng_cond_model 自检通过：零输入、比特序、多块吸收、输出分布均正确")


if __name__ == "__main__":
    _self_test()
