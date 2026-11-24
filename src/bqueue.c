#include "bqueue.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct bqueue {
    pthread_mutex_t lock; /* protects head, count, closed and the buffer */
    unsigned char *buf;
    size_t capacity, item_size;
    size_t head;  /* index of the oldest item */
    size_t count; /* items currently stored */
    int closed;
    bq_backend backend;

    /* BQ_CONDVAR only */
    pthread_cond_t not_full, not_empty;

    /* BQ_SEMAPHORE only */
    sem_t empty_slots, full_slots;
};

/* Ring buffer helpers. The caller holds q->lock. */

static void put(bqueue *q, const void *item)
{
    /* Same as (head + count) % capacity, but cannot overflow. */
    size_t to_end = q->capacity - q->head;
    size_t tail = q->count < to_end ? q->head + q->count : q->count - to_end;
    memcpy(q->buf + tail * q->item_size, item, q->item_size);
    q->count++;
}

static void take(bqueue *q, void *out)
{
    memcpy(out, q->buf + q->head * q->item_size, q->item_size);
    if (++q->head == q->capacity)
        q->head = 0;
    q->count--;
}

/* ---- condition variable backend ---------------------------------------- */

static int cv_push(bqueue *q, const void *item, int block)
{
    int rc = BQ_OK;

    pthread_mutex_lock(&q->lock);
    /* while, not if: wakeups can be spurious, and another producer may have
     * taken the free slot between the signal and our reacquiring the lock. */
    while (block && q->count == q->capacity && !q->closed)
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->closed) {
        rc = BQ_CLOSED;
    } else if (q->count == q->capacity) {
        rc = BQ_FULL;
    } else {
        put(q, item);
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->lock);
    return rc;
}

static int cv_pop(bqueue *q, void *out, int block)
{
    int rc = BQ_OK;

    pthread_mutex_lock(&q->lock);
    while (block && q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count > 0) {
        take(q, out);
        pthread_cond_signal(&q->not_full);
    } else {
        rc = q->closed ? BQ_CLOSED : BQ_EMPTY;
    }
    pthread_mutex_unlock(&q->lock);
    return rc;
}

/* ---- semaphore backend ------------------------------------------------- */

/* Take one token. Returns 0 if block is 0 and none is available. */
static int sem_take(sem_t *s, int block)
{
    while ((block ? sem_wait(s) : sem_trywait(s)) != 0)
        if (errno != EINTR) /* a signal handler ran: just retry */
            return 0;
    return 1;
}

/*
 * The semaphore is always taken before the mutex. Taking them the other way
 * round would deadlock: a thread would sleep on the semaphore while holding
 * the mutex that the thread able to wake it needs.
 *
 * bq_close adds one extra token to each semaphore. A thread that finds it has
 * no work because the queue is closed puts its token back, which wakes the
 * next waiter, and so on until all of them have left.
 */
static int sem_push(bqueue *q, const void *item, int block)
{
    int rc = BQ_OK;
    int got = sem_take(&q->empty_slots, block);

    pthread_mutex_lock(&q->lock);
    if (q->closed)
        rc = BQ_CLOSED;
    else if (!got)
        rc = BQ_FULL;
    else
        put(q, item);
    pthread_mutex_unlock(&q->lock);

    if (rc == BQ_OK)
        sem_post(&q->full_slots);
    else if (got)
        sem_post(&q->empty_slots);
    return rc;
}

static int sem_pop(bqueue *q, void *out, int block)
{
    int rc = BQ_OK;
    int got = sem_take(&q->full_slots, block);

    pthread_mutex_lock(&q->lock);
    if (got && q->count > 0)
        take(q, out);
    else
        rc = q->closed && q->count == 0 ? BQ_CLOSED : BQ_EMPTY;
    pthread_mutex_unlock(&q->lock);

    if (rc == BQ_OK)
        sem_post(&q->empty_slots);
    else if (got)
        sem_post(&q->full_slots);
    return rc;
}

/* ---- public API -------------------------------------------------------- */

bqueue *bq_create(size_t capacity, size_t item_size, bq_backend backend)
{
    if (capacity == 0 || item_size == 0 || capacity > SIZE_MAX / item_size)
        return NULL;
    if (backend != BQ_CONDVAR && backend != BQ_SEMAPHORE)
        return NULL;
    /* leave room for the extra token posted by bq_close */
    if (backend == BQ_SEMAPHORE && capacity >= (size_t)SEM_VALUE_MAX)
        return NULL;

    bqueue *q = calloc(1, sizeof *q);
    if (!q)
        return NULL;
    q->capacity = capacity;
    q->item_size = item_size;
    q->backend = backend;

    q->buf = malloc(capacity * item_size);
    if (!q->buf)
        goto fail_buf;
    if (pthread_mutex_init(&q->lock, NULL) != 0)
        goto fail_lock;

    if (backend == BQ_CONDVAR) {
        if (pthread_cond_init(&q->not_full, NULL) != 0)
            goto fail_sync;
        if (pthread_cond_init(&q->not_empty, NULL) != 0) {
            pthread_cond_destroy(&q->not_full);
            goto fail_sync;
        }
    } else {
        if (sem_init(&q->empty_slots, 0, (unsigned)capacity) != 0)
            goto fail_sync;
        if (sem_init(&q->full_slots, 0, 0) != 0) {
            sem_destroy(&q->empty_slots);
            goto fail_sync;
        }
    }
    return q;

fail_sync:
    pthread_mutex_destroy(&q->lock);
fail_lock:
    free(q->buf);
fail_buf:
    free(q);
    return NULL;
}

static int push(bqueue *q, const void *item, int block)
{
    if (!q || !item)
        return BQ_INVALID;
    return q->backend == BQ_CONDVAR ? cv_push(q, item, block)
                                    : sem_push(q, item, block);
}

static int pop(bqueue *q, void *out, int block)
{
    if (!q || !out)
        return BQ_INVALID;
    return q->backend == BQ_CONDVAR ? cv_pop(q, out, block)
                                    : sem_pop(q, out, block);
}

int bq_push(bqueue *q, const void *item) { return push(q, item, 1); }
int bq_pop(bqueue *q, void *out) { return pop(q, out, 1); }
int bq_try_push(bqueue *q, const void *item) { return push(q, item, 0); }
int bq_try_pop(bqueue *q, void *out) { return pop(q, out, 0); }

void bq_close(bqueue *q)
{
    if (!q)
        return;

    pthread_mutex_lock(&q->lock);
    int already = q->closed;
    q->closed = 1;
    if (q->backend == BQ_CONDVAR) {
        pthread_cond_broadcast(&q->not_full);
        pthread_cond_broadcast(&q->not_empty);
    } else if (!already) {
        sem_post(&q->empty_slots);
        sem_post(&q->full_slots);
    }
    pthread_mutex_unlock(&q->lock);
}

void bq_destroy(bqueue *q)
{
    if (!q)
        return;
    if (q->backend == BQ_CONDVAR) {
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->not_empty);
    } else {
        sem_destroy(&q->empty_slots);
        sem_destroy(&q->full_slots);
    }
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}
