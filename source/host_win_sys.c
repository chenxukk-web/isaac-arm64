/* Android host layer -- Windows system-service imports
 *
 * Covers: ole32 (COM init) / WINMM (timers) / ADVAPI32 (privileges) /
 *         SHELL32 (file ops) / dbghelp (symbolication) / bcrypt (randomness)
 *
 * Every function's parameter byte count was measured by disassembling the call
 * sites; the RVA is noted in the comment. Rule: number of consecutive pushes
 * before the call x 4 (stdcall, callee cleans up).
 *
 * Semantic principle: these functions have well-defined return values for "the
 * feature is unavailable on PC", and we reproduce that value instead of
 * returning 0 and pretending success.
 *   . CoInitialize/CoInitializeEx -> S_OK. COM can always be implicitly
 *     initialized on Windows, and the game then calls steam_api, which does not
 *     depend on real COM objects.
 *   . timeGetTime -> a real millisecond count (host monotonic clock). This one
 *     must be genuine: the game uses it for frame timing and animation, and
 *     faking it makes animations skip.
 *   . OpenProcessToken/AdjustTokenPrivileges -> failure. An ordinary user
 *     process has no SeLockMemoryPrivilege, so failure is the true behaviour.
 *   . SymInitialize -> TRUE, SymFromAddr/SymGetLineFromAddr -> FALSE. That is
 *     exactly what dbghelp returns with no PDB present; the game merely fails to
 *     get symbol names.
 *   . SHFileOperationA -> success code 0 (and it really does delete/move files,
 *     see below).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}
static char *G(uint32_t a) { return (char *)(uintptr_t)a; }

/* ================= ole32 ================= */
/* CoInitialize(pvReserved)  RVA 0x88bcdd：push 1 */
static void h_CoInitialize(CPU *c)
{
    (void)A(c, 0);
    c->eax = 0;          /* S_OK */
    R(c, 4);
}
/* CoInitializeEx(pvReserved, dwCoInit)  RVA 0x99ca29：push 2 */
static void h_CoInitializeEx(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    /* S_OK. Returning RPC_E_CHANGED_MODE makes the game retry as MTA; we
     * support both (essentially doing nothing), so hand back S_OK and let it
     * take the first path. */
    c->eax = 0;
    R(c, 8);
}
/* CoUninitialize()  RVA 0x88bec5：push 0 */
static void h_CoUninitialize(CPU *c) { R(c, 0); }

/* ================= WINMM ================= */
/* timeGetTime()  RVA 0x408f68 (334 call sites): push 0 -> DWORD milliseconds
 * Must be real: the game derives frame intervals and animation phase from it.
 *
 * ★ 2026-11-04: added TIME_SCALE=<N> speed-up (N may be fractional).
 *
 * Why it is needed: the opening cutscene advances on real timeGetTime, about
 * 144 seconds total (anm2 at 30fps, f0 -> f4332). Every verification meant
 * waiting two and a half minutes; a debugging round of seven or eight runs is
 * twenty minutes, all spent "waiting for the animation".
 *
 * How: return (now - t0) * scale + t0. To the game, time simply moves faster;
 * frame intervals, animation phase and timer semantics all stay consistent, so
 * there is no "time going backwards". Scaling affects only the clock the game
 * sees, not host sleep, so CPU usage is unchanged.
 *
 * Usage: TIME_SCALE=10 finishes the 144-second cutscene in about 15 seconds.
 * For debugging observation only; do not set it for normal runs. */
static void h_timeGetTime(CPU *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
    static double scale = 0.0;
    static uint64_t t0 = 0;
    if (scale == 0.0) {
        const char *e = getenv("TIME_SCALE");
        scale = e ? atof(e) : 1.0;
        if (scale <= 0.0) scale = 1.0;
        t0 = ms;
    }
    if (scale != 1.0) {
        uint64_t out = t0 + (uint64_t)((double)(ms - t0) * scale);
        c->eax = (uint32_t)out;
    } else {
        c->eax = (uint32_t)ms;
    }
    R(c, 0);
}
/* timeBeginPeriod(uPeriod)  RVA 0x998d40: push 1
 * Android has no concept of timer resolution; accepting the request is enough
 * (return TIMERR_NOERROR=0). */
