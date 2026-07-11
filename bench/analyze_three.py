"""Paired within-block analysis for three_way.py output.

For each (workload, metric) and each comparison arm (ring, array),
computes the per-block ratio arm/std, then median + bootstrap 95% CI +
exact sign test. Writes RATIOS.tsv (block, workload, metric, size, arm,
ratio) for plotting.

Usage: python3 analyze_three.py RESULTS.tsv [RATIOS.tsv]
"""
from __future__ import annotations

import collections
import math
import random
import sys


def main():
    cells = collections.defaultdict(dict)
    sizes = {}
    with open(sys.argv[1], encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#"):
                print(line.rstrip())
                continue
            if not line.strip():
                continue
            block, workload, metric, arm, n, value, _ = line.split("\t")
            key = (int(block), workload, metric)
            cells[key][arm] = float(value)
            sizes[key] = int(n)

    groups = collections.defaultdict(list)
    rows = []
    for key, arms in sorted(cells.items()):
        if "std" not in arms or arms["std"] <= 0:
            continue
        block, workload, metric = key
        for arm in ("ring", "array"):
            if arm in arms:
                ratio = arms[arm] / arms["std"]
                groups[(workload, metric, arm)].append(ratio)
                rows.append((block, workload, metric, sizes[key], arm,
                             ratio))

    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8") as out:
            for row in rows:
                out.write("\t".join(str(x) for x in row) + "\n")

    rng = random.Random(0)
    print(f"{'workload':<24} {'metric':<8} {'arm':<6} {'n':>4} "
          f"{'median':>8} {'95% CI':>20} {'sign p':>8}")
    for (workload, metric, arm), ratios in sorted(groups.items()):
        ratios.sort()
        m = len(ratios)
        med = ratios[m // 2]
        boots = []
        for _ in range(10_000):
            s = sorted(rng.choices(ratios, k=m))
            boots.append(s[m // 2])
        boots.sort()
        lo, hi = boots[249], boots[9749]
        wins = sum(r < 1 for r in ratios)
        k = min(wins, m - wins)
        p = min(1.0, 2 * sum(math.comb(m, i)
                             for i in range(k + 1)) / 2 ** m)
        print(f"{workload:<24} {metric:<8} {arm:<6} {m:>4} {med:>8.4f} "
              f"[{lo:>8.4f},{hi:>8.4f}] {p:>8.4f}")
    print("\nratio < 1 => arm faster than collections.deque")


if __name__ == "__main__":
    main()
