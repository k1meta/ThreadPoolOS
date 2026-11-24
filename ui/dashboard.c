/*
 * dashboard: shows the library at work in a web browser.
 *
 * A small HTTP server for this computer only. Each connection is a task on a
 * threadpool, so the library also serves its own user interface. The pool being
 * watched is a separate one, in sim.c. The page itself is ui/index.html.
 */
#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
typedef SOCKET sock;
#define chdir _chdir
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock;
#define closesocket close
#define INVALID_SOCKET (-1)
#define SEND_FLAGS MSG_NOSIGNAL
#endif

#include "sim.h"
#include "strategies.h"
#include "threadpool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FIRST_PORT 8377
#define LAST_PORT 8386
#define SERVER_WORKERS 8
#define REQUEST_MAX 8192
#define IDLE_EXIT 180 /* seconds without a request: the page was closed */

static int port;

/* Protects everything in this block. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static double last_request;
static int quitting;
static struct {
    int running, stage; /* stage: strategies finished */
    long tasks, us, workers;
    double seconds[3], stage_started;
} bench;
static struct {
    int running, finished, exit_code;
} tests;

static void fail(const char *message)
{
#ifdef _WIN32
    MessageBoxA(NULL, message, "Thread pool dashboard", MB_ICONERROR);
#else
    fprintf(stderr, "dashboard: %s\n", message);
#endif
    exit(1);
}

/* Returns 0 if a detached thread could be started. */
static int spawn(void *(*fn)(void *))
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}

/* ---- background jobs: benchmark race and unit tests ---------------------- */

static void stage_done(int i, double seconds)
{
    pthread_mutex_lock(&state_lock);
    bench.seconds[i] = seconds;
    bench.stage = i + 1;
    bench.stage_started = now();
    pthread_mutex_unlock(&state_lock);
}

static void *bench_main(void *arg)
{
    (void)arg;
    size_t n = (size_t)bench.tasks, workers = (size_t)bench.workers;
    set_work(bench.us);
    stage_done(0, run_sequential(cpu_task, n));
    stage_done(1, run_thread_per_task(cpu_task, n));
    stage_done(2, run_pool(cpu_task, n, workers));
    pthread_mutex_lock(&state_lock);
    bench.running = 0;
    pthread_mutex_unlock(&state_lock);
    sim_release(); /* last: the next benchmark may start right after */
    return NULL;
}

/* Runs build/test with its output in build/test.log. Returns its exit status. */
static int run_test_program(void)
{
#ifdef _WIN32
    /* Not system(): that would open a console window. */
    char cmd[] = "cmd.exe /c \"build\\test.exe > build\\test.log 2>&1\"";
    STARTUPINFOA si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    return system("build/test > build/test.log 2>&1");
#endif
}

static void *tests_main(void *arg)
{
    (void)arg;
    int code = run_test_program();
    pthread_mutex_lock(&state_lock);
    tests.running = 0;
    tests.finished = 1;
    tests.exit_code = code;
    pthread_mutex_unlock(&state_lock);
    return NULL;
}

/* ---- HTTP ------------------------------------------------------------------ */

static void send_all(sock s, const char *data, size_t n)
{
    while (n > 0) {
        int sent = (int)send(s, data, n > 65536 ? 65536 : (int)n, SEND_FLAGS);
        if (sent <= 0)
            return; /* the browser went away */
        data += sent;
        n -= (size_t)sent;
    }
}

static void respond(sock s, int status, const char *type, const char *body,
                    size_t n)
{
    char head[512];
    int len = snprintf(head, sizeof head,
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Cache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\n"
                       "Content-Security-Policy: frame-ancestors 'none'\r\n"
                       "Connection: close\r\n\r\n",
                       status, status == 200 ? "OK" : "Error", type, n);
    send_all(s, head, (size_t)len);
    send_all(s, body, n);
}

static void respond_text(sock s, int status, const char *body)
{
    respond(s, status, "text/plain; charset=utf-8", body, strlen(body));
}

static void respond_file(sock s, const char *path, const char *type)
{
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0) {
        rewind(f);
        data = malloc((size_t)size + 1);
    }
    if (data)
        respond(s, 200, type, data, fread(data, 1, (size_t)size, f));
    else
        respond_text(s, 404, "file not found");
    free(data);
    if (f)
        fclose(f);
}

