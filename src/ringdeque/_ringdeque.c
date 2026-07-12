/* ringdeque._ringdeque — a growable ring-buffer deque.
 *
 * collections.deque-compatible API with O(1) indexing and O(k)
 * slicing, backed by a single contiguous ring buffer that grows with
 * the list over-allocation policy and never over-allocates past
 * maxlen + 1 for bounded deques.
 *
 * Ring-core algorithms (wrap_index, circular_mem_move,
 * ring_copy_to_contiguous, allocation policy) by Matthias Görgens,
 * from his growable-ring-buffer CPython branch.  Extension
 * scaffolding written fresh against the public C API only.
 *
 * MIT License; see LICENSE.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Object layout                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    PyObject **items;       /* ring buffer, `allocated` slots (or NULL) */
    Py_ssize_t allocated;   /* slots in `items`                        */
    Py_ssize_t mask;        /* allocated-1 if allocated is a power of
                               two (always true for unbounded deques
                               under doubling growth), else 0          */
    Py_ssize_t first;       /* physical index of logical element 0     */
    Py_ssize_t size;        /* number of live elements                 */
    Py_ssize_t maxlen;      /* -1 = unbounded                          */
    uint64_t state;         /* bumped on every mutation                */
    PyObject *weakreflist;
} ringdeque;

typedef struct {
    PyObject_HEAD
    ringdeque *deque;       /* strong reference, NULL when exhausted   */
    Py_ssize_t index;       /* next logical index to yield             */
    uint64_t state;         /* deque->state at creation                */
    int reversed;
} ringdeque_iter;

static PyTypeObject ringdeque_type;
static PyTypeObject ringdeque_iter_type;

#define ringdeque_CheckExact(op) Py_IS_TYPE((op), &ringdeque_type)
#define ringdeque_Check(op) PyObject_TypeCheck((op), &ringdeque_type)

static inline void rd_set_mask(ringdeque *self);

/* ------------------------------------------------------------------ */
/* Ring core (transplanted)                                           */
/* ------------------------------------------------------------------ */

static size_t
size_min(size_t a, size_t b)
{
    return a < b ? a : b;
}

/* We want zero to behave like +infinity, hence the -1 and unsigned
   overflow. */
static Py_ssize_t
min3_special(Py_ssize_t a, Py_ssize_t b, Py_ssize_t c)
{
    return size_min(a - 1, size_min(b - 1, c - 1)) + 1;
}

static inline Py_ssize_t
wrap_index(Py_ssize_t index, Py_ssize_t allocated)
{
    assert(allocated > 0);
    if ((size_t)index < (size_t)allocated) {
        return index;
    }
    if (index >= 0) {
        index -= allocated;
        if (index < allocated) {
            return index;
        }
        index %= allocated;
        return index;
    }
    index += allocated;
    if (index >= 0) {
        return index;
    }
    Py_ssize_t rem = (-index) % allocated;
    return rem ? allocated - rem : 0;
}

/* Move m slots within the ring from src_start to dst_start, handling
   overlap in either direction, in large memmove segments. */
static void
circular_mem_move(PyObject **items, Py_ssize_t allocated,
                  Py_ssize_t dst_start, Py_ssize_t src_start, Py_ssize_t m)
{
    assert(m <= allocated);
    assert(dst_start >= 0 && dst_start < allocated);
    assert(src_start >= 0 && src_start < allocated);

    Py_ssize_t distance = src_start - dst_start;
    if (distance < 0) {
        distance += allocated;
    }

    if (distance < m) {
        Py_ssize_t remaining = m;
        while (remaining > 0) {
            Py_ssize_t src_until_wrap = allocated - src_start;
            Py_ssize_t dst_until_wrap = allocated - dst_start;
            if (src_until_wrap == 0) {
                src_until_wrap = allocated;
            }
            if (dst_until_wrap == 0) {
                dst_until_wrap = allocated;
            }
            Py_ssize_t step = min3_special(src_until_wrap, dst_until_wrap,
                                           remaining);
            memmove(&items[dst_start], &items[src_start],
                    step * sizeof(PyObject *));
            remaining -= step;
            assert(remaining >= 0);
            assert(step > 0);
            src_start += step;
            dst_start += step;
            if (src_start >= allocated) {
                src_start -= allocated;
            }
            if (dst_start >= allocated) {
                dst_start -= allocated;
            }
        }
    }
    else {
        Py_ssize_t remaining = m;
        Py_ssize_t src_end = src_start + remaining;
        Py_ssize_t dst_end = dst_start + remaining;
        if (src_end >= allocated) {
            src_end -= allocated;
        }
        if (dst_end >= allocated) {
            dst_end -= allocated;
        }
        while (remaining > 0) {
            Py_ssize_t src_until_wrap = src_end;
            Py_ssize_t dst_until_wrap = dst_end;
            if (src_until_wrap == 0) {
                src_until_wrap = allocated;
            }
            if (dst_until_wrap == 0) {
                dst_until_wrap = allocated;
            }
            Py_ssize_t step = min3_special(src_until_wrap, dst_until_wrap,
                                           remaining);
            Py_ssize_t src_pos = src_end - step;
            Py_ssize_t dst_pos = dst_end - step;
            if (src_pos < 0) {
                src_pos += allocated;
            }
            if (dst_pos < 0) {
                dst_pos += allocated;
            }
            memmove(&items[dst_pos], &items[src_pos],
                    step * sizeof(PyObject *));
            remaining -= step;
            assert(remaining >= 0);
            assert(step > 0);
            src_end = src_pos;
            dst_end = dst_pos;
        }
    }
}

