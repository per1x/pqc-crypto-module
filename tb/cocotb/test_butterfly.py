"""cocotb testbench：CT / GS 蝶形对拍"""
import sys
from pathlib import Path

import cocotb
from cocotb.triggers import Timer

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "model"))

from ref_model import ct_butterfly, gs_butterfly  # noqa: E402

VEC = ROOT / "vectors" / "rtl"


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def load(name: str):
    path = VEC / name
    if not path.exists():
        raise FileNotFoundError(f"{path} 不存在 —— 先跑 model/export_vectors.py")
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        rows.append(line.split())
    return rows


async def _run(dut, vecfile, model_fn, name):
    rows = load(vecfile)
    assert len(rows) == 1000
    for i, (a_h, b_h, z_h, ao_h, bo_h) in enumerate(rows):
        a, b, z = s16(int(a_h, 16)), s16(int(b_h, 16)), s16(int(z_h, 16))
        exp_a, exp_b = s16(int(ao_h, 16)), s16(int(bo_h, 16))
        dut.a.value = a & 0xFFFF
        dut.b.value = b & 0xFFFF
        dut.zeta.value = z & 0xFFFF
        await Timer(1, unit="ns")
        got_a = s16(int(dut.a_out.value))
        got_b = s16(int(dut.b_out.value))
        m_a, m_b = model_fn(a, b, z)
        assert (got_a, got_b) == (exp_a, exp_b) == (m_a, m_b), (
            f"{name} 第 {i} 条：RTL=({got_a},{got_b}) 向量=({exp_a},{exp_b}) "
            f"模型=({m_a},{m_b})，输入 a={a} b={b} zeta={z}")
    dut._log.info(f"{name}: {len(rows)} 条三方一致")


@cocotb.test()
async def test_butterfly(dut):
    """按 TOPLEVEL 自动选 CT 还是 GS"""
    top = str(dut._name)
    if "gs" in top:
        await _run(dut, "butterfly_gs.hex", gs_butterfly, "butterfly_gs")
    else:
        await _run(dut, "butterfly_ct.hex", ct_butterfly, "butterfly_ct")
