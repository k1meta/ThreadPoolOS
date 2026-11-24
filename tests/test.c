/* Unit and stress tests. Exit status is 0 only if every check passed. */
#include "bqueue.h"
#include "threadpool.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WATCHDOG_SECONDS 60

static atomic_int failures;
static int tests_run;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            atomic_fetch_add(&failures, 1);                                   \
        }                                                                     \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void start(pthread_t *t, void *(*fn)(void *), void *arg)
{
    if (pthread_create(t, NULL, fn, arg) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        exit(2);
    }
}

/* A deadlock must fail the run rather than hang it. */
static void *watchdog(void *arg)
{
    (void)arg;
    sleep_ms(WATCHDOG_SECONDS * 1000L);
    fprintf(stderr, "  FAIL: still running after %d s, probable deadlock\n",
            WATCHDOG_SECONDS);
    _Exit(3);
}

/* ======================= bounded queue, both backends ==================== */

static void queue_invalid(bq_backend b)
{
    CHECK(bq_create(0, 4, b) == NULL);
    CHECK(bq_create(4, 0, b) == NULL);
    CHECK(bq_create(SIZE_MAX, 2, b) == NULL);
    CHECK(bq_create(4, 4, (bq_backend)99) == NULL);

    bqueue *q = bq_create(1, sizeof(int), b);
    int x = 0;
    CHECK(bq_push(NULL, &x) == BQ_INVALID);
    CHECK(bq_push(q, NULL) == BQ_INVALID);
    CHECK(bq_pop(q, NULL) == BQ_INVALID);
    CHECK(bq_try_push(q, NULL) == BQ_INVALID);
    CHECK(bq_try_pop(NULL, &x) == BQ_INVALID);
    bq_close(NULL);
    bq_destroy(NULL);
    bq_destroy(q);
}

static void queue_fifo_wraparound(bq_backend b)
{
    bqueue *q = bq_create(3, sizeof(int), b);
    int next_in = 0, next_out = 0, v;

    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 2; i++, next_in++)
            CHECK(bq_push(q, &next_in) == BQ_OK);
        for (int i = 0; i < 2; i++, next_out++) {
            CHECK(bq_pop(q, &v) == BQ_OK);
            CHECK(v == next_out);
        }
    }
    bq_destroy(q);
}

static void queue_snapshot(bq_backend b)
{
    bqueue *q = bq_create(3, sizeof(int), b);
    int v, out[3];
    size_t head = 99;

    CHECK(bq_snapshot(q, out, 3, &head) == 0 && head == 0);
    for (int i = 0; i < 3; i++)
        bq_push(q, &i);
    bq_pop(q, &v);
    bq_pop(q, &v);
    for (int i = 3; i < 5; i++) /* wraps around */
        bq_push(q, &i);
    CHECK(bq_snapshot(q, out, 3, &head) == 3 && head == 2);
    for (int i = 0; i < 3; i++)
        CHECK(out[i] == i + 2);
    CHECK(bq_snapshot(q, out, 1, NULL) == 1 && out[0] == 2);
    CHECK(bq_snapshot(NULL, out, 3, NULL) == 0);
    CHECK(bq_snapshot(q, NULL, 3, NULL) == 0);

    bq_close(q); /* nothing was removed */
    for (int i = 2; i < 5; i++)
        CHECK(bq_pop(q, &v) == BQ_OK && v == i);
    bq_destroy(q);
}

static void queue_struct_items(bq_backend b)
{
    struct item {
        char name[20];
        double d;
    } in = {"hello", 2.5}, out;
    bqueue *q = bq_create(2, sizeof in, b);

    CHECK(bq_push(q, &in) == BQ_OK);
    in.d = 9; /* the queue holds a copy */
    CHECK(bq_pop(q, &out) == BQ_OK);
    CHECK(out.name[4] == 'o' && out.d == 2.5);
    bq_destroy(q);
}

