"""Differential tests: ringdeque.deque vs collections.deque as oracle.

Random operation sequences are applied to both implementations; any
observable divergence (values, lengths, raised exception types) fails.
This is how the package guarantees stdlib-compatible semantics without
copying stdlib tests.
"""
import collections

import pytest
from hypothesis import given, settings, strategies as st

import ringdeque

ELEMENTS = st.integers(-5, 5)
MAXLEN = st.one_of(st.none(), st.integers(0, 8))


class Op:
    """One deque operation, applied identically to both arms."""

    def __init__(self, name, *args):
        self.name = name
        self.args = args

    def __repr__(self):
        return f"{self.name}{self.args!r}"

    def apply(self, d):
        if self.name == "getitem":
            return d[self.args[0]]
        if self.name == "setitem":
            d[self.args[0]] = self.args[1]
            return None
        if self.name == "delitem":
            del d[self.args[0]]
            return None
        if self.name == "iter":
            return list(d)
        if self.name == "reversed":
            return list(reversed(d))
        if self.name == "len":
            return len(d)
        if self.name == "contains":
            return self.args[0] in d
        return getattr(d, self.name)(*self.args)


OPS = st.one_of(
    st.builds(Op, st.just("append"), ELEMENTS),
    st.builds(Op, st.just("appendleft"), ELEMENTS),
    st.builds(Op, st.just("pop")),
    st.builds(Op, st.just("popleft")),
    st.builds(Op, st.just("extend"), st.lists(ELEMENTS, max_size=5)),
    st.builds(Op, st.just("extendleft"), st.lists(ELEMENTS, max_size=5)),
    st.builds(Op, st.just("rotate"), st.integers(-10, 10)),
    st.builds(Op, st.just("reverse")),
    st.builds(Op, st.just("clear")),
    st.builds(Op, st.just("count"), ELEMENTS),
    st.builds(Op, st.just("remove"), ELEMENTS),
    st.builds(Op, st.just("insert"), st.integers(-10, 10), ELEMENTS),
    st.builds(Op, st.just("getitem"), st.integers(-10, 10)),
    st.builds(Op, st.just("setitem"), st.integers(-10, 10), ELEMENTS),
    st.builds(Op, st.just("delitem"), st.integers(-10, 10)),
    st.builds(Op, st.just("iter")),
    st.builds(Op, st.just("reversed")),
    st.builds(Op, st.just("len")),
    st.builds(Op, st.just("contains"), ELEMENTS),
    st.builds(Op, st.just("index"), ELEMENTS),
)


def apply_both(op, ours, theirs):
    our_exc = their_exc = None
    our_result = their_result = None
    try:
        our_result = op.apply(ours)
    except Exception as e:          # noqa: BLE001
        our_exc = type(e)
    try:
        their_result = op.apply(theirs)
    except Exception as e:          # noqa: BLE001
        their_exc = type(e)
    assert our_exc == their_exc, (
        f"{op}: exception mismatch {our_exc} vs {their_exc}")
    assert our_result == their_result, (
        f"{op}: result mismatch {our_result!r} vs {their_result!r}")
    assert list(ours) == list(theirs), f"{op}: content diverged"
    assert len(ours) == len(theirs)


@settings(max_examples=400, deadline=None)
@given(init=st.lists(ELEMENTS, max_size=12), maxlen=MAXLEN,
       ops=st.lists(OPS, max_size=40))
def test_differential(init, maxlen, ops):
    ours = ringdeque.deque(init, maxlen)
    theirs = collections.deque(init, maxlen)
    assert list(ours) == list(theirs)
    assert ours.maxlen == theirs.maxlen
    for op in ops:
        apply_both(op, ours, theirs)


@settings(max_examples=100, deadline=None)
@given(init=st.lists(ELEMENTS, max_size=20),
       start=st.integers(-25, 25) | st.none(),
       stop=st.integers(-25, 25) | st.none(),
       step=(st.integers(-5, 5).filter(lambda x: x != 0)) | st.none())
def test_slicing_vs_list(init, start, stop, step):
    # Oracle for slicing is list (collections.deque has no slicing).
    d = ringdeque.deque(init)
    assert list(d[start:stop:step]) == init[start:stop:step]


@given(init=st.lists(ELEMENTS, max_size=20), maxlen=MAXLEN)
def test_slice_returns_deque_with_maxlen(init, maxlen):
    d = ringdeque.deque(init, maxlen)
    s = d[1:5]
    assert isinstance(s, ringdeque.deque)
    assert s.maxlen == d.maxlen


def test_mutation_during_iteration():
    d = ringdeque.deque(range(10))
    it = iter(d)
    next(it)
    d.append(99)
    with pytest.raises(RuntimeError):
        next(it)


def test_pickle_copy_roundtrip():
    import copy
    import pickle
    for maxlen in (None, 0, 3, 100):
        d = ringdeque.deque([1, 2, 3], maxlen)
        assert pickle.loads(pickle.dumps(d)) == d
        assert copy.copy(d) == d
        assert d.copy() == d
        assert d.copy().maxlen == d.maxlen


def test_bounded_allocation_clamp():
    import sys
    header = sys.getsizeof(ringdeque.deque())
    slot = 8
    for maxlen in (0, 1, 10, 100, 1000):
        d = ringdeque.deque(maxlen=maxlen)
        for i in range(3 * maxlen + 10):
            d.append(i)
        for i in range(maxlen + 5):
            d.appendleft(i)
        assert sys.getsizeof(d) <= header + (maxlen + 1) * slot


def test_weakref():
    import weakref
    d = ringdeque.deque([1])
    r = weakref.ref(d)
    assert r() is d
    del d
    assert r() is None


def test_subclass():
    class MyDeque(ringdeque.deque):
        pass

    d = MyDeque([1, 2, 3])
    assert list(d) == [1, 2, 3]
    assert isinstance(d.copy(), MyDeque)
    assert isinstance(d[0:2], MyDeque)
