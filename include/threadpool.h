/*
 * threadpool: a fixed number of worker threads that run submitted tasks.
 * Built on bqueue: submitters are the producers, workers are the consumers.
 * See docs/API.md for the full reference.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

typedef struct threadpool threadpool;
typedef void (*tp_task_fn)(void *arg);

enum {
    TP_OK = 0,
    TP_CLOSED, /* tp_destroy has started */
    TP_FULL,   /* tp_try_submit only: would have blocked */
    TP_INVALID /* pool or fn is NULL */
};

/* Start nthreads workers with room for queue_capacity waiting tasks.
 * Returns NULL on a zero argument or if resources run out. */
threadpool *tp_create(size_t nthreads, size_t queue_capacity);

/* Queue fn(arg). Blocks while the queue is full. Do not call this from inside
 * a task of the same pool: if all workers block here, nobody drains the queue.
 * Use tp_try_submit there. */
int tp_submit(threadpool *tp, tp_task_fn fn, void *arg);

/* Like tp_submit but returns TP_FULL instead of blocking. */
int tp_try_submit(threadpool *tp, tp_task_fn fn, void *arg);

/* Block until every submitted task has finished. The pool stays usable.
 * Must not be called from a task of the same pool. */
void tp_wait(threadpool *tp);

/* Stop accepting tasks, run everything already queued, join the workers and
 * free the pool. Only the pool's own tasks may still be using it, and it must
 * not be called from one of them. NULL is ignored. */
void tp_destroy(threadpool *tp);

#endif
