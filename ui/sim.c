#include "sim.h"
#include "strategies.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TASKS 4096          /* far more than can be unfinished at once */
#define EVENTS 1024         /* kept for a browser that polls late */
#define BLOCKED_AFTER 0.003 /* seconds in tp_submit before we call it blocked */

enum { T_FREE, T_PUSHING, T_QUEUED, T_HELD, T_RUNNING, T_FINISHED };
enum { STOPPED, RUNNING, STOPPING, DRAINING };
enum { W_IDLE, W_HELD, W_RUNNING };
enum { E_DONE, E_REJECTED, E_BLOCKED, E_NOTE };

typedef struct {
    long id;
    int state, producer, worker, cpu, ms;
    double created, queued, started, ended;
} sim_task;

typedef struct {
    int state;
    long task, done;
    double busy; /* seconds spent in finished tasks */
} sim_worker;

typedef struct {
    pthread_t thread;
    int pushing;
    long task, submitted;
    double since, next; /* start of this push; next automatic submit */
} sim_producer;

typedef struct {
    int type;
    long seq;
    double at, ms;
    sim_task task;
    char text[96];
} sim_event;

/*
 * One mutex protects everything below. It is never held across tp_submit,
 * tp_wait or tp_destroy, which can block until a task (which needs it) is done.
 * tp_snapshot is called with it held, so the order is: this one, then the
 * queue's.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work = PTHREAD_COND_INITIALIZER; /* producers */
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER; /* paused workers */

static int life = STOPPED;
static threadpool *pool; /* may be used while RUNNING or STOPPING */
static sim_config config = {4, 8, 2, BQ_CONDVAR}, next_config;
static sim_settings settings = {1, 400, 0, 0, 5};
static int restart_wanted, paused, reserved;

static sim_task tasks[TASKS]; /* a task's arg is its id, not a pointer */
static long last_id;
static long requested, done, rejected, cancelled, to_submit;
static double total_wait, total_run;
static sim_worker workers[SIM_MAX_WORKERS];
static int workers_seen;
static sim_producer producers[SIM_MAX_PRODUCERS];
static pthread_t waiter;
static int waiting, waiter_started;

static sim_event events[EVENTS];
static long last_seq;
static double t0, started_at, stopped_at;

/* What was queued when tp_destroy began, for display while it drains. */
static void *final_args[SIM_MAX_CAPACITY];
static size_t final_count, final_head;

static _Thread_local int my_worker = -1;

/* ---- events (caller holds the lock) ------------------------------------- */

static sim_event *event(int type)
{
    last_seq++;
    sim_event *e = &events[last_seq % EVENTS];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->seq = last_seq;
    e->at = now();
    return e;
}

/* The text goes into JSON unescaped: no quotes or backslashes. */
static void note(const char *s)
{
    sim_event *e = event(E_NOTE);
    snprintf(e->text, sizeof e->text, "%s", s);
}

static sim_task *task_of(long id)
{
    return &tasks[id % TASKS];
}

/* ---- the task body: runs on a worker of the pool ------------------------- */

static void run_task(void *arg)
{
    sim_task *t = task_of((long)(intptr_t)arg);

    pthread_mutex_lock(&lock);
    if (my_worker < 0)
        my_worker = workers_seen++;
    sim_worker *w = &workers[my_worker];

    if (t->state == T_PUSHING) /* we were faster than the producer's return */
        t->queued = now();
    t->worker = my_worker;
    t->state = T_HELD;
    w->task = t->id;
    w->state = W_HELD;
    while (paused)
        pthread_cond_wait(&gate, &lock);
    t->state = T_RUNNING;
    t->started = now();
    w->state = W_RUNNING;
    int cpu = t->cpu, ms = t->ms;
    pthread_mutex_unlock(&lock);

    if (cpu) {
        burn((long)(ms * 1000.0 * iterations_per_us));
    } else {
        struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }

    pthread_mutex_lock(&lock);
    t->ended = now();
    t->state = T_FINISHED;
    w->state = W_IDLE;
    w->done++;
    w->busy += t->ended - t->started;
    done++;
    total_wait += t->started - t->created;
    total_run += t->ended - t->started;
    event(E_DONE)->task = *t;
    pthread_mutex_unlock(&lock);
}

/* ---- producers ----------------------------------------------------------- */

