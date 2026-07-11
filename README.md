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

Measured against `collections.deque` (paired randomized-block
benchmarks, details in the repo):

- **random access**: ~1000× faster at n = 10⁶ (O(n) → O(1))
- **small deques**: 6–9× less memory below ~64 elements (88 B empty
  vs 760 B)
- **bounded deques** (`maxlen=`): never more memory (allocation is
  clamped to `maxlen + 1` slots), better p99/p99.9 append tails
- **steady-state queue throughput**: parity (within ~2%)
- honest costs: growing from empty is ~10–30% slower (a ring
  reallocates; the block list never does), and single-op worst case is
  O(n) during a growth copy — amortised O(1), and worst-case O(1) in
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