static inline void
ring_copy_to_contiguous(PyObject **src, Py_ssize_t allocated,
                        Py_ssize_t start, PyObject **dst, Py_ssize_t count)
{
    if (count <= 0 || allocated == 0) {
        return;
    }
    assert(start >= 0 && start < allocated);
    Py_ssize_t first_run = Py_MIN(count, allocated - start);
    memcpy(dst, src + start, first_run * sizeof(PyObject *));
    Py_ssize_t remaining = count - first_run;
    if (remaining > 0) {
        memcpy(dst + first_run, src, remaining * sizeof(PyObject *));
    }
}

/* Geometric doubling with a small floor.  A standalone container can
   afford up-to-2x slack for unbounded deques (bounded deques are
   clamped to maxlen + 1 regardless); doubling halves the number of
   growth copies compared to list-style 1.125x growth, which measurably
   improves append throughput and growth-latency tails. */
static Py_ssize_t
rd_calculate_allocation(Py_ssize_t min_needed)
{
    if (min_needed <= 0) {
        return 0;
    }
    size_t needed = (size_t)min_needed;
    size_t new_allocated = 8;
    while (new_allocated < needed) {
        if (new_allocated > ((size_t)PY_SSIZE_T_MAX >> 1)) {
            return -1;
        }
        new_allocated <<= 1;
    }
    return (Py_ssize_t)new_allocated;
}

/* Over-allocation clamped for bounded deques: capacity beyond
   maxlen + 1 (one transient slot for append-then-trim) can never be
   used.  Never clamps below min_needed. */
static Py_ssize_t
rd_target_allocation(ringdeque *self, Py_ssize_t min_needed)
{
    Py_ssize_t target = rd_calculate_allocation(min_needed);
    Py_ssize_t maxlen = self->maxlen;
    if (target > 0 && maxlen >= 0 && maxlen < PY_SSIZE_T_MAX
        && target > maxlen + 1) {
        target = Py_MAX(min_needed, maxlen + 1);
    }
    return target;
}

/* ------------------------------------------------------------------ */
/* Capacity management                                                */
/* ------------------------------------------------------------------ */

