# Benchmark results (2026-07-11)

Three-way randomized-block comparison on CPython 3.14.6 (system build):
`ringdeque.deque` (ring buffer) vs `collections.deque` (block linked
list, stdlib baseline) vs `arraydeque.ArrayDeque` 1.4.0 (linear array
with centered slack). Method: each block samples (workload, size
log-uniform in [1, 10⁶]), runs all three containers once in random
order in separate processes with identical workloads; paired
within-block ratios, medians with bootstrap 95% CIs and sign tests.
~300 blocks per run; harness `three_way.py` (append-only/anytime,
container fingerprints recorded in the data).

## Caveats, stated up front

- The stdlib deque lives inside a PGO/LTO-built interpreter; both
  extensions are plain setuptools builds. Ratios vs `std` are therefore
  *pessimistic* for both extensions; ring-vs-array is the
  like-for-like comparison.
- `arraydeque.__sizeof__` does not account its buffer (reports 64 B
  flat), so no getsizeof-based memory comparisons are made against it.
- Single machine, single code-layout draw per binary; effects below
  a few percent should be treated as indicative.

## Growth policy A/B (ring only, one constant changed)

list-style 1.125× growth vs geometric doubling, 300 blocks each:

| metric (ring/std) | 1.125× | doubling |
|---|---|---|
| append_only ns/op | 1.22 | **1.09** |
| growth p99 | 1.36 | **1.07** |
| growth p99.9 | 2.11 | **1.69** |
| queue steady ns/op | 1.07 | **1.05** |
| bounded p99/p999 | 0.94 / 0.89 | 1.03 / 1.00 (CIs overlap) |

Doubling adopted as default: growth copies halve in frequency; the
bounded regime is unaffected (its allocation is clamped to maxlen + 1
regardless); worst-case slack for unbounded deques is 2×, same class
as arraydeque and Python lists. (The 1.125× policy exists for the
upstream-CPython context where memory do-no-harm dominates.)

## Final table (doubling growth), median paired ratio vs stdlib deque

| workload | metric | ring | arraydeque |
|---|---|---|---|
| getitem_random | ns/op | **0.79** | **0.66** |
| append_only | ns/op | 1.09 | 1.04 |
| append_popleft_steady | ns/op | 1.05 | 1.01 |
| append growth | p50 | 1.04 | 1.04 |
| append growth | p99.9 | 1.69 | 1.18 |
| append growth | max | 2.78 | 1.88 |
| bounded steady (maxlen) | p50 | 0.98 | 1.00 |
| bounded steady (maxlen) | p99 | **1.03** | 1.12 |
| bounded steady (maxlen) | p99.9 | **1.00** | **2.46** |
| bounded steady (maxlen) | max | **1.00** | **2.89** |

## Reading

- **Random access**: both array-backed designs are O(1) vs the
  stdlib's O(n); the medians above blend all sizes — at n = 10⁶ the
  advantage is ~three orders of magnitude. arraydeque's flat indexing
  is ~15% faster than the ring's wrap arithmetic (see roadmap:
  power-of-two mask fast path).
- **Bounded queues are the ring's home turf**: at maxlen steady state
  the ring never copies (wrap only) and matches the stdlib's tails,
  while arraydeque recenter-copies perpetually — p99.9/max 2.5–2.9×
  worse than stdlib. This confirms the structural prediction from
  reading arraydeque's source: a linear array must recenter under
  FIFO drift; a ring wraps.
- **Unbounded growth is the honest cost of any single-buffer design**:
  both extensions pay realloc spikes the stdlib's 64-element block
  scheme never pays (it allocates one small block per 64 appends
  instead — different tail *shape*: frequent-small vs rare-large).
- Memory (vs stdlib, measured via getsizeof in the test suite): 88 B
  empty vs 760 B; bounded deques clamped to maxlen + 1 slots — never
  more than the stdlib equivalent across all sizes tested.

Raw data: threeway*.tsv (slow-growth and doubling runs kept separate;
never mix treatments in one file).

## Addendum: pow2 mask fast path + rotate workload (2026-07-12)

Unbounded capacities are always powers of two under doubling growth, so
physical indexing takes `(first + i) & mask` there; bounded deques keep
the general wrap (the maxlen + 1 clamp intentionally breaks the pow2
invariant). Effect on the three-way medians: below cross-run
resolution — run-to-run size-mixture variation dominates; no
regression observed, no measured win claimed. (A dedicated
mask-vs-no-mask two-arm block design would resolve it if it matters.)

New `rotate1_steady` workload (within-run, tight CIs):

| arm | ns/op vs stdlib |
|---|---|
| ring | **1.17** |
| arraydeque | 1.83 |

arraydeque implements rotate via pop/append loops; the ring moves one
element circularly. Also notable this run: the ring's bounded-steady
**max** latency measured 0.68x the stdlib's (CI [0.43, 0.99]) — at
maxlen steady state the ring never allocates or copies at all.