static void queue_try_ops(bq_backend b)
{
    bqueue *q = bq_create(2, sizeof(int), b);
    int v = 7;
    CHECK(bq_try_pop(q, &v) == BQ_EMPTY);
    CHECK(bq_try_push(q, &v) == BQ_OK);
    CHECK(bq_try_push(q, &v) == BQ_OK);
    CHECK(bq_try_push(q, &v) == BQ_FULL);
    CHECK(bq_try_pop(q, &v) == BQ_OK && v == 7);
    CHECK(bq_try_push(q, &v) == BQ_OK);
    bq_destroy(q);
}

typedef struct {
    bqueue *q;
    atomic_int done;
    int rc, value;
} blocked;

static void *do_push(void *arg)
{
    blocked *x = arg;
    x->rc = bq_push(x->q, &x->value);
    atomic_store(&x->done, 1);
    return NULL;
}

static void *do_pop(void *arg)
{
    blocked *x = arg;
    x->rc = bq_pop(x->q, &x->value);
    atomic_store(&x->done, 1);
    return NULL;
}

static void queue_push_blocks_when_full(bq_backend b)
{
    bqueue *q = bq_create(1, sizeof(int), b);
    int v = 1;
    pthread_t t;
    blocked x = {.q = q, .value = 2};

    bq_push(q, &v);
    start(&t, do_push, &x);
    sleep_ms(50);
    CHECK(!atomic_load(&x.done));
    CHECK(bq_pop(q, &v) == BQ_OK && v == 1);
    pthread_join(t, NULL);
    CHECK(x.rc == BQ_OK);
    CHECK(bq_pop(q, &v) == BQ_OK && v == 2);
    bq_destroy(q);
}

static void queue_pop_blocks_when_empty(bq_backend b)
{
    bqueue *q = bq_create(1, sizeof(int), b);
    int v = 5;
    pthread_t t;
    blocked x = {.q = q};

    start(&t, do_pop, &x);
    sleep_ms(50);
    CHECK(!atomic_load(&x.done));
    CHECK(bq_push(q, &v) == BQ_OK);
    pthread_join(t, NULL);
    CHECK(x.rc == BQ_OK && x.value == 5);
    bq_destroy(q);
}

#define BLOCKED 4

/* Block several threads in fn, close the queue, expect all to get BQ_CLOSED. */
static void close_wakes(bqueue *q, void *(*fn)(void *))
{
    pthread_t t[BLOCKED];
    blocked x[BLOCKED] = {{0}};

    for (int i = 0; i < BLOCKED; i++) {
        x[i].q = q;
        start(&t[i], fn, &x[i]);
    }
    sleep_ms(50);
    bq_close(q);
    for (int i = 0; i < BLOCKED; i++) {
        pthread_join(t[i], NULL);
        CHECK(x[i].rc == BQ_CLOSED);
    }
    bq_destroy(q);
}

static void queue_close_wakes_all(bq_backend b)
{
    int v = 0;

    close_wakes(bq_create(1, sizeof(int), b), do_pop);

    bqueue *full = bq_create(1, sizeof(int), b);
    bq_push(full, &v);
    close_wakes(full, do_push);
}

static void queue_close_drains(bq_backend b)
{
    bqueue *q = bq_create(4, sizeof(int), b);
    int v;

    for (v = 0; v < 3; v++)
        bq_push(q, &v);
    bq_close(q);
    bq_close(q); /* idempotent */
    CHECK(bq_push(q, &v) == BQ_CLOSED);
    CHECK(bq_try_push(q, &v) == BQ_CLOSED);
    for (int i = 0; i < 3; i++)
        CHECK(bq_pop(q, &v) == BQ_OK && v == i);
    for (int i = 0; i < 3; i++) {
        CHECK(bq_pop(q, &v) == BQ_CLOSED);
        CHECK(bq_try_pop(q, &v) == BQ_CLOSED);
    }
    bq_destroy(q);

    /* closed takes priority over full */
    q = bq_create(1, sizeof(int), b);
    bq_push(q, &v);
    bq_close(q);
    CHECK(bq_try_push(q, &v) == BQ_CLOSED);
    bq_destroy(q);
}

#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 25000

static atomic_uchar seen[PRODUCERS * PER_PRODUCER];