/* The result of a sim_ function as a response. */
static void respond_rc(sock s, int rc)
{
    if (rc == 0)
        respond_text(s, 200, "ok");
    else
        respond_text(s, 409, "Not possible right now: the pool is stopped, "
                             "shutting down, or busy.");
}

/* Reads an integer parameter. If it is missing, not a number or out of range,
 * clears *ok. */
static int param(const char *query, const char *name, int min, int max, int *ok)
{
    size_t len = strlen(name);
    for (const char *p = query; p && *p; p = strchr(p, '&')) {
        if (*p == '&')
            p++;
        if (strncmp(p, name, len) == 0 && p[len] == '=') {
            char *end;
            long v = strtol(p + len + 1, &end, 10);
            if (end != p + len + 1 && (*end == '&' || *end == '\0') && v >= min &&
                v <= max)
                return (int)v;
            break;
        }
    }
    *ok = 0;
    return min;
}

static void get_bench(sock s)
{
    char body[512];
    pthread_mutex_lock(&state_lock);
    int n = snprintf(body, sizeof body,
                     "{\"running\":%d,\"stage\":%d,\"elapsed\":%.6f,\"tasks\":%ld,"
                     "\"us\":%ld,\"workers\":%ld,\"seconds\":[%.6f,%.6f,%.6f]}",
                     bench.running, bench.stage,
                     bench.running ? now() - bench.stage_started : 0, bench.tasks,
                     bench.us, bench.workers, bench.seconds[0], bench.seconds[1],
                     bench.seconds[2]);
    pthread_mutex_unlock(&state_lock);
    respond(s, 200, "application/json", body, (size_t)n);
}

static void post_bench(sock s, int tasks, int us, int workers)
{
    if ((long long)tasks * us > 5000000) {
        respond_text(s, 400, "Too much work: at most 5 s of sequential time.");
        return;
    }
    if (sim_reserve() != 0) {
        respond_text(s, 409, "The live pool is busy, which would spoil the timing. "
                             "Let it finish (or stop automatic mode) first.");
        return;
    }
    pthread_mutex_lock(&state_lock);
    int rc = -1;
    if (!tests.running) { /* sim_reserve allows only one benchmark at a time */
        memset(&bench, 0, sizeof bench);
        bench.running = 1;
        bench.tasks = tasks;
        bench.us = us;
        bench.workers = workers;
        bench.stage_started = now();
        rc = spawn(bench_main);
        if (rc != 0)
            bench.running = 0;
    }
    pthread_mutex_unlock(&state_lock);
    if (rc != 0)
        sim_release();
    respond_rc(s, rc);
}

static void get_tests(sock s)
{
    char body[128];
    pthread_mutex_lock(&state_lock);
    int n = snprintf(body, sizeof body,
                     "{\"running\":%d,\"finished\":%d,\"exit_code\":%d}",
                     tests.running, tests.finished, tests.exit_code);
    pthread_mutex_unlock(&state_lock);
    respond(s, 200, "application/json", body, (size_t)n);
}

static void post_tests(sock s)
{
    pthread_mutex_lock(&state_lock);
    int rc = -1;
    if (!tests.running && !bench.running) {
        tests.running = 1;
        tests.finished = 0;
        rc = spawn(tests_main);
        if (rc != 0)
            tests.running = 0;
    }
    pthread_mutex_unlock(&state_lock);
    respond_rc(s, rc);
}

static void handle_get(sock s, const char *path, const char *query)
{
    int ok = 1;
    if (strcmp(path, "/") == 0) {
        respond_file(s, "ui/index.html", "text/html; charset=utf-8");
    } else if (strcmp(path, "/results/bench.csv") == 0) {
        respond_file(s, "results/bench.csv", "text/csv");
    } else if (strcmp(path, "/api/test.log") == 0) {
        respond_file(s, "build/test.log", "text/plain; charset=utf-8");
    } else if (strcmp(path, "/api/state") == 0) {
        int since = param(query, "since", 0, 2000000000, &ok);
        char *body = sim_json(ok ? since : 0);
        if (body)
            respond(s, 200, "application/json", body, strlen(body));
        else
            respond_text(s, 500, "out of memory");
        free(body);
    } else if (strcmp(path, "/api/bench") == 0) {
        get_bench(s);
    } else if (strcmp(path, "/api/tests") == 0) {
        get_tests(s);
    } else {
        respond_text(s, 404, "not found");
    }
}

