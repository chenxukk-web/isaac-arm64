/* Android host layer -- steam_api.dll (11 imports)
 *
 * ★ This is NOT a fake stub.
 *
 * The criterion comes from PC behaviour: on a machine with no Steam installed
 * and no steam_appid.txt, SteamAPI_Init() returns false and the game continues
 * down its offline degraded path. So what is implemented here is the correct
 * semantics for the real scenario "no Steam runtime present", not "pretend it
 * worked". Pretending success would be the bug: the game would believe
 * achievements, cloud saves and friends are available, then fail in all sorts
 * of ways from a half-working state.
 *
 * Calling conventions were measured one by one by disassembling the call sites,
 * not copied from the SDK headers (measurement and headers actually disagree on
 * RegisterCallback). Rule: an `add esp,N` immediately after the call means
 * cdecl, where N is the total argument bytes; no `add esp` means the callee
 * cleans up (stdcall), and the total is the number of pushes before the call.
 *
 *   function                          conv       arg bytes  evidence (RVA 0x)
 *   SteamAPI_Init                    stdcall    0        598c80 then test al,al
 *   SteamAPI_Shutdown                stdcall    0        598e91 then mov esi,...
 *   SteamAPI_RunCallbacks            stdcall    0        598d9e then mov ecx,...
 *   SteamAPI_GetHSteamPipe           stdcall    0        18a32  then test eax,eax
 *   SteamAPI_GetHSteamUser           stdcall    0        18a55  then mov ebx,eax
 *   SteamAPI_RegisterCallback        cdecl      8        1dea   thunk stack balanced 12/12
 *   SteamAPI_UnregisterCallback      cdecl      4        605a28 then add esp,4
 *   SteamAPI_RegisterCallResult      cdecl      12       3f808b then add esp,0xc
 *   SteamAPI_UnregisterCallResult    cdecl      12       3f8055 then add esp,0xc
 *   SteamInternal_CreateInterface    cdecl      4        18a72  then add esp,4
 *   SteamInternal_ContextInit        cdecl      4        25b747 then add esp,4
 *
 * There is no steamAppId / steam_appid.txt either -- if the game cannot read it,
 * it will not contact Steam.
 *
 * Low-4GB constraint (a trap this project hit 4 times): SteamInternal_ContextInit
 * must return a pointer the guest can dereference. The call site is
 *      push 0xbaa3c8 / call ContextInit / add esp,4 / cmp [eax+0x24],0
 * Returning 0 crashes immediately on reading [0x24]. So return a zero-filled
 * low-4GB structure: [eax+0x24] == 0 is exactly "interface unavailable", and the
 * game skips the Steam path.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"

/* ---- calling-convention helpers (same as host_android.c) ---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
/* stdcall: pop the return address + clear n bytes of arguments */
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}
/* cdecl: pop only the return address; the caller clears arguments with add esp,N */
static void Rc(CPU *__restrict c)
{
    (void)gpop(c);
}

/* ---- ContextInit return block (low 4GB, zero-filled = no interface) ---- */
#define STEAM_CTX_ADDR 0x3A000000u
#define STEAM_CTX_SIZE 0x1000u

static uint32_t steam_ctx_addr;
static int      steam_ctx_state;   /* 0=not attempted 1=ready -1=failed */

static uint32_t steam_ctx(void)
{
    if (steam_ctx_state == 1) return steam_ctx_addr;
    if (steam_ctx_state == -1) return 0;
    void *p = mmap((void *)(uintptr_t)STEAM_CTX_ADDR, STEAM_CTX_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)STEAM_CTX_ADDR) {
        fprintf(stderr,
                "[steam] cannot establish the context block at 0x%08x (see errno above);"
                "SteamInternal_ContextInit cannot hand out a low-4GB pointer\n", STEAM_CTX_ADDR);
        steam_ctx_state = -1;
        return 0;
    }
/* mmap ANONYMOUS is already all zero -- [ctx+0x24] == 0 means "interface unavailable" */
    steam_ctx_addr = STEAM_CTX_ADDR;
    steam_ctx_state = 1;
    return steam_ctx_addr;
}

/* ---- lifecycle ---- */
static void h_SteamAPI_Init(CPU *c)
{
/* No Steam runtime -> false. The game takes its offline path based on this. */
    c->eax = 0;
    fprintf(stderr, "[steam] SteamAPI_Init -> false (no Steam runtime; offline mode)\n");
    R(c, 0);
}

static void h_SteamAPI_Shutdown(CPU *c)
{
/* Never initialised, so there is nothing to shut down. */
    R(c, 0);
}

static void h_SteamAPI_RunCallbacks(CPU *c)
{
/* With no Steam there are no callbacks to deliver. Doing nothing is the only correct behaviour here. */
    R(c, 0);
}

/* ---- handles: the invalid value is 0 ---- */
static void h_SteamAPI_GetHSteamPipe(CPU *c) { c->eax = 0; R(c, 0); }
static void h_SteamAPI_GetHSteamUser(CPU *c) { c->eax = 0; R(c, 0); }

