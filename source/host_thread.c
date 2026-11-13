/* host_thread.c -- _beginthreadex / thread primitives
 *
 * Isaac uses std::thread, which lands on MSVC's `_beginthreadex`. The call site
 * at RVA 0x598b56 pushes 6 arguments (cdecl; the caller cleans with
 * `add esp,0x1c`):
 *   +0 security  +4 stack_size  +8 start_address
 *   +12 arglist  +16 initflag   +20 &thrdaddr
 *
 * The thread entry at RVA 0x5a76d0 is MSVC's thread proxy: it takes the vtable
 * and arguments out of arglist, calls the real thread function, and finally
 * fires the completion notification through [`thrdaddr`]. So guest code must
 * really execute -- we cannot pretend to finish on the calling thread, because
 * a join follows.
 *
 * ---------------------------------------------------------------------------
 * Each guest thread needs three host-side things, none of them shareable:
 *   1) An independent CPU context (registers + stack bounds) -- guest_stack_bind
 *      is already bound per CPU (stack_owner == c), so multiple instances work
 *      naturally.
 *   2) An independent guest stack, which must live in the low 4GB (the guest's
 *      esp is uint32_t).
 *   3) Its own pthread.
 *
 * Known limitations (stated honestly, not glossed over):
 *   . TIB / static TLS blocks are currently shared globally. Windows' fs:[...]
 *     is per-thread; sharing means two threads writing the same TLS slot
 *     corrupt each other. If you later see "data bleeding between threads",
 *     look here first.
 *   . g_cur_cpu_diag is a global diagnostic pointer; under multithreading it
 *     only reflects the last CPU to enter.
 *   . Thread handles are synthetic values; WaitForSingleObject does not actually
 *     block waiting for thread exit.
 *   None of these stop the program from making progress, but all three must be
 *   fixed before it is truly stable.
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

/* ---- guest thread stack: low 4GB, 4 MB per thread ---- */
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

/* ---- thread handles (synthetic; the guest only uses them for Wait/Close) ---- */
#define THR_HANDLE_BASE 0x00010000u
static uint32_t g_thr_handle_next = THR_HANDLE_BASE + 0x100u;

typedef struct {
    uint32_t      start;      /* guest start_address (RVA) */
    uint32_t      arg;        /* guest arglist pointer */
    uint32_t      stack;      /* guest stack base */
    uint32_t      handle;     /* handle returned to the guest */
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

    /* Lay out the call frame per cdecl: the stack layout for call start(arg) is
     *   [esp]   = return address (fake here -- start's ret is just GPOP-discarded)
     *   [esp+4] = arg
     * so esp must be decremented by 8 first. */
    c.esp = c.stack_ceiling - 64u;
    c.esp -= 8u;
    st32(c.esp, 0u);          /* fake return address */
    st32(c.esp + 4u, t->arg);

    fprintf(stderr, "[thread] thread started start=%08x arg=%08x stack %08x..%08x\n",
            t->start, t->arg, t->stack + THR_MARGIN,
            t->stack + THR_STACK_SIZE - THR_MARGIN);

    /* ★ start is already an absolute guest VA (0x3c5a76d0); do not add GUEST_IMAGE_BASE.
     * Doing so yields 0x605a76d0 -- measured exactly as "FAULT at 605a76d0". */
    guest_call(&c, t->start);

    if (c.fault)
        fprintf(stderr, "[thread] fault inside thread: %s\n", c.fault);
    else
        fprintf(stderr, "[thread] thread exited normally (eax=%08x)\n", c.eax);
    return NULL;
}

/* `uintptr_t _beginthreadex(void*, unsigned, unsigned(__stdcall*)(void*),
 *                          void*, unsigned, unsigned*)` -- cdecl, 24 bytes */
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
/* A slot whose thread already exited can be reused: judge by whether the tid is valid */
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

/* MSVC semantics: on success write the thread id into *thrdaddr */
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