typedef struct {
    bqueue *q;
    int id;
} stress_arg;

static void *stress_producer(void *arg)
{
    stress_arg *a = arg;
    for (int i = 0; i < PER_PRODUCER; i++) {
        int v = a->id * PER_PRODUCER + i;
        CHECK(bq_push(a->q, &v) == BQ_OK);
    }
    return NULL;
}

static void *stress_consumer(void *arg)
{
    stress_arg *a = arg;
    int rc, v, last[PRODUCERS];

    for (int i = 0; i < PRODUCERS; i++)
        last[i] = -1;
    while ((rc = bq_pop(a->q, &v)) == BQ_OK) {
        int p = v / PER_PRODUCER;
        CHECK(v > last[p]); /* FIFO: one producer's items arrive in order */
        last[p] = v;
        CHECK(atomic_fetch_add(&seen[v], 1) == 0);
    }
    CHECK(rc == BQ_CLOSED); /* a blocking pop must never report BQ_EMPTY */
    return NULL;
}

static void queue_mpmc_stress(bq_backend b)
{
    size_t caps[] = {1, 64};
    for (size_t c = 0; c < 2; c++) {
        bqueue *q = bq_create(caps[c], sizeof(int), b);
        pthread_t prod[PRODUCERS], cons[CONSUMERS];
        stress_arg args[PRODUCERS];

        for (int i = 0; i < PRODUCERS * PER_PRODUCER; i++)
            atomic_store(&seen[i], 0);
        for (int i = 0; i < PRODUCERS; i++) {
            args[i] = (stress_arg){q, i};
            start(&prod[i], stress_producer, &args[i]);
        }
        for (int i = 0; i < CONSUMERS; i++)
            start(&cons[i], stress_consumer, &args[0]);
        for (int i = 0; i < PRODUCERS; i++)
            pthread_join(prod[i], NULL);
        bq_close(q);
        for (int i = 0; i < CONSUMERS; i++)
            pthread_join(cons[i], NULL);

        int missing = 0;
        for (int i = 0; i < PRODUCERS * PER_PRODUCER; i++)
            missing += atomic_load(&seen[i]) != 1;
        CHECK(missing == 0);
        bq_destroy(q);
    }
}

/* ============================== thread pool ============================== */

static atomic_int counter;

static void count_task(void *arg)
{
    (void)arg;
    atomic_fetch_add(&counter, 1);
}

static void pool_invalid(void)
{
    CHECK(tp_create(0, 1) == NULL);
    CHECK(tp_create(1, 0) == NULL);
    CHECK(tp_create(1, SIZE_MAX) == NULL);
    CHECK(tp_submit(NULL, count_task, NULL) == TP_INVALID);

    threadpool *tp = tp_create(1, 1);
    CHECK(tp_submit(tp, NULL, NULL) == TP_INVALID);
    CHECK(tp_try_submit(tp, NULL, NULL) == TP_INVALID);
    tp_wait(NULL);
    tp_wait(tp); /* nothing submitted: returns at once */
    tp_destroy(NULL);
    tp_destroy(tp);
}

static void mark_task(void *arg)
{
    atomic_fetch_add((atomic_int *)arg, 1);
}

static void pool_runs_each_task_once(void)
{
    enum { N = 10000 };
    static atomic_int runs[N];
    threadpool *tp = tp_create(4, 16);

    for (int i = 0; i < N; i++)
        CHECK(tp_submit(tp, mark_task, &runs[i]) == TP_OK);
    tp_wait(tp);

    int wrong = 0;
    for (int i = 0; i < N; i++)
        wrong += atomic_load(&runs[i]) != 1;
    CHECK(wrong == 0);
    tp_destroy(tp);
}

static void barrier_task(void *arg)
{
    pthread_barrier_wait(arg);
}

/* The barrier only opens if all workers are running tasks at the same time. */
static void pool_is_parallel(void)
{
    enum { N = 8 };
    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, N);
    threadpool *tp = tp_create(N, N);

    for (int i = 0; i < N; i++)
        tp_submit(tp, barrier_task, &bar);
    tp_wait(tp);
    tp_destroy(tp);
    pthread_barrier_destroy(&bar);
}