/* Android host layer -- steam_api.dll (11 imports)
 *
 * ★ This is NOT a fake stub.
 *
 * The criterion comes from PC behaviour: on a machine with no Steam installed
 * and no steam_appid.txt, SteamAPI_Init() returns false and the game continues
 * down its offline degraded path. So what is implemented here is the correct
 * semantics for the real scenario "no Steam runtime present", not "pretend it
 * worked". Pretending success would be the bug: the game would believe
 * achievements, cloud saves and friends are available, then fail in all sorts
 * of ways from a half-working state.
 *
 * Calling conventions were measured one by one by disassembling the call sites,
 * not copied from the SDK headers (measurement and headers actually disagree on
 * RegisterCallback). Rule: an `add esp,N` immediately after the call means
 * cdecl, where N is the total argument bytes; no `add esp` means the callee
 * cleans up (stdcall), and the total is the number of pushes before the call.
 *
 *   function                          conv       arg bytes  evidence (RVA 0x)
 *   SteamAPI_Init                    stdcall    0        598c80 then test al,al
 *   SteamAPI_Shutdown                stdcall    0        598e91 then mov esi,...
 *   SteamAPI_RunCallbacks            stdcall    0        598d9e then mov ecx,...
 *   SteamAPI_GetHSteamPipe           stdcall    0        18a32  then test eax,eax
 *   SteamAPI_GetHSteamUser           stdcall    0        18a55  then mov ebx,eax
 *   SteamAPI_RegisterCallback        cdecl      8        1dea   thunk stack balanced 12/12
 *   SteamAPI_UnregisterCallback      cdecl      4        605a28 then add esp,4
 *   SteamAPI_RegisterCallResult      cdecl      12       3f808b then add esp,0xc
 *   SteamAPI_UnregisterCallResult    cdecl      12       3f8055 then add esp,0xc
 *   SteamInternal_CreateInterface    cdecl      4        18a72  then add esp,4
 *   SteamInternal_ContextInit        cdecl      4        25b747 then add esp,4
 *
 * There is no steamAppId / steam_appid.txt either -- if the game cannot read it,
 * it will not contact Steam.
 *
 * Low-4GB constraint (a trap this project hit 4 times): SteamInternal_ContextInit
 * must return a pointer the guest can dereference. The call site is
 *      push 0xbaa3c8 / call ContextInit / add esp,4 / cmp [eax+0x24],0
 * Returning 0 crashes immediately on reading [0x24]. So return a zero-filled
 * low-4GB structure: [eax+0x24] == 0 is exactly "interface unavailable", and the
 * game skips the Steam path.
 */
static void h_SteamAPI_RegisterCallback(CPU *c)
{
    (void)A(c, 0);   /* pCallback */
    (void)A(c, 1);   /* iCallback */
    Rc(c);           /* cdecl, 8 argument bytes cleared by the caller with add esp,8 */
}

static void h_SteamAPI_UnregisterCallback(CPU *c)
{
    (void)A(c, 0);
    Rc(c);           /* cdecl, 4 bytes */
}

static void h_SteamAPI_RegisterCallResult(CPU *c)
{
    (void)A(c, 0);   /* pCallback */
    (void)A(c, 1);   /* hAPICall low 32 bits */
    (void)A(c, 2);   /* hAPICall high 32 bits */
    Rc(c);           /* cdecl, 12 bytes */
}

static void h_SteamAPI_UnregisterCallResult(CPU *c)
{
    (void)A(c, 0);
    (void)A(c, 1);
    (void)A(c, 2);
    Rc(c);           /* cdecl, 12 bytes */
}

/* ---- interfaces ---- */
static void h_SteamInternal_CreateInterface(CPU *c)
{
    uint32_t ver = A(c, 0);
    const char *name = ver ? (const char *)(uintptr_t)ver : "(null)";
/* No Steam process, so no interface version can be obtained -> NULL. The call site checks the return value. */
    fprintf(stderr, "[steam] CreateInterface(\"%s\") -> NULL\n", name);
    c->eax = 0;
    Rc(c);           /* cdecl, 4 bytes */
}

static void h_SteamInternal_ContextInit(CPU *c)
{
    (void)A(c, 0);   /* pContextInitData -- real Steam writes into it; not needed here */
/* Must return a dereferenceable low-4GB pointer: [+0x24] == 0 means the interface is unavailable. */
    c->eax = steam_ctx();
    Rc(c);           /* cdecl, 4 bytes */
}

/* ---- dispatch ---- */
typedef struct { const char *name; void (*fn)(CPU *); } steam_entry;

static const steam_entry s_steam[] = {
    { "steam_api.dll!SteamAPI_Init",                  h_SteamAPI_Init },
    { "steam_api.dll!SteamAPI_Shutdown",              h_SteamAPI_Shutdown },
    { "steam_api.dll!SteamAPI_RunCallbacks",          h_SteamAPI_RunCallbacks },
    { "steam_api.dll!SteamAPI_GetHSteamPipe",         h_SteamAPI_GetHSteamPipe },
    { "steam_api.dll!SteamAPI_GetHSteamUser",         h_SteamAPI_GetHSteamUser },
    { "steam_api.dll!SteamAPI_RegisterCallback",      h_SteamAPI_RegisterCallback },
    { "steam_api.dll!SteamAPI_UnregisterCallback",    h_SteamAPI_UnregisterCallback },
    { "steam_api.dll!SteamAPI_RegisterCallResult",    h_SteamAPI_RegisterCallResult },
    { "steam_api.dll!SteamAPI_UnregisterCallResult",  h_SteamAPI_UnregisterCallResult },
    { "steam_api.dll!SteamInternal_CreateInterface",  h_SteamInternal_CreateInterface },
    { "steam_api.dll!SteamInternal_ContextInit",      h_SteamInternal_ContextInit },
};

/* Returns 1 = handled; 0 = not an import this layer owns */
int host_steam_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof s_steam / sizeof s_steam[0]; i++) {
        if (strcmp(s_steam[i].name, name) == 0) {
            s_steam[i].fn(c);
            return 1;
        }
    }
    return 0;
}

int host_steam_count(void)
{
    return (int)(sizeof s_steam / sizeof s_steam[0]);
}
