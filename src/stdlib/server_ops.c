/*
 * server_ops.c — Process architecture and operations for std.http.
 *
 * Epic 67: graceful reload, binary upgrade, config validation,
 * dynamic worker scaling, log rotation, health probes, graceful
 * shutdown, lifecycle hooks, kqueue/epoll event loop, hybrid model.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>

/* ── Forward declarations for metrics.c log rotation ────────────────── */
extern void metrics_reopen_logs(void);

/* ── Process state ──────────────────────────────────────────────────── */

typedef enum {
    PROC_PHASE_STARTING,
    PROC_PHASE_RUNNING,
    PROC_PHASE_DRAINING,
    PROC_PHASE_STOPPED,
} ProcPhase;

static volatile sig_atomic_t g_proc_phase = PROC_PHASE_STARTING;
static volatile sig_atomic_t g_proc_scale_up   = 0;
static volatile sig_atomic_t g_proc_scale_down = 0;
static volatile sig_atomic_t g_proc_reload = 0;
static volatile sig_atomic_t g_proc_shutdown = 0;

/* Grace period for SIGTERM shutdown (67.2.2) */
static uint64_t g_grace_period_secs = 30;

/* Lifecycle hooks (67.2.3) */
typedef void (*ProcHook)(void);
static ProcHook g_startup_hook  = NULL;
static ProcHook g_shutdown_hook = NULL;

/* ── Signal handlers (67.1.1, 67.1.4, 67.1.5) ──────────────────────── */

static void proc_signal_handler(int sig)
{
    switch (sig) {
    case SIGHUP:
        g_proc_reload = 1;
        break;
    case SIGTERM:
        g_proc_shutdown = 1;
        break;
    case SIGUSR1:
        g_proc_scale_up = 1;
        break;
    case SIGUSR2:
        g_proc_scale_down = 1;
        break;
    default:
        break;
    }
}

/* ── Configuration API ──────────────────────────────────────────────── */

void proc_set_grace_period(uint64_t secs)
{
    g_grace_period_secs = secs;
}

void proc_set_startup_hook(ProcHook hook)
{
    g_startup_hook = hook;
}

void proc_set_shutdown_hook(ProcHook hook)
{
    g_shutdown_hook = hook;
}

/* ── Signal installation ────────────────────────────────────────────── */

void proc_install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = proc_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

/* ── SIGHUP graceful reload (67.1.1) ────────────────────────────────── */

/*
 * proc_check_reload — called from the parent supervision loop.
 * Returns 1 if a reload was requested (and clears the flag).
 */
int proc_check_reload(void)
{
    if (g_proc_reload) {
        g_proc_reload = 0;
        return 1;
    }
    return 0;
}

/* ── Binary upgrade with socket inheritance (67.1.2) ────────────────── */

/*
 * proc_binary_upgrade — exec a new binary, passing the listening socket
 * fd via the TK_LISTEN_FD environment variable. The new process inherits
 * the socket and starts accepting connections; this process drains and
 * exits after the new process signals readiness.
 *
 * binary_path: path to the new binary (NULL = re-exec current).
 * listen_fd:   the listening socket fd to pass.
 * argv:        argument vector for the new process (NULL = inherit).
 *
 * Returns 0 on success (old process should drain), -1 on error.
 */
int proc_binary_upgrade(const char *binary_path, int listen_fd,
                         char *const argv[])
{
    char fd_str[16];
    snprintf(fd_str, sizeof(fd_str), "%d", listen_fd);
    setenv("TK_LISTEN_FD", fd_str, 1);

    /* Clear close-on-exec so the fd survives exec */
    int flags = fcntl(listen_fd, F_GETFD);
    if (flags >= 0)
        fcntl(listen_fd, F_SETFD, flags & ~FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: exec new binary */
        const char *bin = binary_path ? binary_path : "/proc/self/exe";
        if (argv)
            execv(bin, argv);
        else {
            char *default_argv[] = { (char *)bin, NULL };
            execv(bin, default_argv);
        }
        _exit(127); /* exec failed */
    }

    /* Parent: new process is running. Caller should drain and exit. */
    return 0;
}

/*
 * proc_inherit_listen_fd — check if TK_LISTEN_FD is set.
 * Returns the inherited fd, or -1 if not set.
 */
int proc_inherit_listen_fd(void)
{
    const char *env = getenv("TK_LISTEN_FD");
    if (!env) return -1;
    int fd = atoi(env);
    if (fd <= 0) return -1;
    unsetenv("TK_LISTEN_FD");
    return fd;
}

/* ── Configuration validation / dry-run (67.1.3) ───────────────────── */

typedef struct {
    const char *cert_path;
    const char *key_path;
    const char *host;
    uint64_t    port;
    uint64_t    workers;
} ProcConfig;

/*
 * proc_config_test — validate configuration without starting the server.
 * Returns 0 on success, -1 on error (prints diagnostics to stderr).
 */
