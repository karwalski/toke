/*
 * fuzz_http_parse.c — libFuzzer entry point for the HTTP/1.1 server's
 * request framing and parsing, driven through the REAL connection loop.
 *
 * Build:  make fuzz-http-parse   (FUZZ_ENGINE=libfuzzer|standalone|auto)
 * Story:  57.4.6; rewritten for 121.30
 *
 * 121.30: this harness used to embed its own COPY of an old parse_request
 * and fuzz that -- src/stdlib/http.c was not compiled into it at all, so no
 * finding it could make was a finding about the server, and every fix to
 * http.c (121.7's chunked limits, 121.17's framing) was invisible to it.  It
 * now feeds the input to http_handle_fd() over a socketpair: the bytes are
 * the whole client stream (one request or several, pipelined), the server
 * frames them with http_read_request(), dispatches, and answers; a thread
 * drains the answers so a long pipeline cannot deadlock the socket.
 * test/fuzz/corpus-adversarial/http_parse_* are replayed through it by
 * fuzz-smoke.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "http.h"

static Res echo(Req r) { return http_Res_ok(r.body ? r.body : ""); }
static Res hello(Req r) { (void)r; return http_Res_ok("hello"); }

static void *drain(void *arg)
{
    int fd = *(int *)arg;
    char buf[4096];
    while (read(fd, buf, sizeof buf) > 0) { }
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int init;
    if (!init) {
        signal(SIGPIPE, SIG_IGN);
        /* A small body limit keeps the per-connection buffer small; a 1 s
         * timeout bounds any wait (the stream is closed after the input, so
         * a well-behaved server never waits at all). */
        http_set_limits(0, 16384, 1);
        http_GET("/", hello);
        http_POST("/echo", echo);
        http_GET("/users/:id", hello);
        init = 1;
    }
    if (size > 65536) return 0;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    int big = 1 << 17;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    /* Queue the input without blocking (the server is not running yet); an
     * input bigger than the socket buffer is cut there, which is still a
     * valid, shorter, client stream.  Then half-close, so the server sees
     * the end of the stream after the last byte. */
    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(sv[0], data + off, size - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    fcntl(sv[0], F_SETFL, fl);
    shutdown(sv[0], SHUT_WR);
    pthread_t t;
    int client = sv[0];
    pthread_create(&t, NULL, drain, &client);
    http_handle_fd(sv[1]);            /* closes sv[1] */
    pthread_join(t, NULL);
    close(sv[0]);
    return 0;
}