static void handle_post(sock s, const char *path, const char *query)
{
    int ok = 1;
    if (strcmp(path, "/api/add") == 0) {
        int n = param(query, "n", 1, SIM_MAX_ADD, &ok);
        if (ok) {
            respond_rc(s, sim_add(n));
            return;
        }
    } else if (strcmp(path, "/api/set") == 0) {
        sim_settings v;
        v.cpu = param(query, "cpu", 0, 1, &ok);
        v.ms = param(query, "ms", 5, 5000, &ok);
        v.try_only = param(query, "try", 0, 1, &ok);
        v.automatic = param(query, "auto", 0, 1, &ok);
        v.rate = param(query, "rate", 1, 50, &ok);
        if (ok) {
            respond_rc(s, sim_set(v));
            return;
        }
    } else if (strcmp(path, "/api/restart") == 0) {
        sim_config c;
        c.workers = param(query, "workers", 1, SIM_MAX_WORKERS, &ok);
        c.capacity = param(query, "capacity", 1, SIM_MAX_CAPACITY, &ok);
        c.producers = param(query, "producers", 1, SIM_MAX_PRODUCERS, &ok);
        c.backend = param(query, "backend", 0, 1, &ok) ? BQ_SEMAPHORE : BQ_CONDVAR;
        if (ok) {
            respond_rc(s, sim_restart(c));
            return;
        }
    } else if (strcmp(path, "/api/pause") == 0) {
        int on = param(query, "on", 0, 1, &ok);
        if (ok) {
            respond_rc(s, sim_pause(on));
            return;
        }
    } else if (strcmp(path, "/api/bench") == 0) {
        int tasks = param(query, "tasks", 1, 10000, &ok);
        int us = param(query, "us", 0, 100000, &ok);
        int workers = param(query, "workers", 1, 64, &ok);
        if (ok) {
            post_bench(s, tasks, us, workers);
            return;
        }
    } else if (strcmp(path, "/api/wait") == 0) {
        respond_rc(s, sim_wait());
        return;
    } else if (strcmp(path, "/api/shutdown") == 0) {
        respond_rc(s, sim_shutdown());
        return;
    } else if (strcmp(path, "/api/tests") == 0) {
        post_tests(s);
        return;
    } else if (strcmp(path, "/api/quit") == 0) {
        respond_text(s, 200, "ok");
        pthread_mutex_lock(&state_lock);
        quitting = 1;
        pthread_mutex_unlock(&state_lock);
        return;
    } else {
        respond_text(s, 404, "not found");
        return;
    }
    respond_text(s, 400, "A value is missing or out of range.");
}

/* Reads until the blank line that ends the headers. Bodies are not used. */
static int read_request(sock s, char *buf)
{
    size_t len = 0;
    double give_up = now() + 5; /* however slowly the bytes arrive */
    while (len < REQUEST_MAX - 1 && now() < give_up) {
        int n = (int)recv(s, buf + len, (int)(REQUEST_MAX - 1 - len), 0);
        if (n <= 0)
            return 0;
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            return 1;
    }
    return 0;
}

/*
 * Other web pages must not be able to press our buttons. Requiring the Host
 * header to be ours stops DNS rebinding. A browser does not let another site
 * send the X-Dashboard header without asking us first, and we never agree.
 */
static int is_our_host(const char *request)
{
    char a[64], b[64];
    snprintf(a, sizeof a, "\r\nHost: 127.0.0.1:%d\r\n", port);
    snprintf(b, sizeof b, "\r\nHost: localhost:%d\r\n", port);
    return strstr(request, a) || strstr(request, b);
}

