/* Task bodies and the three ways of running them that are compared. Shared by
 * bench/bench.c and the dashboard. */
#ifndef STRATEGIES_H
#define STRATEGIES_H

#include "threadpool.h"

extern double iterations_per_us; /* set by calibrate */
extern long sleep_us;            /* used by sleep_task */

double now(void); /* seconds, monotonic */
void die(const char *what);
void burn(long iterations);
void calibrate(void);     /* call first */
void set_work(long us);   /* CPU work of one cpu_task */
void cpu_task(void *arg); /* arg is ignored */
void sleep_task(void *arg);

/* Run `tasks` copies of fn, return elapsed seconds. */
double run_sequential(tp_task_fn fn, size_t tasks);
double run_thread_per_task(tp_task_fn fn, size_t tasks);
double run_pool(tp_task_fn fn, size_t tasks, size_t nthreads);

#endif