static atomic_int running, peak;

static void peak_task(void *arg)
{
    (void)arg;
    int now = atomic_fetch_add(&running, 1) + 1;
    int old = atomic_load(&peak);
    while (now > old && !atomic_compare_exchange_weak(&peak, &old, now))
        ;
    sleep_ms(1);
    atomic_fetch_sub(&running, 1);
}

static void pool_limits_concurrency(void)
{
    threadpool *tp = tp_create(3, 100);
    for (int i = 0; i < 30; i++)
        tp_submit(tp, peak_task, NULL);
    tp_wait(tp);
    CHECK(atomic_load(&peak) >= 1 && atomic_load(&peak) <= 3);
    tp_destroy(tp);
}

static void pool_reuse_after_wait(void)
{
    threadpool *tp = tp_create(4, 8);
    atomic_store(&counter, 0);
    for (int round = 1; round <= 100; round++) {
        for (int i = 0; i < 20; i++)
            tp_submit(tp, count_task, NULL);
        tp_wait(tp);
        CHECK(atomic_load(&counter) == round * 20);
    }
    tp_destroy(tp);
}

static void pool_destroy_drains(void)
{
    threadpool *tp = tp_create(2, 1000);
    atomic_store(&counter, 0);
    for (int i = 0; i < 1000; i++)
        tp_submit(tp, count_task, NULL);
    tp_destroy(tp); /* no tp_wait */
    CHECK(atomic_load(&counter) == 1000);
}

typedef struct {
    sem_t started, go;
} gate;

static void gate_task(void *arg)
{
    gate *g = arg;
    sem_post(&g->started);
    sem_wait(&g->go);
}

static void pool_try_submit_full(void)
{
    gate g;
    sem_init(&g.started, 0, 0);
    sem_init(&g.go, 0, 0);
    atomic_store(&counter, 0);

    threadpool *tp = tp_create(1, 1);
    CHECK(tp_submit(tp, gate_task, &g) == TP_OK);
    sem_wait(&g.started); /* the only worker is now busy, queue is empty */
    CHECK(tp_try_submit(tp, count_task, NULL) == TP_OK);
    CHECK(tp_try_submit(tp, count_task, NULL) == TP_FULL);
    sem_post(&g.go);
    tp_wait(tp); /* the rejected task must not be counted as pending */
    CHECK(atomic_load(&counter) == 1);
    tp_destroy(tp);
    sem_destroy(&g.started);
    sem_destroy(&g.go);
}

static void *submitter(void *arg)
{
    for (int i = 0; i < 2000; i++)
        CHECK(tp_submit(arg, count_task, NULL) == TP_OK);
    tp_wait(arg); /* several threads may wait at once */
    return NULL;
}

static void pool_concurrent_submitters(void)
{
    pthread_t t[4];
    threadpool *tp = tp_create(4, 8);
    atomic_store(&counter, 0);
    for (int i = 0; i < 4; i++)
        start(&t[i], submitter, tp);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    CHECK(atomic_load(&counter) == 8000);
    tp_destroy(tp);
}

static threadpool *tree_pool;

static void tree_task(void *arg)
{
    intptr_t depth = (intptr_t)arg;
    atomic_fetch_add(&counter, 1);
    if (depth == 0)
        return;
    for (int i = 0; i < 2; i++)
        if (tp_try_submit(tree_pool, tree_task, (void *)(depth - 1)) != TP_OK)
            tree_task((void *)(depth - 1)); /* queue full: do it ourselves */
}

static void pool_subtasks(void)
{
    tree_pool = tp_create(4, 8);
    atomic_store(&counter, 0);
    tp_submit(tree_pool, tree_task, (void *)(intptr_t)10);
    tp_wait(tree_pool); /* must also wait for tasks submitted by tasks */
    CHECK(atomic_load(&counter) == 2047);
    tp_destroy(tree_pool);
}

static void until_closed_task(void *arg)
{
    while (tp_try_submit(arg, count_task, NULL) != TP_CLOSED)
        sched_yield();
}

