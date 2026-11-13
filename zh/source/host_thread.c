/* host_thread.c —— _beginthreadex / 线程原语
 *
 * 《以撒》用 std::thread，落到 MSVC 的 `_beginthreadex`。RVA 0x598b56 的
 * 调用点压了 6 个参数（cdecl，调用方 add esp,0x1c 清理）：
 *   +0 security  +4 stack_size  +8 start_address
 *   +12 arglist  +16 initflag   +20 &thrdaddr
 *
 * 线程入口 RVA 0x5a76d0 是 MSVC 的 thread proxy：它从 arglist 里取虚表和
 * 参数、调真正的线程函数、最后调一次 [`thrdaddr`] 那边的完成通知。所以
 * 必须真的执行 guest 代码，不能在调用线程里假装跑完 —— 后面会有 join。
 *
 * ---------------------------------------------------------------------------
 * 每个 guest 线程需要三样宿主侧东西，都不能共享：
 *   ① 独立的 CPU 上下文（寄存器 + 栈边界）—— guest_stack_bind 本来就是
 *      按 CPU 绑定的（stack_owner == c），所以这里天然支持多实例。
 *   ② 独立的 guest 栈，且必须落在低 4GB（guest 的 esp 是 uint32_t）。
 *   ③ 线程自己的 pthread。
 *
 * 已知局限（诚实标注，不假装完整）：
 *   · TIB / 静态 TLS 块目前是全局共享的。Windows 的 fs:[...] 是每线程的，
 *     共享意味着两个线程写同一个 TLS 槽会互相踩。如果后续出现「线程间
 *     数据串味」的症状，先查这里。
 *   · g_cur_cpu_diag 是全局诊断指针，多线程下只反映最后一个进入的 CPU。
 *   · 线程句柄是合成值，WaitForSingleObject 不真的阻塞等线程退出。
 *   这三点都不影响「程序能继续跑下去」，但要跑稳必须逐个补。
 * ---------------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }

/* ---- guest 线程栈：低 4GB，每个线程 4 MB ---- */
#define THR_STACK_BASE  0x41000000u
#define THR_STACK_SIZE  (4u * 1024u * 1024u)
#define THR_STACK_MAX   16u
#define THR_MARGIN      4096u

static uint32_t g_thr_slot_used[THR_STACK_MAX];

static uint32_t thr_stack_alloc(void)
{
    for (unsigned i = 0; i < THR_STACK_MAX; i++) {
        if (g_thr_slot_used[i]) continue;
        uint32_t base = THR_STACK_BASE + i * THR_STACK_SIZE;
        void *p = mmap((void *)(uintptr_t)base, THR_STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)(uintptr_t)base) continue;
        g_thr_slot_used[i] = 1;
        return base;
    }
    return 0;
}

/* ---- 线程句柄（合成值；guest 只会拿它去 Wait/Close） ---- */
#define THR_HANDLE_BASE 0x00010000u
static uint32_t g_thr_handle_next = THR_HANDLE_BASE + 0x100u;

typedef struct {
    uint32_t      start;      /* guest 的 start_address（RVA） */
    uint32_t      arg;        /* guest 的 arglist 指针 */
    uint32_t      stack;      /* guest 栈基址 */
    uint32_t      handle;     /* 返回给 guest 的句柄 */
    pthread_t     tid;
} thr_rec;

static thr_rec g_thr_recs[THR_STACK_MAX];
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;