int proc_config_test(const ProcConfig *cfg)
{
    int ok = 1;

    if (!cfg) {
        fprintf(stderr, "config-test: NULL configuration\n");
        return -1;
    }

    /* Check port range */
    if (cfg->port == 0 || cfg->port > 65535) {
        fprintf(stderr, "config-test: invalid port %llu\n",
                (unsigned long long)cfg->port);
        ok = 0;
    }

    /* Check port availability */
    if (cfg->port > 0 && cfg->port <= 65535) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            int opt = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port   = htons((uint16_t)cfg->port);
            if (cfg->host && cfg->host[0])
                inet_pton(AF_INET, cfg->host, &addr.sin_addr);
            else
                addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                fprintf(stderr, "config-test: port %llu not available: %s\n",
                        (unsigned long long)cfg->port, strerror(errno));
                ok = 0;
            }
            close(s);
        }
    }

    /* Check cert/key files exist and are readable */
    if (cfg->cert_path) {
        if (access(cfg->cert_path, R_OK) != 0) {
            fprintf(stderr, "config-test: cert not readable: %s\n",
                    cfg->cert_path);
            ok = 0;
        }
    }
    if (cfg->key_path) {
        if (access(cfg->key_path, R_OK) != 0) {
            fprintf(stderr, "config-test: key not readable: %s\n",
                    cfg->key_path);
            ok = 0;
        }
    }

    /* Check worker count */
    if (cfg->workers > 256) {
        fprintf(stderr, "config-test: workers %llu exceeds max 256\n",
                (unsigned long long)cfg->workers);
        ok = 0;
    }

    return ok ? 0 : -1;
}

/* ── Dynamic worker scaling (67.1.4) ────────────────────────────────── */

/*
 * proc_check_scale — called from parent supervision loop.
 * Returns +1 if scale-up requested, -1 if scale-down, 0 if neither.
 * Clears the flag.
 */
int proc_check_scale(void)
{
    if (g_proc_scale_up) {
        g_proc_scale_up = 0;
        return 1;
    }
    if (g_proc_scale_down) {
        g_proc_scale_down = 0;
        return -1;
    }
    return 0;
}

/* ── Log rotation (67.1.5) ──────────────────────────────────────────── */

/*
 * proc_reopen_logs — reopen all log file descriptors.
 * Hooks into metrics_reopen_logs() from metrics.c.
 * Called during SIGHUP reload to support logrotate.
 */
void proc_reopen_logs(void)
{
    metrics_reopen_logs();
}

/* ── Health probe endpoints (67.2.1) ────────────────────────────────── */

/*
 * proc_healthz — liveness probe. Returns HTTP 200 if the process is alive.
 * Always succeeds unless the process is in STOPPED phase.
 */
int proc_healthz(char *buf, size_t cap)
{
    const char *body;
    int status;

    if (g_proc_phase == PROC_PHASE_STOPPED) {
        status = 503;
        body = "{\"status\":\"stopped\"}";
    } else {
        status = 200;
        body = "{\"status\":\"ok\"}";
    }

    int n = snprintf(buf, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        status, status == 200 ? "OK" : "Service Unavailable",
        strlen(body), body);
    return n;
}

/*
 * proc_readyz — readiness probe. Returns HTTP 200 only when the server
 * is in RUNNING phase (accepting traffic). Returns 503 during startup
 * and drain phases.
 */
int proc_readyz(char *buf, size_t cap)
{
    int status;
    char body[128];

    if (g_proc_phase == PROC_PHASE_RUNNING) {
        status = 200;
        snprintf(body, sizeof(body), "{\"status\":\"ready\"}");
    } else {
        status = 503;
        const char *phase_str = "unknown";
        switch ((int)g_proc_phase) {
        case PROC_PHASE_STARTING: phase_str = "starting"; break;
        case PROC_PHASE_DRAINING: phase_str = "draining"; break;
        case PROC_PHASE_STOPPED:  phase_str = "stopped";  break;
        default: break;
        }
        snprintf(body, sizeof(body),
                 "{\"status\":\"not_ready\",\"phase\":\"%s\"}", phase_str);
    }

    int n = snprintf(buf, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        status, status == 200 ? "OK" : "Service Unavailable",
        strlen(body), body);
    return n;
}

/* Phase transitions */
void proc_set_phase(int phase) { g_proc_phase = phase; }
int  proc_get_phase(void)      { return (int)g_proc_phase; }

/* ── SIGTERM graceful shutdown (67.2.2) ─────────────────────────────── */

int proc_check_shutdown(void)
{
    return g_proc_shutdown ? 1 : 0;
}

uint64_t proc_get_grace_period(void)
{
    return g_grace_period_secs;
}

/* ── Lifecycle hooks (67.2.3) ───────────────────────────────────────── */

void proc_run_startup_hook(void)
{
    if (g_startup_hook) g_startup_hook();
}

void proc_run_shutdown_hook(void)
{
    if (g_shutdown_hook) g_shutdown_hook();
}

/* ── Event-driven I/O (67.3.1, 67.3.2) ─────────────────────────────── */

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>

