/* Smallest useful program: 8 tasks on 3 workers. */
#include "threadpool.h"

#include <stdio.h>
#include <time.h>

typedef struct {
    int id;
    long result;
} job;

static void square(void *arg)
{
    job *j = arg;
    struct timespec ts = {0, 200 * 1000000L}; /* pretend to work for 200 ms */

    printf("task %d started\n", j->id);
    nanosleep(&ts, NULL);
    j->result = (long)j->id * j->id;
    printf("task %d finished\n", j->id);
}

int main(void)
{
    job jobs[8];
    threadpool *pool = tp_create(3, 16);
    if (!pool) {
        fprintf(stderr, "could not create the pool\n");
        return 1;
    }

    for (int i = 0; i < 8; i++) {
        jobs[i] = (job){.id = i};
        tp_submit(pool, square, &jobs[i]);
    }
    tp_wait(pool); /* results are safe to read after this */

    for (int i = 0; i < 8; i++)
        printf("%d squared is %ld\n", jobs[i].id, jobs[i].result);
    tp_destroy(pool);
    return 0;
}
