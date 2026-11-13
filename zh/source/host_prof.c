/* host_prof.c —— 采样型 CPU 画像（2026-11-06）
 *
 * 目的：回答"启动的 106 秒到底花在哪些函数里"。
 *
 * 做法：SIGPROF + ITIMER_PROF 按固定频率采样当前 PC，
 * 只把 PC 追加进一个静态数组（信号处理里绝不做任何非异步安全的事），
 * 退出时把 PC 列表写成文本，由外部用 nm 离线解析成函数名。
 *
 * 用法：
 *     PROF=1 ./isaac_final          采样到 PROF_HZ（默认 500Hz）
 *     PROF_SECS=N                   只采样前 N 秒（默认 200）
 * 输出：
 *     prof_pc.txt   每行一个十六进制 PC
 *     prof_meta.txt 采样数 / 频率 / 进程内符号基址
 *
 * 为什么不直接在信号里解析符号：dladdr 不是 async-signal-safe，
 * 在 handler 里调用可能死锁（malloc 锁）。宁可事后再算。
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
static uintptr_t prof_lr[PROF_MAX];   /* 调用者返回地址（x30），用来定位"谁调了它" */
static volatile unsigned prof_n;
static volatile int prof_on;
static uintptr_t prof_base;          /* 用于把 PIE 地址换算成符号偏移 */
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
        /* ARM64 的 x30 在 ucontext 里是 regs[30]。读它完全安全
         * （不遍历栈、不调用任何库），却能回答"这个热点被谁调用"。 */
        prof_lr[n] = (uintptr_t)u->uc_mcontext.regs[30];
        prof_n = n + 1u;             /* 单写者，无需原子 */
    } else {
        prof_on = 0;                 /* 满了就停，不再进 handler 干活 */
    }
}

/* 供外部查询：已经采了多少 */
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
    /* 同时 dump maps：离线解析时用它判断 PC 落在哪个 .so */
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

/* 后台看门狗：到点或收到 TERM 就落盘。
 * 游戏的主循环永远不会返回，所以不能指望 host_prof_stop 被调用。 */
void host_prof_stop(void);
static void *prof_watchdog(void *arg)
{
    int secs = (int)(intptr_t)arg;
    for (int i = 0; i < secs * 4; i++) {
        usleep(250000);
        if (!prof_on && prof_n) break;      /* 采样已停（满了） */
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
