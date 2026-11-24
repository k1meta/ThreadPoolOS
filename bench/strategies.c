#include "strategies.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define QUEUE_CAPACITY 1024

double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void die(const char *what)
{
    fprintf(stderr, "bench: %s failed\n", what);
    exit(1);
}

/* ---- task bodies ------------------------------------------------------- */

double iterations_per_us;
long sleep_us;
static long work_iterations;

/*
 * A fixed amount of arithmetic, not "spin until the clock says so". If tasks
 * compete for cores they must take longer, as real work would.
 */
void burn(long iterations)
{
    volatile uint32_t sink;
    uint32_t x = 1;
    for (long i = 0; i < iterations; i++)
        x = x * 1664525u + 1013904223u + (x >> 16);
    sink = x;
    (void)sink;
}

void cpu_task(void *arg)
{
    (void)arg;
    burn(work_iterations);
}

void sleep_task(void *arg)
{
    (void)arg;
    struct timespec ts = {sleep_us / 1000000, (sleep_us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

void calibrate(void)
{
    const long n = 200000000;
    burn(n); /* let the CPU reach its running frequency */
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        double t = now();
        burn(n);
        t = now() - t;
        if (t < best)
            best = t;
    }
    iterations_per_us = n / (best * 1e6);
    fprintf(stderr, "calibration: %.0f iterations per us\n", iterations_per_us);
}

void set_work(long us)
{
    work_iterations = (long)(us * iterations_per_us);
}

/* ---- strategies: run `tasks` copies of fn, return elapsed seconds -------- */

double run_sequential(tp_task_fn fn, size_t tasks)
{
    double t = now();
    for (size_t i = 0; i < tasks; i++)
        fn(NULL);
    return now() - t;
}

static void *thread_main(void *arg)
{
    (*(tp_task_fn *)arg)(NULL);
    return NULL;
}

double run_thread_per_task(tp_task_fn fn, size_t tasks)
{
    pthread_t *threads = malloc(tasks * sizeof *threads);
    size_t joined = 0, hit_limit = 0;
    if (!threads)
        die("malloc");

    double t = now();
    for (size_t i = 0; i < tasks; i++) {
        int rc;
        while ((rc = pthread_create(&threads[i], NULL, thread_main, &fn)) == EAGAIN &&
               joined < i) {
            pthread_join(threads[joined++], NULL); /* free a slot, retry */
            hit_limit++;
        }
        if (rc != 0)
            die("pthread_create");
    }
    while (joined < tasks)
        pthread_join(threads[joined++], NULL);
    t = now() - t;

    free(threads);
    if (hit_limit)
        fprintf(stderr, "note: thread limit hit %zu times\n", hit_limit);
    return t;
}

/* Includes creating and destroying the pool, so the pool pays for its own
 * threads just as thread-per-task does. */
double run_pool(tp_task_fn fn, size_t tasks, size_t nthreads)
{
    double t = now();
    threadpool *tp = tp_create(nthreads, QUEUE_CAPACITY);
    if (!tp)
        die("tp_create");
    for (size_t i = 0; i < tasks; i++)
        tp_submit(tp, fn, NULL);
    tp_destroy(tp);
    return now() - t;
}
