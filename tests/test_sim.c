/* Checks that the dashboard's live pool loses no task, whatever the user clicks.
 * Exit status is 0 only if every check passed. */
#include "sim.h"
#include "strategies.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *watchdog(void *arg)
{
    (void)arg;
    sleep_ms(60000);
    fprintf(stderr, "  FAIL: still running after 60 s, probable deadlock\n");
    _Exit(3);
}

static long field(const char *name)
{
    char key[32], *json = sim_json(0);
    snprintf(key, sizeof key, "\"%s\":", name);
    const char *at = strstr(json, key);
    long v = at ? strtol(at + strlen(key), NULL, 10) : -1;
    free(json);
    return v;
}

static int contains(const char *part)
{
    char *json = sim_json(0);
    int yes = strstr(json, part) != NULL;
    free(json);
    return yes;
}

static void until(const char *part)
{
    while (!contains(part))
        sleep_ms(1);
}

static long accounted(void)
{
    return field("done") + field("rejected") + field("cancelled");
}

static void finish_all(void)
{
    while (accounted() != field("requested"))
        sleep_ms(1);
}

#define IS_RUNNING "\"life\":\"running\""
#define IS_STOPPED "\"life\":\"stopped\""

static void scenario(bq_backend b)
{
    sim_settings blocking = {.cpu = 0, .ms = 5, .rate = 50};
    sim_settings trying = {.cpu = 0, .ms = 5, .try_only = 1, .rate = 50};
    sim_settings automatic = {.cpu = 1, .ms = 1, .automatic = 1, .rate = 50};
    sim_config c = {3, 4, 2, b};

    /* every task runs once */
    CHECK(sim_set(blocking) == 0);
    CHECK(sim_restart(c) == 0);
    CHECK(sim_add(60) == 0);
    CHECK(sim_reserve() == -1); /* tasks are in progress */
    CHECK(sim_wait() == 0);
    CHECK(sim_wait() == -1); /* one at a time */
    finish_all();
    CHECK(field("done") == 60);

    /* a reservation keeps new tasks out */
    until("\"waiting\":0");
    CHECK(sim_reserve() == 0);
    CHECK(sim_add(1) == -1);
    CHECK(sim_set(automatic) == -1);
    sim_release();

    /* a full queue rejects tp_try_submit: 4 fit, plus up to 3 held by workers */
    CHECK(sim_pause(1) == 0);
    CHECK(sim_set(trying) == 0);
    CHECK(sim_add(30) == 0);
    sleep_ms(50);
    CHECK(field("rejected") >= 30 - 7 && field("rejected") <= 30 - 4);
    CHECK(sim_pause(0) == 0);
    finish_all();

    /* shutdown while paused, with blocked producers and a waiter */
    CHECK(sim_set(blocking) == 0);
    CHECK(sim_pause(1) == 0);
    CHECK(sim_add(40) == 0);
    CHECK(sim_wait() == 0);
    sleep_ms(50);
    CHECK(contains("\"state\":\"blocked\""));
    CHECK(contains("\"state\":\"held\""));
    CHECK(sim_shutdown() == 0);
    CHECK(sim_add(1) == -1);
    until(IS_STOPPED);
    CHECK(accounted() == field("requested"));
    CHECK(sim_pause(1) == -1 && sim_wait() == -1);

    /* restart in the middle of a shutdown, under automatic load */
    CHECK(sim_restart(c) == 0);
    CHECK(sim_set(automatic) == 0);
    sleep_ms(60);
    CHECK(field("requested") > 0);
    CHECK(sim_shutdown() == 0);
    CHECK(sim_restart(c) == 0);
    until(IS_RUNNING);
    CHECK(sim_add(5) == 0);
    finish_all();
    CHECK(sim_shutdown() == 0);
    until(IS_STOPPED);
    CHECK(accounted() == field("requested"));
}

int main(void)
{
    pthread_t wd;
    if (pthread_create(&wd, NULL, watchdog, NULL) != 0)
        return 2;
    pthread_detach(wd);

    iterations_per_us = 700; /* no need for a real calibration */
    sim_init();
    scenario(BQ_CONDVAR);
    scenario(BQ_SEMAPHORE);

    for (int i = 0; i < 20; i++) { /* short-lived pools */
        sim_config c = {16, 1, 8, i % 2 ? BQ_SEMAPHORE : BQ_CONDVAR};
        CHECK(sim_restart(c) == 0);
        sim_add(20); /* may be refused */
        sleep_ms(i % 3 * 10);
    }
    until(IS_RUNNING);
    sim_shutdown();
    until(IS_STOPPED);
    CHECK(accounted() == field("requested"));

    printf("%s sim, %d failed checks\n", failures ? "FAIL" : "ok  ", failures);
    return failures != 0;
}
