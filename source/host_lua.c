/* Lua 5.3.3 forwarding layer on Android
 *
 * All 67 Lua5.3.3r.dll imports of the game forward to the genuinely compiled Lua 5.3.3.
 * No emulation, no rewrite -- the upstream implementation, behaving exactly as on PC.
 *
 * * All memory goes through the low 4GB (guest_low_realloc). This is a recurring constraint here:
 *   any pointer handed to the guest must be representable as uint32_t.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }

/* 64-bit arguments (lua_Integer / size_t) occupy 8 bytes on the stack */
static uint64_t A64(CPU *__restrict c, int i)
{
    uint32_t off = c->esp + 4u + (uint32_t)i * 4u;
    uint32_t lo = ld32(guest_stack_address(c, off, 4u, 0u));
    uint32_t hi = ld32(guest_stack_address(c, off + 4u, 4u, 0u));
    return ((uint64_t)hi << 32) | lo;
}

/* ---------------- low-4GB allocator (used internally by Lua) ---------------- */
#define LUA_HEAP_BASE 0xB0000000UL
#define LUA_HEAP_END  0xC0000000UL
static uintptr_t g_lua_cur = LUA_HEAP_BASE;

typedef struct { size_t size; size_t pad[2]; } hdr_t;

void *guest_low_realloc(void *ptr, size_t osize, size_t nsize)
{
    static int dbg;
    if (dbg < 8) {
        dbg++;
        fprintf(stderr, "[lua-heap] realloc(ptr=%p, old=%lu, new=%lu) cur=0x%lx\n",
                ptr, (unsigned long)osize, (unsigned long)nsize, (unsigned long)g_lua_cur);
    }
    (void)osize;
    if (nsize == 0) {
        /* Really return it: munmap this run, so that a second luaL_newstate after
         * lua_close does not exhaust the whole pool because the bump pointer only grows. */
        if (ptr) {
            void *base = (void *)((uintptr_t)ptr - sizeof(hdr_t));
            size_t sz = ((hdr_t *)base)->size;
            if (sz) munmap(base, sz);
        }
        return NULL;
    }
    size_t pg = 4096;
    size_t need = (nsize + sizeof(hdr_t) + pg - 1) & ~(pg - 1);
    if (!ptr) {
        if (g_lua_cur + need > LUA_HEAP_END) return NULL;
        void *p = mmap((void *)g_lua_cur, need, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)g_lua_cur) { g_lua_cur += need; return NULL; }
        ((hdr_t *)p)->size = need;
        g_lua_cur += need;
        return (void *)((uintptr_t)p + sizeof(hdr_t));
    }
    /* realloc: always allocate fresh and copy (simple and reliable) */
    void *base = (void *)((uintptr_t)ptr - sizeof(hdr_t));
    size_t old = ((hdr_t *)base)->size - sizeof(hdr_t);
    void *np = guest_low_realloc(NULL, 0, nsize);
    if (np) memcpy(np, ptr, old < nsize ? old : nsize);
    return np;
}

/* ======================================================================
 * guest -> host Lua callback (lua_CFunction trampoline)
 *
 * The C functions the game registers with Lua are **guest code** (e.g. luaL_requiref's openf
 * receives 0x5e15a3, the IAT thunk of luaopen_base: `jmp [IAT]`).
 * Handing it straight to host Lua makes Lua call 0x3c5e15a3 -- a guest address
 * the host cannot execute (measured SIGBUS, pc in no loaded module).
 *
 * So every guest function address needs a host-callable trampoline wrapper:
 *   trampoline(L) -> push L on the current guest stack -> guest_call(guest_addr) -> take eax
 *
 * lua_CFunction takes only lua_State* with no userdata slot, so each distinct
 * guest address needs its **own** trampoline body (see
 * host_lua_tramp.inc). The table entry g_tramp_guest[i] records which address it stands for.
 * ====================================================================== */
#define TRAMP_N 4096
static uint32_t g_tramp_guest[TRAMP_N];
static lua_CFunction g_tramp_tab_addr[TRAMP_N];
static int g_tramp_n;