static void h_timeBeginPeriod(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* timeEndPeriod(uPeriod)  RVA 0x998eee：push 1 */
static void h_timeEndPeriod(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* timeGetDevCaps(ptc, cbtc)  RVA 0x998d34: push 2
 * TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; } -- host scheduling granularity
 * is 1ms. */
static void h_timeGetDevCaps(CPU *c)
{
    uint32_t ptc = A(c, 0), cbtc = A(c, 1);
    if (ptc && cbtc >= 8) {
        st32(ptc + 0, 1);      /* wPeriodMin */
        st32(ptc + 4, 1000);   /* wPeriodMax */
    }
    c->eax = 0;                /* TIMERR_NOERROR */
    R(c, 8);
}

/* ================= ADVAPI32 ================= */
/* OpenProcessToken(h, access, &token)  RVA 0x90a8b0: push 3
 * An ordinary process cannot obtain an adjustable token, so return the real
 * failure (the game then skips it). */
static void h_OpenProcessToken(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    uint32_t out = A(c, 2);
    if (out) st32(out, 0);
    c->eax = 0;                /* FALSE */
    R(c, 12);
}
/* LookupPrivilegeValueA(sys, name, &luid)  RVA 0x9e7905：push 3 */
static void h_LookupPrivilegeValueA(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    uint32_t luid = A(c, 2);
    if (luid) { st32(luid, 0); st32(luid + 4, 0); }
    c->eax = 0;                /* FALSE */
    R(c, 12);
}
/* AdjustTokenPrivileges(h, dis, new, len, prev, &ret)  RVA 0x9e792a：push 6 */
static void h_AdjustTokenPrivileges(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    (void)A(c, 3); (void)A(c, 4);
    uint32_t ret = A(c, 5);
    if (ret) st32(ret, 0);
    c->eax = 0;                /* FALSE */
    R(c, 24);
}

/* ================= SHELL32 =================
 * SHFileOperationA(lpFileOp)  RVA 0x87572c: push 1
 *
 * This deletes files (the game cleans up temp files). It must really do the
 * deletion -- otherwise temp files pile up, and the game believes it succeeded.
 * SHFILEOPSTRUCTA: hwnd(0) wFunc(4) pFrom(8) pTo(12) fFlags(16)
 *                  fAnyOperationsAborted(20) hNameMappings(24) lpszProgressTitle(28)
 * pFrom is a **double-\0 terminated** multi-string. */
static void h_SHFileOperationA(CPU *c)
{
    uint32_t fo = A(c, 0);
    if (!fo) { c->eax = 0x71; R(c, 4); return; }   /* DE_INVALIDFILES */

    uint32_t wFunc = ld32(fo + 4);
    uint32_t pFrom = ld32(fo + 8);
    int rc = 0;

    if (pFrom) {
        /* Walk the double-\0 separated paths one at a time */
        const char *p = (const char *)(uintptr_t)pFrom;
        while (*p) {
            size_t len = strlen(p);
            switch (wFunc) {
            case 3:   /* FO_DELETE */
                if (remove(p) != 0 && errno != ENOENT) rc = 0x7C;  /* DE_ACCESSDENIEDSRC */
                else fprintf(stderr, "[shell] delete \"%s\"\n", p);
                break;
            case 1:   /* FO_MOVE */
            case 2:   /* FO_COPY */
            case 4:   /* FO_RENAME */
                fprintf(stderr, "[shell] wFunc=%u \"%s\" (actual move not implemented)\n", wFunc, p);
                break;
            default:
                break;
            }
            p += len + 1;
        }
    }
    uint32_t aborted = ld32(fo + 20);
    (void)aborted;
    c->eax = (uint32_t)rc;
    R(c, 4);
}

/* ================= dbghelp ================= */
/* SymSetOptions(o)  RVA 0x87ba93：push 1 */
static void h_SymSetOptions(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* SymInitialize(h, path, invade)  RVA 0x87baa4: push 3
 * No PDB, but still return TRUE -- that is the normal result when the symbol
 * server is empty. */
static void h_SymInitialize(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 1;                /* TRUE */
    R(c, 12);
}
/* SymFromAddr(hProcess, Address64, &Displacement, pSymbol)
 * Args: +0 hProcess(4), +4/+8 Address(8), +12 &Displacement(4), +16 pSymbol(4)
 * 20 bytes total. Returning FALSE with no PDB is the real behaviour. */
static void h_SymFromAddr(CPU *c)
{
    (void)A(c, 0);                 /* hProcess */
    (void)A(c, 1); (void)A(c, 2);  /* Address low/high */
    uint32_t disp = A(c, 3);
    if (disp) st32(disp, 0);
    (void)A(c, 4);                 /* pSymbol */
    c->eax = 0;                    /* FALSE: symbol not found */
    R(c, 20);
}
/* SymGetLineFromAddr(h, addr, &disp, line)  RVA 0x87bc18：push 4 */
static void h_SymGetLineFromAddr(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 0;                /* FALSE */
    R(c, 16);
}

/* ================= bcrypt ================= */
/* BCryptGenRandom(alg, buf, len, flags) */
static void h_BCryptGenRandom(CPU *c)
{
    (void)A(c, 0);
    uint32_t buf = A(c, 1), len = A(c, 2);
    (void)A(c, 3);
    if (buf && len) {
        FILE *f = fopen("/dev/urandom", "rb");
        size_t got = 0;
        if (f) { got = fread(G(buf), 1, len, f); fclose(f); }
        for (uint32_t i = (uint32_t)got; i < len; i++)
            st8(buf + i, (uint8_t)(rand() & 0xFF));
    }
    c->eax = 0;                /* STATUS_SUCCESS */
    R(c, 16);
}

/* ================= dispatch ================= */
typedef struct { const char *name; void (*fn)(CPU *); } sys_entry;

static const sys_entry s_sys[] = {
    { "ole32.dll!CoInitialize",          h_CoInitialize },
    { "ole32.dll!CoInitializeEx",        h_CoInitializeEx },
    { "ole32.dll!CoUninitialize",        h_CoUninitialize },
    { "WINMM.dll!timeGetTime",           h_timeGetTime },
    { "WINMM.dll!timeBeginPeriod",       h_timeBeginPeriod },
    { "WINMM.dll!timeEndPeriod",         h_timeEndPeriod },
    { "WINMM.dll!timeGetDevCaps",        h_timeGetDevCaps },
    { "ADVAPI32.dll!OpenProcessToken",   h_OpenProcessToken },
    { "ADVAPI32.dll!LookupPrivilegeValueA", h_LookupPrivilegeValueA },
    { "ADVAPI32.dll!AdjustTokenPrivileges", h_AdjustTokenPrivileges },
    { "SHELL32.dll!SHFileOperationA",    h_SHFileOperationA },
    { "dbghelp.dll!SymSetOptions",       h_SymSetOptions },
    { "dbghelp.dll!SymInitialize",       h_SymInitialize },
    { "dbghelp.dll!SymFromAddr",         h_SymFromAddr },
    { "dbghelp.dll!SymGetLineFromAddr",  h_SymGetLineFromAddr },
    { "bcrypt.dll!BCryptGenRandom",      h_BCryptGenRandom },
};

int host_sys_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof s_sys / sizeof s_sys[0]; i++) {
        if (strcmp(s_sys[i].name, name) == 0) { s_sys[i].fn(c); return 1; }
    }
    return 0;
}
int host_sys_count(void) { return (int)(sizeof s_sys / sizeof s_sys[0]); }
