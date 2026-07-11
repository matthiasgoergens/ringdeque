"""ringdeque: a growable ring-buffer deque.

Drop-in replacement for collections.deque with O(1) indexing and
O(k) slicing, implemented as a single contiguous ring buffer.

    from ringdeque import deque
"""

from ringdeque._ringdeque import deque

RingDeque = deque

__all__ = ["deque", "RingDeque"]
__version__ = "0.1.0a0"