static void wait_until(double when)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double left = when - now();
    if (left < 0)
        left = 0;
    long ns = ts.tv_nsec + (long)((left - (long)left) * 1e9);
    ts.tv_sec += (long)left + ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    pthread_cond_timedwait(&work, &lock, &ts);
}

static sim_task *new_task(int producer)
{
    sim_task *t;
    do { /* a long task may still occupy its slot after TASKS newer ones */
        last_id++;
        t = task_of(last_id);
    } while (t->state != T_FREE && t->state != T_FINISHED);
    *t = (sim_task){.id = last_id, .state = T_PUSHING, .producer = producer,
                    .worker = -1, .cpu = settings.cpu, .ms = settings.ms,
                    .created = now()};
    return t;
}

static void *producer_main(void *arg)
{
    int me = (int)(intptr_t)arg;
    sim_producer *p = &producers[me];

    pthread_mutex_lock(&lock);
    for (;;) {
        while (life == RUNNING && to_submit == 0 &&
               !(settings.automatic && now() >= p->next)) {
            if (settings.automatic)
                wait_until(p->next);
            else
                pthread_cond_wait(&work, &lock);
        }
        if (life != RUNNING)
            break;

        if (to_submit > 0) {
            to_submit--;
        } else {
            requested++;
            p->next = now() + 1.0 / settings.rate;
        }

        sim_task *t = new_task(me);
        long id = t->id;
        int try_only = settings.try_only;
        p->pushing = 1;
        p->task = id;
        p->since = t->created;
        pthread_mutex_unlock(&lock);

        void *task_arg = (void *)(intptr_t)id;
        int rc = try_only ? tp_try_submit(pool, run_task, task_arg)
                          : tp_submit(pool, run_task, task_arg);

        pthread_mutex_lock(&lock);
        double took = now() - p->since;
        p->pushing = 0;
        if (rc != TP_OK) {
            t->state = T_FINISHED;
            rejected++;
            event(E_REJECTED)->task = *t;
            continue;
        }
        p->submitted++;
        /* By now the task may be running, or finished and its slot reused. */
        if (t->id == id && t->state == T_PUSHING) {
            t->state = T_QUEUED;
            t->queued = now();
        }
        if (took >= BLOCKED_AFTER) {
            sim_event *e = event(E_BLOCKED);
            e->task.id = id;
            e->task.producer = me;
            e->ms = took * 1e3;
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* ---- starting and stopping ------------------------------------------------ */

/* Caller holds the lock and life is STOPPED. */
static int start_pool(sim_config c)
{
    threadpool *tp =
        tp_create_with((size_t)c.workers, (size_t)c.capacity, c.backend);
    if (!tp) {
        note("tp_create failed");
        return -1;
    }
    pool = tp;
    config = c;
    requested = done = rejected = cancelled = to_submit = 0;
    total_wait = total_run = 0;
    paused = waiting = workers_seen = 0;
    memset(workers, 0, sizeof workers);
    memset(producers, 0, sizeof producers);
    started_at = now();
    life = RUNNING;
    for (int i = 0; i < c.producers; i++)
        if (pthread_create(&producers[i].thread, NULL, producer_main,
                           (void *)(intptr_t)i) != 0) {
            config.producers = i; /* go on with the ones that did start */
            note("could not start every producer");
            break;
        }
    note("tp_create: workers and producers started");
    return 0;
}

/* Everybody who uses the pool must be gone before tp_destroy (API rule 3). */
static void *teardown_main(void *arg)
{
    (void)arg;
    for (int i = 0; i < config.producers; i++)
        pthread_join(producers[i].thread, NULL);
    if (waiter_started)
        pthread_join(waiter, NULL);

    pthread_mutex_lock(&lock);
    waiter_started = 0;
    cancelled += to_submit;
    to_submit = 0;
    final_count = tp_snapshot(pool, final_args, SIM_MAX_CAPACITY, &final_head);
    life = DRAINING;
    note("tp_destroy: running what is left in the queue");
    pthread_mutex_unlock(&lock);

    tp_destroy(pool);

    pthread_mutex_lock(&lock);
    pool = NULL;
    life = STOPPED;
    stopped_at = now();
    note("tp_destroy: returned, all workers joined");
    if (restart_wanted)
        start_pool(next_config);
    restart_wanted = 0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* Caller holds the lock and life is RUNNING. */
static int begin_teardown(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, teardown_main, NULL) != 0)
        return -1;
    pthread_detach(t);
    life = STOPPING;
    paused = 0; /* paused workers would never finish */
    settings.automatic = 0;
    note("shutdown: waiting for the producers");
    pthread_cond_broadcast(&work);
    pthread_cond_broadcast(&gate);
    return 0;
}

static void *waiter_main(void *arg)
{
    (void)arg;
    char s[96];
    double t = now();
    tp_wait(pool);
    snprintf(s, sizeof s, "tp_wait: returned after %.0f ms", (now() - t) * 1e3);
    pthread_mutex_lock(&lock);
    waiting = 0;
    note(s);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* ---- public functions ------------------------------------------------------ */

void sim_init(void)
{
    t0 = started_at = stopped_at = now();
}

int sim_restart(sim_config c)
{
    int rc = 0;
    pthread_mutex_lock(&lock);
    if (life == STOPPED) {
        rc = start_pool(c);
    } else {
        if (life == RUNNING)
            rc = begin_teardown();
        if (rc == 0) {
            next_config = c;
            restart_wanted = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_shutdown(void)
{
    pthread_mutex_lock(&lock);
    restart_wanted = 0;
    int rc = life == RUNNING ? begin_teardown() : 0;
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_set(sim_settings s)
{
    int rc = -1;
    pthread_mutex_lock(&lock);
    if (!s.automatic || (life == RUNNING && !reserved)) {
        if (s.automatic && !settings.automatic) /* spread the first submits */
            for (int i = 0; i < config.producers; i++)
                producers[i].next =
                    now() + (double)i / config.producers / s.rate;
        settings = s;
        pthread_cond_broadcast(&work);
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_add(int n)
{
    int rc = -1;
    pthread_mutex_lock(&lock);
    if (life == RUNNING && !reserved && to_submit + n <= SIM_MAX_ADD) {
        to_submit += n;
        requested += n;
        pthread_cond_broadcast(&work);
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_pause(int on)
{
    int rc = -1;
    pthread_mutex_lock(&lock);
    if (life == RUNNING) {
        if (paused != on)
            note(on ? "workers paused: they hold their next task"
                    : "workers resumed");
        paused = on;
        pthread_cond_broadcast(&gate);
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_wait(void)
{
    int rc = -1;
    pthread_mutex_lock(&lock);
    if (life == RUNNING && !waiting) {
        if (waiter_started) /* it is past tp_wait and about to return */
            pthread_detach(waiter);
        waiter_started = waiting =
            pthread_create(&waiter, NULL, waiter_main, NULL) == 0;
        if (waiting) {
            note("tp_wait: blocking until no task is pending");
            rc = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int sim_reserve(void)
{
    int rc = -1;
    pthread_mutex_lock(&lock);
    if ((life == RUNNING || life == STOPPED) && !settings.automatic &&
        !reserved && requested == done + rejected + cancelled) {
        reserved = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

void sim_release(void)
{
    pthread_mutex_lock(&lock);
    reserved = 0;
    pthread_mutex_unlock(&lock);
}

/* ---- JSON ------------------------------------------------------------------ */

typedef struct {
    char *buf; /* NULL after running out of memory */
    size_t len, cap;
} json;

static void put(json *o, const char *fmt, ...)
{
    while (o->buf) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < o->cap - o->len) {
            o->len += (size_t)n;
            return;
        }
        size_t cap = o->cap * 2 + (n > 0 ? (size_t)n : 0);
        char *bigger = n < 0 ? NULL : realloc(o->buf, cap);
        if (!bigger)
            free(o->buf);
        o->buf = bigger;
        o->cap = cap;
    }
}

static double rel_ms(double t)
{
    return t > 0 ? (t - t0) * 1e3 : 0;
}

static void put_task(json *o, const sim_task *t)
{
    put(o,
        "{\"id\":%ld,\"producer\":%d,\"worker\":%d,\"cpu\":%d,\"ms\":%d,"
        "\"created\":%.1f,\"queued\":%.1f,\"started\":%.1f,\"ended\":%.1f}",
        t->id, t->producer, t->worker, t->cpu, t->ms, rel_ms(t->created),
        rel_ms(t->queued), rel_ms(t->started), rel_ms(t->ended));
}

static void put_queue(json *o)
{
    void *args[SIM_MAX_CAPACITY];
    size_t head = 0, n = 0;

    if (life == RUNNING || life == STOPPING) {
        n = tp_snapshot(pool, args, SIM_MAX_CAPACITY, &head);
    } else if (life == DRAINING) { /* FIFO: the started ones are in front */
        size_t gone = 0;
        while (gone < final_count &&
               task_of((long)(intptr_t)final_args[gone])->state > T_QUEUED)
            gone++;
        n = final_count - gone;
        memcpy(args, final_args + gone, n * sizeof *args);
        head = (final_head + gone) % (size_t)config.capacity;
    }
    put(o, "\"head\":%zu,\"queue\":[", head);
    for (size_t i = 0; i < n; i++) {
        put(o, "%s", i ? "," : "");
        put_task(o, task_of((long)(intptr_t)args[i]));
    }
    put(o, "],");
}

char *sim_json(long since)
{
    static const char *const life_name[] = {"stopped", "running", "stopping",
                                            "draining"};
    static const char *const worker_name[] = {"idle", "held", "running"};
    static const char *const event_name[] = {"done", "rejected", "blocked",
                                             "note"};
    json out = {malloc(16384), 0, 16384}, *o = &out;

    pthread_mutex_lock(&lock);
    double t = now();
    double uptime = (life == STOPPED ? stopped_at : t) - started_at;
    put(o,
        "{\"now\":%.1f,\"uptime\":%.1f,\"life\":\"%s\",\"restarting\":%d,"
        "\"config\":{\"workers\":%d,\"capacity\":%d,\"producers\":%d,"
        "\"backend\":%d},\"settings\":{\"cpu\":%d,\"ms\":%d,\"try\":%d,"
        "\"auto\":%d,\"rate\":%d},\"paused\":%d,\"waiting\":%d,\"reserved\":%d,",
        rel_ms(t), uptime * 1e3, life_name[life], restart_wanted, config.workers,
        config.capacity, config.producers, (int)config.backend, settings.cpu,
        settings.ms, settings.try_only, settings.automatic, settings.rate, paused,
        waiting, reserved);
    put(o,
        "\"requested\":%ld,\"done\":%ld,\"rejected\":%ld,\"cancelled\":%ld,"
        "\"to_submit\":%ld,\"avg_wait\":%.1f,\"avg_run\":%.1f,",
        requested, done, rejected, cancelled, to_submit,
        done ? total_wait / done * 1e3 : 0, done ? total_run / done * 1e3 : 0);
    put_queue(o);

    put(o, "\"workers\":[");
    for (int i = 0; i < config.workers; i++) {
        const sim_worker *w = &workers[i];
        const sim_task *task = task_of(w->task);
        double busy = w->busy;
        if (w->state == W_RUNNING)
            busy += t - task->started;
        put(o, "%s{\"state\":\"%s\",\"done\":%ld,\"busy\":%.1f,\"task\":",
            i ? "," : "", worker_name[w->state], w->done, busy * 1e3);
        if (w->state == W_IDLE)
            put(o, "null");
        else
            put_task(o, task);
        put(o, "}");
    }

    put(o, "],\"producers\":[");
    for (int i = 0; i < config.producers; i++) {
        const sim_producer *p = &producers[i];
        double pushing = p->pushing ? t - p->since : 0;
        const char *state = !p->pushing                ? "idle"
                            : pushing >= BLOCKED_AFTER ? "blocked"
                                                       : "submitting";
        put(o, "%s{\"state\":\"%s\",\"task\":%ld,\"for\":%.1f,\"submitted\":%ld}",
            i ? "," : "", state, p->task, pushing * 1e3, p->submitted);
    }

    put(o, "],\"seq\":%ld,\"events\":[", last_seq);
    if (since < last_seq - EVENTS)
        since = last_seq - EVENTS;
    if (since < 0)
        since = 0;
    for (long s = since + 1; s <= last_seq; s++) {
        const sim_event *e = &events[s % EVENTS];
        put(o,
            "%s{\"seq\":%ld,\"type\":\"%s\",\"at\":%.1f,\"ms\":%.1f,"
            "\"text\":\"%s\",\"task\":",
            s > since + 1 ? "," : "", e->seq, event_name[e->type], rel_ms(e->at),
            e->ms, e->text);
        put_task(o, &e->task);
        put(o, "}");
    }
    put(o, "]}");
    pthread_mutex_unlock(&lock);
    return out.buf;
}
