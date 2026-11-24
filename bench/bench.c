/*
 * Benchmarks: thread pool vs one thread per task vs sequential, and the two
 * queue backends against each other. CSV on stdout, progress on stderr.
 *
 * Meaning of the "param" column:
 *   granularity, pool_size   CPU work per task in microseconds
 *   io                       requested sleep per task in microseconds
 *   queue                    queue capacity
 */
#include "bqueue.h"
#include "strategies.h"
#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>

#define REPS 5 /* plus one discarded warm-up */
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static void row(const char *experiment, const char *strategy, long param,
                size_t threads, size_t tasks, int rep, double seconds)
{
    if (rep == 0) /* warm-up */
        return;
    printf("%s,%s,%ld,%zu,%zu,%d,%.6f\n", experiment, strategy, param, threads,
           tasks, rep, seconds);
    fflush(stdout);
}

/* ---- experiments -------------------------------------------------------- */

static void granularity(void)
{
    const struct { long us; size_t tasks; } sizes[] = {
        {0, 10000}, {1, 10000}, {10, 10000}, {100, 10000}, {1000, 2000}};

    for (size_t s = 0; s < LEN(sizes); s++) {
        long us = sizes[s].us;
        size_t n = sizes[s].tasks;
        fprintf(stderr, "granularity: %ld us\n", us);
        set_work(us);
        for (int rep = 0; rep <= REPS; rep++) {
            row("granularity", "sequential", us, 1, n, rep,
                run_sequential(cpu_task, n));
            row("granularity", "thread_per_task", us, n, n, rep,
                run_thread_per_task(cpu_task, n));
            row("granularity", "pool", us, 12, n, rep, run_pool(cpu_task, n, 12));
        }
    }
}

static void pool_size(void)
{
    const size_t sizes[] = {1, 2, 4, 6, 8, 12, 16, 24}, n = 10000;
    const long us = 100;

    fprintf(stderr, "pool_size\n");
    set_work(us);
    for (int rep = 0; rep <= REPS; rep++) {
        row("pool_size", "sequential", us, 1, n, rep, run_sequential(cpu_task, n));
        for (size_t s = 0; s < LEN(sizes); s++)
            row("pool_size", "pool", us, sizes[s], n, rep,
                run_pool(cpu_task, n, sizes[s]));
    }
}

static void io(void)
{
    const size_t sizes[] = {12, 48, 96}, n = 500;

    fprintf(stderr, "io\n");
    sleep_us = 10000;
    double t = run_sequential(sleep_task, 20) / 20;
    fprintf(stderr, "requested sleep 10 ms, measured %.2f ms\n", t * 1e3);
    for (int rep = 0; rep <= REPS; rep++) {
        row("io", "thread_per_task", sleep_us, n, n, rep,
            run_thread_per_task(sleep_task, n));
        for (size_t s = 0; s < LEN(sizes); s++)
            row("io", "pool", sleep_us, sizes[s], n, rep,
                run_pool(sleep_task, n, sizes[s]));
    }
}

#define QUEUE_ITEMS 100000

typedef struct {
    bqueue *q;
    size_t items;
} producer_arg;

static void *producer(void *arg)
{
    producer_arg *a = arg;
    for (size_t i = 0; i < a->items; i++)
        bq_push(a->q, &i);
    return NULL;
}

static void *consumer(void *arg)
{
    size_t v;
    while (bq_pop(arg, &v) == BQ_OK)
        ;
    return NULL;
}

/* `pairs` producers and `pairs` consumers move QUEUE_ITEMS items in total. */
static double run_queue(bq_backend backend, size_t capacity, size_t pairs)
{
    pthread_t prod[8], cons[8];
    bqueue *q = bq_create(capacity, sizeof(size_t), backend);
    if (!q)
        die("bq_create");
    producer_arg a = {q, QUEUE_ITEMS / pairs};

    double t = now();
    for (size_t i = 0; i < pairs; i++)
        if (pthread_create(&prod[i], NULL, producer, &a) != 0 ||
            pthread_create(&cons[i], NULL, consumer, q) != 0)
            die("pthread_create");
    for (size_t i = 0; i < pairs; i++)
        pthread_join(prod[i], NULL);
    bq_close(q);
    for (size_t i = 0; i < pairs; i++)
        pthread_join(cons[i], NULL);
    t = now() - t;

    bq_destroy(q);
    return t;
}

static void queue(void)
{
    const size_t caps[] = {1, 64, 1024}, pairs[] = {1, 2, 4};

    for (size_t c = 0; c < LEN(caps); c++)
        for (size_t p = 0; p < LEN(pairs); p++) {
            fprintf(stderr, "queue: capacity %zu, %zuP%zuC\n", caps[c], pairs[p],
                    pairs[p]);
            for (int rep = 0; rep <= REPS; rep++) {
                row("queue", "condvar", (long)caps[c], pairs[p], QUEUE_ITEMS, rep,
                    run_queue(BQ_CONDVAR, caps[c], pairs[p]));
                row("queue", "semaphore", (long)caps[c], pairs[p], QUEUE_ITEMS,
                    rep, run_queue(BQ_SEMAPHORE, caps[c], pairs[p]));
            }
        }
}

int main(void)
{
    printf("experiment,strategy,param,threads,tasks,rep,seconds\n");
    calibrate();
    granularity();
    pool_size();
    io();
    queue();
    return 0;
}
