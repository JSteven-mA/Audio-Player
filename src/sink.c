#define _GNU_SOURCE
#include "sink.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void sink_init(sink_t *s, sink_kind_t kind)
{
    memset(s, 0, sizeof *s);
    s->kind = kind; s->fd = -1;
}

static void reap(sink_t *s, int drain)
{
    if (s->fd >= 0) {
        if (!drain && s->pid > 0) kill(s->pid, SIGTERM);
        close(s->fd);
        s->fd = -1;
    }
    if (s->pid > 0) {
        while (waitpid(s->pid, NULL, 0) < 0 && errno == EINTR) ;
        s->pid = 0;
    }
}

static void degrade(sink_t *s)
{
    if (!s->warned) {
        fprintf(stderr, "[sink] reproductor externo no disponible: usando sumidero 'null' (sin sonido)\n");
        s->warned = 1;
    }
    reap(s, 0);
    s->kind = SINK_NULL;
}

int sink_open(sink_t *s, int rate, int ch)
{
    s->rate = rate; s->ch = ch; s->fd = -1; s->pid = 0;
    clock_gettime(CLOCK_MONOTONIC, &s->next);
    if (s->kind == SINK_NULL) return 0;

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) { degrade(s); return 0; }
    char rs[16], cs[16], rarg[32], carg[32];
    snprintf(rs, sizeof rs, "%d", rate);  snprintf(cs, sizeof cs, "%d", ch);
    snprintf(rarg, sizeof rarg, "--rate=%d", rate); snprintf(carg, sizeof carg, "--channels=%d", ch);

    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); degrade(s); return 0; }
    if (pid == 0) {
        dup2(p[0], STDIN_FILENO);
        if (s->kind == SINK_APLAY)
            execlp("aplay", "aplay", "-q", "-B", "200000", "-t", "raw", "-f", "S16_LE",
                   "-r", rs, "-c", cs, "-", (char *)NULL);
        else
            execlp("paplay", "paplay", "--raw", "--format=s16le", rarg, carg, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    s->fd = p[1]; s->pid = pid;
    return 0;
}

static void pace(sink_t *s, size_t len)
{
    uint64_t ns = (uint64_t)len * 1000000000ULL / ((uint64_t)s->rate * (uint64_t)s->ch * 2);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->next.tv_sec < now.tv_sec ||
        (s->next.tv_sec == now.tv_sec && s->next.tv_nsec < now.tv_nsec))
        s->next = now;                                   /* no "ponerse al día" tras pausas */
    s->next.tv_nsec += (long)(ns % 1000000000ULL);
    s->next.tv_sec  += (time_t)(ns / 1000000000ULL);
    if (s->next.tv_nsec >= 1000000000L) { s->next.tv_nsec -= 1000000000L; s->next.tv_sec++; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &s->next, NULL) == EINTR) ;
}

int sink_write(sink_t *s, const uint8_t *buf, size_t len)
{
    if (s->kind != SINK_NULL && s->fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(s->fd, buf + off, len - off);   /* bloquea (sin espera activa) si el pipe está lleno */
            if (n < 0) {
                if (errno == EINTR) continue;
                degrade(s);
                break;
            }
            off += (size_t)n;
        }
        if (off == len) return 0;
    }
    pace(s, len);
    return 0;
}

void sink_close(sink_t *s, int drain)
{
    reap(s, drain);
    s->rate = 0;
}