static int
rd_ensure_capacity(ringdeque *self, Py_ssize_t min_needed)
{
    Py_ssize_t allocated = self->allocated;
    if (allocated >= min_needed && allocated != 0) {
        return 0;
    }
    Py_ssize_t new_allocated = rd_target_allocation(self, min_needed);
    if (new_allocated < 0) {
        PyErr_NoMemory();
        return -1;
    }
    PyObject **new_items = PyMem_New(PyObject *, new_allocated);
    if (new_items == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    if (self->size > 0 && self->items != NULL && allocated > 0) {
        ring_copy_to_contiguous(self->items, allocated, self->first,
                                new_items, self->size);
    }
    PyMem_Free(self->items);
    self->items = new_items;
    self->allocated = new_allocated;
    rd_set_mask(self);
    self->first = 0;
    return 0;
}

static void
rd_maybe_shrink(ringdeque *self)
{
    Py_ssize_t size = self->size;
    Py_ssize_t allocated = self->allocated;

    if (size == 0) {
        PyMem_Free(self->items);
        self->items = NULL;
        self->allocated = 0;
        self->mask = 0;
        self->first = 0;
        return;
    }
    if (allocated <= 1 || size >= (allocated >> 1)) {
        return;
    }
    Py_ssize_t target = rd_target_allocation(self, size);
    if (target <= 0 || target >= allocated) {
        return;
    }
    PyObject **new_items = PyMem_New(PyObject *, target);
    if (new_items == NULL) {
        return;  /* shrinking is optional */
    }
    ring_copy_to_contiguous(self->items, allocated, self->first,
                            new_items, size);
    PyMem_Free(self->items);
    self->items = new_items;
    self->allocated = target;
    rd_set_mask(self);
    self->first = 0;
}

/* ------------------------------------------------------------------ */
/* Core mutations (no user code runs inside these)                    */
/* ------------------------------------------------------------------ */

static inline void
rd_set_mask(ringdeque *self)
{
    Py_ssize_t a = self->allocated;
    self->mask = (a > 0 && (a & (a - 1)) == 0) ? a - 1 : 0;
}

static inline Py_ssize_t
rd_phys(ringdeque *self, Py_ssize_t logical)
{
    if (self->mask) {
        return (self->first + logical) & self->mask;
    }
    return wrap_index(self->first + logical, self->allocated);
}

/* Steals a reference to item.  Returns the trimmed-out element (owned)
   or NULL; *error set on failure. */
static PyObject *
rd_push_back(ringdeque *self, PyObject *item, int *error)
{
    *error = 0;
    if (rd_ensure_capacity(self, self->size + 1) < 0) {
        Py_DECREF(item);
        *error = 1;
        return NULL;
    }
    self->items[rd_phys(self, self->size)] = item;
    self->size++;
    self->state++;
    if (self->maxlen >= 0 && self->size > self->maxlen) {
        PyObject *old = self->items[self->first];
        self->first = wrap_index(self->first + 1, self->allocated);
        self->size--;
        return old;
    }
    return NULL;
}

static PyObject *
rd_push_front(ringdeque *self, PyObject *item, int *error)
{
    *error = 0;
    if (rd_ensure_capacity(self, self->size + 1) < 0) {
        Py_DECREF(item);
        *error = 1;
        return NULL;
    }
    self->first = self->mask
        ? ((self->first - 1) & self->mask)
        : wrap_index(self->first - 1 + self->allocated,
                     self->allocated);
    self->items[self->first] = item;
    self->size++;
    self->state++;
    if (self->maxlen >= 0 && self->size > self->maxlen) {
        Py_ssize_t last = rd_phys(self, self->size - 1);
        PyObject *old = self->items[last];
        self->size--;
        return old;
    }
    return NULL;
}

/* Returns owned reference or NULL with IndexError. */
static PyObject *
rd_pop_back(ringdeque *self)
{
    if (self->size == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    Py_ssize_t last = rd_phys(self, self->size - 1);
    PyObject *item = self->items[last];
    self->items[last] = NULL;
    self->size--;
    self->state++;
    rd_maybe_shrink(self);
    return item;
}

static PyObject *
rd_pop_front(ringdeque *self)
{
    if (self->size == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    PyObject *item = self->items[self->first];
    self->items[self->first] = NULL;
    self->first = self->mask
        ? ((self->first + 1) & self->mask)
        : wrap_index(self->first + 1, self->allocated);
    self->size--;
    self->state++;
    rd_maybe_shrink(self);
    return item;
}

/* ------------------------------------------------------------------ */
/* Methods                                                            */
/* ------------------------------------------------------------------ */

static PyObject *
rd_append(PyObject *op, PyObject *item)
{
    ringdeque *self = (ringdeque *)op;
    int error;
    PyObject *old = rd_push_back(self, Py_NewRef(item), &error);
    if (error) {
        return NULL;
    }
    Py_XDECREF(old);   /* may run user __del__; state already consistent */
    Py_RETURN_NONE;
}

static PyObject *
rd_appendleft(PyObject *op, PyObject *item)
{
    ringdeque *self = (ringdeque *)op;
    int error;
    PyObject *old = rd_push_front(self, Py_NewRef(item), &error);
    if (error) {
        return NULL;
    }
    Py_XDECREF(old);
    Py_RETURN_NONE;
}

static PyObject *
rd_pop_method(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return rd_pop_back((ringdeque *)op);
}

static PyObject *
rd_popleft_method(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return rd_pop_front((ringdeque *)op);
}

static int
rd_extend_internal(ringdeque *self, PyObject *iterable, int left)
{
    /* Self-extension must iterate over a snapshot. */
    if ((PyObject *)self == iterable) {
        PyObject *copy = PySequence_List(iterable);
        if (copy == NULL) {
            return -1;
        }
        int result = rd_extend_internal(self, copy, left);
        Py_DECREF(copy);
        return result;
    }
    PyObject *it = PyObject_GetIter(iterable);
    if (it == NULL) {
        return -1;
    }
    PyObject *(*next)(PyObject *) = *Py_TYPE(it)->tp_iternext;
    for (;;) {
        PyObject *item = next(it);
        if (item == NULL) {
            if (PyErr_Occurred()) {
                if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    Py_DECREF(it);
                    return -1;
                }
                PyErr_Clear();
            }
            break;
        }
        int error;
        PyObject *old = left ? rd_push_front(self, item, &error)
                             : rd_push_back(self, item, &error);
        if (error) {
            Py_DECREF(it);
            return -1;
        }
        Py_XDECREF(old);
    }
    Py_DECREF(it);
    return 0;
}

static PyObject *
rd_extend(PyObject *op, PyObject *iterable)
{
    if (rd_extend_internal((ringdeque *)op, iterable, 0) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
rd_extendleft(PyObject *op, PyObject *iterable)
{
    if (rd_extend_internal((ringdeque *)op, iterable, 1) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
rd_clear_method(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque *self = (ringdeque *)op;
    /* Detach first so reentrant callbacks see a consistent deque. */
    PyObject **items = self->items;
    Py_ssize_t allocated = self->allocated;
    Py_ssize_t first = self->first;
    Py_ssize_t size = self->size;
    self->items = NULL;
    self->allocated = 0;
    self->mask = 0;
    self->first = 0;
    self->size = 0;
    self->state++;
    for (Py_ssize_t i = 0; i < size; i++) {
        Py_DECREF(items[wrap_index(first + i, allocated)]);
    }
    PyMem_Free(items);
    Py_RETURN_NONE;
}

static PyObject *rd_new_from(PyTypeObject *type, ringdeque *src,
                             Py_ssize_t start, Py_ssize_t step,
                             Py_ssize_t count, Py_ssize_t maxlen);

static PyObject *
rd_copy_method(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque *self = (ringdeque *)op;
    return rd_new_from(Py_TYPE(self), self, 0, 1, self->size,
                       self->maxlen);
}

static PyObject *
rd_rotate(PyObject *op, PyObject *const *args, Py_ssize_t nargs)
{
    ringdeque *self = (ringdeque *)op;
    Py_ssize_t n = 1;
    if (nargs == 1) {
        n = PyNumber_AsSsize_t(args[0], PyExc_OverflowError);
        if (n == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    else if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError,
                        "rotate() takes at most 1 argument");
        return NULL;
    }
    Py_ssize_t size = self->size;
    if (size <= 1) {
        Py_RETURN_NONE;
    }
    n %= size;
    if (n < 0) {
        n += size;
    }
    if (n == 0) {
        Py_RETURN_NONE;
    }
    /* rotate right by n == move the last n elements in front of the
       first.  If the ring is full this is just a pointer adjustment. */
    if (self->size == self->allocated) {
        self->first = wrap_index(self->first - n + self->allocated,
                                 self->allocated);
    }
    else if (n <= size - n) {
        /* move n tail elements to the front */
        Py_ssize_t src = rd_phys(self, size - n);
        Py_ssize_t dst = wrap_index(self->first - n + self->allocated,
                                    self->allocated);
        circular_mem_move(self->items, self->allocated, dst, src, n);
        self->first = dst;
    }
    else {
        /* move size-n head elements after the tail */
        Py_ssize_t m = size - n;
        Py_ssize_t src = self->first;
        Py_ssize_t dst = rd_phys(self, size);
        circular_mem_move(self->items, self->allocated, dst, src, m);
        self->first = wrap_index(self->first + m, self->allocated);
    }
    self->state++;
    Py_RETURN_NONE;
}

static PyObject *
rd_reverse(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque *self = (ringdeque *)op;
    Py_ssize_t lo = 0, hi = self->size - 1;
    while (lo < hi) {
        Py_ssize_t plo = rd_phys(self, lo);
        Py_ssize_t phi = rd_phys(self, hi);
        PyObject *tmp = self->items[plo];
        self->items[plo] = self->items[phi];
        self->items[phi] = tmp;
        lo++;
        hi--;
    }
    self->state++;
    Py_RETURN_NONE;
}

static PyObject *
rd_count(PyObject *op, PyObject *value)
{
    ringdeque *self = (ringdeque *)op;
    Py_ssize_t count = 0;
    uint64_t start_state = self->state;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        PyObject *item = Py_NewRef(self->items[rd_phys(self, i)]);
        int cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0) {
            return NULL;
        }
        count += (cmp > 0);
        if (self->state != start_state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return NULL;
        }
    }
    return PyLong_FromSsize_t(count);
}

static int
rd_contains(PyObject *op, PyObject *value)
{
    ringdeque *self = (ringdeque *)op;
    uint64_t start_state = self->state;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        PyObject *item = Py_NewRef(self->items[rd_phys(self, i)]);
        int cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        Py_DECREF(item);
        if (cmp != 0) {
            return cmp;   /* found or error */
        }
        if (self->state != start_state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return -1;
        }
    }
    return 0;
}

static PyObject *
rd_index(PyObject *op, PyObject *const *args, Py_ssize_t nargs)
{
    ringdeque *self = (ringdeque *)op;
    Py_ssize_t start = 0, stop = self->size;
    PyObject *value;
    if (nargs < 1 || nargs > 3) {
        PyErr_SetString(PyExc_TypeError,
                        "index() takes 1 to 3 arguments");
        return NULL;
    }
    value = args[0];
    if (nargs > 1 && args[1] != Py_None) {
        start = PyNumber_AsSsize_t(args[1], PyExc_OverflowError);
        if (start == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    if (nargs > 2 && args[2] != Py_None) {
        stop = PyNumber_AsSsize_t(args[2], PyExc_OverflowError);
        if (stop == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    if (start < 0) {
        start += self->size;
        if (start < 0) {
            start = 0;
        }
    }
    if (stop < 0) {
        stop += self->size;
        if (stop < 0) {
            stop = 0;
        }
    }
    if (stop > self->size) {
        stop = self->size;
    }
    uint64_t start_state = self->state;
    for (Py_ssize_t i = start; i < stop; i++) {
        PyObject *item = Py_NewRef(self->items[rd_phys(self, i)]);
        int cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0) {
            return NULL;
        }
        if (cmp > 0) {
            return PyLong_FromSsize_t(i);
        }
        if (self->state != start_state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return NULL;
        }
    }
    PyErr_Format(PyExc_ValueError, "%R is not in deque", value);
    return NULL;
}

static PyObject *
rd_remove(PyObject *op, PyObject *value)
{
    ringdeque *self = (ringdeque *)op;
    uint64_t start_state = self->state;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        PyObject *item = Py_NewRef(self->items[rd_phys(self, i)]);
        int cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0) {
            return NULL;
        }
        if (self->state != start_state) {
            PyErr_SetString(PyExc_IndexError,
                            "deque mutated during remove()");
            return NULL;
        }
        if (cmp > 0) {
            /* delete logical index i: move the shorter side */
            PyObject *victim = self->items[rd_phys(self, i)];
            if (i < self->size - i - 1) {
                /* shift prefix right */
                circular_mem_move(self->items, self->allocated,
                                  wrap_index(self->first + 1,
                                             self->allocated),
                                  self->first, i);
                self->items[self->first] = NULL;
                self->first = wrap_index(self->first + 1,
                                         self->allocated);
            }
            else {
                /* shift suffix left */
                Py_ssize_t m = self->size - i - 1;
                if (m > 0) {
                    circular_mem_move(self->items, self->allocated,
                                      rd_phys(self, i),
                                      rd_phys(self, i + 1), m);
                }
                self->items[rd_phys(self, self->size - 1)] = NULL;
            }
            self->size--;
            self->state++;
            Py_DECREF(victim);
            rd_maybe_shrink(self);
            Py_RETURN_NONE;
        }
    }
    PyErr_SetString(PyExc_ValueError, "deque.remove(x): x not in deque");
    return NULL;
}

static PyObject *
rd_insert(PyObject *op, PyObject *const *args, Py_ssize_t nargs)
{
    ringdeque *self = (ringdeque *)op;
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError,
                        "insert() takes exactly 2 arguments");
        return NULL;
    }
    Py_ssize_t index = PyNumber_AsSsize_t(args[0], PyExc_OverflowError);
    if (index == -1 && PyErr_Occurred()) {
        return NULL;
    }
    PyObject *value = args[1];
    Py_ssize_t n = self->size;
    if (self->maxlen >= 0 && n >= self->maxlen) {
        PyErr_SetString(PyExc_IndexError,
                        "deque already at its maximum size");
        return NULL;
    }
    if (index < 0) {
        index += n;
        if (index < 0) {
            index = 0;
        }
    }
    if (index > n) {
        index = n;
    }
    if (rd_ensure_capacity(self, n + 1) < 0) {
        return NULL;
    }
    if (index <= n - index) {
        /* shift prefix left by one */
        Py_ssize_t new_first = wrap_index(self->first - 1
                                          + self->allocated,
                                          self->allocated);
        if (index > 0) {
            circular_mem_move(self->items, self->allocated,
                              new_first, self->first, index);
        }
        self->first = new_first;
    }
    else {
        /* shift suffix right by one */
        Py_ssize_t m = n - index;
        if (m > 0) {
            circular_mem_move(self->items, self->allocated,
                              rd_phys(self, index + 1),
                              rd_phys(self, index), m);
        }
    }
    self->items[rd_phys(self, index)] = Py_NewRef(value);
    self->size++;
    self->state++;
    Py_RETURN_NONE;
}

/* ------------------------------------------------------------------ */
/* Sequence protocol                                                  */
/* ------------------------------------------------------------------ */

static Py_ssize_t
rd_len(PyObject *op)
{
    return ((ringdeque *)op)->size;
}

static PyObject *rd_copy_method(PyObject *op, PyObject *ignored);

static PyObject *
rd_concat(PyObject *op, PyObject *other)
{
    if (!ringdeque_Check(other)) {
        PyErr_Format(PyExc_TypeError,
                     "can only concatenate deque (not \"%.200s\") to deque",
                     Py_TYPE(other)->tp_name);
        return NULL;
    }
    PyObject *result = rd_copy_method(op, NULL);
    if (result == NULL) {
        return NULL;
    }
    if (rd_extend_internal((ringdeque *)result, other, 0) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

static PyObject *
rd_inplace_concat(PyObject *op, PyObject *other)
{
    if (rd_extend_internal((ringdeque *)op, other, 0) < 0) {
        return NULL;
    }
    return Py_NewRef(op);
}

static int
rd_repeat_into(ringdeque *self, Py_ssize_t n)
{
    /* Append (n-1) further copies of the current contents; n <= 0
       clears.  Trimming for maxlen falls out of rd_push_back. */
    if (n <= 0) {
        PyObject *ignored = rd_clear_method((PyObject *)self, NULL);
        Py_XDECREF(ignored);
        return 0;
    }
    Py_ssize_t orig_size = self->size;
    if (orig_size == 0 || n == 1) {
        return 0;
    }
    PyObject *snapshot = PySequence_List((PyObject *)self);
    if (snapshot == NULL) {
        return -1;
    }
    for (Py_ssize_t rep = 1; rep < n; rep++) {
        for (Py_ssize_t i = 0; i < orig_size; i++) {
            int error;
            PyObject *item = Py_NewRef(PyList_GET_ITEM(snapshot, i));
            PyObject *old = rd_push_back(self, item, &error);
            if (error) {
                Py_DECREF(snapshot);
                return -1;
            }
            Py_XDECREF(old);
        }
    }
    Py_DECREF(snapshot);
    return 0;
}

static PyObject *
rd_repeat(PyObject *op, Py_ssize_t n)
{
    PyObject *result = rd_copy_method(op, NULL);
    if (result == NULL) {
        return NULL;
    }
    if (rd_repeat_into((ringdeque *)result, n) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

static PyObject *
rd_inplace_repeat(PyObject *op, Py_ssize_t n)
{
    if (rd_repeat_into((ringdeque *)op, n) < 0) {
        return NULL;
    }
    return Py_NewRef(op);
}

static PyObject *
rd_item(PyObject *op, Py_ssize_t index)
{
    ringdeque *self = (ringdeque *)op;
    if (index < 0 || index >= self->size) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return NULL;
    }
    return Py_NewRef(self->items[rd_phys(self, index)]);
}

static int
rd_ass_item(PyObject *op, Py_ssize_t index, PyObject *value)
{
    ringdeque *self = (ringdeque *)op;
    if (index < 0 || index >= self->size) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return -1;
    }
    Py_ssize_t phys = rd_phys(self, index);
    if (value == NULL) {
        /* del d[i]: reuse remove's shifting via a small local copy */
        PyObject *victim = self->items[phys];
        if (index < self->size - index - 1) {
            circular_mem_move(self->items, self->allocated,
                              wrap_index(self->first + 1,
                                         self->allocated),
                              self->first, index);
            self->items[self->first] = NULL;
            self->first = wrap_index(self->first + 1, self->allocated);
        }
        else {
            Py_ssize_t m = self->size - index - 1;
            if (m > 0) {
                circular_mem_move(self->items, self->allocated,
                                  phys, rd_phys(self, index + 1), m);
            }
            self->items[rd_phys(self, self->size - 1)] = NULL;
        }
        self->size--;
        self->state++;
        Py_DECREF(victim);
        rd_maybe_shrink(self);
        return 0;
    }
    PyObject *old = self->items[phys];
    self->items[phys] = Py_NewRef(value);
    self->state++;
    Py_DECREF(old);
    return 0;
}

/* Build a new deque of `count` elements from src starting at logical
   `start`, striding by `step`. */
static PyObject *
rd_new_from(PyTypeObject *type, ringdeque *src, Py_ssize_t start,
            Py_ssize_t step, Py_ssize_t count, Py_ssize_t maxlen)
{
    ringdeque *result =
        (ringdeque *)type->tp_alloc(type, 0);
    if (result == NULL) {
        return NULL;
    }
    result->maxlen = maxlen;
    if (count > 0) {
        if (rd_ensure_capacity(result, count) < 0) {
            Py_DECREF(result);
            return NULL;
        }
        for (Py_ssize_t i = 0; i < count; i++) {
            PyObject *item =
                src->items[rd_phys(src, start + i * step)];
            result->items[i] = Py_NewRef(item);
        }
        result->size = count;
    }
    return (PyObject *)result;
}

static PyObject *
rd_subscript(PyObject *op, PyObject *key)
{
    ringdeque *self = (ringdeque *)op;
    if (PyIndex_Check(key)) {
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (index < 0) {
            index += self->size;
        }
        return rd_item(op, index);
    }
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
            return NULL;
        }
        Py_ssize_t count = PySlice_AdjustIndices(self->size, &start,
                                                 &stop, step);
        return rd_new_from(Py_TYPE(self), self, start, step, count,
                           self->maxlen);
    }
    PyErr_Format(PyExc_TypeError,
                 "deque indices must be integers or slices, not %.200s",
                 Py_TYPE(key)->tp_name);
    return NULL;
}

static int
rd_ass_subscript(PyObject *op, PyObject *key, PyObject *value)
{
    ringdeque *self = (ringdeque *)op;
    if (PyIndex_Check(key)) {
        Py_ssize_t index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (index == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (index < 0) {
            index += self->size;
        }
        return rd_ass_item(op, index, value);
    }
    PyErr_SetString(PyExc_TypeError,
                    "deque slice assignment is not supported (yet)");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Iterators                                                          */
/* ------------------------------------------------------------------ */

static PyObject *
rd_make_iter(ringdeque *self, int reversed)
{
    ringdeque_iter *it = PyObject_GC_New(ringdeque_iter,
                                         &ringdeque_iter_type);
    if (it == NULL) {
        return NULL;
    }
    it->deque = (ringdeque *)Py_NewRef(self);
    it->index = reversed ? self->size - 1 : 0;
    it->state = self->state;
    it->reversed = reversed;
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static PyObject *
rd_iter(PyObject *op)
{
    return rd_make_iter((ringdeque *)op, 0);
}

static PyObject *
rd_reversed_method(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return rd_make_iter((ringdeque *)op, 1);
}

static PyObject *
rd_iter_next(PyObject *op)
{
    ringdeque_iter *it = (ringdeque_iter *)op;
    ringdeque *d = it->deque;
    if (d == NULL) {
        return NULL;
    }
    if (it->state != d->state) {
        it->index = it->reversed ? -1 : d->size;   /* exhaust */
        PyErr_SetString(PyExc_RuntimeError,
                        "deque mutated during iteration");
        return NULL;
    }
    if (it->reversed ? it->index < 0 : it->index >= d->size) {
        Py_CLEAR(it->deque);
        return NULL;
    }
    PyObject *item = Py_NewRef(d->items[rd_phys(d, it->index)]);
    it->index += it->reversed ? -1 : 1;
    return item;
}

static void
rd_iter_dealloc(PyObject *op)
{
    ringdeque_iter *it = (ringdeque_iter *)op;
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->deque);
    PyObject_GC_Del(it);
}

static int
rd_iter_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(((ringdeque_iter *)op)->deque);
    return 0;
}

static PyObject *
rd_iter_len(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque_iter *it = (ringdeque_iter *)op;
    Py_ssize_t len = 0;
    if (it->deque != NULL && it->state == it->deque->state) {
        len = it->reversed ? it->index + 1
                           : it->deque->size - it->index;
        if (len < 0) {
            len = 0;
        }
    }
    return PyLong_FromSsize_t(len);
}

static PyMethodDef rd_iter_methods[] = {
    {"__length_hint__", rd_iter_len, METH_NOARGS, NULL},
    {NULL, NULL},
};

static PyTypeObject ringdeque_iter_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "ringdeque._deque_iterator",
    .tp_basicsize = sizeof(ringdeque_iter),
    .tp_dealloc = rd_iter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = rd_iter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = rd_iter_next,
    .tp_methods = rd_iter_methods,
};

/* ------------------------------------------------------------------ */
/* Object lifecycle                                                   */
/* ------------------------------------------------------------------ */

static PyObject *
rd_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ringdeque *self = (ringdeque *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->maxlen = -1;
    return (PyObject *)self;
}

static int
rd_init(PyObject *op, PyObject *args, PyObject *kwds)
{
    ringdeque *self = (ringdeque *)op;
    PyObject *iterable = NULL;
    PyObject *maxlenobj = NULL;
    static char *kwlist[] = {"iterable", "maxlen", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO:deque", kwlist,
                                     &iterable, &maxlenobj)) {
        return -1;
    }
    Py_ssize_t maxlen = -1;
    if (maxlenobj != NULL && maxlenobj != Py_None) {
        maxlen = PyNumber_AsSsize_t(maxlenobj, PyExc_OverflowError);
        if (maxlen == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (maxlen < 0) {
            PyErr_SetString(PyExc_ValueError, "maxlen must be non-negative");
            return -1;
        }
    }
    self->maxlen = maxlen;
    if (self->size > 0) {
        PyObject *ignored = rd_clear_method(op, NULL);
        Py_XDECREF(ignored);
    }
    else if (maxlen >= 0 && self->allocated > maxlen + 1) {
        rd_maybe_shrink(self);
    }
    if (iterable != NULL) {
        if (rd_extend_internal(self, iterable, 0) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
rd_traverse(PyObject *op, visitproc visit, void *arg)
{
    ringdeque *self = (ringdeque *)op;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        Py_VISIT(self->items[rd_phys(self, i)]);
    }
    return 0;
}

static int
rd_tp_clear(PyObject *op)
{
    PyObject *ignored = rd_clear_method(op, NULL);
    Py_XDECREF(ignored);
    return 0;
}

static void
rd_dealloc(PyObject *op)
{
    ringdeque *self = (ringdeque *)op;
    PyObject_GC_UnTrack(self);
    if (self->weakreflist != NULL) {
        PyObject_ClearWeakRefs(op);
    }
    rd_tp_clear(op);
    Py_TYPE(self)->tp_free(self);
}

/* ------------------------------------------------------------------ */
/* Comparison, repr, reduce                                           */
/* ------------------------------------------------------------------ */

static PyObject *
rd_richcompare(PyObject *v, PyObject *w, int cmpop)
{
    if (!ringdeque_Check(v) || !ringdeque_Check(w)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    /* Compare via list snapshots: simple and safe against mutation
       during user __eq__ callbacks. */
    PyObject *lv = PySequence_List(v);
    if (lv == NULL) {
        return NULL;
    }
    PyObject *lw = PySequence_List(w);
    if (lw == NULL) {
        Py_DECREF(lv);
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(lv, lw, cmpop);
    Py_DECREF(lv);
    Py_DECREF(lw);
    return result;
}

static PyObject *
rd_repr(PyObject *op)
{
    ringdeque *self = (ringdeque *)op;
    int status = Py_ReprEnter(op);
    if (status != 0) {
        if (status < 0) {
            return NULL;
        }
        return PyUnicode_FromFormat("%s([...])", Py_TYPE(self)->tp_name);
    }
    PyObject *list = PySequence_List(op);
    if (list == NULL) {
        Py_ReprLeave(op);
        return NULL;
    }
    PyObject *result;
    if (self->maxlen >= 0) {
        result = PyUnicode_FromFormat("%s(%R, maxlen=%zd)",
                                      Py_TYPE(self)->tp_name, list,
                                      self->maxlen);
    }
    else {
        result = PyUnicode_FromFormat("%s(%R)",
                                      Py_TYPE(self)->tp_name, list);
    }
    Py_DECREF(list);
    Py_ReprLeave(op);
    return result;
}

static PyObject *
rd_reduce(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque *self = (ringdeque *)op;
    PyObject *list = PySequence_List(op);
    if (list == NULL) {
        return NULL;
    }
    PyObject *args;
    if (self->maxlen >= 0) {
        args = Py_BuildValue("(On)", list, self->maxlen);
    }
    else {
        args = PyTuple_Pack(1, list);
    }
    Py_DECREF(list);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(2, (PyObject *)Py_TYPE(self), args);
    Py_DECREF(args);
    return result;
}

static PyObject *
rd_sizeof(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    ringdeque *self = (ringdeque *)op;
    size_t res = (size_t)Py_TYPE(self)->tp_basicsize;
    res += (size_t)self->allocated * sizeof(PyObject *);
    return PyLong_FromSize_t(res);
}

static PyObject *
rd_get_maxlen(PyObject *op, void *Py_UNUSED(closure))
{
    ringdeque *self = (ringdeque *)op;
    if (self->maxlen < 0) {
        Py_RETURN_NONE;
    }
    return PyLong_FromSsize_t(self->maxlen);
}

/* ------------------------------------------------------------------ */
/* Type definition                                                    */
/* ------------------------------------------------------------------ */

static PyMethodDef rd_methods[] = {
    {"append", rd_append, METH_O,
     "Add an element to the right side of the deque."},
    {"appendleft", rd_appendleft, METH_O,
     "Add an element to the left side of the deque."},
    {"pop", rd_pop_method, METH_NOARGS,
     "Remove and return the rightmost element."},
    {"popleft", rd_popleft_method, METH_NOARGS,
     "Remove and return the leftmost element."},
    {"extend", rd_extend, METH_O,
     "Extend the right side of the deque with elements from the iterable."},
    {"extendleft", rd_extendleft, METH_O,
     "Extend the left side of the deque with elements from the iterable."},
    {"clear", rd_clear_method, METH_NOARGS,
     "Remove all elements from the deque."},
    {"copy", rd_copy_method, METH_NOARGS,
     "Return a shallow copy of the deque."},
    {"__copy__", rd_copy_method, METH_NOARGS, NULL},
    {"rotate", (PyCFunction)(void (*)(void))rd_rotate, METH_FASTCALL,
     "Rotate the deque n steps to the right (default 1)."},
    {"reverse", rd_reverse, METH_NOARGS,
     "Reverse *IN PLACE*."},
    {"count", rd_count, METH_O,
     "Return number of occurrences of value."},
    {"index", (PyCFunction)(void (*)(void))rd_index, METH_FASTCALL,
     "Return first index of value."},
    {"insert", (PyCFunction)(void (*)(void))rd_insert, METH_FASTCALL,
     "Insert value before index."},
    {"remove", rd_remove, METH_O,
     "Remove first occurrence of value."},
    {"__reversed__", rd_reversed_method, METH_NOARGS,
     "Return a reverse iterator over the deque."},
    {"__reduce__", rd_reduce, METH_NOARGS, NULL},
    {"__sizeof__", rd_sizeof, METH_NOARGS, NULL},
    {NULL, NULL},
};

static PyGetSetDef rd_getset[] = {
    {"maxlen", rd_get_maxlen, NULL,
     "maximum size of a deque or None if unbounded", NULL},
    {NULL},
};

static PySequenceMethods rd_as_sequence = {
    .sq_length = rd_len,
    .sq_concat = rd_concat,
    .sq_repeat = rd_repeat,
    .sq_item = rd_item,
    .sq_ass_item = rd_ass_item,
    .sq_contains = rd_contains,
    .sq_inplace_concat = rd_inplace_concat,
    .sq_inplace_repeat = rd_inplace_repeat,
};

static PyMappingMethods rd_as_mapping = {
    .mp_length = rd_len,
    .mp_subscript = rd_subscript,
    .mp_ass_subscript = rd_ass_subscript,
};

static PyTypeObject ringdeque_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "ringdeque.deque",
    .tp_basicsize = sizeof(ringdeque),
    .tp_dealloc = rd_dealloc,
    .tp_repr = rd_repr,
    .tp_as_sequence = &rd_as_sequence,
    .tp_as_mapping = &rd_as_mapping,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
              | Py_TPFLAGS_BASETYPE,
    .tp_doc = "deque([iterable[, maxlen]]) --> ring-buffer deque",
    .tp_traverse = rd_traverse,
    .tp_clear = rd_tp_clear,
    .tp_richcompare = rd_richcompare,
    .tp_iter = rd_iter,
    .tp_methods = rd_methods,
    .tp_getset = rd_getset,
    .tp_init = rd_init,
    .tp_new = rd_new,
    .tp_free = PyObject_GC_Del,
    .tp_weaklistoffset = offsetof(ringdeque, weakreflist),
};

/* ------------------------------------------------------------------ */
/* Module                                                             */
/* ------------------------------------------------------------------ */

static struct PyModuleDef ringdeque_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ringdeque._ringdeque",
    .m_doc = "Growable ring-buffer deque.",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit__ringdeque(void)
{
    if (PyType_Ready(&ringdeque_type) < 0) {
        return NULL;
    }
    if (PyType_Ready(&ringdeque_iter_type) < 0) {
        return NULL;
    }
    PyObject *m = PyModule_Create(&ringdeque_module);
    if (m == NULL) {
        return NULL;
    }
    if (PyModule_AddObjectRef(m, "deque",
                              (PyObject *)&ringdeque_type) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
