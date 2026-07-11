# ringdeque

A growable **ring-buffer deque** for Python: the `collections.deque`
API with **O(1) indexing** and **O(k) slicing**, backed by a single
contiguous buffer instead of a linked list of blocks.

```python
from ringdeque import deque

d = deque(range(1_000_000))
d[500_000]        # O(1) — collections.deque takes ~250,000 steps here
d[10:20:2]        # O(k) slice, returned as a deque (same maxlen)
d.appendleft(-1)  # all the usual deque operations
```

## Why

`collections.deque` is a linked list of 64-element blocks: excellent at
its ends, **O(n)** for `d[i]`, no slicing, and a 760-byte floor even
when empty. A growable ring buffer (like Rust's `VecDeque` or C++'s
`std::deque` promise) keeps end-operations fast while making the whole
container indexable.

Measured with paired randomized-block benchmarks against
`collections.deque` and `arraydeque` (full method, caveats, and raw
data in `bench/RESULTS.md`):

- **random access**: O(n) → O(1); ~three orders of magnitude at
  n = 10⁶
- **small deques**: 88 B empty vs the stdlib's 760 B (6–9× less
  memory below ~64 elements)
- **bounded deques** (`maxlen=`): allocation clamped to `maxlen + 1`
  slots — never more memory than the stdlib — and steady-state append
  tails at stdlib level, where the array-with-slack design
  (`arraydeque`) measures 2.5–2.9× worse p99.9/max from perpetual
  recenter-copies
- **steady-state queue throughput**: within ~5% of the stdlib
- honest costs: growing from empty is ~10% slower and growth
  reallocations produce rare-but-large latency spikes (the stdlib's
  block design instead pays a small malloc every 64 appends — a
  different tail shape, not a free lunch); single-op worst case is
  O(n) during a growth copy, amortised O(1), and worst-case O(1) at
  the bounded steady state.

## Compatibility

Drop-in for the `collections.deque` API: `append`, `appendleft`, `pop`,
`popleft`, `extend`, `extendleft`, `rotate`, `reverse`, `clear`,
`copy`, `count`, `index`, `insert`, `remove`, `maxlen`, iteration,
reversed iteration, pickling, weakrefs, subclassing — plus indexing and
slicing that `collections.deque` doesn't have. Semantics are enforced
by a differential Hypothesis test suite that runs random operation
sequences against `collections.deque` as the oracle.

Not yet implemented: slice assignment/deletion, `+` and `*` operators,
O(1) slice *views*, free-threading (the module currently relies on the
GIL). See ROADMAP in the issue tracker.

## Install

```
pip install ringdeque
```

CPython ≥ 3.11. C extension; wheels planned for common platforms.

## License

MIT. The ring-buffer core derives from the author's own CPython
experiment branch (`faster-cpython/ideas#731`); the extension
scaffolding is written against the public C API only.
