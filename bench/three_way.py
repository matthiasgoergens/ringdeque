#!/usr/bin/env python3
"""Three-way randomized-block benchmark:
ringdeque.deque vs collections.deque vs arraydeque.ArrayDeque.

Each BLOCK samples theta = (workload, size log-uniform), then runs every
container once at theta in randomized order, one subprocess per
measurement (process isolation), identical workloads. Results append to
a TSV; block ids continue; entropy fresh per invocation. Fingerprints
(container class + sizeof signature) recorded as '#' lines.

Usage: python3 three_way.py OUT.tsv [BLOCKS] [MIN_SIZE] [MAX_SIZE]
Run under the project venv (needs ringdeque + arraydeque installed).
"""
from __future__ import annotations

import json
import math
import os
import random
import subprocess
import sys

CONTAINERS = {
    "std": "collections:deque",
    "ring": "ringdeque:deque",
    "array": "arraydeque:ArrayDeque",
}

PAYLOAD = r"""
import importlib, json, random, sys, time

modname, clsname = sys.argv[1].split(":")
D = getattr(importlib.import_module(modname), clsname)
workload = sys.argv[2]
n = int(sys.argv[3])
seed = int(sys.argv[4])

MIN_NS = 100_000_000
MAX_REPS = 1 << 20

def calibrated(op_count, run_pass):
    run_pass()
    reps = 1
    while True:
        t0 = time.perf_counter_ns()
        for _ in range(reps):
            run_pass()
        dt = time.perf_counter_ns() - t0
        if dt > MIN_NS or reps >= MAX_REPS:
            return dt / (reps * op_count)
        reps *= 2

def quantiles(samples):
    samples.sort()
    m = len(samples)
    return {"p50_ns": samples[m // 2],
            "p99_ns": samples[(m * 99) // 100],
            "p999_ns": samples[(m * 999) // 1000],
            "max_ns": samples[-1]}

def fingerprint():
    d = D(range(1000))
    return {"class": f"{modname}:{clsname}",
            "sizeof_1000": sys.getsizeof(d),
            "sizeof_0": sys.getsizeof(D())}

if workload == "fingerprint":
    print(json.dumps(fingerprint())); raise SystemExit

metrics = {}
if workload == "append_only":
    r = range(max(n, 1))
    def run_pass():
        d = D()
        append = d.append
        for v in r:
            append(v)
    metrics["ns_per_op"] = calibrated(max(n, 1), run_pass)
elif workload == "append_popleft_steady":
    d = D(range(n))
    ops = 10_000
    r = range(ops)
    def run_pass():
        append, popleft = d.append, d.popleft
        for v in r:
            append(v); popleft()
    metrics["ns_per_op"] = calibrated(2 * ops, run_pass)
elif workload == "rotate1_steady":
    d = D(range(max(n, 1)))
    ops = 10_000
    r = range(ops)
    def run_pass():
        rotate = d.rotate
        for _ in r:
            rotate(1); rotate(-1)
    metrics["ns_per_op"] = calibrated(2 * ops, run_pass)
elif workload == "getitem_random":
    m = max(n, 1)
    d = D(range(m))
    rng = random.Random(seed)
    idx = [rng.randrange(m) for _ in range(1000)]
    def run_pass():
        for i in idx:
            d[i]
    metrics["ns_per_op"] = calibrated(len(idx), run_pass)
elif workload == "append_latency":
    m = max(n, 1)
    d = D()
    append = d.append
    pcn = time.perf_counter_ns
    samples = [0] * m
    for i in range(m):
        t0 = pcn(); append(i); samples[i] = pcn() - t0
    metrics = quantiles(samples)
elif workload == "steady_maxlen_latency":
    m = max(n, 1)
    d = D(range(m), maxlen=m)
    append = d.append
    pcn = time.perf_counter_ns
    ops = max(min(2 * m, 1_000_000), 10_000)
    samples = [0] * ops
    for i in range(ops):
        t0 = pcn(); append(i); samples[i] = pcn() - t0
    metrics = quantiles(samples)
else:
    raise SystemExit(f"unknown workload {workload!r}")

print(json.dumps({"metrics": metrics}))
"""

WORKLOADS = ["append_only", "append_popleft_steady", "getitem_random",
             "rotate1_steady", "append_latency", "steady_maxlen_latency"]


def run_payload(spec, workload, n, seed):
    cmd = [sys.executable, "-c", PAYLOAD, spec, workload, str(n), str(seed)]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def first_free_block(path):
    if not os.path.exists(path):
        return 0
    blocks = [int(line.split("\t", 1)[0])
              for line in open(path, encoding="utf-8")
              if line.strip() and not line.startswith("#")]
    return max(blocks) + 1 if blocks else 0


def main():
    out_path = sys.argv[1]
    blocks = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    min_size = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    max_size = int(sys.argv[4]) if len(sys.argv) > 4 else 1_000_000
    rng = random.Random()
    block = first_free_block(out_path)

    with open(out_path, "a", encoding="utf-8") as out:
        for name, spec in CONTAINERS.items():
            fp = run_payload(spec, "fingerprint", 0, 0)
            out.write(f"# fingerprint\t{name}\t{json.dumps(fp)}\n")
            print(f"fingerprint {name}: {fp}", file=sys.stderr)
        out.flush()

        for done in range(blocks):
            n = int(round(math.exp(rng.uniform(math.log(min_size),
                                               math.log(max_size)))))
            workload = rng.choice(WORKLOADS)
            seed = rng.randrange(2**31)
            arms = list(CONTAINERS.items())
            rng.shuffle(arms)
            for order, (name, spec) in enumerate(arms):
                data = run_payload(spec, workload, n, seed)
                for metric, value in data["metrics"].items():
                    out.write(f"{block}\t{workload}\t{metric}\t{name}"
                              f"\t{n}\t{value}\t{order}\n")
            out.flush()
            block += 1
            if (done + 1) % 20 == 0:
                print(f"{done + 1}/{blocks} blocks", file=sys.stderr)


if __name__ == "__main__":
    main()
