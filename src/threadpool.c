#include "threadpool.h"
#include "bqueue.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    tp_task_fn fn;
    void *arg;
} task;

struct threadpool {
    bqueue *queue;
    pthread_t *threads;
    size_t nthreads; /* workers actually started */

    pthread_mutex_t lock;    /* protects pending */
    pthread_cond_t all_done; /* broadcast when pending reaches 0 */
    size_t pending;          /* tasks submitted but not finished */
};

static void task_finished(threadpool *tp)
{
    pthread_mutex_lock(&tp->lock);
    if (--tp->pending == 0)
        pthread_cond_broadcast(&tp->all_done);
    pthread_mutex_unlock(&tp->lock);
}

static void *worker(void *arg)
{
    threadpool *tp = arg;
    task t;

    while (bq_pop(tp->queue, &t) == BQ_OK) {
        t.fn(t.arg);
        task_finished(tp);
    }
    return NULL;
}

/* Let the given number of workers drain the queue, then release everything. */
static void shut_down(threadpool *tp, size_t started)
{
    bq_close(tp->queue);
    for (size_t i = 0; i < started; i++)
        pthread_join(tp->threads[i], NULL);
    pthread_cond_destroy(&tp->all_done);
    pthread_mutex_destroy(&tp->lock);
    bq_destroy(tp->queue);
    free(tp->threads);
    free(tp);
}

threadpool *tp_create_with(size_t nthreads, size_t queue_capacity,
                           bq_backend backend)
{
    if (nthreads == 0 || queue_capacity == 0)
        return NULL;

    threadpool *tp = calloc(1, sizeof *tp);
    if (!tp)
        return NULL;
    tp->threads = calloc(nthreads, sizeof *tp->threads);
    if (!tp->threads)
        goto fail_threads;
    tp->queue = bq_create(queue_capacity, sizeof(task), backend);
    if (!tp->queue)
        goto fail_queue;
    if (pthread_mutex_init(&tp->lock, NULL) != 0)
        goto fail_lock;
    if (pthread_cond_init(&tp->all_done, NULL) != 0)
        goto fail_cond;

    tp->nthreads = nthreads;
    for (size_t i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) {
            shut_down(tp, i); /* join only the workers that did start */
            return NULL;
        }
    }
    return tp;

fail_cond:
    pthread_mutex_destroy(&tp->lock);
fail_lock:
    bq_destroy(tp->queue);
fail_queue:
    free(tp->threads);
fail_threads:
    free(tp);
    return NULL;
}

threadpool *tp_create(size_t nthreads, size_t queue_capacity)
{
    return tp_create_with(nthreads, queue_capacity, BQ_CONDVAR);
}

static int submit(threadpool *tp, tp_task_fn fn, void *arg, int block)
{
    if (!tp || !fn)
        return TP_INVALID;

    task t = {fn, arg};

    /* Count the task before it becomes visible to the workers. Otherwise one
     * could finish it and take pending below zero before we add it. */
    pthread_mutex_lock(&tp->lock);
    tp->pending++;
    pthread_mutex_unlock(&tp->lock);

    int rc = block ? bq_push(tp->queue, &t) : bq_try_push(tp->queue, &t);
    if (rc == BQ_OK)
        return TP_OK;

    task_finished(tp); /* it never got in: undo the count */
    return rc == BQ_FULL ? TP_FULL : TP_CLOSED;
}

int tp_submit(threadpool *tp, tp_task_fn fn, void *arg)
{
    return submit(tp, fn, arg, 1);
}

int tp_try_submit(threadpool *tp, tp_task_fn fn, void *arg)
{
    return submit(tp, fn, arg, 0);
}

void tp_wait(threadpool *tp)
{
    if (!tp)
        return;
    pthread_mutex_lock(&tp->lock);
    while (tp->pending > 0)
        pthread_cond_wait(&tp->all_done, &tp->lock);
    pthread_mutex_unlock(&tp->lock);
}

size_t tp_snapshot(threadpool *tp, void **args, size_t max, size_t *head)
{
    if (!tp || !args || max == 0 || max > SIZE_MAX / sizeof(task))
        return 0;
    task *tasks = malloc(max * sizeof *tasks);
    if (!tasks)
        return 0;
    size_t n = bq_snapshot(tp->queue, tasks, max, head);
    for (size_t i = 0; i < n; i++)
        args[i] = tasks[i].arg;
    free(tasks);
    return n;
}

void tp_destroy(threadpool *tp)
{
    if (tp)
        shut_down(tp, tp->nthreads);
}