/* CPU context of the thread currently running guest code (set by host_lua_dispatch) */
static __thread CPU *t_lua_cpu;

static int tramp_call(int idx, lua_State *L);

/* Bodies and array for 512 trampolines */
#include "host_lua_tramp.inc"

/* diagnostics: the most recently executed guest callbacks (printed by the signal handler on a crash) */
uint32_t g_lua_cb_ring[16];
unsigned g_lua_cb_n;

static int tramp_call(int idx, lua_State *L)
{
    CPU *c = t_lua_cpu;
    if (!c) {
        fprintf(stderr, "[lua] ✗ no active CPU context during the guest callback\n");
        return 0;
    }
    uint32_t addr = g_tramp_guest[idx];
    if (!addr) return 0;

    /* Build a cdecl call frame. The x86 cdecl layout is (low address first):
     *     [esp]   = return address
     *     [esp+4] = argument 1
     * So the argument must be pushed **before** the return address -- reversing them makes L the return address.
     * (Measured symptom when reversed: luaopen_base's caller return address read as 0x50000020,
     *   after which Lua dereferenced NULL+0x20 on an error path.)
     *
     * lua_State is allocated by guest_low_realloc and already lives in the low 4GB, so truncation is safe. */
    uint32_t saved = c->esp;
    c->esp -= 4u;
    st32(c->esp, (uint32_t)(uintptr_t)L);   /* argument */
    c->esp -= 4u;
    st32(c->esp, 0u);                       /* return address */

    g_lua_cb_ring[g_lua_cb_n & 15u] = addr;
    g_lua_cb_n++;

    guest_call(c, addr);

    c->esp = saved;
    return (int)c->eax;
}

/* Turn a guest function address into a host lua_CFunction. 0 = pool full. */
static lua_CFunction wrap_guest_cfunction(uint32_t guest_addr)
{
    if (!guest_addr) {
        /* Passing NULL means "register an empty function", a guaranteed crash source:
         * lua_pushcclosure(L, NULL, 0) builds a closure whose f is NULL, and
         * calling it makes Lua call 0. Better to fail here with attributable information. */
        fprintf(stderr, "[lua] ✗ a NULL C function was registered\n");
        abort();
    }
    for (int i = 0; i < g_tramp_n; i++)
        if (g_tramp_guest[i] == guest_addr) return tramp_tab[i];
    if (g_tramp_n >= TRAMP_N) {
        fprintf(stderr, "[lua] ✗ trampoline pool full (%d); cannot wrap %08x\n",
                TRAMP_N, guest_addr);
        return NULL;
    }
    int i = g_tramp_n++;
    g_tramp_guest[i] = guest_addr;
    g_tramp_tab_addr[i] = tramp_tab[i];
    return tramp_tab[i];
}

/* ---------------- forwarding macros ---------------- */
/* * Inline functions are required instead of macros: Lua's lua_newuserdata / lua_pushcfunction are
 * macros, and a comma inside the argument would be miscounted as extra parameters. Inline functions avoid that. */
static inline void       *p32(CPU *c, int i) { return (void *)(uintptr_t)A(c, i); }
static inline const char *s32(CPU *c, int i) { return (const char *)(uintptr_t)A(c, i); }
static inline int         i32(CPU *c, int i) { return (int)A(c, i); }
static inline lua_State  *stp(CPU *c, int i) { return (lua_State *)(uintptr_t)A(c, i); }
/* * Inline functions rather than macros: a comma in a macro argument (e.g. after expanding lua_newuserdata(L,s))
 * would be treated as extra parameters. Function-call parentheses protect inner commas correctly. */
static inline void setp(CPU *c, const void *v) { c->eax = (uint32_t)(uintptr_t)v; }
static inline void seti(CPU *c, int64_t v)     { c->eax = (uint32_t)v; }
static inline void setu64(CPU *c, uint64_t v)  { c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); }
static inline void setd(CPU *c, double v)      { memcpy(&c->x[0].u64[0], &v, 8); }