static void *thr_main(void *p)
{
    thr_rec *t = (thr_rec *)p;

    CPU c;
    guest_cpu_init(&c);
    if (guest_stack_bind(&c, t->stack + THR_MARGIN,
                         t->stack + THR_STACK_SIZE - THR_MARGIN) != 0) {
        fprintf(stderr, "[thread] ✗ stack binding failed 0x%08x\n", t->stack);
        return NULL;
    }

    /* 按 cdecl 摆好调用帧：call start(arg) 的栈布局是
     *   [esp]   = 返回地址（这里是假的 —— start 的 ret 只是 GPOP 丢弃）
     *   [esp+4] = arg
     * 所以 esp 要先减 8。 */
    c.esp = c.stack_ceiling - 64u;
    c.esp -= 8u;
    st32(c.esp, 0u);          /* 假返回地址 */
    st32(c.esp + 4u, t->arg);

    fprintf(stderr, "[thread] thread started start=%08x arg=%08x stack %08x..%08x\n",
            t->start, t->arg, t->stack + THR_MARGIN,
            t->stack + THR_STACK_SIZE - THR_MARGIN);

    /* ★ start 已经是绝对的 guest VA（0x3c5a76d0），不能再加 GUEST_IMAGE_BASE。
     * 加了会变成 0x605a76d0 —— 实测就是 "FAULT at 605a76d0"。 */
    guest_call(&c, t->start);

    if (c.fault)
        fprintf(stderr, "[thread] fault inside thread: %s\n", c.fault);
    else
        fprintf(stderr, "[thread] thread exited normally (eax=%08x)\n", c.eax);
    return NULL;
}

/* `uintptr_t _beginthreadex(void*, unsigned, unsigned(__stdcall*)(void*),
 *                          void*, unsigned, unsigned*)` —— cdecl, 24 字节 */
static void h_beginthreadex(CPU *c)
{
    uint32_t security = A(c, 0);
    uint32_t stack_sz = A(c, 1);
    uint32_t start    = A(c, 2);
    uint32_t arglist  = A(c, 3);
    uint32_t initflag = A(c, 4);
    uint32_t pthr     = A(c, 5);
    (void)security; (void)stack_sz; (void)initflag;

    if (!start) {
        fprintf(stderr, "[thread] _beginthreadex start_address is null\n");
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    pthread_mutex_lock(&g_thr_mu);

    thr_rec *rec = NULL;
    for (unsigned i = 0; i < THR_STACK_MAX; i++)
        if (!g_thr_slot_used[i] || g_thr_recs[i].start == 0) { rec = &g_thr_recs[i]; break; }
    /* 上一轮已退出的槽可以复用：用 tid 是否有效判断 */
    if (!rec) {
        for (unsigned i = 0; i < THR_STACK_MAX; i++)
            if (g_thr_recs[i].tid == 0) { rec = &g_thr_recs[i]; break; }
    }

    if (!rec) {
        pthread_mutex_unlock(&g_thr_mu);
        fprintf(stderr, "[thread] ✗ thread table full\n");
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    uint32_t stk = rec->stack;
    if (!stk) {
        stk = thr_stack_alloc();
        if (!stk) {
            pthread_mutex_unlock(&g_thr_mu);
            fprintf(stderr, "[thread] ✗ cannot allocate a guest thread stack\n");
            c->eax = 0;
            cdecl_ret(c);
            return;
        }
        rec->stack = stk;
    }

    rec->start  = start;
    rec->arg    = arglist;
    rec->handle = g_thr_handle_next++;

    int rc = pthread_create(&rec->tid, NULL, thr_main, rec);
    if (rc != 0) {
        pthread_mutex_unlock(&g_thr_mu);
        fprintf(stderr, "[thread] ✗ pthread_create failed rc=%d\n", rc);
        rec->tid = 0;
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    /* MSVC 语义：成功时把线程 id 写进 *thrdaddr */
    if (pthr) st32(pthr, (uint32_t)(uintptr_t)rec->tid);

    uint32_t h = rec->handle;
    pthread_mutex_unlock(&g_thr_mu);

    fprintf(stderr, "[thread] _beginthreadex(start=%08x, arg=%08x) -> handle %08x\n",
            start, arglist, h);
    c->eax = h;
    cdecl_ret(c);
}

int host_thread_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    const char *n = strchr(name, '!');
    n = n ? n + 1 : name;
    if (!strcmp(n, "_beginthreadex")) { h_beginthreadex(c); return 1; }
    return 0;
}
int host_thread_count(void) { return 1; }
