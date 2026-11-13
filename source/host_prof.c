/* host_prof.c -- sampling CPU profiler (2026-11-06)
 *
 * Purpose: answer "where did the 106-second startup actually go".
 *
 * Approach: SIGPROF + ITIMER_PROF samples the current PC at a fixed rate,
 * appending only the PC into a static array (a handler must do nothing that is
 * not async-signal-safe); the list is written out on exit for offline nm lookup.
 *
 * Usage:
 *     PROF=1 ./isaac_final          sample at PROF_HZ (default 500Hz)
 *     PROF_SECS=N                   sample only the first N seconds (default 200)
 * Output:
 *     prof_pc.txt   one hexadecimal PC per line
 *     prof_meta.txt sample count / rate / in-process symbol base
 *
 * Why not resolve symbols inside the handler: dladdr is not async-signal-safe,
 * and calling it there can deadlock (malloc lock). Better to do it afterwards.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define PROF_MAX 400000u

static uintptr_t prof_pc[PROF_MAX];
static uintptr_t prof_lr[PROF_MAX];   /* caller return address (x30), locates "who called it" */
static volatile unsigned prof_n;
static volatile int prof_on;
static uintptr_t prof_base;          /* converts a PIE address to a symbol offset */
static struct timespec prof_t0;

static void prof_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    if (!prof_on) return;
    ucontext_t *u = (ucontext_t *)uc;
    uintptr_t pc = (uintptr_t)u->uc_mcontext.pc;
    unsigned n = prof_n;
    if (n < PROF_MAX) {
        prof_pc[n] = pc;
        /* ARM64's x30 lives in ucontext as regs[30]. Reading it is completely safe
         * (no stack walk, no library calls), yet answers "who called this hot spot". */
        prof_lr[n] = (uintptr_t)u->uc_mcontext.regs[30];
        prof_n = n + 1u;             /* single writer, no atomics needed */
    } else {
        prof_on = 0;                 /* stop when full, do no more work in the handler */
    }
}

/* For external queries: how many samples have been taken */
unsigned host_prof_count(void) { return prof_n; }

void host_prof_dump(void)
{
    if (!prof_n) return;

    FILE *f = fopen("prof_pc.txt", "w");
    if (f) {
        for (unsigned i = 0; i < prof_n; i++)
            fprintf(f, "%llx %llx\n", (unsigned long long)prof_pc[i],
                    (unsigned long long)prof_lr[i]);
        fclose(f);
    }
    FILE *m = fopen("prof_meta.txt", "w");
    if (m) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double secs = (double)(ts.tv_sec - prof_t0.tv_sec)
                    + (double)(ts.tv_nsec - prof_t0.tv_nsec) / 1e9;
        fprintf(m, "samples=%u\nbase=%llx\nsecs=%.3f\n",
                prof_n, (unsigned long long)prof_base, secs);
        fclose(m);
    }
    /* Also dump maps: used offline to decide which .so a PC falls in */
    FILE *mp = fopen("prof_maps.txt", "w");
    if (mp) {
        FILE *m2 = fopen("/proc/self/maps", "r");
        if (m2) {
            char line[512];
            while (fgets(line, sizeof line, m2)) fputs(line, mp);
            fclose(m2);
        }
        fclose(mp);
    }
    fprintf(stderr, "[prof] wrote %u PC samples -> prof_pc.txt\n", prof_n);
}

/* Background watchdog: writes out on timeout or TERM.
 * The main loop never returns, so host_prof_stop cannot be relied on. */
void host_prof_stop(void);
static void *prof_watchdog(void *arg)
{
    int secs = (int)(intptr_t)arg;
    for (int i = 0; i < secs * 4; i++) {
        usleep(250000);
        if (!prof_on && prof_n) break;      /* sampling stopped (buffer full) */
    }
    host_prof_stop();
    return NULL;
}

void host_prof_start(uintptr_t elf_base)
{
    prof_base = elf_base;
    clock_gettime(CLOCK_MONOTONIC, &prof_t0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);

    int hz = 500;
    const char *e = getenv("PROF_HZ");
    if (e) { int v = atoi(e); if (v >= 10 && v <= 5000) hz = v; }

    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = (long)(1000000 / hz);
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);

    prof_on = 1;
    fprintf(stderr, "[prof] sampling started at %d Hz\n", hz);

    int secs = 200;
    const char *se = getenv("PROF_SECS");
    if (se) { int v = atoi(se); if (v > 0 && v <= 3600) secs = v; }
    pthread_t t;
    if (pthread_create(&t, NULL, prof_watchdog, (void *)(intptr_t)secs) == 0)
        pthread_detach(t);
}

void host_prof_stop(void)
{
    if (!prof_on && !prof_n) return;
    prof_on = 0;
    struct itimerval it;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_PROF, &it, NULL);
    host_prof_dump();
}
