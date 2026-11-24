/*
 * bqueue: thread-safe, fixed-capacity FIFO queue for the producer-consumer
 * problem. Items are copied in and out by value.
 *
 * Every function except bq_create and bq_destroy may be called from any number
 * of threads at once. See docs/API.md for the full reference.
 */
#ifndef BQUEUE_H
#define BQUEUE_H

#include <stddef.h>

typedef struct bqueue bqueue;

/* How blocked threads wait. Both behave identically from the outside. */
typedef enum {
    BQ_CONDVAR,  /* mutex + two condition variables */
    BQ_SEMAPHORE /* mutex + two counting semaphores */
} bq_backend;

enum {
    BQ_OK = 0,
    BQ_CLOSED, /* push: queue is closed. pop: queue is closed and drained */
    BQ_FULL,   /* bq_try_push only: would have blocked */
    BQ_EMPTY,  /* bq_try_pop only: would have blocked */
    BQ_INVALID /* a NULL argument */
};

/* Returns NULL on a zero size, size overflow, unknown backend, or no memory. */
bqueue *bq_create(size_t capacity, size_t item_size, bq_backend backend);

/* Copy *item in. Blocks while the queue is full and open. */
int bq_push(bqueue *q, const void *item);

/* Move the oldest item into *out. Blocks while the queue is empty and open. */
int bq_pop(bqueue *q, void *out);

/* Non-blocking versions. They may wait briefly for the internal mutex. */
int bq_try_push(bqueue *q, const void *item);
int bq_try_pop(bqueue *q, void *out);

/*
 * Refuse further pushes and wake every blocked thread. Items already queued
 * can still be popped. Calling it again has no effect.
 */
void bq_close(bqueue *q);

/* Free the queue. No other thread may be using it. NULL is ignored. */
void bq_destroy(bqueue *q);

#endif
