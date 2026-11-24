/*
 * sim: a live thread pool to watch. Producer threads submit instrumented tasks
 * to a real threadpool, and sim_json reports what every part is doing.
 *
 * All functions are thread-safe. Those that change something return 0, or -1 if
 * that is not possible right now (for example while the pool shuts down).
 */
#ifndef SIM_H
#define SIM_H

#include "threadpool.h"

enum {
    SIM_MAX_WORKERS = 16,
    SIM_MAX_PRODUCERS = 8,
    SIM_MAX_CAPACITY = 64,
    SIM_MAX_ADD = 1000
};

typedef struct {
    int workers, capacity, producers;
    bq_backend backend;
} sim_config;

typedef struct {
    int cpu;      /* 1: arithmetic, 0: sleep */
    int ms;       /* length of one task */
    int try_only; /* 1: tp_try_submit, 0: tp_submit */
    int automatic;
    int rate; /* tasks per second and producer, in automatic mode; not 0 */
} sim_settings;

void sim_init(void); /* call once, after calibrate() */

int sim_restart(sim_config c); /* shuts a running pool down first */
int sim_shutdown(void);        /* returns at once, see "life" in sim_json */
int sim_set(sim_settings s);   /* affects tasks created from now on */
int sim_add(int n);            /* hand n tasks to the producers */
int sim_pause(int on);         /* workers hold their next task */
int sim_wait(void);            /* call tp_wait on a helper thread */

/* Keep tasks out while something else needs the CPU. Fails if tasks are in
 * progress. */
int sim_reserve(void);
void sim_release(void);

/* Current state and all events after `since`. malloc'd; NULL if no memory. */
char *sim_json(long since);

#endif