static void serve(void *arg)
{
    sock s = (sock)(intptr_t)arg;
    char request[REQUEST_MAX], method[8], url[512];

    if (read_request(s, request) &&
        sscanf(request, "%7s %511s", method, url) == 2) {
        char *query = strchr(url, '?');
        if (query)
            *query++ = '\0';
        else
            query = url + strlen(url);

        pthread_mutex_lock(&state_lock);
        last_request = now();
        pthread_mutex_unlock(&state_lock);

        if (!is_our_host(request))
            respond_text(s, 403, "wrong host");
        else if (strcmp(method, "GET") == 0)
            handle_get(s, url, query);
        else if (strcmp(method, "POST") != 0)
            respond_text(s, 405, "method not allowed");
        else if (!strstr(request, "\r\nX-Dashboard: 1\r\n"))
            respond_text(s, 403, "missing header");
        else
            handle_post(s, url, query);
    }
    closesocket(s);
}

/* ---- startup --------------------------------------------------------------- */

static sock listen_on_free_port(void)
{
    for (port = FIRST_PORT; port <= LAST_PORT; port++) {
        sock s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET)
            break;
        int on = 1;
#ifdef _WIN32
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&on, sizeof on);
#else
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
        struct sockaddr_in a = {.sin_family = AF_INET};
        a.sin_port = htons((unsigned short)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* this computer only */
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 64) == 0)
            return s;
        closesocket(s);
    }
    return INVALID_SOCKET;
}

/* Wait up to a second for a connection. */
static sock next_connection(sock listener)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    struct timeval second = {1, 0};
    if (select((int)listener + 1, &set, NULL, NULL, &second) <= 0)
        return INVALID_SOCKET;

    sock s = accept(listener, NULL, NULL);
    if (s != INVALID_SOCKET) {
        /* Browsers open spare connections: do not let them hold a worker. */
#ifdef _WIN32
        DWORD limit = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&limit, sizeof limit);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&limit, sizeof limit);
#else
        struct timeval limit = {2, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
#endif
    }
    return s;
}

static int should_stop(void)
{
    pthread_mutex_lock(&state_lock);
    int stop = quitting || (!bench.running && !tests.running &&
                            now() - last_request > IDLE_EXIT);
    pthread_mutex_unlock(&state_lock);
    return stop;
}

/* The program is build/dashboard: go to the folder above it, so that it finds
 * its files wherever it is started from. */
static void enter_project_folder(const char *program)
{
    char path[1024];
#ifdef _WIN32
    (void)program;
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path)
        return;
#else
    snprintf(path, sizeof path, "%s", program);
#endif
    char *slash = NULL;
    for (char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            slash = p;
    if (slash) {
        *slash = '\0';
        if (chdir(path) != 0 || chdir("..") != 0)
            fail("Cannot open the project folder.");
    }
}

static void open_browser(void)
{
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    char cmd[128];
    printf("dashboard: %s\n", url);
    snprintf(cmd, sizeof cmd, "xdg-open %s >/dev/null 2>&1 &", url);
    if (system(cmd) != 0)
        printf("open that address in a browser\n");
#endif
}

int main(int argc, char **argv)
{
    enter_project_folder(argv[0]);
    FILE *page = fopen("ui/index.html", "rb");
    if (!page)
        fail("ui/index.html not found. Keep the program in the project's build "
             "folder.");
    fclose(page);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        fail("Cannot start networking.");
#endif
    sock listener = listen_on_free_port();
    if (listener == INVALID_SOCKET)
        fail("No free port between 8377 and 8386.");

    calibrate(); /* about a second */
    sim_init();
    sim_config c = {4, 8, 2, BQ_CONDVAR};
    threadpool *server = tp_create(SERVER_WORKERS, 64);
    if (!server || sim_restart(c) != 0)
        fail("Cannot start the thread pools.");

    last_request = now();
    /* --no-browser is for testing */
    if (argc < 2 || strcmp(argv[1], "--no-browser") != 0)
        open_browser();

    while (!should_stop()) {
        sock s = next_connection(listener);
        if (s != INVALID_SOCKET &&
            tp_submit(server, serve, (void *)(intptr_t)s) != TP_OK)
            closesocket(s);
    }
    closesocket(listener);
    tp_destroy(server); /* finish the requests in progress */
    /* The live pool may be in the middle of long tasks: let the OS end it. */
    exit(0);
}