/* 1 arg, void */
#define W1V(name) static void w_##name(CPU*c){ name(stp(c,0)); cdecl_ret(c); }
/* 2 args, void */
#define W2V(name) static void w_##name(CPU*c){ name(stp(c,0), i32(c,1)); cdecl_ret(c); }
/* 2 args, int */
#define W2I(name) static void w_##name(CPU*c){ seti(c, name(stp(c,0), i32(c,1))); cdecl_ret(c); }
/* 3 args, void */
#define W3V(name) static void w_##name(CPU*c){ name(stp(c,0), i32(c,1), i32(c,2)); cdecl_ret(c); }
/* 3 args, int */
#define W3I(name) static void w_##name(CPU*c){ seti(c, name(stp(c,0), i32(c,1), i32(c,2))); cdecl_ret(c); }

static void w_lua_close(CPU*c)      { lua_close(stp(c,0)); cdecl_ret(c); }
static void w_luaL_openlibs(CPU*c)  { luaL_openlibs(stp(c,0)); cdecl_ret(c); }
static void w_lua_pushnil(CPU*c)    { lua_pushnil(stp(c,0)); cdecl_ret(c); }
static void w_lua_pushvalue(CPU*c)  {
    lua_State*S=stp(c,0); int i=i32(c,1);
    if ((uintptr_t)S > 0x100000000ULL)
        fprintf(stderr, "[lua] ★ pushvalue L=%p exceeds 32 bits!\n", (void*)S);
    lua_pushvalue(S, i); cdecl_ret(c);
}
static void w_lua_settop(CPU*c)     { lua_settop(stp(c,0), i32(c,1)); cdecl_ret(c); }
static void w_lua_rawset(CPU*c)     { lua_rawset(stp(c,0), i32(c,1)); cdecl_ret(c); }
static void w_lua_len(CPU*c)        { lua_len(stp(c,0), i32(c,1)); cdecl_ret(c); }
static void w_lua_rotate(CPU*c)     { lua_rotate(stp(c,0), i32(c,1), i32(c,2)); cdecl_ret(c); }
static void w_lua_copy(CPU*c)       { lua_copy(stp(c,0), i32(c,1), i32(c,2)); cdecl_ret(c); }
static void w_lua_gettop(CPU*c)     { seti(c, lua_gettop(stp(c,0))); cdecl_ret(c); }
static void w_lua_iscfunction(CPU*c){ seti(c, lua_iscfunction(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_toboolean(CPU*c)  { seti(c, lua_toboolean(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_setmetatable(CPU*c){ seti(c, lua_setmetatable(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_rawget(CPU*c)     { seti(c, lua_rawget(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_gettable(CPU*c)   { seti(c, lua_gettable(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_type(CPU*c)       { seti(c, lua_type(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_isstring(CPU*c)   { seti(c, lua_isstring(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_isuserdata(CPU*c) { seti(c, lua_isuserdata(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_absindex(CPU*c)   { seti(c, lua_absindex(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_getmetatable(CPU*c){ seti(c, lua_getmetatable(stp(c,0), i32(c,1))); cdecl_ret(c); }
static void w_lua_typename(CPU*c)   { lua_State*S=stp(c,0); int t=i32(c,1);
    setp(c, lua_typename(S, t)); cdecl_ret(c); }
static void w_lua_touserdata(CPU*c) { lua_State*S=stp(c,0); int i=i32(c,1);
    setp(c, lua_touserdata(S, i)); cdecl_ret(c); }
static void w_lua_pushboolean(CPU*c){ lua_pushboolean(stp(c,0), i32(c,1)); cdecl_ret(c); }
static void w_lua_pushinteger(CPU*c){ lua_pushinteger(stp(c,0), (lua_Integer)A64(c,1)); cdecl_ret(c); }
static void w_lua_pushnumber(CPU*c) {
    double d; uint64_t v = A64(c, 1); memcpy(&d, &v, 8);
    lua_pushnumber(stp(c,0), (lua_Number)d); cdecl_ret(c);
}
static void w_lua_pushstring(CPU*c) { lua_State*S=stp(c,0); const char*k=s32(c,1);
    setp(c, lua_pushstring(S, k)); cdecl_ret(c); }
static void w_lua_pushlstring(CPU*c){ lua_State*S=stp(c,0); const char*k=s32(c,1); size_t n=(size_t)A64(c,2);
    setp(c, lua_pushlstring(S, k, n)); cdecl_ret(c); }
static void w_lua_setglobal(CPU*c)  { lua_setglobal(stp(c,0), s32(c,1)); cdecl_ret(c); }
static void w_lua_getglobal(CPU*c)  { seti(c, lua_getglobal(stp(c,0), s32(c,1))); cdecl_ret(c); }
static void w_lua_setfield(CPU*c)   { lua_setfield(stp(c,0), i32(c,1), s32(c,2)); cdecl_ret(c); }
static void w_lua_getfield(CPU*c)   { seti(c, lua_getfield(stp(c,0), i32(c,1), s32(c,2))); cdecl_ret(c); }
static void w_lua_tolstring(CPU*c) {
    size_t len = 0;
    const char *r = lua_tolstring(stp(c,0), i32(c,1), &len);
    uint32_t out = A(c, 2);
    if (out) st32(out, (uint32_t)len);
    setp(c, r); cdecl_ret(c);
}
static void w_luaL_tolstring(CPU*c) {
    size_t len = 0;
    const char *r = luaL_tolstring(stp(c,0), i32(c,1), &len);
    uint32_t out = A(c, 2);
    if (out) st32(out, (uint32_t)len);
    setp(c, r); cdecl_ret(c);
}
static void w_luaL_checklstring(CPU*c) {
    size_t len = 0;
    const char *r = luaL_checklstring(stp(c,0), i32(c,1), &len);
    uint32_t out = A(c, 2);
    if (out) st32(out, (uint32_t)len);
    setp(c, r); cdecl_ret(c);
}
static void w_luaL_checknumber(CPU*c) { lua_State*S=stp(c,0); int i=i32(c,1);
    double d=luaL_checknumber(S, i); setd(c, d); cdecl_ret(c); }
static void w_luaL_checkinteger(CPU*c){ lua_State*S=stp(c,0); int i=i32(c,1);
    setu64(c, (uint64_t)luaL_checkinteger(S, i)); cdecl_ret(c); }
static void w_lua_rawequal(CPU*c)   { seti(c, lua_rawequal(stp(c,0), i32(c,1), i32(c,2))); cdecl_ret(c); }
static void w_lua_compare(CPU*c)    { seti(c, lua_compare(stp(c,0), i32(c,1), i32(c,2), i32(c,3))); cdecl_ret(c); }
static void w_lua_rawgeti(CPU*c)    { lua_State*S=stp(c,0); int i=i32(c,1); lua_Integer n=(lua_Integer)A64(c,2);
    seti(c, lua_rawgeti(S, i, n)); cdecl_ret(c); }
static void w_lua_rawgetp(CPU*c)    { lua_State*S=stp(c,0); int i=i32(c,1); void*p=p32(c,2);
    seti(c, lua_rawgetp(S, i, p)); cdecl_ret(c); }
static void w_lua_rawsetp(CPU*c)    { lua_State*S=stp(c,0); int i=i32(c,1); void*p=p32(c,2);
    lua_rawsetp(S, i, p); cdecl_ret(c); }
static void w_lua_createtable(CPU*c){ lua_createtable(stp(c,0), i32(c,1), i32(c,2)); cdecl_ret(c); }
static void w_lua_newuserdata(CPU*c){ lua_State*S=stp(c,0); size_t sz=(size_t)A(c,1);
    setp(c, lua_newuserdata(S, sz)); cdecl_ret(c); }
static void w_luaL_ref(CPU*c)       {
    lua_State*S=stp(c,0); int t=i32(c,1);
    int r = luaL_ref(S, t);
    fprintf(stderr, "[lua] luaL_ref(L=%p, t=%d) = %d\n", (void*)S, t, r);
    seti(c, r); cdecl_ret(c);
}
static void w_luaL_unref(CPU*c)     { luaL_unref(stp(c,0), i32(c,1), i32(c,2)); cdecl_ret(c); }
static void w_lua_pushcclosure(CPU*c){
    { lua_State*S=stp(c,0);
      /* the value passed in is a guest function address and must be wrapped as a host trampoline */
      lua_CFunction f=wrap_guest_cfunction(A(c,1)); int n=i32(c,2);
      lua_pushcclosure(S, f, n); } cdecl_ret(c);
}
static void w_luaL_newstate(CPU*c) {
    fprintf(stderr, "[lua] heap used before newstate: %lu MB (base=%p cur=%p)\n",
            (unsigned long)((g_lua_cur - LUA_HEAP_BASE) / (1024*1024)),
            (void*)LUA_HEAP_BASE, (void*)g_lua_cur);
    lua_State *S = luaL_newstate();
    fprintf(stderr, "[lua] luaL_newstate() = %p (heap now %lu MB)\n", (void*)S,
            (unsigned long)((g_lua_cur - LUA_HEAP_BASE) / (1024*1024)));
    setp(c, S); cdecl_ret(c);
}
/* ---- bridge the guest's lua_Alloc into a host-callable one ----
 *
 * When the game calls lua_newstate(allocf, ud), allocf is a **guest code address**.
 * It used to be cast straight to lua_Alloc and handed to host Lua -- the first
 * allocation then called a guest address like 0x3c3fca80, which the host cannot execute,
 * giving SIGILL on the spot with only "pc in no loaded module" as evidence.
 *
 * The bridge uses the same trick as the lua_CFunction trampoline: place arguments
 * on the current guest stack, push a fake return address, guest_call, then take eax. */
static uint32_t g_guest_allocf;

static void *guest_alloc_bridge(void *ud, void *ptr, size_t osize, size_t nsize)
{
    CPU *c = t_lua_cpu;
    if (!c || !g_guest_allocf) return NULL;
    uint32_t saved = c->esp;
    gpush(c, (uint32_t)nsize);
    gpush(c, (uint32_t)osize);
    gpush(c, (uint32_t)(uintptr_t)ptr);
    gpush(c, (uint32_t)(uintptr_t)ud);
    gpush(c, 0xabc0a110u);              /* fake return address */
    guest_call(c, g_guest_allocf);
    void *r = (void *)(uintptr_t)c->eax;
    c->esp = saved;
    return r;
}

/* Default when the game supplies no allocator: use the low-4GB pool; never let Lua use host realloc
 * (that returns pointers above 4 GiB, which do not fit the guest's uint32 pointers). */
static void *low_alloc_bridge(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    return guest_low_realloc(ptr, osize, nsize);
}

static void w_lua_newstate(CPU*c) {
    uint32_t f = A(c, 0);
    void *ud = p32(c, 1);
    lua_Alloc a;
    t_lua_cpu = c;                      /* the bridge needs the current CPU context */
    if (f) { g_guest_allocf = f; a = guest_alloc_bridge; }
    else   { g_guest_allocf = 0; a = low_alloc_bridge; }
    setp(c, lua_newstate(a, ud));
    cdecl_ret(c);
}
static void w_lua_atpanic(CPU*c)    { lua_State*S=stp(c,0);
    lua_CFunction f=wrap_guest_cfunction(A(c,1));
    setp(c, lua_atpanic(S, f)); cdecl_ret(c); }
static void w_luaopen_base(CPU*c)   { seti(c, luaopen_base(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_table(CPU*c)  { seti(c, luaopen_table(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_string(CPU*c) { seti(c, luaopen_string(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_math(CPU*c)   { seti(c, luaopen_math(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_debug(CPU*c)  { seti(c, luaopen_debug(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_coroutine(CPU*c){ seti(c, luaopen_coroutine(stp(c,0))); cdecl_ret(c); }
static void w_luaopen_utf8(CPU*c)   { seti(c, luaopen_utf8(stp(c,0))); cdecl_ret(c); }
static void w_luaL_requiref(CPU*c) {
    lua_State*S=stp(c,0); const char*m=s32(c,1);
    /* openf is a guest address (e.g. the IAT thunk of luaopen_base) */
    lua_CFunction o=wrap_guest_cfunction(A(c,2)); int g=i32(c,3);
    luaL_requiref(S, m, o, g); cdecl_ret(c);
}
static void w_luaL_argerror(CPU*c)  { seti(c, luaL_argerror(stp(c,0), i32(c,1), s32(c,2))); cdecl_ret(c); }
static void w_lua_callk(CPU*c) {
    lua_State*S=stp(c,0); int na=i32(c,1), nr=i32(c,2);
    lua_KContext x=(lua_KContext)A64(c,3);
    /* continuation is also a guest address, but there is no KFunction trampoline
     * (its signature takes 3 arguments, so the lua_CFunction pool cannot be used). NULL means
     * "no continuation"; Lua takes its ordinary continuation path after returning. */
    lua_KFunction k=NULL; (void)A(c,4);
    lua_callk(S, na, nr, x, k); cdecl_ret(c);
}
static void w_lua_pcallk(CPU*c) {
    lua_State*S=stp(c,0); int na=i32(c,1), nr=i32(c,2), ef=i32(c,3);
    lua_KContext x=(lua_KContext)A64(c,4);
    lua_KFunction k=NULL; (void)A(c,5);
    seti(c, lua_pcallk(S, na, nr, ef, x, k));
    cdecl_ret(c);
}
static void w_lua_gc(CPU*c)         { seti(c, lua_gc(stp(c,0), i32(c,1), i32(c,2))); cdecl_ret(c); }
static void w_lua_getinfo(CPU*c)    { lua_State*S=stp(c,0); const char*w=s32(c,1); lua_Debug*d=(lua_Debug*)(uintptr_t)A(c,2);
    seti(c, lua_getinfo(S, w, d)); cdecl_ret(c); }
static void w_lua_getstack(CPU*c)   { lua_State*S=stp(c,0); int lv=i32(c,1); lua_Debug*d=(lua_Debug*)(uintptr_t)A(c,2);
    seti(c, lua_getstack(S, lv, d)); cdecl_ret(c); }
static void w_luaL_loadbufferx(CPU*c) {
    lua_State*S=stp(c,0); const char*b=s32(c,1); size_t n=(size_t)A64(c,2);
    const char*nm=s32(c,3), *md=s32(c,4);
    seti(c, luaL_loadbufferx(S, b, n, nm, md)); cdecl_ret(c);
}
static void w_luaL_loadfilex(CPU*c) {
    lua_State*S=stp(c,0); const char*f=s32(c,1), *m=s32(c,2);
    seti(c, luaL_loadfilex(S, f, m)); cdecl_ret(c);
}
/* Variadic function: forward to real Lua, taking the first N arguments verbatim from the guest stack */
static void w_lua_pushfstring(CPU*c) {
    lua_State*S=stp(c,0); const char*f=s32(c,1);
    uint32_t a1=A(c,2), a2=A(c,3), a3=A(c,4);
    setp(c, lua_pushfstring(S, f, a1, a2, a3)); cdecl_ret(c);
}
static void w_luaL_error(CPU*c) {
    lua_State*S=stp(c,0); const char*f=s32(c,1);
    uint32_t a1=A(c,2), a2=A(c,3);
    seti(c, luaL_error(S, f, a1, a2)); cdecl_ret(c);
}

typedef struct { const char *n; void (*f)(CPU *); } E;
static const E TBL[] = {
    {"lua_close", w_lua_close}, {"lua_rawset", w_lua_rawset},
    {"lua_iscfunction", w_lua_iscfunction}, {"lua_getfield", w_lua_getfield},
    {"lua_typename", w_lua_typename}, {"lua_toboolean", w_lua_toboolean},
    {"lua_setmetatable", w_lua_setmetatable}, {"lua_compare", w_lua_compare},
    {"lua_settop", w_lua_settop}, {"luaopen_debug", w_luaopen_debug},
    {"luaL_openlibs", w_luaL_openlibs}, {"luaL_tolstring", w_luaL_tolstring},
    {"luaL_requiref", w_luaL_requiref}, {"luaopen_coroutine", w_luaopen_coroutine},
    {"luaL_argerror", w_luaL_argerror}, {"lua_pushnil", w_lua_pushnil},
    {"lua_len", w_lua_len}, {"lua_rawget", w_lua_rawget},
    {"lua_gettop", w_lua_gettop}, {"lua_pushnumber", w_lua_pushnumber},
    {"luaL_checknumber", w_luaL_checknumber}, {"lua_getstack", w_lua_getstack},
    {"luaopen_math", w_luaopen_math}, {"lua_pushstring", w_lua_pushstring},
    {"lua_touserdata", w_lua_touserdata}, {"lua_pushfstring", w_lua_pushfstring},
    {"lua_atpanic", w_lua_atpanic}, {"lua_getglobal", w_lua_getglobal},
    {"lua_isstring", w_lua_isstring}, {"luaopen_string", w_luaopen_string},
    {"lua_callk", w_lua_callk}, {"lua_rawgetp", w_lua_rawgetp},
    {"luaL_loadbufferx", w_luaL_loadbufferx}, {"lua_tolstring", w_lua_tolstring},
    {"lua_isuserdata", w_lua_isuserdata}, {"lua_pushboolean", w_lua_pushboolean},
    {"lua_newstate", w_lua_newstate}, {"lua_setglobal", w_lua_setglobal},
    {"lua_newuserdata", w_lua_newuserdata}, {"luaL_newstate", w_luaL_newstate},
    {"luaL_checkinteger", w_luaL_checkinteger}, {"luaL_error", w_luaL_error},
    {"lua_rawsetp", w_lua_rawsetp}, {"luaopen_utf8", w_luaopen_utf8},
    {"lua_setfield", w_lua_setfield}, {"lua_gettable", w_lua_gettable},
    {"luaopen_base", w_luaopen_base}, {"lua_type", w_lua_type},
    {"lua_rawequal", w_lua_rawequal}, {"lua_rotate", w_lua_rotate},
    {"luaL_checklstring", w_luaL_checklstring}, {"lua_getmetatable", w_lua_getmetatable},
    {"lua_absindex", w_lua_absindex}, {"lua_pushlstring", w_lua_pushlstring},
    {"lua_getinfo", w_lua_getinfo}, {"lua_copy", w_lua_copy},
    {"luaopen_table", w_luaopen_table}, {"luaL_loadfilex", w_luaL_loadfilex},
    {"lua_gc", w_lua_gc}, {"lua_pcallk", w_lua_pcallk},
    {"lua_pushinteger", w_lua_pushinteger}, {"lua_pushcclosure", w_lua_pushcclosure},
    {"luaL_unref", w_luaL_unref}, {"lua_rawgeti", w_lua_rawgeti},
    {"lua_createtable", w_lua_createtable}, {"luaL_ref", w_luaL_ref},
    {"lua_pushvalue", w_lua_pushvalue},
};
#define NT ((int)(sizeof TBL / sizeof TBL[0]))

int host_lua_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    /* Handles only the "Lua5.3.3r.dll!xxx" prefix */
    const char *bang = strchr(name, '!');
    if (!bang || strncmp(name, "Lua5.3.3r.dll", 13) != 0) return 0;
    const char *sym = bang + 1;
    /* Record the current guest context: guest callbacks inside Lua use it to find the CPU */
    t_lua_cpu = c;
    for (int i = 0; i < NT; i++)
        if (strcmp(TBL[i].n, sym) == 0) { TBL[i].f(c); return 1; }
    return 0;
}
int host_lua_count(void) { return NT; }