static void pool_rejects_during_destroy(void)
{
    threadpool *tp = tp_create(1, 1);
    tp_submit(tp, until_closed_task, tp);
    tp_destroy(tp); /* returns only if the task saw TP_CLOSED */
}

static void pool_create_destroy_loop(void)
{
    for (int i = 0; i < 200; i++) {
        threadpool *tp = tp_create(4, 4);
        CHECK(tp != NULL);
        tp_destroy(tp);
    }
}

static void pool_snapshot(void)
{
    const bq_backend backends[] = {BQ_CONDVAR, BQ_SEMAPHORE};
    CHECK(tp_create_with(1, 1, (bq_backend)99) == NULL);

    for (int b = 0; b < 2; b++) {
        gate g;
        void *args[8];
        size_t head = 99;
        sem_init(&g.started, 0, 0);
        sem_init(&g.go, 0, 0);
        atomic_store(&counter, 0);

        threadpool *tp = tp_create_with(1, 4, backends[b]);
        CHECK(tp_snapshot(tp, args, 8, &head) == 0);
        tp_submit(tp, gate_task, &g);
        sem_wait(&g.started); /* the gate task has left slot 0 */
        for (intptr_t i = 1; i <= 3; i++)
            tp_submit(tp, count_task, (void *)i);
        CHECK(tp_snapshot(tp, args, 8, &head) == 3 && head == 1);
        for (intptr_t i = 1; i <= 3; i++)
            CHECK(args[i - 1] == (void *)i);
        CHECK(tp_snapshot(tp, args, 2, NULL) == 2);
        CHECK(tp_snapshot(NULL, args, 8, NULL) == 0);
        CHECK(tp_snapshot(tp, NULL, 8, NULL) == 0);

        sem_post(&g.go);
        tp_wait(tp);
        CHECK(atomic_load(&counter) == 3);
        tp_destroy(tp);
        sem_destroy(&g.started);
        sem_destroy(&g.go);
    }
}

/* ================================ runner ================================= */

static void report(const char *name, const char *variant, int failures_before)
{
    tests_run++;
    printf("%s %s%s\n",
           atomic_load(&failures) == failures_before ? "ok  " : "FAIL", name,
           variant);
    fflush(stdout);
}

#define RUN_QUEUE(fn)                   \
    do {                                \
        int f = atomic_load(&failures); \
        fn(BQ_CONDVAR);                 \
        report(#fn, " [condvar]", f);   \
        f = atomic_load(&failures);     \
        fn(BQ_SEMAPHORE);               \
        report(#fn, " [semaphore]", f); \
    } while (0)

#define RUN(fn)                         \
    do {                                \
        int f = atomic_load(&failures); \
        fn();                           \
        report(#fn, "", f);             \
    } while (0)

int main(void)
{
    pthread_t wd;
    start(&wd, watchdog, NULL);
    pthread_detach(wd);

    RUN_QUEUE(queue_invalid);
    RUN_QUEUE(queue_fifo_wraparound);
    RUN_QUEUE(queue_struct_items);
    RUN_QUEUE(queue_try_ops);
    RUN_QUEUE(queue_push_blocks_when_full);
    RUN_QUEUE(queue_pop_blocks_when_empty);
    RUN_QUEUE(queue_close_wakes_all);
    RUN_QUEUE(queue_close_drains);
    RUN_QUEUE(queue_mpmc_stress);
    RUN_QUEUE(queue_snapshot);

    RUN(pool_invalid);
    RUN(pool_runs_each_task_once);
    RUN(pool_is_parallel);
    RUN(pool_limits_concurrency);
    RUN(pool_reuse_after_wait);
    RUN(pool_destroy_drains);
    RUN(pool_try_submit_full);
    RUN(pool_concurrent_submitters);
    RUN(pool_subtasks);
    RUN(pool_rejects_during_destroy);
    RUN(pool_create_destroy_loop);
    RUN(pool_snapshot);

    printf("\n%d tests, %d failed checks\n", tests_run, atomic_load(&failures));
    return atomic_load(&failures) != 0;
}
