# API documentation

Two modules. `threadpool` is built on `bqueue`; each can be used alone.

```
 submitters (producers)          bqueue               workers (consumers)
   tp_submit  ──────────►  [ task | task | task ]  ──────────►  fn(arg)
   blocks while full            bounded FIFO            block while empty
```

All functions are thread-safe except the `create` and `destroy` functions.

---

## threadpool.h

### `threadpool *tp_create(size_t nthreads, size_t queue_capacity)`

Starts `nthreads` workers. At most `queue_capacity` tasks can wait at once, not
counting those already running.

Returns the pool, or `NULL` if an argument is 0, memory runs out, or a thread
cannot be created. On failure nothing is leaked: workers that did start are
stopped and joined.

### `int tp_submit(threadpool *tp, tp_task_fn fn, void *arg)`

Queues the call `fn(arg)`, where `tp_task_fn` is `void (*)(void *)`. Blocks while
the queue is full, which slows down submitters that outrun the workers.

| Returns | Meaning |
|---|---|
| `TP_OK` | queued, will run exactly once |
| `TP_CLOSED` | `tp_destroy` has started, the task will not run |
| `TP_INVALID` | `tp` or `fn` is `NULL` |

The pool stores the pointer `arg`, not what it points to. That memory must stay
valid until the task has finished. To return a result, write it through `arg`
and read it after `tp_wait`.

### `int tp_try_submit(threadpool *tp, tp_task_fn fn, void *arg)`

Same, but returns `TP_FULL` at once instead of blocking.

### `void tp_wait(threadpool *tp)`

Blocks until no submitted task is queued or running, including tasks submitted
by other tasks. The pool can be used again afterwards. Several threads may wait
at the same time. Everything the tasks wrote is visible after it returns.

### `void tp_destroy(threadpool *tp)`

Refuses new tasks, runs everything already queued, joins the workers and frees
the pool. `NULL` is ignored.

### Rules

1. A task must not call `tp_wait` or `tp_destroy` on its own pool. It would wait
   for itself.
2. A task that submits to its own pool must use `tp_try_submit` and handle
   `TP_FULL`, for example by running the subtask itself. If every worker blocked
   in `tp_submit`, no worker would be left to empty the queue.
3. When `tp_destroy` is called, only the pool's own tasks may still be using it.
4. Tasks that share data must do their own locking.

### Example

`examples/demo.c` (run it with `make demo`):

```c
typedef struct { int id; long result; } job;

static void square(void *arg)
{
    job *j = arg;
    j->result = (long)j->id * j->id;
}

int main(void)
{
    job jobs[8];
    threadpool *pool = tp_create(3, 16);
    if (!pool)
        return 1;
    for (int i = 0; i < 8; i++) {
        jobs[i] = (job){.id = i};
        tp_submit(pool, square, &jobs[i]);
    }
    tp_wait(pool); /* results are safe to read after this */
    tp_destroy(pool);
    return 0;
}
```

---

## bqueue.h

### `bqueue *bq_create(size_t capacity, size_t item_size, bq_backend backend)`

Creates a FIFO queue holding up to `capacity` items of `item_size` bytes.
`backend` is `BQ_CONDVAR` or `BQ_SEMAPHORE`. Both behave the same.

Returns `NULL` if a size is 0, the total size overflows, the backend is unknown,
or resources run out. The semaphore backend also requires `capacity` to be less
than `SEM_VALUE_MAX`.

### Push and pop

| Function | Full queue | Empty queue |
|---|---|---|
| `int bq_push(bqueue *q, const void *item)` | blocks | |
| `int bq_try_push(bqueue *q, const void *item)` | `BQ_FULL` | |
| `int bq_pop(bqueue *q, void *out)` | | blocks |
| `int bq_try_pop(bqueue *q, void *out)` | | `BQ_EMPTY` |

Items are copied in and out with `memcpy`, so `item` and `out` can be local
variables. All four return `BQ_OK` on success and `BQ_INVALID` for a `NULL`
argument.

After `bq_close`, both pushes return `BQ_CLOSED`. Both pops keep returning the
remaining items and then return `BQ_CLOSED`. The usual consumer loop is:

```c
while (bq_pop(q, &item) == BQ_OK)
    handle(item);
```

The `try` functions may wait briefly for the internal mutex. With the semaphore
backend and other threads active, they can report full or empty for an instant
while another thread is between updating the queue and posting the semaphore.

### `void bq_close(bqueue *q)`

Refuses further pushes and wakes every blocked thread. Safe to call repeatedly.

### `void bq_destroy(bqueue *q)`

Frees the queue, dropping any items left. No thread may still be using it, so
close it and join the threads first. `NULL` is ignored.

---

## Synchronization design

**Shared state.** The ring buffer, `head`, `count` and `closed` are only touched
while holding the queue mutex. `pending` is only touched while holding the pool
mutex. The two mutexes are never held at the same time, so they cannot deadlock
against each other.

**Condition variable backend.** Producers wait on `not_full`, consumers on
`not_empty`. Waits are in a `while` loop because wakeups can be spurious and
because another thread may take the slot or item between the signal and the
waiter getting the mutex back. One item frees one waiter, so push and pop use
`signal`. Closing affects everybody, so it uses `broadcast`.

**Semaphore backend.** `empty_slots` starts at `capacity`, `full_slots` at 0. A
push takes from the first and posts to the second; a pop does the opposite. The
mutex only protects the buffer. The semaphore is always taken before the mutex:
in the other order, a producer could sleep on a full queue while holding the
mutex that a consumer needs in order to make room.

**Closing with semaphores.** A semaphore has no broadcast. `bq_close` posts one
extra token to each semaphore. A thread that wakes to find the queue closed, and
for pop also empty, posts its token back before returning. That wakes the next
waiter, and so on until all have left.

**Waiting for tasks.** `tp_submit` increments `pending` before the push. Doing
it after would let a worker finish the task and decrement first. A rejected push
undoes the increment. `tp_wait` sleeps on `all_done` until `pending` is 0.

**Bounded queue.** A fast submitter blocks instead of filling memory. The cost
is rule 2 above.

### Not included

Task return values (use `arg`), priorities, cancelling queued tasks, resizing
the pool, work stealing.