typedef struct {
    int kq;
    int listen_fd;
} TkEventLoop;

TkEventLoop *proc_event_loop_new(int listen_fd)
{
    TkEventLoop *el = calloc(1, sizeof(TkEventLoop));
    if (!el) return NULL;

    el->kq = kqueue();
    if (el->kq < 0) { free(el); return NULL; }
    el->listen_fd = listen_fd;

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(el->kq, &ev, 1, NULL, 0, NULL) < 0) {
        close(el->kq);
        free(el);
        return NULL;
    }

    return el;
}

int proc_event_loop_add(TkEventLoop *el, int fd)
{
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    return kevent(el->kq, &ev, 1, NULL, 0, NULL);
}

int proc_event_loop_remove(TkEventLoop *el, int fd)
{
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(el->kq, &ev, 1, NULL, 0, NULL);
}

int proc_event_loop_poll(TkEventLoop *el, int *out_fds, int max_fds,
                          int timeout_ms)
{
    struct kevent events[256];
    int nmax = max_fds < 256 ? max_fds : 256;

    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(el->kq, NULL, 0, events, nmax, tsp);
    if (n < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++)
        out_fds[i] = (int)events[i].ident;

    return n;
}

void proc_event_loop_free(TkEventLoop *el)
{
    if (!el) return;
    close(el->kq);
    free(el);
}

#elif defined(__linux__)
#include <sys/epoll.h>

typedef struct {
    int epfd;
    int listen_fd;
} TkEventLoop;

TkEventLoop *proc_event_loop_new(int listen_fd)
{
    TkEventLoop *el = calloc(1, sizeof(TkEventLoop));
    if (!el) return NULL;

    el->epfd = epoll_create1(0);
    if (el->epfd < 0) { free(el); return NULL; }
    el->listen_fd = listen_fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(el->epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        close(el->epfd);
        free(el);
        return NULL;
    }

    return el;
}

int proc_event_loop_add(TkEventLoop *el, int fd)
{
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(el->epfd, EPOLL_CTL_ADD, fd, &ev);
}

int proc_event_loop_remove(TkEventLoop *el, int fd)
{
    return epoll_ctl(el->epfd, EPOLL_CTL_DEL, fd, NULL);
}

int proc_event_loop_poll(TkEventLoop *el, int *out_fds, int max_fds,
                          int timeout_ms)
{
    struct epoll_event events[256];
    int nmax = max_fds < 256 ? max_fds : 256;

    int n = epoll_wait(el->epfd, events, nmax, timeout_ms);
    if (n < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++)
        out_fds[i] = events[i].data.fd;

    return n;
}

void proc_event_loop_free(TkEventLoop *el)
{
    if (!el) return;
    close(el->epfd);
    free(el);
}

#else
/* Fallback: poll-based */
#include <poll.h>

typedef struct {
    int listen_fd;
    int fds[4096];
    int nfds;
} TkEventLoop;

TkEventLoop *proc_event_loop_new(int listen_fd)
{
    TkEventLoop *el = calloc(1, sizeof(TkEventLoop));
    if (!el) return NULL;
    el->listen_fd = listen_fd;
    el->fds[0] = listen_fd;
    el->nfds = 1;
    return el;
}

int proc_event_loop_add(TkEventLoop *el, int fd)
{
    if (el->nfds >= 4096) return -1;
    el->fds[el->nfds++] = fd;
    return 0;
}

int proc_event_loop_remove(TkEventLoop *el, int fd)
{
    for (int i = 0; i < el->nfds; i++) {
        if (el->fds[i] == fd) {
            el->fds[i] = el->fds[--el->nfds];
            return 0;
        }
    }
    return -1;
}

int proc_event_loop_poll(TkEventLoop *el, int *out_fds, int max_fds,
                          int timeout_ms)
{
    struct pollfd pfd[4096];
    int n = el->nfds < 4096 ? el->nfds : 4096;
    for (int i = 0; i < n; i++) {
        pfd[i].fd = el->fds[i];
        pfd[i].events = POLLIN;
    }
    int ret = poll(pfd, (nfds_t)n, timeout_ms);
    if (ret < 0) return (errno == EINTR) ? 0 : -1;

    int cnt = 0;
    for (int i = 0; i < n && cnt < max_fds; i++) {
        if (pfd[i].revents & POLLIN)
            out_fds[cnt++] = pfd[i].fd;
    }
    return cnt;
}

void proc_event_loop_free(TkEventLoop *el)
{
    free(el);
}

#endif

/* ── Hybrid model helper (67.3.2) ───────────────────────────────────── */

int proc_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── SO_REUSEPORT (67.3.3) ──────────────────────────────────────────── */

int proc_reuseport_available(void)
{
#ifdef SO_REUSEPORT
    return 1;
#else
    return 0;
#endif
}

int proc_enable_reuseport(int fd)
{
#ifdef SO_REUSEPORT
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#else
    (void)fd;
    return -1;
#endif
}
