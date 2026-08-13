#!/usr/bin/env python3
"""SP 800-90B §3.1.4.3 重启测试：对 1000×1000 矩阵按行、按列各估一次最小熵

    python3 tools/restart_test.py restart.bin [--h-original 0.871234]

============================================================================
【它补的是顺序采集答不了的那个问题】
============================================================================
顺序采集（board/trngraw.c + tools/sp800_90b.py）估的是**一条长序列**的最小熵。
它答不了：**噪声源每次重启之后，是不是都从同一个地方开始？**

如果环振上电后有一段确定性的暖机行为，顺序数据会看着很随机，而"重启后第 k 个
样本"这一列可能高度可预测 —— 而攻击者恰恰能反复重启。所以要另采一个矩阵：
重启 N 次、每次取前 M 个样本，**按行**（一次重启之内）和**按列**（跨重启的同一
位置）各算一次最小熵，取两者的最小。

判据（§3.1.4.3）：min(H_行, H_列) 不得低于顺序数据估计值 H_I 的一半，
否则该熵源不通过验证。

============================================================================
【口径：这里的 "restart" 是代理，不是原物】
============================================================================
标准说的 restart 是**噪声源重新上电**。采集端做的是 TRNG 复位（zeroize：
环振停振再起、海绵状态清零、启动检测重跑），芯片没断电。
覆盖得到"每次起振的相位/暖机是否可预测"，覆盖不到"上电瞬态"。

这个区别写在 .bin 的文件头里，本脚本会把它原样打印出来 ——
一个代理测试被当成原物报出去，比不做还糟。
"""
import sys
from collections import Counter
from math import log2, sqrt

Z = 2.5758293035489008          # 与 tools/sp800_90b.py 的 99% 单边一致


def mcv(bits):
    """§6.3.1 最常见值估计（对二元序列）"""
    L = len(bits)
    ph = max(Counter(bits).values()) / L
    pu = min(1.0, ph + Z * sqrt(ph * (1 - ph) / (L - 1)))
    return -log2(pu)


def load(path):
    head, payload = [], b''
    with open(path, 'rb') as f:
        data = f.read()
    # 文件头是若干以 '#' 开头的 ASCII 行，之后是二进制
    pos = 0
    while data[pos:pos + 1] == b'#':
        nl = data.index(b'\n', pos)
        head.append(data[pos:nl].decode('utf-8', 'replace'))
        pos = nl + 1
    payload = data[pos:]
    meta = {}
    for line in head:
        if '=' in line:
            for kv in line.lstrip('#').split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    meta.setdefault(k, v)
    rows = int(meta.get('rows', 0))
    cols = int(meta.get('cols', 0))
    stride = (cols + 7) // 8
    if rows == 0 or cols == 0 or len(payload) < rows * stride:
        sys.exit('文件头或长度不对：rows=%s cols=%s payload=%d 需要 %d'
                 % (rows, cols, len(payload), rows * stride))
    m = []
    for r in range(rows):
        chunk = payload[r * stride:(r + 1) * stride]
        row = [(chunk[i >> 3] >> (7 - (i & 7))) & 1 for i in range(cols)]
        m.append(row)
    return head, m, rows, cols


def selftest():
    """两个合成算例，先证明这把尺子本身是准的

    ⚠️ 第二个算例是关键：它构造一个**行方向完全正常、列方向完全可预测**的源。
    顺序采集那一套对它毫无察觉（行的 MCV 看着很健康），只有列方向会塌到 0。
    §3.1.4.3 存在的全部理由就是这个形状 —— 跑不出这个结果，
    说明列方向根本没在算，那这份报告一文不值。
    """
    import random
    random.seed(7)
    n = 200

    rand_m = [[random.getrandbits(1) for _ in range(n)] for _ in range(n)]
    hr = min(mcv(r) for r in rand_m)
    hc = min(mcv([rand_m[r][c] for r in range(n)]) for c in range(n))
    print('算例 1  全随机          行 %.4f  列 %.4f  → 两个方向都应当健康' % (hr, hc))
    ok1 = hr > 0.4 and hc > 0.4

    colpat = [random.getrandbits(1) for _ in range(n)]
    col_m = [[colpat[c] for c in range(n)] for _ in range(n)]
    hr2 = min(mcv(r) for r in col_m)
    hc2 = min(mcv([col_m[r][c] for r in range(n)]) for c in range(n))
    print('算例 2  列恒定/行随机    行 %.4f  列 %.4f  → 行应当健康、列必须塌到 0'
          % (hr2, hc2))
    ok2 = hr2 > 0.4 and hc2 < 1e-6

    print('自检%s' % ('通过' if (ok1 and ok2) else '**失败**'))
    return 0 if (ok1 and ok2) else 1


def main():
    if '--selftest' in sys.argv:
        return selftest()
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    h_orig = None
    if '--h-original' in sys.argv:
        h_orig = float(sys.argv[sys.argv.index('--h-original') + 1])

    head, m, rows, cols = load(path)

    print('=== 重启矩阵 %d x %d ===' % (rows, cols))
    print('采集端写进文件头的口径：')
    for line in head:
        print('  ' + line)
    print()

    # 按行：每一行是一次重启内的连续样本
    h_rows = [mcv(r) for r in m]
    # 按列：每一列是"重启后第 i 个样本"跨全部重启
    h_cols = [mcv([m[r][c] for r in range(rows)]) for c in range(cols)]

    hr, hc = min(h_rows), min(h_cols)
    print('按行（一次重启之内）  最小 %.6f  中位 %.6f  最大 %.6f'
          % (hr, sorted(h_rows)[len(h_rows) // 2], max(h_rows)))
    print('按列（跨重启同一位置）最小 %.6f  中位 %.6f  最大 %.6f'
          % (hc, sorted(h_cols)[len(h_cols) // 2], max(h_cols)))
    print()
    h_restart = min(hr, hc)
    print('H_restart = min(行, 列) = %.6f 比特/样本' % h_restart)

    if h_orig is not None:
        print('H_原始（顺序采集）      = %.6f 比特/样本' % h_orig)
        print('判据：H_restart >= H_原始 / 2 = %.6f' % (h_orig / 2))
        if h_restart >= h_orig / 2:
            print('→ **通过**')
        else:
            print('→ **不通过** —— 重启后的可预测性显著高于顺序数据')
            return 1
    else:
        print('（没给 --h-original，只报数不判定）')

    # 一条独立的健全性检查：整体 1 的占比。
    # 若矩阵整体严重偏置，上面两个数会一起变低，那时"两个都低"不是巧合。
    ones = sum(sum(r) for r in m)
    print('\n整体 1 占比 %.6f（%d / %d）' % (ones / (rows * cols), ones, rows * cols))
    return 0


if __name__ == '__main__':
    sys.exit(main())
