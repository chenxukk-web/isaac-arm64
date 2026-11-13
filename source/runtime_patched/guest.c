/* Runtime for the recompiled guest: image mapping, dispatch, faults.
 *
 * PC-first, so this file uses Win32 for image reservation and the guest stack.
 * The CPU/dispatch core is portable C; Vita still needs target image/stack and
 * host-API/platform backends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>
#if defined(__GNUC__) || defined(__clang__)
#include <dlfcn.h>   /* diagnostics: symbolicate host return addresses to sub_xxxx */
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define GUEST_STACK_NATIVE_CALLER() ((uintptr_t)_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#define GUEST_STACK_NATIVE_CALLER() \
    ((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))
#else
#define GUEST_STACK_NATIVE_CALLER() ((uintptr_t)0U)
#endif

#if defined(__arm__) && (defined(__GNUC__) || defined(__clang__))
/* Keep the canonical incoming LR live without emitting a hot-path move.  GCC
 * supports an explicit local register only as an extended-asm operand, so do
 * not rely on ordinary C reads of a variable bound to LR.  The empty tied
 * operand snapshots LR into compiler SSA at function entry; GCC may keep or
 * spill that value normally until a rejecting branch consumes it.  This is a
 * pinned ARM/GCC code-generation contract and the target gate disassembles
 * every helper after any compiler or flag change. */
#define GUEST_STACK_NATIVE_LOCAL(name) \
    register uintptr_t name##_link __asm__("lr") = \
        GUEST_STACK_NATIVE_CALLER(); \
    uintptr_t name; \
    __asm__ __volatile__("" : "=r"(name) : "0"(name##_link) : "memory")
#else
#define GUEST_STACK_NATIVE_LOCAL(name) \
    uintptr_t name = GUEST_STACK_NATIVE_CALLER()
#endif
#ifdef __vita__
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <windows.h>
#endif

#include "guest.h"
#include "guest_pe.h"
#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#include "host_vita_gl.h"
#if defined(ISAAC_VITA_STALL_PROBE) || defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
#include "gl_bridge.h"
#endif
#endif
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS) && \
    (!defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH) || \
     !defined(ISAAC_VITA_GUEST_DISPATCH_TABLE))
# error ISAAC_VITA_GL_SHIM_TABLE_TOKENS requires ISAAC_VITA_GL_SHIM_FASTDISPATCH and ISAAC_VITA_GUEST_DISPATCH_TABLE
#endif

#ifdef ISAAC_VITA_IMPORT_ID_DISPATCH
#include "host_vita_import_id.h"
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* Sampler builds: the indirect-target word is published by guest_call, by
 * guest_try_direct_sync_import_call (declared above its definition here) and
 * by guest_import_call (ISAAC_VITA_IMPORT_DIRECT); kage_vita_guest_sampler.h
 * owns the publish/restore contract. */
#include "kage_vita_guest_sampler.h"
#endif
#if defined(ISAAC_VITA_LUA) && ISAAC_VITA_LUA
#include "host_vita_lua.h"
#endif
#if defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH) && \
    !defined(ISAAC_VITA_IMPORT_ID_DISPATCH)
#error ISAAC_VITA_SYNC_IMPORT_FASTPATH requires validated import-ID dispatch
#endif
#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#if !defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
#error ISAAC_VITA_SYNC_INLINE_FASTPATH requires ISAAC_VITA_SYNC_IMPORT_FASTPATH
#endif
#include "host_vita_sync_fastpath.h"
#endif
#ifdef __vita__
#include "kage_vita_stall_probe.h"

/* The Vita build edge owns the privileged fixed-VA allocation.  Keeping the
 * hook here lets the PE loader/TLS/dispatch core stay shared with the working
 * PC runtime without teaching it about kubridge handles. */
extern void *isaac_vita_fixed_alloc(uint32_t address, uint32_t size);
extern void  isaac_vita_fixed_free(void *address);
#endif

/* ------------------------------------------------------------- image ---- */

static void    *g_image_mem;
static uint32_t g_image_base, g_image_size;
static const guest_import *g_imports;
static uint32_t            g_import_count;
static void               *g_stack_mem;
static CPU                *g_stack_owner;
/* Thread-local: each guest thread has its own stack bounds and its own TIB. */
static __thread uint32_t   g_stack_base, g_stack_top;
static __thread unsigned char *g_tib;

/* * Low-4GB allocator
 *
  * Guest pointers are uint32_t, so any host memory handed to guest code must
  * live in the low 4GB -- otherwise (uint32_t)(uintptr_t)p truncates to garbage.
  * On Android, libc calloc/malloc return high addresses around 0xb4xxxxxx, and
  * truncating them makes the SEH chain point at an illegal address, crashing at startup.
 *
  * This is the most easily overlooked constraint of running a 32-bit guest on a 64-bit host.
 *
  * Multithreading allocates concurrently, hence a lock; the lock protects the bump pointer. */
static pthread_mutex_t     g_low_alloc_mu = PTHREAD_MUTEX_INITIALIZER;
static void *guest_low_alloc_locked(size_t size);

static void *guest_low_alloc(size_t size)
{
    void *p;
    pthread_mutex_lock(&g_low_alloc_mu);
    p = guest_low_alloc_locked(size);
    pthread_mutex_unlock(&g_low_alloc_mu);
    return p;
}

static void *guest_low_alloc_locked(size_t size)
{
        /* Range tightened to 0x32000000..0x3F000000, avoiding the 0x30000000 image and
              the 0x40000000 stack. Going up to 0x42000000 used to collide with the stack. */
    static uintptr_t cursor = 0x32000000UL;
    size_t pg = 4096;
    size_t sz = (size + pg - 1) & ~(pg - 1);
    int i;
    if (!sz) sz = pg;
    for (i = 0; i < 200 && cursor < 0x3F000000UL; i++) {
        void *p = mmap((void *)cursor, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
        cursor += 0x00100000UL;
        if (p != MAP_FAILED && p == (void *)(cursor - 0x00100000UL))
            return p;
        if (p != MAP_FAILED)
            munmap(p, sz);
    }
    return NULL;
}
#if defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
static int g_vita_sync_import_fastpath_ready;
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* Rebuilt (or emptied, fail closed) whenever the function table, the import
 * table or the mapped image changes; see the dispatch-table section. */
static void guest_dispatch_table_rebuild(void);
#endif

#if defined(ISAAC_VITA_IMPORT_DIRECT)
#if !defined(ISAAC_VITA_IMPORT_ID_DISPATCH)
#error ISAAC_VITA_IMPORT_DIRECT requires validated import-ID dispatch
#endif
/* One row per dense import ID (guest.h: guest_import_direct).  Filled only
 * after the complete 413-row registration passed, only for IDs whose family
 * endpoint is directly addressable in this build; slot_va is the token
 * tag_imports writes into the slot, so a generated site can reach a row only
 * with the exact ID/slot pair the emitter proved.  Cleared on every
 * re-registration first, so a rejected table fails closed to guest_call. */
guest_import_direct g_guest_import_direct[GUEST_IMPORT_DIRECT_CAPACITY];
static void guest_import_direct_rebuild(int registration_ok);
#endif
#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
/* ---- ISAAC_VITA_GL_SHIM_FASTDISPATCH (wf/opt-glshim) begin ----------
 * guest_call may hand a 0x7e typed-GL token to the GL dynamic entry before
 * the dispatch cache (which rejects that family by construction) and the IAT
 * search only while no registered slot key and no in-image RVA can alias a
 * token of the family; both are startup facts, recomputed whenever the import
 * table or the image window changes, so the per-call test is one flag. */
static int g_gl_dynamic_first;

static void guest_gl_dynamic_first_refresh(void)
{
    const uint32_t family_lo = UINT32_C(0x7e000000);
    const uint32_t family_hi = UINT32_C(0x7effffff);
    int imports_clear = g_import_count == 0U ||
        g_imports[g_import_count - 1U].slot_rva < family_lo;
    int image_clear = !g_image_base || !g_image_size ||
        g_image_base > family_hi ||
        (uint64_t)g_image_base + g_image_size <= family_lo;

    g_gl_dynamic_first = imports_clear && image_clear;
}
# define GUEST_GL_DYNAMIC_FIRST_REFRESH() guest_gl_dynamic_first_refresh()
/* ---- ISAAC_VITA_GL_SHIM_FASTDISPATCH end ------------------------- */
#else
# define GUEST_GL_DYNAMIC_FIRST_REFRESH() ((void)0)
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib) begin ---------- */
#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
#error ISAAC_VITA_SHADER_ATTRIB_FASTPATH requires ISAAC_VITA_GL_SHIM_FASTDISPATCH
#endif
int guest_gl_dynamic_first_holds(void)
{
    return g_gl_dynamic_first;
}
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH end ---------------------------- */
#endif

/* Parent-held file-backed semantic coverage.  Keep the public pointers plain
 * so the generated inline hook stays one nullable branch plus one byte store;
 * the hook casts the actual store volatile. */
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;
#if defined(ISAAC_VITA_PHASE_PROFILE)
GuestPhaseProfileCounters g_guest_phase_profile_counters;
# if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
     defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* The one per-import census (guest.h): the dispatch table's ph120.i and the
 * import-kinds ph120.ik/ph120.ih records both read this array. */
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#  if defined(ISAAC_VITA_IMPORT_ID_COUNT) && \
      (ISAAC_VITA_IMPORT_ID_COUNT != GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS)
#   error "per-import profile census does not cover the frozen import ID range"
#  endif
# endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
/* One increment per translated body entry (guest.h guest_coverage_function);
 * ph120.ik ent=.  Opt-in: every generated unit carries the store. */
uint32_t g_guest_function_entries;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
uint32_t g_guest_lookup_cache_hits;
uint32_t g_guest_lookup_cache_misses;
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_HIT() \
    (++g_guest_lookup_cache_hits)
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_MISS() \
    (++g_guest_lookup_cache_misses)
#else
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_HIT() ((void)0)
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_MISS() ((void)0)
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE)
void guest_phase_profile_note_lookup_cache_hit(void)
{
    GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_HIT();
}
#endif

#define GUEST_COVERAGE_MAGIC "REPENTOGXM-COV1"
#define GUEST_COVERAGE_VERSION 1U
#define GUEST_COVERAGE_HEADER_SIZE 4096U
#define GUEST_COVERAGE_SCHEMA_OFFSET 48U
#define GUEST_COVERAGE_BUILD_OFFSET 112U
#define GUEST_COVERAGE_IDENTITY_BYTES 64U

#ifndef __vita__
static HANDLE   g_coverage_file = INVALID_HANDLE_VALUE;
static HANDLE   g_coverage_mapping;
static void    *g_coverage_view;
static char     g_coverage_schema[GUEST_COVERAGE_IDENTITY_BYTES + 1U];
static char     g_coverage_build[GUEST_COVERAGE_IDENTITY_BYTES + 1U];
static uint32_t g_coverage_function_count;
static uint32_t g_coverage_import_count;
static uint32_t g_coverage_case_count;
static int      g_coverage_contract_registered;
static int      g_coverage_atexit_registered;
static char     g_coverage_error[192];
#endif

#define GUEST_TLS_SLOTS 64U
#define GUEST_TLS_CALLBACKS_MAX 64U
static __thread void      *g_tls_block;      /* this thread's TLS instance */
static void               *g_tls_template;   /* global read-only template (filled by setup_static_tls) */
static uint32_t            g_tls_block_size;
static uint32_t            g_tls_index = 0xFFFFFFFFu;
static uint32_t            g_tls_callbacks[GUEST_TLS_CALLBACKS_MAX];
static uint32_t            g_tls_callback_count;
static int                 g_tls_present;
static int                 g_tls_process_attached;

#ifndef GUEST_LINK_ID
#define GUEST_LINK_ID "link:unstamped"
#endif
#ifndef GUEST_BUILD_OPT
#define GUEST_BUILD_OPT "unknown"
#endif

#ifdef GUEST_GENERATION_SYMBOL
#define GUEST_COMPILED_GENERATION GUEST_GENERATION_SYMBOL
#else
static const char s_standalone_generation[] = "gen:standalone";
#define GUEST_COMPILED_GENERATION s_standalone_generation
#endif

static int g_generation_noted;

uint64_t guest_atomic_cmpxchg64(uint32_t addr, uint64_t expected,
                                uint64_t desired)
{
    G_CHK(addr, 8, 1);
    /* InterlockedCompareExchange64 requires natural alignment on 32-bit
     * multiprocessor x86.  All measured mimalloc stat fields satisfy it; a
     * future unaligned guest form needs a separate exact backend, not UB. */
    if (addr & 7U) {
        fprintf(stderr, "guest: unaligned LOCK CMPXCHG8B at %08x\n", addr);
        abort();
    }
#ifdef __vita__
    /* GCC 10.3 for the Vita emits this inline as LDREXD/STREXD plus DMB; the
     * final softfp ARM link has no libatomic or libgcc helper dependency. */
    return __sync_val_compare_and_swap(
        (volatile uint64_t *)(uintptr_t)addr, expected, desired);
#else
    return (uint64_t)InterlockedCompareExchange64(
        (volatile LONG64 *)(uintptr_t)addr,
        (LONG64)desired, (LONG64)expected);
#endif
}

void guest_note_generation(const char *generation_id)
{
    if (!generation_id || strcmp(generation_id, GUEST_COMPILED_GENERATION) != 0) {
        fprintf(stderr, "guest: generated table/header identity mismatch\n");
        abort();
    }
    if (!g_generation_noted) {
        printf("guest: build %s %s opt:%s\n",
               generation_id, GUEST_LINK_ID, GUEST_BUILD_OPT);
        g_generation_noted = 1;
    }
}

const char *guest_generation_id(void)
{
    return GUEST_COMPILED_GENERATION;
}

/* ---------------------------------------------------------- coverage ---- */

#ifdef __vita__

/* Semantic coverage is a PC exploration transport.  Generated hooks remain
 * present on Vita, but all three pointers stay NULL for this milestone. */
void guest_register_coverage_contract(const char *schema_id,
                                      const char *build_id,
                                      uint32_t function_count,
                                      uint32_t import_count,
                                      uint32_t case_count)
{
    (void)schema_id;
    (void)build_id;
    (void)function_count;
    (void)import_count;
    (void)case_count;
}

const char *guest_coverage_error(void)
{
    return NULL;
}

void guest_coverage_shutdown(void)
{
    g_guest_coverage_functions = NULL;
    g_guest_coverage_imports = NULL;
    g_guest_coverage_cases = NULL;
}

int guest_coverage_init_from_env(void)
{
    guest_coverage_shutdown();
    return 0;
}

#else

static int coverage_hex_identity(const char *value)
{
    uint32_t i;
    if (!value || strlen(value) != GUEST_COVERAGE_IDENTITY_BYTES)
        return 0;
    for (i = 0U; i < GUEST_COVERAGE_IDENTITY_BYTES; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return 0;
    }
    return 1;
}

static int coverage_fail(const char *message)
{
    _snprintf_s(g_coverage_error, sizeof g_coverage_error, _TRUNCATE,
                "%s", message ? message : "unknown coverage error");
    fprintf(stderr, "guest coverage: %s\n", g_coverage_error);
    return -1;
}

static uint32_t coverage_u32(const unsigned char *bytes, uint32_t offset)
{
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof value);
    return value;
}

void guest_register_coverage_contract(const char *schema_id,
                                      const char *build_id,
                                      uint32_t function_count,
                                      uint32_t import_count,
                                      uint32_t case_count)
{
    uint64_t total = (uint64_t)GUEST_COVERAGE_HEADER_SIZE + function_count +
                     import_count + case_count;
    if (!coverage_hex_identity(schema_id) ||
        !coverage_hex_identity(build_id) ||
        !function_count || !import_count || !case_count ||
        total > 0xffffffffULL) {
        fprintf(stderr, "guest coverage: invalid generated contract\n");
        abort();
    }
    if (g_coverage_contract_registered) {
        if (strcmp(schema_id, g_coverage_schema) != 0 ||
            strcmp(build_id, g_coverage_build) != 0 ||
            function_count != g_coverage_function_count ||
            import_count != g_coverage_import_count ||
            case_count != g_coverage_case_count) {
            fprintf(stderr, "guest coverage: generated contract changed "
                            "during registration\n");
            abort();
        }
        return;
    }
    memcpy(g_coverage_schema, schema_id, GUEST_COVERAGE_IDENTITY_BYTES + 1U);
    memcpy(g_coverage_build, build_id, GUEST_COVERAGE_IDENTITY_BYTES + 1U);
    g_coverage_function_count = function_count;
    g_coverage_import_count = import_count;
    g_coverage_case_count = case_count;
    g_coverage_contract_registered = 1;
}

const char *guest_coverage_error(void)
{
    return g_coverage_error[0] ? g_coverage_error : NULL;
}

void guest_coverage_shutdown(void)
{
    g_guest_coverage_functions = NULL;
    g_guest_coverage_imports = NULL;
    g_guest_coverage_cases = NULL;
    if (g_coverage_view) {
        FlushViewOfFile(g_coverage_view, 0U);
        UnmapViewOfFile(g_coverage_view);
        g_coverage_view = NULL;
    }
    if (g_coverage_mapping) {
        CloseHandle(g_coverage_mapping);
        g_coverage_mapping = NULL;
    }
    if (g_coverage_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_coverage_file);
        g_coverage_file = INVALID_HANDLE_VALUE;
    }
}

int guest_coverage_init_from_env(void)
{
    static const unsigned char magic[16] = GUEST_COVERAGE_MAGIC;
    char enabled[3], *path = NULL;
    DWORD enabled_length, path_length, copied;
    LARGE_INTEGER file_size;
    unsigned char *bytes;
    uint32_t total_size, expected_size, i;

    g_coverage_error[0] = '\0';
    SetLastError(ERROR_SUCCESS);
    enabled_length = GetEnvironmentVariableA(
        "REPENTOGXM_PC_COVERAGE", enabled, sizeof enabled);
    if (!enabled_length) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return 0;
        return coverage_fail("REPENTOGXM_PC_COVERAGE must be exactly 1");
    }
    if (enabled_length != 1U || enabled[0] != '1')
        return coverage_fail("REPENTOGXM_PC_COVERAGE must be exactly 1");
    if (!g_coverage_contract_registered)
        return coverage_fail("generated coverage contract was not registered");
    if (g_import_count != g_coverage_import_count)
        return coverage_fail("registered import table/count mismatch");
    if (g_coverage_view)
        return coverage_fail("coverage mapping was initialized twice");

    path_length = GetEnvironmentVariableA(
        "REPENTOGXM_PC_COVERAGE_FILE", NULL, 0U);
    if (!path_length)
        return coverage_fail("REPENTOGXM_PC_COVERAGE_FILE is missing");
    path = (char *)malloc(path_length);
    if (!path)
        return coverage_fail("coverage path allocation failed");
    copied = GetEnvironmentVariableA(
        "REPENTOGXM_PC_COVERAGE_FILE", path, path_length);
    if (!copied || copied >= path_length) {
        free(path);
        return coverage_fail("coverage path changed while reading it");
    }

    g_coverage_file = CreateFileA(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (g_coverage_file == INVALID_HANDLE_VALUE)
        return coverage_fail("could not open parent coverage file");
    if (!GetFileSizeEx(g_coverage_file, &file_size) ||
        file_size.QuadPart < GUEST_COVERAGE_HEADER_SIZE ||
        file_size.QuadPart > 0xffffffffLL) {
        guest_coverage_shutdown();
        return coverage_fail("coverage file size is invalid");
    }
    g_coverage_mapping = CreateFileMappingA(
        g_coverage_file, NULL, PAGE_READWRITE, 0U, 0U, NULL);
    if (!g_coverage_mapping) {
        guest_coverage_shutdown();
        return coverage_fail("could not create coverage file mapping");
    }
    g_coverage_view = MapViewOfFile(
        g_coverage_mapping, FILE_MAP_WRITE, 0U, 0U, 0U);
    if (!g_coverage_view) {
        guest_coverage_shutdown();
        return coverage_fail("could not map coverage file");
    }
    bytes = (unsigned char *)g_coverage_view;
    total_size = coverage_u32(bytes, 24U);
    expected_size = GUEST_COVERAGE_HEADER_SIZE +
                    g_coverage_function_count + g_coverage_import_count +
                    g_coverage_case_count;
    if (memcmp(bytes, magic, sizeof magic) != 0 ||
        coverage_u32(bytes, 16U) != GUEST_COVERAGE_VERSION ||
        coverage_u32(bytes, 20U) != GUEST_COVERAGE_HEADER_SIZE ||
        total_size != expected_size ||
        total_size != (uint32_t)file_size.QuadPart ||
        coverage_u32(bytes, 28U) != 0U ||
        coverage_u32(bytes, 32U) != g_coverage_function_count ||
        coverage_u32(bytes, 36U) != g_coverage_import_count ||
        coverage_u32(bytes, 40U) != g_coverage_case_count ||
        coverage_u32(bytes, 44U) != 0U ||
        memcmp(bytes + GUEST_COVERAGE_SCHEMA_OFFSET,
               g_coverage_schema, GUEST_COVERAGE_IDENTITY_BYTES) != 0 ||
        memcmp(bytes + GUEST_COVERAGE_BUILD_OFFSET,
               g_coverage_build, GUEST_COVERAGE_IDENTITY_BYTES) != 0) {
        guest_coverage_shutdown();
        return coverage_fail("coverage header/schema/build mismatch");
    }
    for (i = GUEST_COVERAGE_BUILD_OFFSET +
             GUEST_COVERAGE_IDENTITY_BYTES;
         i < GUEST_COVERAGE_HEADER_SIZE; ++i) {
        if (bytes[i] != 0U) {
            guest_coverage_shutdown();
            return coverage_fail("coverage header padding is nonzero");
        }
    }
    g_guest_coverage_functions = bytes + GUEST_COVERAGE_HEADER_SIZE;
    g_guest_coverage_imports = g_guest_coverage_functions +
                               g_coverage_function_count;
    g_guest_coverage_cases = g_guest_coverage_imports +
                             g_coverage_import_count;
    if (!g_coverage_atexit_registered) {
        if (atexit(guest_coverage_shutdown) != 0) {
            guest_coverage_shutdown();
            return coverage_fail("could not register coverage cleanup");
        }
        g_coverage_atexit_registered = 1;
    }
    /* Writer-ready is a one-way child handshake.  The parent rejects an all-
     * zero capture with this word still zero, which distinguishes a genuine
     * no-coverage path from a stale executable that never mapped the file. */
    InterlockedExchange((volatile LONG *)(bytes + 28U), 1L);
    printf("guest coverage: schema %.12s build %.12s functions=%u "
           "imports=%u cases=%u\n",
           g_coverage_schema, g_coverage_build,
           g_coverage_function_count, g_coverage_import_count,
           g_coverage_case_count);
    return 0;
}

#endif

/* The image is NOT mapped at its own 0x400000. ntdll creates the default
 * process heap there during process initialisation -- before the entry point
 * and before any TLS callback -- so no hook is early enough to reserve it;
 * measured, VirtualAlloc returns 487 and a region walk shows a heap segment
 * sitting at 0x400000. The Vita would not offer 0x400000 either, its user
 * addresses being around 0x8xxxxxxx.
 *
 * So the image is relocated to GUEST_IMAGE_BASE using its own .reloc section,
 * and the translator disassembles bytes relocated identically. Keep this
 * value in step with recomp/image.py's DEFAULT_BASE. */
#define GUEST_IMAGE_RESERVE 0x00900000u

#ifndef __vita__
int g_image_reserved;

/* Reserving before the CRT starts is no longer strictly needed at this base,
 * but it costs nothing and removes any dependence on allocation order. */
extern int mainCRTStartup(void);

void guest_entry(void)
{
    g_image_reserved = VirtualAlloc((LPVOID)(uintptr_t)GUEST_IMAGE_BASE,
                                    GUEST_IMAGE_RESERVE,
                                    MEM_RESERVE, PAGE_NOACCESS) != NULL;
    mainCRTStartup();
}
#endif

static void *guest_pe_stdio_open(void *context, const char *path)
{
    FILE *file = NULL;
    (void)context;
#ifdef _MSC_VER
    if (fopen_s(&file, path, "rb") != 0)
        return NULL;
#else
    file = fopen(path, "rb");
#endif
    return file;
}

static int guest_pe_stdio_size(
    void *context, void *file_pointer, uint64_t *size_out)
{
    FILE *file = (FILE *)file_pointer;
    long size;
    (void)context;
    if (!file || !size_out || fseek(file, 0L, SEEK_END) != 0)
        return 1;
    size = ftell(file);
    if (size < 0L)
        return 1;
    *size_out = (uint64_t)(unsigned long)size;
    return 0;
}

static int guest_pe_stdio_rewind(void *context, void *file_pointer)
{
    (void)context;
    return fseek((FILE *)file_pointer, 0L, SEEK_SET) == 0 ? 0 : 1;
}

static size_t guest_pe_stdio_read(
    void *context, void *file_pointer, unsigned char *destination, size_t size)
{
    (void)context;
    return fread(destination, 1U, size, (FILE *)file_pointer);
}

static int guest_pe_stdio_close(void *context, void *file_pointer)
{
    (void)context;
    return fclose((FILE *)file_pointer) == 0 ? 0 : 1;
}

static void *guest_pe_raw_alloc(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void guest_pe_raw_free(void *context, void *allocation)
{
    (void)context;
    free(allocation);
}

static void *guest_pe_platform_image_alloc(
    void *context, uint32_t address, uint32_t size)
{
    void *allocation;
    (void)context;
#ifdef __vita__
    allocation = isaac_vita_fixed_alloc(address, size);
    if (!allocation) {
        fprintf(stderr,
                "guest: fixed Vita image allocation at %08x size 0x%x failed\n",
                address, size);
    }
#else
    allocation = VirtualAlloc((LPVOID)(uintptr_t)address, size,
                              MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!allocation) {
        MEMORY_BASIC_INFORMATION mbi;
        uint32_t cursor;
        fprintf(stderr,
                "guest: VirtualAlloc at 0x%08x size 0x%x failed (err %lu)\n",
                address, size, (unsigned long)GetLastError());
        fprintf(stderr, "guest: our own module is at %p\n",
                (void *)GetModuleHandleW(NULL));
        fprintf(stderr, "guest: what occupies [%08x, %08x):\n",
                address, address + size);
        for (cursor = address; cursor < address + size; ) {
            uintptr_t next;
            if (!VirtualQuery((LPCVOID)(uintptr_t)cursor, &mbi, sizeof mbi))
                break;
            if (mbi.State != MEM_FREE) {
                fprintf(stderr,
                        "   %08x + %-9lu state=%lx type=%lx prot=%lx\n",
                        (unsigned)(uintptr_t)mbi.BaseAddress,
                        (unsigned long)mbi.RegionSize,
                        (unsigned long)mbi.State, (unsigned long)mbi.Type,
                        (unsigned long)mbi.Protect);
            }
            next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (next <= cursor || next > UINT32_MAX)
                break;
            cursor = (uint32_t)next;
        }
    }
#endif
    return allocation;
}

static void guest_pe_platform_image_free(void *context, void *allocation)
{
    (void)context;
    if (!allocation)
        return;
#ifdef __vita__
    isaac_vita_fixed_free(allocation);
#else
    /* guest_entry owns the enclosing fixed reservation.  Decommit only the
     * authenticated image so a failed TLS/IAT publication can retry in the
     * same process instead of losing that reservation. */
    VirtualFree(allocation, GUEST_PE_EXPECTED_IMAGE_SIZE, MEM_DECOMMIT);
#endif
}

static const guest_pe_loader_ops g_guest_pe_loader_ops = {
    NULL,
    guest_pe_stdio_open,
    guest_pe_stdio_size,
    guest_pe_stdio_rewind,
    guest_pe_stdio_read,
    guest_pe_stdio_close,
    guest_pe_raw_alloc,
    guest_pe_raw_free,
    guest_pe_platform_image_alloc,
    guest_pe_platform_image_free
};

/* Replace host-loader-owned IAT contents with stable guest tokens.  The token
 * is the relocated address of the slot itself: unique, already meaningful in
 * the guest address space, and reversible without reserving a magic range. */
static int tag_imports(void)
{
    uint32_t i, previous = 0;
    for (i = 0; i < g_import_count; i++) {
        uint32_t slot = g_imports[i].slot_rva;
        if ((i && slot <= previous) || g_image_size < 4 || slot > g_image_size - 4) {
            fprintf(stderr, "guest: invalid generated IAT slot %08x at index %u\n",
                    slot, i);
            return 1;
        }
        *(uint32_t *)((unsigned char *)g_image_mem + slot) = g_image_base + slot;
        previous = slot;
    }
    return 0;
}

int guest_image_contains(uint32_t address, uint32_t size)
{
    uint32_t offset = address - g_image_base;
    return offset <= g_image_size && size <= g_image_size - offset;
}

/* Reproduce the part of the PE loader that ordinary LoadLibrary would own:
 * copy the static TLS template, write AddressOfIndex, and remember callbacks.
 * Generated code reads the result directly through fs:[0x2c][index], so a
 * zero-filled fake TIB without this directory processing is observably wrong. */
static int setup_static_tls(uint32_t directory_rva, uint32_t directory_size)
{
    unsigned char *directory;
    uint32_t start, end, index_address, callbacks_address, zero_fill;
    uint32_t template_size, total_size, i;

    if (!directory_rva && !directory_size) {
        g_tls_present = 0;
        return 0;
    }
    if (directory_size < 24U || g_image_size < 24U ||
        directory_rva > g_image_size - 24U) {
        fprintf(stderr, "guest: invalid IMAGE_TLS_DIRECTORY at %08x size %u\n",
                directory_rva, directory_size);
        return 1;
    }
    directory = (unsigned char *)g_image_mem + directory_rva;
    start             = *(uint32_t *)(directory + 0U);
    end               = *(uint32_t *)(directory + 4U);
    index_address     = *(uint32_t *)(directory + 8U);
    callbacks_address = *(uint32_t *)(directory + 12U);
    zero_fill         = *(uint32_t *)(directory + 16U);
    if (end < start) {
        fprintf(stderr, "guest: static TLS template ends before it starts\n");
        return 1;
    }
    template_size = end - start;
    if (zero_fill > 0x01000000U || template_size > 0x01000000U - zero_fill ||
        !guest_image_contains(start, template_size) ||
        !guest_image_contains(index_address, 4U)) {
        fprintf(stderr, "guest: invalid static TLS template/index range\n");
        return 1;
    }
    total_size = template_size + zero_fill;
        /* There is a single (read-only) template and each guest thread copies its own
          * instance -- Windows static TLS is per-thread. A single global block used to be kept,
          * so a new thread writing TLS polluted the main thread; measured as the main thread reading NULL then crashing on +0x64. */
    g_tls_template = guest_low_alloc(total_size ? total_size : 1U);
    g_guest_fs_base_cached = 0U;     /* fs:[0x2c][index] must be rebound */
    if (!g_tls_template) {
        fprintf(stderr, "guest: cannot allocate %u bytes of static TLS\n",
                total_size);
        return 1;
    }
    if (template_size)
        memcpy(g_tls_template, (void *)(uintptr_t)start, template_size);
        g_tls_block = g_tls_template;    /* the main thread uses the template directly */
    g_tls_block_size = total_size;
    g_tls_index = 0U;             /* first slot in our private fake TIB array */
    g_guest_fs_base_cached = 0U;
    *(uint32_t *)(uintptr_t)index_address = g_tls_index;

    g_tls_callback_count = 0U;
    if (callbacks_address) {
        for (i = 0; i < GUEST_TLS_CALLBACKS_MAX; ++i) {
            uint32_t callback;
            uint32_t cell = callbacks_address + i * 4U;
            if (!guest_image_contains(cell, 4U)) {
                fprintf(stderr, "guest: unterminated TLS callback array\n");
                return 1;
            }
            callback = *(uint32_t *)(uintptr_t)cell;
            if (!callback) break;
            if (!guest_image_contains(callback, 1U)) {
                fprintf(stderr, "guest: TLS callback %08x is outside the image\n",
                        callback);
                return 1;
            }
            g_tls_callbacks[g_tls_callback_count++] = callback;
        }
        if (i == GUEST_TLS_CALLBACKS_MAX) {
            fprintf(stderr, "guest: too many TLS callbacks\n");
            return 1;
        }
    }
    printf("guest: static TLS index %u, %u bytes, %u callback(s)\n",
           g_tls_index, g_tls_block_size, g_tls_callback_count);
    g_tls_present = 1;
    return 0;
}

/* * Where GUEST_CHECKED_MEMORY lands.
 *
  * This symbol used to be declared but never defined, so `G_CHK` stayed an empty macro -- every
  * address check was never compiled in. Illegal addresses segfaulted instead of calling guest_fault, so a crash
  * looked like "the host code crashed" when the source was guest code.
 *
  * The criterion is the weakest useful one: a 32-bit guest's legal address must fall in
  * 64 KiB .. 2 GiB. That is not enough to prove an address valid, but enough to make
  * obviously negative offsets like 0xfffffff4 surface immediately.
 */

/* Function-entry stack depth validation.
 *
  * Conclusion from 2026-11-02: guessing drift from "esp jumped up N bytes between two entries" is wrong --
  * esp necessarily rises on a normal return, so a 64-byte threshold misses and a 16-byte one cries wolf constantly.
  * Only two criteria genuinely avoid false positives:
 *
  *   1. the entry esp must lie within the guest stack (a hard boundary; outside it is a real error);
  *   2. entering the same function twice in a row, esp **must not rise**. Falling is legal: recursion,
  *      or a caller accumulating arguments for several cdecl calls on the stack (measured at 005ec152,
  *      called three times in a row, esp 12 lower each time, finally cleared by add esp,0x24).
  *      A rise means the previous frame was popped before unwinding -- that is real drift.
 *
  * Note the stronger form "esp must be equal on two consecutive entries" is wrong: the argument-
  * accumulation case above differs by 12 every time and would always false-positive. Measured and disproved; do not reuse.
 *
  * The more thorough check "does exit esp return to the entry value" comes from trace_stack.c's
  * -finstrument-functions paired probes -- that is the actual definition of stack balance.
  * The ring buffer only has entry values, so it cannot do this by nature; it is a fallback only.
 *
  * * 2026-11-03: the fallback criterion below ("same function entered twice but esp rose")
  *   was measured to be a **false-positive machine** and is now off by default (enable with STACK_TRACE=1).
  *   Reason: "two adjacent entries with the same id" in the ring does not mean recursion for a leaf function, only
  *   that "the same code called it twice in a row". The stack depth at those two call sites can differ,
  *   so esp naturally differs. Of 74347 measured warnings, the three most frequent ids were:
  *     12570 = sub_005ec14c  a one-instruction `jmp [IAT]` thunk (memmove family)
 *     12500 = sub_005eacd7  __security_check_cookie
  *     11892 = sub_005b9430  a small hot function
  *   These functions push nothing themselves; esp is entirely the caller's choice -- the criterion is
  *   always a false positive on them. To genuinely check stack balance use trace_stack.c's paired probes,
  *   not guesses from the entry ring.
  *   The "entry esp out of range" check below is a hard boundary (outside it is a real error) and stays unsuppressed.
 */
#define GUEST_FUNC_RING 256u
#define GUEST_FUNC_RING_MASK (GUEST_FUNC_RING - 1u)

static int g_stack_trace = -1;      /* -1 uninitialised; 1 = print drift warnings */
static unsigned long g_stack_warn_n;

/* Function-entry probe filtered by id (does not touch guest.h, so no need to rebuild 192 gen units).
 *
  * Purpose: confirm "did this translated function get entered, and with what arguments" when it lives in gen/
  * and cannot be edited. Every generated function body calls guest_coverage_function -> here at entry,
  * so editing here is equivalent to setting a breakpoint in gen/ at zero cost.
 *
  * Usage: GUEST_ENTER_PROBE=7972,11360
  *   id is the N in guest_coverage_function(N) (see the comment lines in gen/*.c).
  * At entry esp points at the return address; the first stack argument of cdecl/thiscall is at esp+4;
  * for thiscall, this is in ecx. Both are printed. */
#define PROBE_MAX 8
static uint32_t g_probe_ids[PROBE_MAX];
static int      g_probe_n = -1;
static unsigned long g_probe_hits[PROBE_MAX];

static int probe_match(uint32_t id, int *slot)
{
    if (g_probe_n < 0) {
        g_probe_n = 0;
        const char *e = getenv("GUEST_ENTER_PROBE");
        if (e) {
            const char *p = e;
            while (*p && g_probe_n < PROBE_MAX) {
                char *end;
                unsigned long v = strtoul(p, &end, 0);
                if (end == p) break;
                g_probe_ids[g_probe_n++] = (uint32_t)v;
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }
    for (int i = 0; i < g_probe_n; i++)
        if (g_probe_ids[i] == id) { if (slot) *slot = i; return 1; }
    return 0;
}

static void probe_report(uint32_t id, int slot, uint32_t esp)
{
    CPU *c = (CPU *)g_cur_cpu_diag;
    if (!c) return;
    g_probe_hits[slot]++;
        /* Print only the first 24 times to avoid flooding.
          * Exception: 309 (attribute lookup) is called extremely often, and its targeted dump filters by name,
          * so it gets a much larger cap, letting EA90_DUMP catch "the crashing instance". */
        /* 2026-11-04: 7972 = sub_00563120 (archive lookup entry). Reading resources from .a does
          * not fopen per file, but every lookup passes through here -- to confirm "was this image requested",
          * this is the cleanest layer. Give it a large quota; volume is controlled by the filter below. */
    if (g_probe_hits[slot] > (id == 309u ? 2000000u :
                              id == 7972u ? 2000000u : 24u)) return;
    uint32_t arg0 = 0, retaddr = 0;
    uint32_t p = esp;
    if (p >= 0x40000000u && p < 0x40400000u) {
        retaddr = *(uint32_t *)(uintptr_t)p;
        arg0    = *(uint32_t *)(uintptr_t)(p + 4u);
    }
    char txt[160]; txt[0] = 0;
    if (arg0 >= 0x10000u && arg0 < 0xC0000000u) {
        int k = 0;
        for (; k < 150; k++) {
            uint8_t ch = *(uint8_t *)(uintptr_t)(arg0 + (uint32_t)k);
            if (!ch) break;
            txt[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
        }
        txt[k] = 0;
    }
        /* 7972 prints only image/animation related paths (wrapped in if, no goto). */
    if (id == 7972u && !(strstr(txt, "intro") || strstr(txt, "cutscene") ||
                         strstr(txt, "Intro"))) {
                /* skip */
    } else {
        fprintf(stderr,
            "[enter] id=%u #%lu from=%08x ecx=%08x arg0=%08x \"%s\"  esp=%08x\n",
            id, g_probe_hits[slot], retaddr, c->ecx, arg0, txt, esp);
    }
        /* Function 309 = sub_0000ea90: looks up a name in a hash bucket chain.
          * At the crash the list it walks held a wild pointer (0x652dd010 / 0x00000004).
          * This dumps the map object and the first few chain nodes in full at entry,
          * to determine what the node structure really is and which field is broken. */
        /* Targeted dump for attribute lookup (sub_0000ea90).
     *
          * Crash chain: 0x48d5e5 looks up "gridgfxroot" -> returns 0 -> 0x48d604 reads [0+4]
          * -> garbage used as a string -> segfault. backdrops.xml definitely contains a gridgfxroot attribute,
          * so either [node+0x20] is not first_attribute, or the chain walk/compare is wrong.
          * This filters by name and prints the node header plus the first few chain entries. */
        /* 310 = sub_0000eb10 (find a child node by name). anm2 parsing descends through it:
          * AnimatedActor -> Content -> Spritesheets, then compares the Path attribute inline.
          * With ANM_DUMP=1 it prints the structure of each looked-up node to check child/attribute chains.
          * 32-bit rapidxml layout: +00 name  +08 name_size  +18 first_node
     *                      +20 first_attribute  +0c value_size  +04 value */
    if (id == 310 && getenv("ANM_DUMP")) {
        char qn[40] = {0};
        if (arg0 >= 0x10000u && arg0 < 0xB0000000u) {
            for (int k = 0; k < 36; k++) {
                uint8_t ch = *(uint8_t *)(uintptr_t)(arg0 + (uint32_t)k);
                if (!ch) break;
                qn[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
            }
        }
        uint32_t t = c->ecx;
        if (t >= 0x10000u && t < 0xB0000000u) {
            uint32_t nm  = *(uint32_t *)(uintptr_t)t;
            uint32_t nsz = *(uint32_t *)(uintptr_t)(t + 8u);
            uint32_t fn  = *(uint32_t *)(uintptr_t)(t + 0x18u);
            uint32_t fa  = *(uint32_t *)(uintptr_t)(t + 0x20u);
            char nn[32] = {0};
            if (nm >= 0x10000u && nm < 0xC0000000u && nsz < 30u) {
                for (uint32_t k = 0; k < nsz; k++) {
                    uint8_t ch = *(uint8_t *)(uintptr_t)(nm + k);
                    nn[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                }
            }
            fprintf(stderr, "[anm] lookup \"%s\" @parent%08x (own name=\"%s\" len=%u) "
                            "first_node=%08x first_attr=%08x\n",
                    qn, t, nn, nsz, fn, fa);
                        /* If the parent has first_node, print that child's name and its attribute names
                          * -- under anm2's <Spritesheets> sits <Spritesheet Path=.../> */
            if (fn >= 0x10000u && fn < 0xB0000000u) {
                uint32_t snm = *(uint32_t *)(uintptr_t)fn;
                uint32_t ssz = *(uint32_t *)(uintptr_t)(fn + 8u);
                uint32_t sfa = *(uint32_t *)(uintptr_t)(fn + 0x20u);
                char sn[32] = {0};
                if (snm >= 0x10000u && snm < 0xC0000000u && ssz < 30u) {
                    for (uint32_t k = 0; k < ssz; k++) {
                        uint8_t ch = *(uint8_t *)(uintptr_t)(snm + k);
                        sn[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                    }
                }
                                /* Also look at this child's own first_node -- in anm2
                                  * Spritesheets' first_node is the first <Spritesheet>. */
                uint32_t sfn = *(uint32_t *)(uintptr_t)(fn + 0x18u);
                uint32_t snx = *(uint32_t *)(uintptr_t)(fn + 0x1cu);
                fprintf(stderr, "[anm]    child node %08x name=\"%s\" first_attr=%08x "
                                "first_node=%08x last_node=%08x",
                        fn, sn, sfa, sfn, snx);
                                /* If the child itself has first_node (e.g. Spritesheets -> Spritesheet),
                                  * print that grandchild's name and its first 3 attributes. */
                if (sfn >= 0x10000u && sfn < 0xB0000000u) {
                    uint32_t gnm = *(uint32_t *)(uintptr_t)sfn;
                    uint32_t gsz = *(uint32_t *)(uintptr_t)(sfn + 8u);
                    char gn[24] = {0};
                    if (gnm >= 0x10000u && gnm < 0xC0000000u && gsz < 20u)
                        for (uint32_t k = 0; k < gsz; k++) {
                            uint8_t ch = *(uint8_t *)(uintptr_t)(gnm + k);
                            gn[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                        }
                    fprintf(stderr, "\n[anm]      grandchild node %08x name=\"%s\" attributes:", sfn, gn);
                    uint32_t ga = *(uint32_t *)(uintptr_t)(sfn + 0x20u);
                    for (int q = 0; q < 4 && ga >= 0x10000u && ga < 0xB0000000u; q++) {
                        uint32_t anm3 = *(uint32_t *)(uintptr_t)ga;
                        uint32_t av  = *(uint32_t *)(uintptr_t)(ga + 4u);
                        uint32_t as3 = *(uint32_t *)(uintptr_t)(ga + 8u);
                        uint32_t vs3 = *(uint32_t *)(uintptr_t)(ga + 0xcu);
                        char n3[20] = {0}, v3[36] = {0};
                        if (anm3 >= 0x10000u && anm3 < 0xC0000000u && as3 < 18u)
                            for (uint32_t k = 0; k < as3; k++) {
                                uint8_t ch = *(uint8_t *)(uintptr_t)(anm3 + k);
                                n3[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                            }
                        if (av >= 0x10000u && av < 0xC0000000u && vs3 < 32u)
                            for (uint32_t k = 0; k < vs3; k++) {
                                uint8_t ch = *(uint8_t *)(uintptr_t)(av + k);
                                v3[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                            }
                        fprintf(stderr, " %s=\"%s\"(len %u)", n3, v3, vs3);
                        ga = *(uint32_t *)(uintptr_t)(ga + 0x18u);
                    }
                }
                if (sfa >= 0x10000u && sfa < 0xB0000000u) {
                    uint32_t anm2 = *(uint32_t *)(uintptr_t)sfa;
                    uint32_t asz = *(uint32_t *)(uintptr_t)(sfa + 8u);
                    char an[24] = {0};
                    if (anm2 >= 0x10000u && anm2 < 0xC0000000u && asz < 20u)
                        for (uint32_t k = 0; k < asz; k++) {
                            uint8_t ch = *(uint8_t *)(uintptr_t)(anm2 + k);
                            an[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                        }
                    fprintf(stderr, " -> attr name=\"%s\" len=%u next_attr=%08x",
                            an, asz, *(uint32_t *)(uintptr_t)(sfa + 0x18u));
                }
                fprintf(stderr, "\n");
            }
            fflush(NULL);
        }
    }
    if (id == 309 && getenv("EA90_DUMP")) {
                /* Read the name string (at most 40 bytes) */
        char nm[48]; nm[0] = 0;
        if (arg0 >= 0x10000u && arg0 < 0xB0000000u) {
            int k = 0;
            for (; k < 40; k++) {
                uint8_t ch = *(uint8_t *)(uintptr_t)(arg0 + (uint32_t)k);
                if (!ch) break;
                nm[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
            }
            nm[k] = 0;
        }
                /* Filter by **name** without deduping on this -- one this looks up several attribute names,
                  * and the crash was on the second name of the same node. */
        static int ngrid;
        int want = (strcmp(nm, "gridgfxroot") == 0);
        uint32_t t = c->ecx;
        if (t >= 0x10000u && t < 0xB0000000u &&
            (!want || ngrid++ < 6)) {
            if (want) {
                fprintf(stderr, "[ea90] ★★★ scene of the gridgfxroot lookup\n");
                                /* 2026-11-03: this produced the conclusion (pool_dump.bin, 8176 bytes):
                                  *   the pool held only <backdrops\0gfxroot\0"gfx/backdrop/\0> ,
                                  *   gridgfxroot / effectsgfxroot were entirely absent.
                                  *   Attributes of child <backdrop .../> elements (values not ending in '/') all parsed fine.
                                  *   => when an attribute value ends in '/', the parser treats it as the self-closing "/>",
                                  *     jumping to the element's closing '>' and swallowing every later attribute. */
                fprintf(stderr, "[ea90]   (pool dump removed; see section 13 of the handoff)\n");
            }
            fprintf(stderr, "[ea90] === this=%08x name=\"%s\" ===\n", t, nm);
            fprintf(stderr, "[ea90]   offsets +0..+0x20:");
            for (int z = 0; z < 9; z++) {
                uint32_t a = t + (uint32_t)z * 4u;
                fprintf(stderr, "\n[ea90]     +%02x @%08x = %08x", z * 4, a,
                        *(uint32_t *)(uintptr_t)a);
            }
            fprintf(stderr, "\n");
                        /* Print the real strings each chain node's name/value pointers point at.
                          * That shows directly "which attributes are actually attached to the list". */
            {
                static const uint32_t poolbase[] = { 0x6058f010u };
                (void)poolbase;
                uint32_t nb = *(uint32_t *)(uintptr_t)(t + 0x20u);
                fprintf(stderr, "[ea90]   attribute chain (including name/value entities):\n");
                for (int k = 0; k < 8 && nb; k++) {
                    if (nb < 0x10000u || nb >= 0xB0000000u) {
                        fprintf(stderr, "[ea90]     attr%d = %08x ★ wild\n", k, nb);
                        break;
                    }
                    uint32_t an = *(uint32_t *)(uintptr_t)nb;
                    uint32_t av = *(uint32_t *)(uintptr_t)(nb + 4u);
                    uint32_t asz = *(uint32_t *)(uintptr_t)(nb + 8u);
                    uint32_t vsz = *(uint32_t *)(uintptr_t)(nb + 0xcu);
                    char anb[40] = {0}, avb[48] = {0};
                    if (an >= 0x10000u && an < 0xC0000000u)
                        for (int z = 0; z < 32; z++) {
                            uint8_t ch = *(uint8_t *)(uintptr_t)(an + (uint32_t)z);
                            if (!ch) break; anb[z] = (char)((ch>=0x20&&ch<0x7f)?ch:'?');
                        }
                    if (av >= 0x10000u && av < 0xC0000000u)
                        for (int z = 0; z < 40; z++) {
                            uint8_t ch = *(uint8_t *)(uintptr_t)(av + (uint32_t)z);
                            if (!ch) break; avb[z] = (char)((ch>=0x20&&ch<0x7f)?ch:'?');
                        }
                    fprintf(stderr, "[ea90]     attr%d @%08x name=\"%s\"(len %u) "
                                    "value=\"%s\"(len %u) next=%08x\n",
                            k, nb, anb, asz, avb, vsz,
                            *(uint32_t *)(uintptr_t)(nb + 0x18u));
                    nb = *(uint32_t *)(uintptr_t)(nb + 0x18u);
                }
            }
            uint32_t node = *(uint32_t *)(uintptr_t)(t + 0x20u);
            for (int k = 0; k < 4 && node; k++) {
                if (node < 0x10000u || node >= 0xB0000000u) {
                    fprintf(stderr, "[ea90]   node%d = %08x ★ wild\n", k, node);
                    break;
                }
                fprintf(stderr, "[ea90]   node%d @%08x:", k, node);
                for (int z = 0; z < 10; z++)
                    fprintf(stderr, " %08x", *(uint32_t *)(uintptr_t)(node + (uint32_t)z * 4u));
                fprintf(stderr, "\n");
                node = *(uint32_t *)(uintptr_t)(node + 0x18u);
            }
            fflush(NULL);
        }
    }
    if (id == 7996) {
        uint32_t t = c->ecx;
        fprintf(stderr, "[enter]      this=%08x [+0]=%08x [+4]=%08x [+8]=%08x [+c]=%08x"
                        " [+0x10]=%08x\n",
                t, *(uint32_t*)(uintptr_t)t, *(uint32_t*)(uintptr_t)(t+4),
                *(uint32_t*)(uintptr_t)(t+8), *(uint32_t*)(uintptr_t)(t+0xc),
                *(uint32_t*)(uintptr_t)(t+0x10));
        uint32_t beg = *(uint32_t*)(uintptr_t)(t+8);
        uint32_t end = *(uint32_t*)(uintptr_t)(t+0xc);
        if (beg >= 0x48000000u && beg < 0xC0000000u && end > beg && end - beg <= 256u) {
            for (uint32_t q = beg; q < end; q += 4u) {
                uint32_t obj = *(uint32_t*)(uintptr_t)q;
                char mp[120]; mp[0] = 0;
                if (obj >= 0x10000u && obj < 0xC0000000u) {
                    uint32_t sp = *(uint32_t*)(uintptr_t)(obj + 0xcu);
                    if (sp >= 0x10000u && sp < 0xC0000000u) {
                        uint32_t cap = *(uint32_t*)(uintptr_t)(sp + 0x14);
                        uint32_t dat = (cap >= 0x10u) ? *(uint32_t*)(uintptr_t)sp : sp;
                        uint32_t len = *(uint32_t*)(uintptr_t)(sp + 0x10);
                        if (len < 100u) {
                            for (uint32_t k2 = 0; k2 < len; k2++) {
                                uint8_t ch = *(uint8_t*)(uintptr_t)(dat + k2);
                                mp[k2] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                            }
                            mp[len] = 0;
                        }
                    }
                }
                fprintf(stderr, "[enter]      mount[%08x] obj=%08x \"%s\"\n", q, obj, mp);
                if (obj >= 0x48000000u && obj < 0xC0000000u) {
                    fprintf(stderr, "[enter]        bytes:");
                    for (int z = 0; z < 48; z++)
                        fprintf(stderr, " %02x", *(uint8_t*)(uintptr_t)(obj + (uint32_t)z));
                    fprintf(stderr, "\n[enter]        ascii:");
                    for (int z = 0; z < 48; z++) {
                        uint8_t ch = *(uint8_t*)(uintptr_t)(obj + (uint32_t)z);
                        fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    if (arg0 >= 0x10000u && arg0 < 0xC0000000u) {
        fprintf(stderr, "[enter]      raw:");
        for (int z = 0; z < 24; z++)
            fprintf(stderr, " %02x", *(uint8_t *)(uintptr_t)(arg0 + (uint32_t)z));
        fprintf(stderr, "\n");
    }
    fflush(NULL);
}

void guest_note_function_enter(uint32_t id)
{
    uint32_t now_esp = g_cur_cpu_diag ? ((CPU *)g_cur_cpu_diag)->esp : 0u;
    {
        int slot = 0;
        if (probe_match(id, &slot)) probe_report(id, slot, now_esp);
    }
    if (now_esp) {
        if (g_stack_top && (now_esp > g_stack_top || now_esp < g_stack_base)) {
            fprintf(stderr,
                    "[stack] ★ entry esp out of range: %u@%08x (stack %08x..%08x)\n",
                    id, now_esp, g_stack_base, g_stack_top);
        }
        if (g_func_ring_n > 0 && g_stack_trace != 0) {
            uint32_t prev_id  = g_func_ring[(g_func_ring_n - 1) & GUEST_FUNC_RING_MASK];
            uint32_t prev_esp = g_esp_ring[(g_func_ring_n - 1) & GUEST_FUNC_RING_MASK];
            if (prev_esp && id == prev_id && now_esp > prev_esp) {
                if (g_stack_trace < 0)
                    g_stack_trace = getenv("STACK_TRACE") != NULL;
                g_stack_warn_n++;
                                /* Keep only the first 32 as evidence that it really hits; no flooding. */
                if (g_stack_trace || g_stack_warn_n <= 32)
                    fprintf(stderr,
                            "[stack] ★ %u consecutive entries but ESP rose: %08x -> %08x (delta %+d)"
                            " - the previous call did not unwind cleanly\n",
                            id, prev_esp, now_esp, (int)(now_esp - prev_esp));
            }
        }
    }
    g_func_ring[g_func_ring_n & GUEST_FUNC_RING_MASK] = id;
    g_esp_ring[g_func_ring_n & GUEST_FUNC_RING_MASK] = now_esp;
    g_last_func_id = id;
    g_func_ring_n++;
}

uint32_t g_last_func_id = 0;
uint32_t g_func_ring[GUEST_FUNC_RING];
uint32_t g_esp_ring[GUEST_FUNC_RING];
uint32_t g_func_ring_n = 0;
extern void *g_cur_cpu_diag;   /* current CPU pointer, set in host_main.c */

__attribute__((noinline))
/* Wild-address guard: the address lies inside the low-4GB heap range but beyond the bump cursor --
  * that is, this address was **never mmap'd**.
 *
  * 2026-11-03: the allocator is bump plus a free list and the address only moves forward, so
  * g_heap_cur is the upper bound of the mapped region. Any heap address >= it is wild,
  * and the range check above (0x10000..0xC0000000) lets such addresses through,
  * so it crashed inside ld32 with only an isolated address as evidence -- extremely hard to localise.
 *
  * This catches it early: on a hit it prints the function ID plus caller, then lets it through
  * (no behaviour change, just giving a crash some context). */
extern uintptr_t heap_bump_cursor(void);
uintptr_t g_last_wild_addr;
unsigned  g_wild_n;

/* Access-trace ring for function 309 (sub_0000ea90, anm2 layer name lookup).
 *
  * The wild address 0x652dd010 is in no readable memory (all 16 segments from maps were scanned),
  * so it is **computed**. To see which valid address turned into it step by step,
  * the only option is to record what was accessed before. The ring is small, allocation-free and IO-free. */
#define EA90_RING 64u
static struct { uint32_t addr; uint32_t size; uint8_t write; } g_ea90_ring[EA90_RING];
static unsigned g_ea90_n;

/* Stack-slot write watch: STACK_SLOT_WATCH=403ee3bc,403ee3b8 (hex, comma separated)
 *
  * [ebp-0x1036c] is written twice inside sub_0048d3f0:
 *   0x48d457  ← [ebx+8]
 *   0x48d8db  ← [esi+0x20]
  * At the crash its value pointed at "backdrops.xml" on the stack, but neither source should be it.
  * Watching this slot tells you who really wrote it and what.
  * The guest stack has no ASLR, so addresses are identical every run and can be watched absolutely. */
static uint32_t g_slot_watch[4];
static int      g_slot_n = -1;
static unsigned g_slot_hits[4];

static void slot_watch(uint32_t addr, int write)
{
    if (g_slot_n < 0) {
        g_slot_n = 0;
        const char *e = getenv("STACK_SLOT_WATCH");
        if (e) {
            const char *q = e;
            while (*q && g_slot_n < 4) {
                char *end;
                unsigned long v = strtoul(q, &end, 16);
                if (end == q) break;
                g_slot_watch[g_slot_n++] = (uint32_t)v;
                q = (*end == ',') ? end + 1 : end;
            }
        }
    }
    for (int i = 0; i < g_slot_n; i++) {
        if (g_slot_watch[i] != addr) continue;
        if (g_slot_hits[i]++ >= 12) return;
        fprintf(stderr, "[slot] ★ %s %08x (occurrence %u) functionID=%u\n",
                write ? "write" : "read", addr, g_slot_hits[i], g_last_func_id);
        fprintf(stderr, "[slot]   last 8 function IDs:");
        { unsigned st = g_func_ring_n > 8 ? g_func_ring_n - 8 : 0;
          for (unsigned k = st; k < g_func_ring_n; k++)
              fprintf(stderr, " %u", g_func_ring[k & 255u]); }
        fprintf(stderr, "\n");
        if (g_cur_cpu_diag) {
            CPU *k = (CPU *)g_cur_cpu_diag;
            fprintf(stderr, "[slot]   eax=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x\n",
                    k->eax, k->ebx, k->esi, k->edi, k->ebp);
        }
        fflush(NULL);
    }
}

static void ea90_trace(uint32_t addr, uint32_t size, int write)
{
    if (g_last_func_id != 309u) return;
    g_ea90_ring[g_ea90_n % EA90_RING].addr  = addr;
    g_ea90_ring[g_ea90_n % EA90_RING].size  = size;
    g_ea90_ring[g_ea90_n % EA90_RING].write = (uint8_t)write;
    g_ea90_n++;
}

static void ea90_dump(void)
{
    unsigned lim = g_ea90_n < EA90_RING ? g_ea90_n : EA90_RING;
    fprintf(stderr, "[ea90] function 309, last %u memory accesses (newest first):\n", lim);
    for (unsigned i = 0; i < lim; i++) {
        unsigned k = (g_ea90_n - 1u - i) % EA90_RING;
        fprintf(stderr, "[ea90]   %08x %s %u\n",
                g_ea90_ring[k].addr,
                g_ea90_ring[k].write ? "write" : "read",
                g_ea90_ring[k].size);
    }
    fflush(NULL);
}

/* Global access ring (WILD_TRACE=1): records the last 256 guest memory accesses as
  * {function id, address, size, read/write}. A wild-address hit dumps the whole ring.
 *
  * Purpose: the crash site reveals only "read 0x652dd010", while the real error
  * happened several steps earlier. With this trace you can see how the address turned from a valid value
  * into a wild one -- far faster than disassembling afterwards and guessing a path. */
#define WTRACE_N 256u
static struct {
        uint32_t func;      /* function id in effect when the access happened */
    uint32_t addr;
    uint32_t size;
    uint8_t  write;
} g_wtrace[WTRACE_N];
static unsigned g_wtrace_n;
static int g_wtrace_on = -1;

static void wtrace_rec(uint32_t func, uint32_t addr, uint32_t size, int write)
{
    if (g_wtrace_on < 0) g_wtrace_on = getenv("WILD_TRACE") != NULL;
    if (!g_wtrace_on) return;
    unsigned k = g_wtrace_n % WTRACE_N;
    g_wtrace[k].func  = func;
    g_wtrace[k].addr  = addr;
    g_wtrace[k].size  = size;
    g_wtrace[k].write = (uint8_t)write;
    g_wtrace_n++;
}

static void wtrace_dump(void)
{
    unsigned lim = g_wtrace_n < WTRACE_N ? g_wtrace_n : WTRACE_N;
    fprintf(stderr, "[wtrace] last %u guest accesses (newest first; ★=wild address):\n", lim);
    for (unsigned i = 0; i < lim; i++) {
        unsigned k = (g_wtrace_n - 1u - i) % WTRACE_N;
        uint32_t a = g_wtrace[k].addr;
        int wild = (a >= 0x48000000u && a < 0xB0000000u &&
                    (uintptr_t)a >= heap_bump_cursor());
        fprintf(stderr, "[wtrace]   %s function%-6u %s %u @%08x\n",
                wild ? "★" : " ", g_wtrace[k].func,
                g_wtrace[k].write ? "write" : "read",
                g_wtrace[k].size, a);
    }
    fflush(NULL);
}

static void wild_addr_probe(uint32_t addr, uint32_t size, int write)
{
        if (addr < 0x48000000u || addr >= 0xB0000000u) return;   /* outside the heap range */
    uintptr_t cur = heap_bump_cursor();
        if ((uintptr_t)addr < cur) return;                        /* inside the mapped region */
    g_last_wild_addr = addr;
    if (g_wild_n++ < 8) {
        extern void ea90_dump(void);
        extern void wtrace_dump(void);
        if (g_last_func_id == 309u) ea90_dump();
        wtrace_dump();
        fprintf(stderr,
            "[wild] ★ wild address addr=%08x size=%u %s - beyond the heap cursor %08lx (delta %ld bytes)"
            " functionID=%u\n",
            addr, size, write ? "write" : "read", (unsigned long)cur,
            (long)((uintptr_t)addr - cur), g_last_func_id);
        { unsigned st = g_func_ring_n > 16 ? g_func_ring_n - 16 : 0;
          fprintf(stderr, "[wild]   last 16 function IDs:");
          for (unsigned i = st; i < g_func_ring_n; i++)
              fprintf(stderr, " %u", g_func_ring[i & 255u]);
          fprintf(stderr, "\n"); }
        if (g_cur_cpu_diag) {
            CPU *k = (CPU *)g_cur_cpu_diag;
                        /* rapidxml string pool (6058f010, 8192 bytes):
                          * How much the pool holds = how much the parser actually consumed.
                          * If the pool lacks "gridgfxroot", it was lost during parsing;
                          * if present, it parsed but was never attached to the attribute chain. */
            {
                uint32_t pool = 0x6058f010u;
                size_t psz = 0;
                extern int heap_block_state(uintptr_t, size_t *);
                int pst = heap_block_state(pool, &psz);
                if (pst == 1) {
                                        /* Do as little as possible in a signal handler: write the pool to a file and analyse offline.
                                          * fopen/fwrite rather than open/write -- the latter needs extra headers,
                                          * and stderr is already unbuffered; the fprintf path is proven in this project. */
                    {
                        FILE *pf = fopen("pool_dump.bin", "wb");
                        if (pf) {
                            size_t w1 = fwrite((void *)(uintptr_t)pool, 1, psz, pf);
                            fclose(pf);
                            fprintf(stderr, "[pool] dumped %zu bytes to pool_dump.bin\n", w1);
                            fflush(NULL);
                        }
                    }
                } else {
                    fprintf(stderr, "[pool] block state=%d unusable\n", pst);
                }
            }
            fprintf(stderr, "[wild]   regs: eax=%08x ebx=%08x ecx=%08x edx=%08x"
                            " esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                    k->eax, k->ebx, k->ecx, k->edx, k->esi, k->edi, k->ebp, k->esp);
                        /* this's bucket head (sub_0000ea90 uses [ecx+0x20]) */
            if (k->ecx >= 0x10000u && k->ecx < 0xB0000000u) {
                fprintf(stderr, "[wild]   [ecx]=%08x [ecx+0x20]=%08x\n",
                        *(uint32_t *)(uintptr_t)k->ecx,
                        *(uint32_t *)(uintptr_t)(k->ecx + 0x20u));
            }
                        /* Search the whole esp..ebp frame for the wild value itself -- it must be some
                          * local variable (the crash site only reads it; it cannot create it). */
            {
                uint32_t base = k->esp < k->ebp ? k->esp : k->ebp;
                uint32_t top  = k->esp < k->ebp ? k->ebp : k->esp;
                if (top - base < 0x40000u && base >= 0x40000000u && top < 0x40400000u) {
                    fprintf(stderr, "[wild]   searching the stack frame [%08x,%08x) for %08x:\n",
                            base, top, addr);
                    int hits = 0;
                    for (uint32_t a = base; a + 4 <= top; a += 4) {
                        uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4);
                                                /* * It stores the "base"; only +0/+4/+8 dereferenced becomes fault_addr.
                                                  * Comparing addr alone misses forms like [eax+4] --
                                                  * an entire wasted scan came from missing those 4 bytes. */
                        if (v != addr && v + 4u != addr && v + 8u != addr &&
                            v + 0x0cu != addr && v + 0x10u != addr &&
                            v - 4u != addr && v - 8u != addr) continue;
                        if (hits++ >= 6) break;
                        fprintf(stderr, "[wild]     ★ stack %08x holds the value (ebp%+d)\n",
                                a, (int)a - (int)k->ebp);
                                                /* 6 nearby dwords as structural clues */
                        fprintf(stderr, "[wild]       window:");
                        for (int w = -4; w <= 4; w++) {
                            int32_t aa = (int32_t)a + w * 4;
                            if (aa < (int32_t)base || aa + 4 > (int32_t)top) continue;
                            uint32_t vv; memcpy(&vv, (void *)(uintptr_t)aa, 4);
                            fprintf(stderr, " %s%08x", w == 0 ? "*" : "", vv);
                        }
                        fprintf(stderr, "\n");
                    }
                    if (!hits) fprintf(stderr, "[wild]     (not in the stack frame)\n");
                }
            }
                        /* sub_0048d3f0 local: [ebp-0x1036c] holds [esi+0x20],
                          * and 0x48e165 reads it then does mov ecx,[eax+4] -- if it is 0,
                          * it reads address 4. Printing this slot settles it. */
            {
                static const int32_t offs[] = {
                    -0x1036c, -0x10370, -0x10364, -0x10368,
                    -0x10374, -0x10378, -0x10360, -0x1035c,
                    -0x1037c, -0x10380, -0x10384, -0x10388 };
                fprintf(stderr, "[wild]   sub_0048d3f0 local slots (ebp=%08x):\n", k->ebp);
                for (int i = 0; i < 12; i++) {
                    int32_t a = (int32_t)k->ebp + offs[i];
                    if (a < 0x40000000 || a + 4 > 0x40400000) continue;
                    uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4);
                    fprintf(stderr, "[wild]     [ebp%+d] @%08x = %08x%s\n",
                            (int)offs[i], (uint32_t)a, v,
                            (v == 0) ? "   ★ is 0" : "");
                                        /* If the slot is a readable pointer, print what it points at too --
                                          * the crashing instruction is mov ecx,[eax+4] and eax comes from here. */
                    if (v >= 0x30000000u && v < 0x40400000u) {
                        uint32_t w0 = 0, w1 = 0, w2 = 0;
                        if (v + 12u < 0x40400000u) {
                            w0 = *(uint32_t *)(uintptr_t)v;
                            w1 = *(uint32_t *)(uintptr_t)(v + 4u);
                            w2 = *(uint32_t *)(uintptr_t)(v + 8u);
                        }
                        fprintf(stderr, "[wild]        → [%08x]=%08x [+4]=%08x [+8]=%08x\n",
                                v, w0, w1, w2);
                    }
                }
            }
                        /* 6058f010 is an object pointer saved on the stack; esi once equalled 6058f025
                          * (= 6058f010 + 0x15, not 4-aligned) -- pointing at an internal string field.
                          * Print it and its neighbours to confirm the object type. */
            {
                static const uint32_t cand[] = { 0x6058f010u, 0x6058a010u, 0x5fec2010u };
                for (int ci = 0; ci < 3; ci++) {
                    uint32_t q = cand[ci];
                    size_t bsz = 0;
                    extern int heap_block_state(uintptr_t, size_t *);
                    int st = heap_block_state(q, &bsz);
                    fprintf(stderr, "[wild]   object %08x block state=%d size=%zu:",
                            q, st, bsz);
                    for (int z = 0; z < 10; z++)
                        fprintf(stderr, " %08x", *(uint32_t *)(uintptr_t)(q + (uint32_t)z * 4u));
                    fprintf(stderr, "\n[wild]      ascii:");
                    for (int z = 0; z < 40; z++) {
                        uint8_t ch = *(uint8_t *)(uintptr_t)(q + (uint32_t)z);
                        fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
                    }
                    fprintf(stderr, "\n");
                }
            }
                        /* Top 16 dwords of the stack, for backtracking clues */
            fprintf(stderr, "[wild]   stack top (esp=%08x):", k->esp);
            for (int z = 0; z < 16; z++) {
                uint32_t a = k->esp + (uint32_t)z * 4u;
                if (a < 0x40000000u || a >= 0x40400000u) break;
                fprintf(stderr, " %08x", *(uint32_t *)(uintptr_t)a);
            }
            fprintf(stderr, "\n");
        }
        fflush(NULL);
    }
}

/* * 2026-11-06: the master diagnostic switch moved to a global so guest.h's inline fast path can read it.
 *
  * It used to be a function-static inside guest_check, and guest_check is an external function,
  * so every memory access had to make a real call to decide -- the profile showed 49.7% of CPU.
 *
  * Now: the normal path reads this variable inline (one ldr) and only enters the slow path when non-zero.
  * Semantics unchanged: with any diagnostic switch on, every access still takes the full check.
 *
  * Initialised in a constructor to avoid the race of "only learning at first access whether diagnostics are on":
  * generated code may touch memory during static initialisation before main. */
volatile int g_guest_diag_on = 0;

__attribute__((constructor))
static void guest_diag_gate_init(void)
{
    g_guest_diag_on = (getenv("WILD_TRACE") || getenv("EA90_DUMP") ||
                       getenv("STACK_SLOT_WATCH") || getenv("GUEST_DIAG")) ? 1 : 0;
    if (g_guest_diag_on)
        fprintf(stderr, "[guest] diagnostic mode on - every memory access is fully checked (3x slower)\n");
}

void guest_check_slow(uint32_t addr, uint32_t size, int write)
{
        /* Diagnostic hook: reached only when g_guest_diag_on is non-zero,
          * and that switch was already tested in guest.h's G_CHK. */
    {
        if (g_guest_diag_on) {
            ea90_trace(addr, size, write);
            wtrace_rec(g_last_func_id, addr, size, write);
            slot_watch(addr, write);
            wild_addr_probe(addr, size, write);
        }
    }
    (void)write;
        /* Upper bound relaxed from 2 GiB to 3 GiB: unpacked resources genuinely need the guest heap
          * to exceed 1.5 GB (textures + audio + Lua). Addresses at or above 0xC0000000 are still
          * always illegal, so negative offsets like 0xfffffff4 are still caught. */
    if (addr < 0x10000u || addr >= 0xC0000000u ||
        (uint64_t)addr + (uint64_t)size > 0xC0000000ull) {
        fprintf(stderr,
                "[guest] illegal address addr=0x%08x size=%u write=%d functionID=%u\n",
                addr, size, write, g_last_func_id);
        { extern void *g_cur_cpu_diag;
          { unsigned st = g_func_ring_n > 48 ? g_func_ring_n - 48 : 0;
            fprintf(stderr, "[guest] last entered function IDs:");
            for (unsigned i = st; i < g_func_ring_n; i++)
                fprintf(stderr, " %u@%08x", g_func_ring[i & 255u], g_esp_ring[i & 255u]);
            fprintf(stderr, "\n"); }
          fprintf(stderr, "[guest] host return address lv0=%p\n", __builtin_return_address(0));
          { extern void *g_cur_cpu_diag;
            if (g_cur_cpu_diag) { CPU *k = (CPU *)g_cur_cpu_diag;
              fprintf(stderr, "[guest] top 12 dwords (from esp=%08x):\n  ", k->esp);
              for (int _i = 0; _i < 12; _i++) {
                  uint32_t a = k->esp + (uint32_t)_i * 4u;
                  if (a < 0x10000u || a >= 0xC0000000u) break;
                  fprintf(stderr, "%08x ", *(uint32_t *)(uintptr_t)a);
              }
              fprintf(stderr, "\n"); } }
          if (g_cur_cpu_diag) {
            CPU *k = (CPU *)g_cur_cpu_diag;
            fprintf(stderr,
              "[guest] eax=%08x ebx=%08x ecx=%08x edx=%08x "
              "esi=%08x edi=%08x ebp=%08x esp=%08x\n",
              k->eax, k->ebx, k->ecx, k->edx, k->esi, k->edi, k->ebp, k->esp);
          } }
                /* Use host return addresses to locate which translated function is accessing out of bounds.
         *
                  * At O0 ld32/st32 are not inlined, so lv0 points at the access helper in
                  * guest.h and lv1/lv2 are the generated code. Symbolicating each with dladdr yields
                  * sub_005eXXXX+0xNN directly, with no need to recover the ASLR base via nm. */
        {
            void *frames[4];
            frames[0] = __builtin_return_address(0);
            frames[1] = __builtin_return_address(1);
            frames[2] = __builtin_return_address(2);
            frames[3] = __builtin_return_address(3);
            for (int _i = 0; _i < 4; _i++) {
                Dl_info _di;
                if (frames[_i] && dladdr(frames[_i], &_di)) {
                    fprintf(stderr,
                            "[guest] host return address lv%d = %p  %s+0x%lx  (base %p)\n",
                            _i, frames[_i],
                            _di.dli_sname ? _di.dli_sname : "?",
                            _di.dli_saddr
                                ? (unsigned long)((uintptr_t)frames[_i] -
                                                  (uintptr_t)_di.dli_saddr)
                                : 0ul,
                            _di.dli_fbase);
                } else {
                    fprintf(stderr, "[guest] host return address lv%d = %p  (no symbol)\n",
                            _i, frames[_i]);
                }
            }
        }
                /* Weak hook for diagnostics: trace_stack.c provides paired stack-frame reports; a release build lacks it */
        { void guest_pair_report(void) __attribute__((weak));
          if (guest_pair_report) guest_pair_report(); }
                {   /* Where the two dead elements' aux points, and what it holds.
                          * If the block aux points at is broken too, the same batch was freed or overwritten at once. */
            uint32_t auxes[2] = {0x652dd010u, 0x652db010u};
            for (int t = 0; t < 2; t++) {
                uint32_t a = auxes[t];
                extern int heap_seg_lookup(uintptr_t, uintptr_t *, size_t *);
                uintptr_t sb2 = 0; size_t ss2 = 0;
                fprintf(stderr, "[guest]   aux %08x:", a);
                if (heap_seg_lookup(a, &sb2, &ss2))
                    fprintf(stderr, " in heap segment %08lx+%zu", (unsigned long)sb2, ss2);
                else
                    fprintf(stderr, " not in an in-use heap segment");
                if (a >= 0x10000u && a + 0x44u < 0xC0000000u)
                    fprintf(stderr, "  [0]=%08x [0x3c]=%08x", ld32(a), ld32(a + 0x3cu));
                fprintf(stderr, "\n");
            }
        }
                {   /* Whether every element's obj in the table is still "alive" ([0] is a vtable in the 0x3066028 family) */
            uint32_t t3 = ld32(0x307e7d00u);
            if (t3 >= 0x10004u && t3 + 32u * 12u < 0xC0000000u) {
                unsigned total = 0, dead = 0;
                for (unsigned b = 0; b < 32u; b++) {
                    uint32_t bg = ld32(t3 + b * 12u), en = ld32(t3 + b * 12u + 4u);
                    if (bg == en || en < bg || en >= 0xC0000000u) continue;
                    for (uint32_t q = bg; q + 8u <= en; q += 8u) {
                        uint32_t o = ld32(q);
                        total++;
                        if (o < 0x10000u || o + 4u >= 0xC0000000u) { dead++; continue; }
                        uint32_t vt = ld32(o);
                                                /* This table's object vtables are in the 0x30xxxxxx range; glyph data looks like ffc6ffc4 */
                        if (vt < 0x30000000u || vt >= 0x31000000u) {
                            dead++;
                            fprintf(stderr,
                                "[guest]   dead element: bucket=%u slot=%08x obj=%08x [0]=%08x aux=%08x\n",
                                b, q, o, vt, ld32(q + 4u));
                        }
                    }
                }
                fprintf(stderr, "[guest]   %u hash-table elements, %u of them have [0] that is not a vtable\n",
                        total, dead);
            }
        }
                {   /* Identity of block 652b2010: did we allocate it? Is it still in use?
                          * Look it up in the seg ring (records base+size of every malloc). */
            extern int heap_seg_lookup(uintptr_t, uintptr_t *, size_t *);
            uint32_t bad2 = (g_cur_cpu_diag
                             ? ld32(((CPU *)g_cur_cpu_diag)->esi) : 0u);
            uintptr_t sb = 0; size_t ss = 0;
            if (bad2 >= 0x10000u && heap_seg_lookup(bad2, &sb, &ss)) {
                { extern void malloc_trace_dump(uint32_t); malloc_trace_dump(bad2); }
                fprintf(stderr, "[guest]   bad object %08x lies in heap segment %08lx+%zu (offset +0x%lx)"
                                " -> a block from this allocator, currently not freed\n",
                        bad2, (unsigned long)sb, ss, (unsigned long)(bad2 - sb));
                                /* Is the vtable intact? Good objects mirror 0xb66028 */
                fprintf(stderr, "[guest]   bad object [0]=%08x (good object [0]=%08x)\n",
                        ld32(bad2), ld32(ld32(ld32(0x307e7d00u))));
            } else {
                fprintf(stderr, "[guest]   bad object %08x is in no **currently in-use** heap segment"
                                " -> already freed (UAF)\n", bad2);
            }
        }
                {   /* Good object vs bad object: same vtable, same 0x90 bytes,
                          * differing only at [+0x3c]. Dump 0x30..0x60 of both side by side
                          * to see whether that field is "never constructed" or "overwritten by something else". */
            uint32_t tbl2 = ld32(0x307e7d00u);
            if (tbl2 >= 0x10004u && tbl2 + 32u * 12u < 0xC0000000u) {
                uint32_t beg = ld32(tbl2), end = ld32(tbl2 + 4u);
                if (end > beg && end < 0xC0000000u) {
                    uint32_t good = ld32(beg);
                    uint32_t bad = (g_cur_cpu_diag
                                    ? ld32(((CPU *)g_cur_cpu_diag)->esi) : 0u);
                    fprintf(stderr,
                        "[guest]   === good object %08x vs bad object %08x ===\n", good, bad);
                    for (uint32_t o = 0x2cu; o <= 0x60u; o += 16u) {
                        fprintf(stderr, "[guest]    +%02x  good:", o);
                        if (good >= 0x10000u && good + o + 16u < 0xC0000000u)
                            fprintf(stderr, " %08x %08x %08x %08x",
                                    ld32(good+o), ld32(good+o+4),
                                    ld32(good+o+8), ld32(good+o+12));
                        fprintf(stderr, "\n[guest]        bad :");
                        if (bad >= 0x10000u && bad + o + 16u < 0xC0000000u)
                            fprintf(stderr, " %08x %08x %08x %08x",
                                    ld32(bad+o), ld32(bad+o+4),
                                    ld32(bad+o+8), ld32(bad+o+12));
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
                {   /* Walk the global 32-bucket hash table (0x307e7d00).
                          * The layout comes from initialisation point RVA 0x5bfb38:
                          *   malloc(4 + 32*12); [0]=32; 12 bytes per bucket = three vector pointers
                          * Elements are 8 bytes; the first field points at the "object", whose +0x3c is the name.
                          * The difference between bad and good elements is visible at a glance. */
            uint32_t tbl = ld32(0x307e7d00u);
            uint32_t esi_mark = g_cur_cpu_diag ? ((CPU *)g_cur_cpu_diag)->esi : 0u;
            fprintf(stderr, "[guest]   --- global hash table [0x307e7d00]=%08x ---\n", tbl);
                        /* * Layout: malloc returns p, [p]=count(32), esi=p+4 stored into 0x307e7d00.
                          * So bucket b's three vector pointers are at tbl + b*12; count is at tbl-4. */
            if (tbl >= 0x10004u && tbl + 32u * 12u < 0xC0000000u) {
                fprintf(stderr, "[guest]   bucket count (count @ tbl-4)=%u\n", ld32(tbl - 4u));
                for (unsigned b = 0; b < 32u; b++) {
                    uint32_t beg = ld32(tbl + b * 12u);
                    uint32_t end = ld32(tbl + b * 12u + 4u);
                    if (beg == end) continue;
                    if (beg < 0x10000u || end >= 0xC0000000u || end < beg) {
                        fprintf(stderr, "[guest]   bucket%u: bad range %08x..%08x\n", b, beg, end);
                        continue;
                    }
                    fprintf(stderr, "[guest]   bucket%u: %u elements %08x..%08x\n",
                            b, (end - beg) / 8u, beg, end);
                    unsigned shown = 0;
                    for (uint32_t q = beg; q + 8u <= end && shown < 40u; q += 8u, shown++) {
                        uint32_t obj = ld32(q);
                        uint32_t aux = ld32(q + 4u);
                        fprintf(stderr, "[guest]     [%08x]%s obj=%08x aux=%08x",
                                q, (q == esi_mark) ? " ←esi" : "", obj, aux);
                        if (obj < 0x10000u || obj + 0x40u >= 0xC0000000u) {
                            fprintf(stderr, "  (obj pointer invalid)\n");
                            continue;
                        }
                        uint32_t nm = ld32(obj + 0x3cu);
                        fprintf(stderr, "  [obj+0x3c]=%08x", nm);
                        if (nm >= 0x10000u && nm < 0xC0000000u) {
                            char nb[64]; int kk = 0;
                            for (; kk < 60; kk++) {
                                uint8_t ch = ld8(nm + (uint32_t)kk);
                                if (!ch) break;
                                nb[kk] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '.');
                            }
                            nb[kk] = 0;
                            fprintf(stderr, " = \"%s\"", nb);
                        } else {
                            fprintf(stderr, " <- unreadable");
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
                {   /* Which heap block the crash registers land in, and whether it is now dead or alive */
            if (g_cur_cpu_diag) {
                extern int heap_block_state(uintptr_t, size_t *);
                extern unsigned heap_seg_count(void);
                extern void heap_hist_dump(uintptr_t);
                CPU *k = (CPU *)g_cur_cpu_diag;
                fprintf(stderr, "[guest]   --- register provenance ---\n");
                struct { const char *n; uint32_t v; } rr[4] = {
                    {"esi", k->esi}, {"edi", k->edi},
                    {"eax_in_[esi]", ld32(k->esi)}, {"ecx", k->ecx}
                };
                for (int r = 0; r < 4; r++) {
                    uint32_t a = rr[r].v;
                    size_t bs = 0;
                    int st;
                                        /* Locate precisely with the seg table first (it records the real block base,
                                          * unaffected by whether the header sits on a 4KB boundary). */
                    { extern int heap_seg_lookup(uintptr_t, uintptr_t *, size_t *);
                      uintptr_t sb = 0;
                      if (heap_seg_lookup((uintptr_t)a, &sb, &bs)) {
                          size_t off2 = (size_t)((uintptr_t)a - sb);
                          void *blk = (void *)(uintptr_t)sb;
                          extern int heap_block_state(uintptr_t, size_t *);
                          st = heap_block_state((uintptr_t)blk + sizeof(void)*2, &bs);
                          fprintf(stderr, "[guest]   %-12s=%08x seg=%08lx+%zu offset+0x%zx\n",
                                  rr[r].n, a, (unsigned long)sb, bs, off2);
                          continue;
                      }
                      st = 0; }
                    fprintf(stderr, "[guest]   %-12s=%08x block state=%s size=%zu\n",
                            rr[r].n, a,
                            st < 0 ? "address invalid" : st == 0 ? "not on heap"
                            : st == 1 ? "in use" : "★ freed", bs);
                }
                heap_hist_dump(k->esi);
            }
        }
                {   /* Dump the object in the hash bucket ([esi]) together with its [+0x3c] field.
                          * The crash form is eax=[[esi]+0x3c] unreadable, so both levels need inspection. */
            if (g_cur_cpu_diag) {
                CPU *k = (CPU *)g_cur_cpu_diag;
                uint32_t esi = k->esi;
                if (esi >= 0x10000u && esi + 8u < 0xC0000000u) {
                    uint32_t obj = ld32(esi);
                    fprintf(stderr, "[guest]   bucket node [esi]=%08x  [esi+4]=%08x\n",
                            obj, ld32(esi + 4u));
                    if (obj >= 0x10000u && obj + 0x50u < 0xC0000000u) {
                        fprintf(stderr, "[guest]   first 0x50 bytes of object %08x:\n", obj);
                        for (uint32_t o = 0; o < 0x50u; o += 16u) {
                            uint32_t a = obj + o;
                            fprintf(stderr, "[guest]      +%02x: %08x %08x %08x %08x\n", o,
                                    ld32(a), ld32(a + 4u), ld32(a + 8u), ld32(a + 12u));
                        }
                        fprintf(stderr, "[guest]   object [+0x3c] = %08x\n", ld32(obj + 0x3cu));
                    } else {
                        fprintf(stderr, "[guest]   object pointer %08x unreadable\n", obj);
                    }
                }
            }
        }
                {   /* Dump 64 bytes at each of several registers. When hash lookup crashes here,
                          * eax=[esi]->[+0x3c] is the "function/field treated as a string",
                          * and ecx/edi is the other side of the comparison. Both must be seen to know which is wrong. */
            if (g_cur_cpu_diag) {
                CPU *k = (CPU *)g_cur_cpu_diag;
                struct { const char *n; uint32_t v; } regs[6] = {
                    {"eax", k->eax}, {"ecx", k->ecx}, {"edx", k->edx},
                    {"esi", k->esi}, {"edi", k->edi}, {"ebx", k->ebx}
                };
                for (int r = 0; r < 6; r++) {
                    uint32_t a = regs[r].v;
                    if (a < 0x10000u || a >= 0xC0000000u) {
                        fprintf(stderr, "[guest]   %s=%08x (unreadable)\n", regs[r].n, a);
                        continue;
                    }
                    fprintf(stderr, "[guest]   %s=%08x:", regs[r].n, a);
                    for (int q = 0; q < 8; q++) {
                        uint32_t aa = a + (uint32_t)q * 4u;
                        if (aa + 4u >= 0xC0000000u) break;
                        fprintf(stderr, " %08x", *(uint32_t *)(uintptr_t)aa);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
                {   /* Whether the faulting address falls in a registered heap segment */
            extern int heap_seg_lookup(uintptr_t, uintptr_t *, size_t *);
            extern unsigned heap_seg_count(void);
            uintptr_t sb; size_t ss;
            fprintf(stderr, "[guest] heap segments=%u, faulting address ", heap_seg_count());
            if (heap_seg_lookup(addr, &sb, &ss))
                fprintf(stderr, "%08x falls in heap segment %08lx+%zu, offset 0x%lx\n",
                        addr, (unsigned long)sb, ss, (unsigned long)(addr - sb));
            else
                fprintf(stderr, "%08x is in no heap segment\n", addr);
        }
                {   /* Failure scene: dump a stretch around ebp.
                          * The three-pointer layout of std::string / std::vector is fixed on the stack,
                          * and registers alone cannot show "which object is empty"; this stretch can be read directly. */
            if (g_cur_cpu_diag) {
                CPU *k = (CPU *)g_cur_cpu_diag;
                uint32_t base = k->ebp - 0x100u;
                if (base >= 0x10000u && base + 0x140u < 0xC0000000u) {
                    fprintf(stderr, "[guest] around ebp=%08x (ebp-0x100..ebp+0x40):\n",
                            k->ebp);
                    for (uint32_t o = 0; o < 0x140u; o += 16u) {
                        uint32_t a = base + o;
                        const char *mark = (a == k->ebp) ? " <-ebp" : "";
                        fprintf(stderr, "  %08x: %08x %08x %08x %08x%s\n", a,
                                *(uint32_t *)(uintptr_t)a,
                                *(uint32_t *)(uintptr_t)(a + 4u),
                                *(uint32_t *)(uintptr_t)(a + 8u),
                                *(uint32_t *)(uintptr_t)(a + 12u), mark);
                    }
                                        /* Print stack constants that look like ASCII as strings while we are here */
                    for (uint32_t off = 0; off < 0x140u; off += 4u) {
                        uint32_t a = base + off;
                        const unsigned char *s = (const unsigned char *)(uintptr_t)a;
                        int printable = 0;
                        for (int q = 0; q < 24; q++) {
                            unsigned char ch = s[q];
                            if (ch >= 0x20 && ch < 0x7f) printable++;
                            else if (ch == 0) break;
                            else { printable = -1; break; }
                        }
                        if (printable >= 6) {
                            char buf[48]; int n2 = 0;
                            for (; n2 < 40 && s[n2] >= 0x20 && s[n2] < 0x7f; n2++)
                                buf[n2] = (char)s[n2];
                            buf[n2] = 0;
                            fprintf(stderr, "  [str] %08x = \"%s\"\n", a, buf);
                        }
                    }
                }
            }
        }
                {   /* State snapshot of key globals */
            uint32_t gg = *(uint32_t *)(uintptr_t)0x307fd680u;
            uint32_t fe = *(uint32_t *)(uintptr_t)0x307fece4u;
            fprintf(stderr, "[snap] g_game=%08x [+29e74]=%02x [+29e77]=%02x singleton=%08x\n",
                    gg, gg ? *(uint8_t *)(uintptr_t)(gg + 0x29e74u) : 0,
                    gg ? *(uint8_t *)(uintptr_t)(gg + 0x29e77u) : 0, fe);
        }
        abort();
    }
}

int guest_image_load(const char *path)
{
    guest_pe_loaded_image loaded;
    guest_pe_error error;

    if (g_image_mem || g_image_base || g_image_size || g_tls_block ||
        g_tls_present) {
        fprintf(stderr, "guest: refusing to replace a live guest image\n");
        return 1;
    }
    if (!guest_pe_target_supported(GUEST_IMAGE_BASE) ||
        GUEST_PE_EXPECTED_IMAGE_SIZE > GUEST_IMAGE_RESERVE) {
        fprintf(stderr,
                "guest: compiled image contract disagrees with loader constants\n");
        return 1;
    }
    error = guest_pe_load_file(&g_guest_pe_loader_ops, path,
                               GUEST_IMAGE_BASE, &loaded);
    if (error != GUEST_PE_OK) {
        fprintf(stderr, "guest: PE load failed for %s: %s\n",
                path ? path : "<null>", guest_pe_error_string(error));
        return 1;
    }

    /* Publish only a fully authenticated, fully mapped image.  From this point
     * guest_image_free owns every failure path, including partially-created
     * TLS state, so the same process can retry without a stale reservation. */
    g_image_mem = loaded.image;
    loaded.image = NULL;
    g_image_base = loaded.plan.target_base;
    g_image_size = loaded.plan.image_size;
    GUEST_GL_DYNAMIC_FIRST_REFRESH();
    if (setup_static_tls(loaded.plan.directories[9].rva,
                         loaded.plan.directories[9].size)) {
        guest_image_free();
        return 1;
    }
    if (tag_imports()) {
        guest_image_free();
        return 1;
    }
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    guest_dispatch_table_rebuild();
#endif
    printf("guest: image %08x -> %08x, %u relocations applied,"
           " %u IAT slots tagged\n",
           loaded.plan.original_base, loaded.plan.target_base,
           loaded.plan.highlow_count, g_import_count);
    return 0;
}

void guest_image_free(void)
{
    if (g_tib && g_tls_index < GUEST_TLS_SLOTS) {
        uint32_t tls_array = *(uint32_t *)(g_tib + 0x2C);
        if (tls_array)
            *(uint32_t *)(uintptr_t)(tls_array + g_tls_index * 4U) = 0U;
    }
    if (g_tls_block) free(g_tls_block);
    g_tls_block = NULL;
    g_guest_fs_base_cached = 0U;
    g_tls_block_size = 0U;
    g_tls_index = 0xFFFFFFFFu;
    g_tls_callback_count = 0U;
    g_tls_present = 0;
    g_tls_process_attached = 0;
    if (g_image_mem)
        guest_pe_platform_image_free(NULL, g_image_mem);
    g_image_mem = NULL;
    g_image_base = 0;
    g_image_size = 0;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    guest_dispatch_table_rebuild();
#endif
    GUEST_GL_DYNAMIC_FIRST_REFRESH();
}

/* ------------------------------------------------------------- stack ---- */

void guest_cpu_init(CPU *c)
{
    if (c) memset(c, 0, sizeof *c);
}

static const char *guest_stack_fault_text(uint32_t kind)
{
    switch (kind) {
    case GUEST_STACK_FAULT_UNBOUND:
        return "guest stack is not bound";
    case GUEST_STACK_FAULT_OWNER:
        return "guest stack owner/bounds are corrupt";
    case GUEST_STACK_FAULT_SET:
        return "guest stack pointer outside bounds";
    case GUEST_STACK_FAULT_ADJUST:
        return "guest stack cleanup overflow";
    case GUEST_STACK_FAULT_ACCESS:
        return "guest stack access outside bounds";
    case GUEST_STACK_FAULT_PUSH:
        return "guest stack underflow";
    case GUEST_STACK_FAULT_POP:
        return "guest stack overflow";
    default:
        return "unknown guest stack violation";
    }
}

GUEST_STACK_COLD_NOINLINE int guest_stack_violation(
    CPU *__restrict c, uint32_t pc, uint32_t kind, uint32_t address,
    uint32_t size)
{
    if (!c)
        return 0;
    c->stack_fault_kind = kind;
    c->stack_fault_address = address;
    c->stack_fault_size = size;
    c->stack_fault_pc = pc;
    c->stack_fault_native_site = (uintptr_t)0U;
    guest_fault(c, pc, guest_stack_fault_text(kind));
    return 0;
}

GUEST_STACK_COLD_NOINLINE int guest_stack_owner_violation(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t kind;
    if (!c)
        return 0;
    kind = c->stack_owner ? GUEST_STACK_FAULT_OWNER
                          : GUEST_STACK_FAULT_UNBOUND;
    return guest_stack_violation(c, pc, kind, c->esp, 0U);
}

/* Generated hot helpers omit a per-site guest-PC literal.  The direct native
 * return address is both smaller and strictly more precise about the final
 * linked instruction which rejected the operation; offline ELF/map lookup
 * recovers the generated owner.  This value is diagnostic host identity and
 * is deliberately never passed through a guest-pointer range predicate. */
static GUEST_STACK_COLD_NOINLINE int guest_stack_native_violation(
    CPU *__restrict c, uintptr_t native_site, uint32_t kind,
    uint32_t address, uint32_t size)
{
    if (!c)
        return 0;
#if defined(__arm__) || defined(__thumb__)
    /* A Thumb BL may leave the ISA-state bit set in physical LR even though
     * ELF/map symbols use an ordinary byte address.  The compiler builtin in
     * the capture expression documents the ABI intent; normalize explicitly
     * here as well because the tied extended asm deliberately snapshots the
     * physical register.  This is cold diagnostic work, never a guest-pointer
     * predicate and never part of the successful stack hot path. */
    native_site &= ~(uintptr_t)1U;
#endif
    fprintf(stderr, "[stack] VIOLATION kind=%u addr=0x%08x size=%u "
            "esp=0x%08x floor=0x%08x ceil=0x%08x\n",
            kind, address, size, c->esp, c->stack_floor, c->stack_ceiling);
    c->stack_fault_kind = kind;
    c->stack_fault_address = address;
    c->stack_fault_size = size;
    c->stack_fault_pc = 0U;
    c->stack_fault_native_site = native_site;
    guest_fault(c, 0U, guest_stack_fault_text(kind));
    return 0;
}

static GUEST_STACK_COLD_NOINLINE int guest_stack_native_owner_violation(
    CPU *__restrict c, uintptr_t native_site)
{
    uint32_t kind;
    if (!c)
        return 0;
    kind = c->stack_owner ? GUEST_STACK_FAULT_OWNER
                          : GUEST_STACK_FAULT_UNBOUND;
    return guest_stack_native_violation(
        c, native_site, kind, c->esp, 0U);
}

#if GUEST_STACK_REQUIRED
GUEST_STACK_HOT_NOINLINE int guest_stack_set(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_SET, value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust(
    CPU *__restrict c, uint32_t amount, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            offset > capacity || amount > capacity - offset))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, c->esp, amount);
    c->esp += amount;
    return 1;
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address(
    CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ACCESS, address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

GUEST_STACK_HOT_NOINLINE void gpush_at(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity, offset, next;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_PUSH, c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_at(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t capacity, offset, value;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_POP, c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated(
    CPU *__restrict c, uint32_t value)
{
    uint32_t capacity;
    GUEST_STACK_NATIVE_LOCAL(native_site);
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_native_owner_violation(
            c, native_site);
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_native_violation(
            c, native_site, GUEST_STACK_FAULT_SET,
            value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust_generated(
    CPU *__restrict c, uint32_t amount)
{
    uint32_t capacity, offset;
    GUEST_STACK_NATIVE_LOCAL(native_site);
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_native_owner_violation(
            c, native_site);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            offset > capacity || amount > capacity - offset))
        return guest_stack_native_violation(
            c, native_site, GUEST_STACK_FAULT_ADJUST,
            c->esp, amount);
    c->esp += amount;
    return 1;
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address_generated(
    CPU *__restrict c, uint32_t address, uint32_t size)
{
    uint32_t capacity, offset;
    GUEST_STACK_NATIVE_LOCAL(native_site);
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_native_owner_violation(
            c, native_site);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_native_violation(
            c, native_site, GUEST_STACK_FAULT_ACCESS,
            address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

GUEST_STACK_HOT_NOINLINE void gpush_generated(
    CPU *__restrict c, uint32_t value)
{
    uint32_t capacity, offset, next;
    GUEST_STACK_NATIVE_LOCAL(native_site);
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_native_owner_violation(
            c, native_site);
        return;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_native_violation(
            c, native_site, GUEST_STACK_FAULT_PUSH,
            c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t capacity, offset, value;
    GUEST_STACK_NATIVE_LOCAL(native_site);
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_native_owner_violation(
            c, native_site);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_native_violation(
            c, native_site, GUEST_STACK_FAULT_POP,
            c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}
#endif

int guest_stack_bind(CPU *c, uint32_t floor, uint32_t ceiling)
{
    if (!c || floor >= ceiling || ceiling - floor < 4U ||
        c->stack_owner || c->stack_floor || c->stack_ceiling ||
        c->stack_low_water)
        return 1;
    c->stack_owner = c;
    c->stack_floor = floor;
    c->stack_ceiling = ceiling;
    c->stack_low_water = ceiling;
    c->stack_fault_address = 0U;
    c->stack_fault_size = 0U;
    c->stack_fault_pc = 0U;
    c->stack_fault_native_site = (uintptr_t)0U;
    c->stack_fault_kind = GUEST_STACK_FAULT_NONE;
    c->esp = ceiling;
    c->ebp = ceiling;
    return 0;
}

int guest_stack_init(CPU *c)
{
    uint32_t base, floor, ceiling;
#define GUEST_STACK_LOW_MARGIN 4096U
#define GUEST_STACK_TOP_MARGIN 4096U

    if (!c || g_stack_mem)
        return 1;
#ifdef __vita__
    void *p = memalign(16U, GUEST_STACK_SIZE);
#else
    void *p = VirtualAlloc(NULL, GUEST_STACK_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
    if (!p) {
        return 1;
    }
    base = (uint32_t)(uintptr_t)p;
    if (
#if UINTPTR_MAX > UINT32_MAX
        (uintptr_t)p > UINT32_MAX ||
#endif
        GUEST_STACK_SIZE <=
            GUEST_STACK_LOW_MARGIN + GUEST_STACK_TOP_MARGIN ||
        base > UINT32_MAX - GUEST_STACK_SIZE) {
#ifdef __vita__
        free(p);
#else
        VirtualFree(p, 0, MEM_RELEASE);
#endif
        return 1;
    }
    floor = base + GUEST_STACK_LOW_MARGIN;
    ceiling = base + GUEST_STACK_SIZE - GUEST_STACK_TOP_MARGIN;
    memset(p, 0, GUEST_STACK_SIZE);
    /* Both margins remain outside the registered/owned stack interval.  They
     * are diagnostic storage rather than hardware guard pages (Vita userland
     * exposes no no-access remap), so every translated operation still has to
     * pass the checked floor/ceiling boundary before touching memory. */
    if (guest_stack_bind(c, floor, ceiling) != 0) {
#ifdef __vita__
        free(p);
#else
        VirtualFree(p, 0, MEM_RELEASE);
#endif
        return 1;
    }
    c->fault = NULL;
    c->fault_addr = 0;
    c->exit_api = NULL;
    c->exit_code = 0;
    c->stop_kind = GUEST_RUN_RETURNED;
    c->run_scope = NULL;
    c->jump_sites = NULL;
    c->jump_value = 0U;
    g_stack_mem = p;
    g_stack_owner = c;
    g_stack_base = c->stack_floor;
    g_stack_top = c->stack_ceiling;
    g_guest_fs_base_cached = 0U;     /* TIB StackBase/StackLimit changed */
#undef GUEST_STACK_LOW_MARGIN
#undef GUEST_STACK_TOP_MARGIN
    return 0;
}

void guest_stack_free(CPU *c)
{
    if (g_stack_mem) {
        if (!c || g_stack_owner != c || c->stack_owner != c) {
            fprintf(stderr,
                    "guest: refusing guest stack free by a non-owner CPU\n");
            return;
        }
#ifdef __vita__
        free(g_stack_mem);
#else
        VirtualFree(g_stack_mem, 0, MEM_RELEASE);
#endif
    }
    g_stack_mem = NULL;
    g_stack_owner = NULL;
    g_stack_base = g_stack_top = 0;
    g_guest_fs_base_cached = 0U;
    if (c && c->stack_owner == c) {
        c->esp = c->ebp = 0;
        c->jump_sites = NULL;
        c->jump_value = 0U;
        c->stack_owner = NULL;
        c->stack_floor = 0U;
        c->stack_ceiling = 0U;
        c->stack_low_water = 0U;
        c->stack_fault_address = 0U;
        c->stack_fault_size = 0U;
        c->stack_fault_pc = 0U;
        c->stack_fault_native_site = (uintptr_t)0U;
        c->stack_fault_kind = GUEST_STACK_FAULT_NONE;
    }
}

/* ---------------------------------------------------------- dispatch ---- */

static const uint32_t *g_addrs;
static const guest_fn *g_fns;
static uint32_t        g_count;

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
#define GUEST_LOOKUP_CACHE_COUNT 256U
#define GUEST_IMPORT_DENSE_SLOT_COUNT 438U

typedef struct guest_lookup_cache_entry {
    uint32_t key;
    guest_fn function;
    /* Set only after guest_call has proved that this exact key is neither an
     * IAT slot nor a native dynamic token.  Public guest_lookup callers may
     * warm function, but they cannot authorize the higher-level bypass. */
    guest_fn dispatch_function;
} guest_lookup_cache_entry;

static guest_lookup_cache_entry
    g_guest_lookup_cache[GUEST_LOOKUP_CACHE_COUNT];
static uint16_t g_guest_import_dense[GUEST_IMPORT_DENSE_SLOT_COUNT];
static uint32_t g_guest_import_dense_base;
static uint32_t g_guest_import_dense_span;

static uint32_t guest_lookup_cache_index(uint32_t key)
{
    /* Multiplicative hashing uses the high byte so the low alignment bits of
     * x86 function RVAs cannot collapse the 256 direct-mapped entries. */
    return (key * UINT32_C(2654435761)) >> 24;
}

static void guest_dispatch_cache_invalidate(void)
{
    uint32_t i;

    for (i = 0U; i < GUEST_LOOKUP_CACHE_COUNT; ++i)
        g_guest_lookup_cache[i].dispatch_function = NULL;
}

static void guest_import_dense_register(const guest_import *imports,
                                        uint32_t count)
{
    uint32_t base, last, span, i;

    memset(g_guest_import_dense, 0xff, sizeof g_guest_import_dense);
    g_guest_import_dense_base = 0U;
    g_guest_import_dense_span = 0U;
    if (!imports || !count || count > UINT16_MAX)
        return;

    base = imports[0].slot_rva;
    last = imports[count - 1U].slot_rva;
    span = (last - base) >> 2;
    if (((last - base) & 3U) != 0U ||
        span >= GUEST_IMPORT_DENSE_SLOT_COUNT)
        return;
    ++span;
    for (i = 0U; i < count; ++i) {
        uint32_t delta = imports[i].slot_rva - base;
        uint32_t slot = delta >> 2;

        if ((delta & 3U) != 0U || slot >= span ||
            g_guest_import_dense[slot] != UINT16_MAX) {
            memset(g_guest_import_dense, 0xff,
                   sizeof g_guest_import_dense);
            return;
        }
        g_guest_import_dense[slot] = (uint16_t)i;
    }
    g_guest_import_dense_base = base;
    g_guest_import_dense_span = span;
}
#endif

typedef struct guest_run_scope {
    jmp_buf env;
} guest_run_scope;

void guest_register(const uint32_t *addrs, const guest_fn *fns, uint32_t n)
{
    g_addrs = addrs; g_fns = fns; g_count = n;
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    memset(g_guest_lookup_cache, 0, sizeof g_guest_lookup_cache);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    guest_dispatch_table_rebuild();
#endif
}

const uint32_t *guest_registered_addresses(uint32_t *count)
{
    if (count)
        *count = g_count;
    return g_addrs;
}

void guest_register_imports(const guest_import *imports, uint32_t n)
{
    g_imports = imports;
    g_import_count = n;
    GUEST_GL_DYNAMIC_FIRST_REFRESH();
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    /* Import precedence is part of guest_call's ABI.  A new table may claim
     * an address previously classified as ordinary translated code. */
    guest_dispatch_cache_invalidate();
    guest_import_dense_register(imports, n);
#endif
#ifdef ISAAC_VITA_IMPORT_ID_DISPATCH
    /* Dense IDs are safe only after the complete generated table and every
     * active family's local index/name pair have matched the frozen contract.
     * Registration itself is side-effect-free and allocation-free. */
    {
        int registered = guest_host_import_ids_register(imports, n);
#if defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
        g_vita_sync_import_fastpath_ready = registered;
#endif
#if defined(ISAAC_VITA_IMPORT_DIRECT)
        guest_import_direct_rebuild(registered);
#endif
        (void)registered;
    }
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    guest_dispatch_table_rebuild();
#endif
}

static uint32_t image_rva(uint32_t addr)
{
    if (g_image_base && addr - g_image_base < g_image_size)
        return addr - g_image_base;
    return addr;
}

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
static inline guest_fn guest_dispatch_cache_probe(uint32_t addr)
{
    uint32_t family = addr & UINT32_C(0xff000000);
    uint32_t key;
    guest_lookup_cache_entry *cached;

    /* guest_host_dynamic owns these raw token families even if a hostile or
     * synthetic registration also gives guest_lookup the same numeric key. */
    if (family == UINT32_C(0x7d000000) ||
        family == UINT32_C(0x7e000000))
        return NULL;

    key = image_rva(addr);
    cached = &g_guest_lookup_cache[guest_lookup_cache_index(key)];
    if (cached->key != key || !cached->function ||
        cached->dispatch_function != cached->function)
        return NULL;
    return cached->dispatch_function;
}

static inline void guest_dispatch_cache_authorize(uint32_t addr,
                                                  guest_fn function)
{
    uint32_t family = addr & UINT32_C(0xff000000);
    uint32_t key;
    guest_lookup_cache_entry *cached;

    if (family == UINT32_C(0x7d000000) ||
        family == UINT32_C(0x7e000000))
        return;

    key = image_rva(addr);
    cached = &g_guest_lookup_cache[guest_lookup_cache_index(key)];
    if (cached->key == key && cached->function == function)
        cached->dispatch_function = function;
}
#endif

static const guest_import *guest_import_lookup(uint32_t slot_addr,
                                               uint32_t *id_out)
{
    uint32_t key = image_rva(slot_addr);
    uint32_t lo = 0, hi = g_import_count;
    if (!hi || key < g_imports[0].slot_rva ||
        key > g_imports[hi - 1U].slot_rva)
        return NULL;
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    if (g_guest_import_dense_span != 0U) {
        uint32_t delta = key - g_guest_import_dense_base;
        uint32_t slot = delta >> 2;

        if ((delta & 3U) == 0U && slot < g_guest_import_dense_span) {
            uint16_t dense_index = g_guest_import_dense[slot];

            /* The dense table is only a candidate accelerator.  Verify the
             * exact slot before returning; holes, stale state, or a future
             * table shape fall through to the unchanged binary search. */
            if (dense_index != UINT16_MAX && dense_index < g_import_count &&
                g_imports[dense_index].slot_rva == key) {
                if (id_out) *id_out = dense_index;
                return &g_imports[dense_index];
            }
        }
    }
#endif
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_imports[mid].slot_rva == key) {
            if (id_out) *id_out = mid;
            return &g_imports[mid];
        }
        if (g_imports[mid].slot_rva < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* ------------------------------------------ O(1) raw-VA dispatch table ----
 *
 * guest_call's hot path is one probe of a two-level multiplicative perfect
 * hash keyed by the exact 32-bit value an indirect `call/jmp r/m32`
 * presents.  Vtables and function pointers in the mapped PE hold relocated
 * in-image VAs, so the key of registered function i is
 * g_image_base + g_addrs[i]; the probe does no per-call normalisation, family
 * check or two-field consistency check.
 *
 * Contract (verified exhaustively after every build, and by the host oracles
 * against the generated table): a probe hit for key K calls exactly the
 * function the complete path would reach for K.  A key is inserted only when
 * that path is "translated function found by guest_lookup": the image is
 * mapped and the RVA normalises back (image_rva(K) == g_addrs[i]), the
 * function pointer is non-NULL, no IAT slot claims the same RVA (import
 * precedence, guest_register_imports), and K is outside the 0x7d/0x7e native
 * token families that guest_host_dynamic owns.  Everything else -- RVA-form
 * keys from direct drivers, every IAT slot, every dynamic token and every
 * unknown value -- misses and takes the unchanged complete path, so every
 * fail-closed fault and the import/dynamic precedence are preserved.
 *
 * Hash: bucket = (K * multiplier) >> 22 selects one of 1024 buckets; the
 * bucket's own odd 16-bit multiplier M2 then places the key at slot
 * (K * M2) >> 17.  The build picks, per bucket, the first of the 32768 odd
 * 16-bit values under which all of the bucket's keys land on distinct free
 * slots (largest buckets first), so unlike a displacement scheme two keys can
 * never be unplaceable: every candidate is an independent hash function.
 *
 * Empty slots hold key GUEST_DISPATCH_EMPTY_KEY (0) and a trampoline to the
 * complete path with that same key, so the fast path needs no NULL test.
 * Empty buckets keep M2 == 0, which sends any key they receive to slot 0;
 * that slot's key is either 0 (trampoline) or a registered key that lives in
 * a non-empty bucket, so it can never equal the probed key.  The table is
 * rebuilt (or emptied, fail closed) whenever its inputs change:
 * guest_register, guest_register_imports, guest_image_load, guest_image_free.
 *
 * Geometry (pinned by the layout check in guest_dispatch_table_oracle.c):
 * 1024 uint16 bucket multipliers at offset 0 (2 KB), the first-level
 * multiplier at offset 2048 and 32768 slots of {key, fn} (256 KB, load 0.46
 * for 15,116 functions) from offset 2056, so the probe materialises a single
 * base address and every load uses an immediate or register offset.  The
 * build is allocation-free and runs once per input change at boot. */
#define GUEST_DISPATCH_SLOT_BITS     15U
#define GUEST_DISPATCH_SLOT_COUNT    (UINT32_C(1) << GUEST_DISPATCH_SLOT_BITS)
#define GUEST_DISPATCH_BUCKET_BITS   10U
#define GUEST_DISPATCH_BUCKET_COUNT  (UINT32_C(1) << GUEST_DISPATCH_BUCKET_BITS)
#define GUEST_DISPATCH_MAX_KEYS      16384U
#define GUEST_DISPATCH_MAX_BUCKET    255U
#define GUEST_DISPATCH_EMPTY_KEY     UINT32_C(0)
#define GUEST_DISPATCH_MULTIPLIER    UINT32_C(0x9E3779B1)
/* Odd 16-bit multipliers, visited in a scrambled order so the first
 * candidates are already well mixed for the narrow in-image key range. */
#define GUEST_DISPATCH_CANDIDATES    32768U
#define GUEST_DISPATCH_CANDIDATE(t)  ((uint16_t)(((t) * 2U + 1U) * 0x9E37U))

#if defined(__GNUC__) || defined(__clang__)
# define GUEST_DISPATCH_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
# define GUEST_DISPATCH_NOINLINE __declspec(noinline)
#else
# define GUEST_DISPATCH_NOINLINE
#endif

typedef struct guest_dispatch_slot {
    uint32_t key;
    guest_fn function;
} guest_dispatch_slot;

typedef struct guest_dispatch_table {
    uint16_t            bucket_multiplier[GUEST_DISPATCH_BUCKET_COUNT];
    uint32_t            multiplier;
    uint32_t            reserved;
    guest_dispatch_slot slots[GUEST_DISPATCH_SLOT_COUNT];
} guest_dispatch_table;

static guest_dispatch_table        g_guest_dispatch;
static guest_dispatch_table_status g_guest_dispatch_status;
/* Build scratch (40 KB of BSS, touched only while a table is being built):
 * key indices grouped by bucket, bucket ranges, and two unions of
 * non-overlapping lifetimes -- the pass-2 fill cursors become the
 * size-ordered bucket list, and the counting-sort offsets become the 4 KB
 * slot-occupancy bitmap.  The multiplier search probes that L1-resident
 * bitmap instead of the 256 KB slot array (some 400k random slot touches
 * for the real table); the slots are written once per placed bucket. */
static uint16_t g_guest_dispatch_order[GUEST_DISPATCH_MAX_KEYS];
static uint16_t g_guest_dispatch_bucket_start[GUEST_DISPATCH_BUCKET_COUNT + 1U];
static union {
    uint16_t fill[GUEST_DISPATCH_BUCKET_COUNT];        /* pass-2 cursors */
    uint16_t order[GUEST_DISPATCH_BUCKET_COUNT];       /* largest first */
} g_guest_dispatch_buckets;
static union {
    uint16_t size_start[GUEST_DISPATCH_MAX_BUCKET + 1U]; /* counting sort */
    uint32_t used[GUEST_DISPATCH_SLOT_COUNT / 32U];      /* occupancy bits */
} g_guest_dispatch_scratch;

static uint64_t guest_dispatch_now_us(void)
{
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    return 0U;
#endif
}

static GUEST_DISPATCH_NOINLINE void guest_call_slow(CPU *__restrict c,
                                                    uint32_t addr);

static void guest_dispatch_empty_slot(CPU *__restrict c)
{
    /* Reached only when the guest presents exactly GUEST_DISPATCH_EMPTY_KEY
     * and its probe lands on a free slot: the same complete classification
     * and the same untranslated-address fault as any other miss. */
    guest_call_slow(c, GUEST_DISPATCH_EMPTY_KEY);
}

static inline uint32_t guest_dispatch_bucket_of(uint32_t key)
{
    return (key * GUEST_DISPATCH_MULTIPLIER) >>
           (32U - GUEST_DISPATCH_BUCKET_BITS);
}

static inline uint32_t guest_dispatch_index_of(uint32_t key,
                                               uint32_t bucket_multiplier)
{
    return (key * bucket_multiplier) >> (32U - GUEST_DISPATCH_SLOT_BITS);
}

static inline const guest_dispatch_slot *guest_dispatch_probe(uint32_t key)
{
    const guest_dispatch_table *table = &g_guest_dispatch;
    uint32_t bucket = (key * table->multiplier) >>
                      (32U - GUEST_DISPATCH_BUCKET_BITS);
    const guest_dispatch_slot *slot = &table->slots[
        guest_dispatch_index_of(key, table->bucket_multiplier[bucket])];

#if defined(__GNUC__)
    /* Codegen pin, no semantic effect: forces GCC to materialise the slot
     * address before the key load so the two slot loads share one base
     * register and guest_call stays frameless (no push/pop around the
     * tail call).  Measured on arm-vita-eabi-gcc 10.3 -O2 -mthumb. */
    __asm__("" : "+r"(slot));
#endif
    return slot;
}

static void guest_dispatch_table_clear(void)
{
    uint32_t i;

    g_guest_dispatch.multiplier = GUEST_DISPATCH_MULTIPLIER;
    g_guest_dispatch.reserved = 0U;
    memset(g_guest_dispatch.bucket_multiplier, 0,
           sizeof g_guest_dispatch.bucket_multiplier);
    for (i = 0U; i < GUEST_DISPATCH_SLOT_COUNT; ++i) {
        g_guest_dispatch.slots[i].key = GUEST_DISPATCH_EMPTY_KEY;
        g_guest_dispatch.slots[i].function = guest_dispatch_empty_slot;
    }
}

/* Binary search of the registered ascending RVA table without any census. */
static int guest_dispatch_find_rva(uint32_t rva, uint32_t *index_out)
{
    uint32_t lo = 0U, hi = g_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;

        if (g_addrs[mid] == rva) {
            *index_out = mid;
            return 1;
        }
        if (g_addrs[mid] < rva) lo = mid + 1U; else hi = mid;
    }
    return 0;
}

/* Non-zero with the exact key when registered function `index` is eligible
 * (see the contract above).  Ineligible functions keep the complete path. */
static int guest_dispatch_key_of(uint32_t index, uint32_t *key_out)
{
    uint32_t rva = g_addrs[index];
    uint32_t key, family;

    if (!g_image_base || rva >= g_image_size || !g_fns[index])
        return 0;
    key = g_image_base + rva;
    family = key & UINT32_C(0xff000000);
    if (key == GUEST_DISPATCH_EMPTY_KEY ||
        family == UINT32_C(0x7d000000) || family == UINT32_C(0x7e000000))
        return 0;
    if (guest_import_lookup(key, NULL))
        return 0;
    *key_out = key;
    return 1;
}

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* ---- ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok) begin ----------------
 * gl_bridge's typed-GL registry is the table's second key source.  Item
 * indices g_count.. in g_guest_dispatch_order name token i - g_count.  A
 * token is eligible exactly when guest_call_slow's 0x7e block would dispatch
 * it: while g_gl_dynamic_first holds (no IAT slot key and no image window
 * can alias the family, so neither the cache probe -- which rejects the
 * family by construction -- nor the import search nor guest_lookup could
 * claim the key) and it has a trampoline.  With the flag down every token
 * stays out of the table and reaches the unchanged complete path; the
 * registry is collision-checked at generation and disjoint from every
 * in-image key while the flag holds, so a twin can only come from a broken
 * invariant, which the bucket placement rejects (table disabled). */
static uint32_t guest_dispatch_gl_token_count(void)
{
    return g_gl_dynamic_first ? guest_gl_table_tokens(NULL, NULL) : 0U;
}

static int guest_dispatch_gl_token_of(uint32_t index, uint32_t *key_out,
                                      guest_fn *fn_out)
{
    const uint32_t *tokens;
    const guest_fn *functions;
    uint32_t count = guest_gl_table_tokens(&tokens, &functions);

    if (!g_gl_dynamic_first || index >= count || !functions[index] ||
        (tokens[index] & UINT32_C(0xff000000)) != UINT32_C(0x7e000000) ||
        tokens[index] == GUEST_DISPATCH_EMPTY_KEY)
        return 0;
    *key_out = tokens[index];
    *fn_out = functions[index];
    return 1;
}

/* Key and function of one order item (a registered function or a token);
 * called only for items that passed eligibility in the same build. */
static uint32_t guest_dispatch_item_key(uint32_t item)
{
    uint32_t key = GUEST_DISPATCH_EMPTY_KEY;
    guest_fn fn;

    if (item < g_count)
        return g_image_base + g_addrs[item];
    (void)guest_dispatch_gl_token_of(item - g_count, &key, &fn);
    return key;
}

static guest_fn guest_dispatch_item_function(uint32_t item)
{
    uint32_t key;
    guest_fn fn = NULL;

    if (item < g_count)
        return g_fns[item];
    (void)guest_dispatch_gl_token_of(item - g_count, &key, &fn);
    return fn;
}

/* Verify helper: non-zero when `slot` is exactly an eligible token's own
 * slot (key, trampoline and probe agree).  Any other 0x7e-family slot falls
 * through to the family rejection of the slot scan. */
static int guest_dispatch_gl_token_slot_ok(const guest_dispatch_slot *slot)
{
    const uint32_t *tokens;
    const guest_fn *functions;
    uint32_t count, i;

    if ((slot->key & UINT32_C(0xff000000)) != UINT32_C(0x7e000000) ||
        !g_gl_dynamic_first)
        return 0;
    count = guest_gl_table_tokens(&tokens, &functions);
    for (i = 0U; i < count; ++i)
        if (tokens[i] == slot->key)
            return functions[i] != NULL &&
                   slot->function == functions[i] &&
                   guest_dispatch_probe(slot->key) == slot;
    return 0;
}
/* ---- ISAAC_VITA_GL_SHIM_TABLE_TOKENS end ---------------------------- */
#endif

/* Places one bucket (its `count` keys) under candidate multiplier m in the
 * occupancy bitmap: marks the keys' slots and returns non-zero when all of
 * them land on distinct free slots, else leaves the bitmap unchanged and
 * returns zero.  The caller writes the slots after a success; a set bit and
 * a non-empty slot always coincide, and the verify pass checks the slots
 * themselves. */
static int guest_dispatch_try_bucket(const uint32_t *keys, uint32_t count,
                                     uint32_t m)
{
    uint32_t a;

    /* Loads only: the common failing candidate (roughly 145k of the real
     * table's 160k) leaves nothing to undo. */
    for (a = 0U; a < count; ++a) {
        uint32_t index = guest_dispatch_index_of(keys[a], m);

        if (g_guest_dispatch_scratch.used[index >> 5] &
            (UINT32_C(1) << (index & 31U)))
            return 0;                    /* occupied */
    }
    /* Every key lands on a free slot: mark them.  Two keys of this bucket
     * sharing one slot (a twin) see the first one's mark and fail here. */
    for (a = 0U; a < count; ++a) {
        uint32_t index = guest_dispatch_index_of(keys[a], m);
        uint32_t *word = &g_guest_dispatch_scratch.used[index >> 5];
        uint32_t bit = UINT32_C(1) << (index & 31U);

        if (*word & bit)
            break;                       /* a twin in this bucket */
        *word |= bit;
    }
    if (a == count)
        return 1;
    while (a-- > 0U) {
        uint32_t index = guest_dispatch_index_of(keys[a], m);

        g_guest_dispatch_scratch.used[index >> 5] &=
            ~(UINT32_C(1) << (index & 31U));
    }
    return 0;
}

static int guest_dispatch_table_build(guest_dispatch_table_status *status)
{
    uint32_t i, bucket, size, max_bucket = 0U, tries = 0U, acc, placed = 0U;
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    uint32_t tokens_placed = 0U;
#endif

    guest_dispatch_table_clear();
    memset(g_guest_dispatch_bucket_start, 0,
           sizeof g_guest_dispatch_bucket_start);
    for (i = 0U; i < g_count; ++i) {
        uint32_t key;

        if (!guest_dispatch_key_of(i, &key))
            continue;
        ++g_guest_dispatch_bucket_start[guest_dispatch_bucket_of(key) + 1U];
    }
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    for (i = 0U; i < guest_dispatch_gl_token_count(); ++i) {
        uint32_t key;
        guest_fn fn;

        if (!guest_dispatch_gl_token_of(i, &key, &fn))
            continue;
        ++g_guest_dispatch_bucket_start[guest_dispatch_bucket_of(key) + 1U];
    }
#endif
    for (bucket = 0U; bucket < GUEST_DISPATCH_BUCKET_COUNT; ++bucket) {
        size = g_guest_dispatch_bucket_start[bucket + 1U];
        if (size > max_bucket)
            max_bucket = size;
        g_guest_dispatch_bucket_start[bucket + 1U] =
            (uint16_t)(size + g_guest_dispatch_bucket_start[bucket]);
    }
    status->max_bucket = max_bucket;
    if (max_bucket > GUEST_DISPATCH_MAX_BUCKET) {
        status->reason = "a first-level hash bucket is too large";
        return 0;
    }
    memcpy(g_guest_dispatch_buckets.fill, g_guest_dispatch_bucket_start,
           sizeof g_guest_dispatch_buckets.fill);
    for (i = 0U; i < g_count; ++i) {
        uint32_t key;

        if (!guest_dispatch_key_of(i, &key))
            continue;
        bucket = guest_dispatch_bucket_of(key);
        g_guest_dispatch_order[g_guest_dispatch_buckets.fill[bucket]++] =
            (uint16_t)i;
    }
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    for (i = 0U; i < guest_dispatch_gl_token_count(); ++i) {
        uint32_t key;
        guest_fn fn;

        if (!guest_dispatch_gl_token_of(i, &key, &fn))
            continue;
        bucket = guest_dispatch_bucket_of(key);
        g_guest_dispatch_order[g_guest_dispatch_buckets.fill[bucket]++] =
            (uint16_t)(g_count + i);
    }
#endif

    /* Largest buckets first (stable counting sort by size): big buckets
     * find their multiplier while the table is still nearly empty. */
    memset(g_guest_dispatch_scratch.size_start, 0,
           sizeof g_guest_dispatch_scratch.size_start);
    for (bucket = 0U; bucket < GUEST_DISPATCH_BUCKET_COUNT; ++bucket)
        ++g_guest_dispatch_scratch.size_start[
            g_guest_dispatch_bucket_start[bucket + 1U] -
            g_guest_dispatch_bucket_start[bucket]];
    acc = 0U;
    for (size = GUEST_DISPATCH_MAX_BUCKET + 1U; size-- > 0U;) {
        uint32_t count = g_guest_dispatch_scratch.size_start[size];

        g_guest_dispatch_scratch.size_start[size] = (uint16_t)acc;
        acc += count;
    }
    for (bucket = 0U; bucket < GUEST_DISPATCH_BUCKET_COUNT; ++bucket) {
        size = g_guest_dispatch_bucket_start[bucket + 1U] -
               g_guest_dispatch_bucket_start[bucket];
        g_guest_dispatch_buckets.order[
            g_guest_dispatch_scratch.size_start[size]++] = (uint16_t)bucket;
    }

    /* The counting-sort offsets are dead from here on: the same scratch now
     * holds the slot-occupancy bitmap (all slots free after the clear). */
    memset(g_guest_dispatch_scratch.used, 0,
           sizeof g_guest_dispatch_scratch.used);
    for (i = 0U; i < GUEST_DISPATCH_BUCKET_COUNT; ++i) {
        uint32_t lo, hi, t, a, m, keys[GUEST_DISPATCH_MAX_BUCKET];

        bucket = g_guest_dispatch_buckets.order[i];
        lo = g_guest_dispatch_bucket_start[bucket];
        hi = g_guest_dispatch_bucket_start[bucket + 1U];
        if (lo == hi)
            break;                       /* only empty buckets remain */
        for (a = lo; a < hi; ++a)        /* hi - lo <= GUEST_DISPATCH_MAX_BUCKET */
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
            keys[a - lo] = guest_dispatch_item_key(g_guest_dispatch_order[a]);
#else
            keys[a - lo] = g_image_base + g_addrs[g_guest_dispatch_order[a]];
#endif
        for (t = 0U; t < GUEST_DISPATCH_CANDIDATES; ++t) {
            ++tries;
            if (guest_dispatch_try_bucket(keys, hi - lo,
                                          GUEST_DISPATCH_CANDIDATE(t)))
                break;
        }
        if (t == GUEST_DISPATCH_CANDIDATES) {
            status->reason = "no multiplier left for a bucket";
            status->bucket_tries = tries;
            return 0;
        }
        m = GUEST_DISPATCH_CANDIDATE(t);
        g_guest_dispatch.bucket_multiplier[bucket] = (uint16_t)m;
        for (a = lo; a < hi; ++a) {
            guest_dispatch_slot *slot = &g_guest_dispatch.slots[
                guest_dispatch_index_of(keys[a - lo], m)];

            slot->key = keys[a - lo];
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
            slot->function =
                guest_dispatch_item_function(g_guest_dispatch_order[a]);
            if (g_guest_dispatch_order[a] >= g_count)
                ++tokens_placed;
#else
            slot->function = g_fns[g_guest_dispatch_order[a]];
#endif
        }
        placed += hi - lo;
    }
    status->bucket_tries = tries;
    status->keys = placed;
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* keys keeps its meaning (translated functions); tokens are separate. */
    status->keys -= tokens_placed;
    status->gl_tokens = tokens_placed;
#endif
    return 1;
}

/* Exhaustive gate: every registered function resolves (or, if skipped,
 * misses), every IAT slot misses, and every occupied slot is exactly what the
 * complete path yields for its key.  Any failure empties the table. */
static int guest_dispatch_table_verify(guest_dispatch_table_status *status)
{
    uint32_t i, hits = 0U;

    for (i = 0U; i < g_count; ++i) {
        uint32_t key, rva = g_addrs[i];
        const guest_dispatch_slot *slot;

        if (guest_dispatch_key_of(i, &key)) {
            slot = guest_dispatch_probe(key);
            if (slot->key != key || slot->function != g_fns[i]) {
                status->reason = "a registered function does not resolve";
                return 0;
            }
            ++hits;
            continue;
        }
        key = g_image_base + rva;
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        /* A skipped function registered under a registry token resolves to
         * that token's own trampoline: the complete path hands the key to
         * the GL family too (guest_call_slow's 0x7e-first block runs
         * whenever a token is a key), never to the function. */
        if ((key != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(key)->key == key &&
             !guest_dispatch_gl_token_slot_ok(guest_dispatch_probe(key))) ||
            (rva != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(rva)->key == rva &&
             !guest_dispatch_gl_token_slot_ok(guest_dispatch_probe(rva)))) {
            status->reason = "a skipped function resolves";
            return 0;
        }
#else
        if ((key != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(key)->key == key) ||
            (rva != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(rva)->key == rva)) {
            status->reason = "a skipped function resolves";
            return 0;
        }
#endif
    }
    if (hits != status->keys) {
        status->reason = "key census mismatch";
        return 0;
    }
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    {
        const uint32_t *tokens;
        const guest_fn *functions;
        uint32_t count = guest_gl_table_tokens(&tokens, &functions);
        uint32_t gl_hits = 0U;

        for (i = 0U; i < count; ++i) {
            uint32_t key;
            guest_fn fn;
            const guest_dispatch_slot *slot = guest_dispatch_probe(tokens[i]);

            if (guest_dispatch_gl_token_of(i, &key, &fn)) {
                if (slot->key != key || slot->function != fn) {
                    status->reason = "a GL token does not resolve";
                    return 0;
                }
                ++gl_hits;
                continue;
            }
            if (tokens[i] != GUEST_DISPATCH_EMPTY_KEY &&
                slot->key == tokens[i]) {
                status->reason = "an ineligible GL token resolves";
                return 0;
            }
        }
        if (gl_hits != status->gl_tokens) {
            status->reason = "GL token census mismatch";
            return 0;
        }
    }
#endif
    for (i = 0U; i < g_import_count; ++i) {
        uint32_t rva = g_imports[i].slot_rva;
        uint32_t va = g_image_base + rva;

        if ((rva != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(rva)->key == rva) ||
            (va != GUEST_DISPATCH_EMPTY_KEY &&
             guest_dispatch_probe(va)->key == va)) {
            status->reason = "an IAT slot resolves as translated code";
            return 0;
        }
    }
    for (i = 0U; i < GUEST_DISPATCH_SLOT_COUNT; ++i) {
        const guest_dispatch_slot *slot = &g_guest_dispatch.slots[i];
        uint32_t index, family;

        if (slot->key == GUEST_DISPATCH_EMPTY_KEY) {
            if (slot->function != guest_dispatch_empty_slot) {
                status->reason = "an empty slot lost its trampoline";
                return 0;
            }
            continue;
        }
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        if (guest_dispatch_gl_token_slot_ok(slot))
            continue;
#endif
        family = slot->key & UINT32_C(0xff000000);
        if (!slot->function ||
            family == UINT32_C(0x7d000000) ||
            family == UINT32_C(0x7e000000) ||
            guest_import_lookup(slot->key, NULL) != NULL ||
            !guest_dispatch_find_rva(image_rva(slot->key), &index) ||
            g_fns[index] != slot->function ||
            guest_dispatch_probe(slot->key) != slot) {
            status->reason = "a slot disagrees with the complete path";
            return 0;
        }
    }
    return 1;
}

static void guest_dispatch_table_rebuild(void)
{
    guest_dispatch_table_status status;
    uint32_t i, key_count = 0U;
    uint64_t started = guest_dispatch_now_us();

    memset(&status, 0, sizeof status);
    /* Fail closed: guest_dispatch_table_build starts from a cleared table
     * and every path that does not end ready clears it again below, so an
     * empty table sends every call down the complete path. */
    for (i = 0U; i < g_count; ++i) {
        uint32_t key;

        if (i && g_addrs[i] <= g_addrs[i - 1U]) {
            status.reason = "function table is not strictly ascending";
            break;
        }
        if (guest_dispatch_key_of(i, &key))
            ++key_count;
        else
            ++status.skipped;
    }
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* Tokens share the key budget.  A table with functions and no tokens
     * (flag down) is still ready; a table with no functions is not. */
    if (key_count)
        key_count += guest_dispatch_gl_token_count();
#endif
    if (!status.reason) {
        if (!key_count)
            status.reason = "no eligible functions";
        else if (key_count > GUEST_DISPATCH_MAX_KEYS)
            status.reason = "too many functions for the table";
    }
    if (!status.reason && guest_dispatch_table_build(&status) &&
        guest_dispatch_table_verify(&status))
        status.ready = 1U;
    if (!status.ready) {
        guest_dispatch_table_clear();
        status.keys = 0U;
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        status.gl_tokens = 0U;
#endif
        if (!status.reason)
            status.reason = "the table did not build";
    }
    g_guest_dispatch_status = status;
    if (g_count && g_image_base) {
        if (status.ready)
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
            printf("guest: dispatch table ready: %u keys, %u skipped, "
                   "%u GL tokens, max bucket %u, %u bucket tries, %u KB, "
                   "built in %u us\n",
                   (unsigned)status.keys, (unsigned)status.skipped,
                   (unsigned)status.gl_tokens,
                   (unsigned)status.max_bucket,
                   (unsigned)status.bucket_tries,
                   (unsigned)(sizeof g_guest_dispatch / 1024U),
                   (unsigned)(guest_dispatch_now_us() - started));
#else
            printf("guest: dispatch table ready: %u keys, %u skipped, "
                   "max bucket %u, %u bucket tries, %u KB, built in %u us\n",
                   (unsigned)status.keys, (unsigned)status.skipped,
                   (unsigned)status.max_bucket,
                   (unsigned)status.bucket_tries,
                   (unsigned)(sizeof g_guest_dispatch / 1024U),
                   (unsigned)(guest_dispatch_now_us() - started));
#endif
        else
            printf("guest: dispatch table disabled (%s): every indirect "
                   "call takes the complete path\n", status.reason);
    }
}

void guest_dispatch_table_get_status(guest_dispatch_table_status *out)
{
    if (out)
        *out = g_guest_dispatch_status;
}
#endif

int guest_note_authenticated_translated_call(
    uint32_t addr, uint32_t coverage_function_id)
{
    guest_fn function;

    /* Match guest_call's logical census even when the native owner executes
     * the authenticated leaf itself.  guest_lookup preserves first-miss,
     * later-hit and lookup-iteration accounting; authorization makes the
     * cache state identical for any later ordinary indirect call. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    function = guest_lookup(addr);
    if (!function)
        return 0;
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    guest_dispatch_cache_authorize(addr, function);
#endif
    guest_coverage_function(coverage_function_id);
    return 1;
}

int guest_note_authenticated_import_call(
    uint32_t slot_addr, uint32_t import_id,
    uint32_t thunk_coverage_function_id)
{
    uint32_t registered_id = 0U;
    const guest_import *imported;

    /* The original path enters the one-instruction translated thunk before
     * guest_call observes the IAT slot. */
    guest_coverage_function(thunk_coverage_function_id);
    GUEST_PHASE_PROFILE_NOTE_CALL();
    imported = guest_import_lookup(slot_addr, &registered_id);
    if (!imported || registered_id != import_id)
        return 0;
    guest_coverage_import(registered_id);
    GUEST_PHASE_PROFILE_NOTE_IMPORT(registered_id);
    ++g_host_import_calls;
    return 1;
}

int guest_try_direct_sync_import_call(
    CPU *__restrict c, uint32_t target, uint32_t exact_slot_rva)
{
#if defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
    uint32_t import_id;
    uint32_t sync_index;

    /* The complete 413-row registration is the authority.  The generated
     * caller additionally pins one of the two IAT slots and its materialized
     * target; any failed re-registration, row-address drift, other slot or
     * other target reaches the original guest_call with no census,
     * coverage, stack or CPU side effect from this probe. */
    if (exact_slot_rva == ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS) {
        import_id = ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS;
        sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS;
    } else if (exact_slot_rva == ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) {
        import_id = ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS;
        sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS;
    } else {
        return 0;
    }
    if (!g_vita_sync_import_fastpath_ready ||
        !guest_direct_translated_target(target, exact_slot_rva) ||
        !g_imports || import_id >= g_import_count ||
        g_imports[import_id].slot_rva != exact_slot_rva)
        return 0;

    /* This is the same logical import call as guest_call's validated branch.
     * Imports do not contribute a guest-lookup event.  The focused oracle
     * checks the call/coverage/host counter and fail-closed result against the
     * original route.  Stall-ring breadcrumbs are diagnostic, not semantics. */
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* Sampler (research-sampler-hotlists finding 9): this route bypasses
     * guest_call, so publish the indirect callee (the IAT slot -> "ext") for
     * the duration of the host import and restore the enclosing target after;
     * without it the lock/unlock host time was named after the previous
     * vtable target (9.7 % of title render on the getter sub_005a0de0). */
    uint32_t sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = target;
#endif
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH();
    GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);
    guest_coverage_import(import_id);
#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
    /* Single-owner success states complete here with the endpoint's exact
     * memory, CPU and census effects; every other state continues into the
     * unchanged registered endpoint (host_vita_sync_fastpath.h).  This is
     * the only inline site: the generated pinned callers already bracket it
     * with GUEST_GPR_FLUSH/RELOAD, and guest_call keeps its OFF shape. */
    if (sync_index == ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS
            ? isaac_vita_sync_inline_enter(c, &g_host_import_calls)
            : isaac_vita_sync_inline_leave(c, &g_host_import_calls)) {
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        /* The inline state completed the import: same restore as below. */
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
        return 1;
    }
#endif
    if (isaac_vita_sync_import_indexed(
            c, sync_index, &g_host_import_calls)) {
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
        return 1;
    }
    guest_fault(c, target,
                "validated Vita sync import rejected its local ID");
    return 1;
#else
    (void)c;
    (void)target;
    (void)exact_slot_rva;
    return 0;
#endif
}

const char *guest_import_name(uint32_t slot_addr)
{
    const guest_import *imported = guest_import_lookup(slot_addr, NULL);
    return imported ? imported->name : NULL;
}

guest_fn guest_lookup(uint32_t addr)
{
#if defined(ISAAC_VITA_PHASE_PROFILE)
    uint32_t profile_iterations = 0u;
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    uint32_t cache_index;
    guest_lookup_cache_entry *cached;
#endif

    /* Aggregate only the binary searches whose CPU cost is under study. */
    GUEST_PHASE_PROFILE_NOTE_LOOKUP();
    /* Generated table keys are canonical RVAs. Direct test drivers and named
     * entry points naturally use those RVAs, but a real vtable or function
     * pointer has passed through the PE relocation table and is an in-image
     * VA. Normalise exactly at that boundary. The subtraction form is also an
     * overflow-safe range check for a 32-bit address space. */
    addr = image_rva(addr);

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    cache_index = guest_lookup_cache_index(addr);
    cached = &g_guest_lookup_cache[cache_index];
    if (cached->function && cached->key == addr) {
        GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_HIT();
        return cached->function;
    }
    GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_MISS();
#endif

    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
#if defined(ISAAC_VITA_PHASE_PROFILE)
        ++profile_iterations;
#endif
        if (g_addrs[mid] == addr) {
            GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(profile_iterations);
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
            cached->key = addr;
            cached->function = g_fns[mid];
            cached->dispatch_function = NULL;
#endif
            return g_fns[mid];
        }
        if (g_addrs[mid] < addr) lo = mid + 1; else hi = mid;
    }
    GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(profile_iterations);
    return NULL;
}

#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* The sampler names the callee of an indirect `call r/m32` from this word:
 * one plain store per indirect dispatch (~7k per frame), read torn-safe from
 * the sampling core.  Translated targets carry the image base, imports are
 * IAT slot addresses, GL/dynamic tokens are whatever the loader handed out.
 *
 * Publish/restore discipline (kage_vita_guest_sampler.c header): the word
 * must name the callee of the innermost *live* indirect dispatch, so a
 * finished vtable callee or import does not name the code that runs after
 * it.  The entry point that carries the one entry store -- the inline probe
 * with ISAAC_VITA_GUEST_DISPATCH_TABLE, the complete classification below
 * without it -- keeps that store unchanged; in the sampler build it compiles
 * as guest_call_dispatch and the wrapper after the probe restores the
 * enclosing value once the callee returns (guest_call_slow is never renamed:
 * the probe reaches it by its own name).  The two routes that never enter
 * guest_call -- guest_try_direct_sync_import_call and guest_import_call
 * (ISAAC_VITA_IMPORT_DIRECT) -- bracket their own endpoint call the same
 * way.  Production builds (sampler OFF) compile guest_call exactly as
 * before. */
volatile uint32_t g_kage_guest_last_indirect_target;
void guest_call_dispatch(CPU *__restrict c, uint32_t addr);
# define guest_call guest_call_dispatch
#endif

/* Complete classification of one indirect target, in the historical order:
 * confirmed dispatch-cache edge, IAT slot, native dynamic token, registered
 * translated function, fault.
 *
 * Without ISAAC_VITA_GUEST_DISPATCH_TABLE this IS guest_call, token for
 * token the historical function, so the option-OFF object is byte-identical
 * to the base (a separate always-inline helper moved GCC's unit-wide inline
 * budget and changed four unrelated functions by 2-6 bytes).  With the table
 * it is guest_call_slow, the out-of-line miss path: guest_call itself
 * (below) is the inline probe plus a tail call here, and this body counts
 * the entries that left the probe so the profiler derives the fast hits as
 * dispatch_calls - dispatch_slow. */
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
static GUEST_DISPATCH_NOINLINE void guest_call_slow(CPU *__restrict c,
                                                    uint32_t addr)
#else
void guest_call(CPU *__restrict c, uint32_t addr)
#endif
{
    uint32_t import_id = 0U;
    guest_fn fn;
        /* Temporary debugging: record the call trace */
    { extern void guest_trace_hit(uint32_t); guest_trace_hit(addr); }

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();
#else
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = addr;
#endif
    GUEST_PHASE_PROFILE_NOTE_CALL();
#endif
#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
    /* ---- ISAAC_VITA_GL_SHIM_FASTDISPATCH (wf/opt-glshim) begin ----------
     * Typed GL tokens live in the exact 0x7e family, which guest_host_dynamic
     * (host_vita_first_fault.c) forwards to the GL dynamic entry after the
     * XInput family (0x7d2xxxxx by static assertion) declines it.  With
     * g_gl_dynamic_first proven at registration (no IAT slot key and no
     * in-image RVA can alias the family) the cache probe and the import search
     * below would reject the token unconditionally, so entering the GL owner
     * here is the same outcome minus that work.  An unregistered token returns
     * zero without side effects and continues down the unchanged path to the
     * same "untranslated address" fault.
     * The family compare comes first: the ~14-20k non-GL indirect calls per
     * loop (perf:wf-gpr-v3 g(c) census) then pay one compare that GCC folds
     * into the cache probe's own 0x7d/0x7e rejection, and the flag load plus
     * its branch sit on the 0x7e edge only (prefix gate in
     * recomp/vita/test_gl_shim_fastdispatch.sh).  With
     * ISAAC_VITA_GUEST_DISPATCH_TABLE this block heads guest_call_slow: a
     * 0x7e token can never hit the inline probe's table (image RVAs only),
     * so it reaches here unchanged and the probe stays byte-identical.
     * With ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok) the registered
     * tokens are table keys while g_gl_dynamic_first holds and hit the probe
     * (gl_bridge.c guest_gl_table_dispatch); this block then serves only an
     * unregistered token, every token while the table is not ready, and
     * every token while the flag is down -- the same outcomes as before. */
    if ((addr & UINT32_C(0xff000000)) == UINT32_C(0x7e000000) &&
        g_gl_dynamic_first) {
#if defined(__vita__) && defined(ISAAC_VITA_STALL_PROBE)
        /* One breadcrumb per dispatch: an unregistered token is noted once,
         * by the legacy note ahead of guest_host_dynamic it falls through to. */
        if (guest_gl_token_is_registered(addr))
            KAGE_VITA_STALL_NOTE_DISPATCH(
                KAGE_VITA_STALL_DISPATCH_INDIRECT, image_rva(addr),
                image_rva(ld32(guest_stack_address(c, c->esp, 4U, addr))));
#endif
        if (isaac_vita_gl_dynamic_counted(c, addr, &g_host_dynamic_calls))
            return;
    }
    /* ---- ISAAC_VITA_GL_SHIM_FASTDISPATCH end ------------------------- */
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    fn = guest_dispatch_cache_probe(addr);
    if (fn) {
        /* Preserve the profiler's definition: this replaces one successful
         * guest_lookup cache hit, not a lookup or call from its census. */
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE_HIT();
#ifdef __vita__
        KAGE_VITA_STALL_NOTE_DISPATCH(
            KAGE_VITA_STALL_DISPATCH_INDIRECT, image_rva(addr),
            image_rva(ld32(guest_stack_address(c, c->esp, 4U, addr))));
#endif
        fn(c);
        return;
    }
#endif
    const guest_import *imported = guest_import_lookup(addr, &import_id);
    if (imported) {
#ifdef __vita__
        KAGE_VITA_STALL_NOTE_DISPATCH(
            KAGE_VITA_STALL_DISPATCH_IMPORT, image_rva(addr),
            image_rva(ld32(guest_stack_address(c, c->esp, 4U, addr))));
#endif
        /* Mark identity at the IAT boundary before either the implementation
         * or its loud unresolved-import fault can transfer control.  Pure
         * diagnostic guest_import_name() queries intentionally do not count. */
        guest_coverage_import(import_id);
        GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);
#ifdef ISAAC_VITA_IMPORT_ID_DISPATCH
#if defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
        if (g_vita_sync_import_fastpath_ready) {
            uint32_t sync_index;

            if (import_id == ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS)
                sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS;
            else if (import_id == ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS)
                sync_index = ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS;
            else
                goto generic_import_id_dispatch;

            /* Registration already proved the exact slot/name/family/local
             * tuple.  Enter the existing counted sync endpoint directly;
             * its Win32 stack, EAX, fault and recursive-lock semantics remain
             * the same implementation as generic ID dispatch.
             *
             * The inline single-owner path (ISAAC_VITA_SYNC_INLINE_FASTPATH)
             * is deliberately not duplicated here.  Doing so grew this
             * dispatcher from 152 to 272 instructions and pushed its non-sync
             * branches out of cbz range (one cbz became cmp + beq.w on the
             * path every host import takes), so guest_call stays
             * byte-identical with the option ON and OFF and only the pinned
             * direct sites (guest_try_direct_sync_import_call) run inline. */
            GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH();
            if (isaac_vita_sync_import_indexed(
                    c, sync_index, &g_host_import_calls))
                return;
            guest_fault(c, addr,
                        "validated Vita sync import rejected its local ID");
            return;
        }
generic_import_id_dispatch:
#endif
        {
            int handled = guest_host_import_id(c, import_id);
            if (handled > 0)
                return;
            if (handled < 0) {
                guest_fault(c, addr, guest_host_import_id_error());
                return;
            }
        }
#else
        if (guest_host_import(c, imported->name))
            return;
#endif
        guest_fault(c, addr, imported->name);
        return;
    }
#ifdef __vita__
    /* Record before either native dynamic dispatch or the translated owner:
     * a hang inside either path must leave its entry breadcrumb behind. */
    KAGE_VITA_STALL_NOTE_DISPATCH(
        KAGE_VITA_STALL_DISPATCH_INDIRECT, image_rva(addr),
        image_rva(ld32(guest_stack_address(c, c->esp, 4U, addr))));
#endif
    if (guest_host_dynamic(c, addr))
        return;
    fn = guest_lookup(addr);
    if (!fn) {
        /* ---- IAT thunk ----
         *
                  * MSVC incremental linking (/INCREMENTAL) generates a 6-byte trampoline per import:
                  *     jmp dword ptr [IAT slot]
                  * The game uses the **address** of these trampolines as a function pointer (typically passing it as a callback to
                  * host Lua: luaL_requiref(L,"base",&thunk_luaopen_base,1)).
                  * They are not functions and recursive descent cannot find them, so they are not in the function table.
         *
                  * This recognises the pattern and forwards to the IAT slot itself -- tag_imports has already written
                  * each slot's contents as its own address, so one more guest_call hits the
                  * import dispatch table. Tried only after a table lookup fails; the normal path costs nothing. */
        if (guest_image_contains(addr, 6U)) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
            if (p[0] == 0xffU && p[1] == 0x25U) {
                uint32_t slot;
                memcpy(&slot, p + 2, 4U);
                if (guest_image_contains(slot, 4U) && slot != addr) {
                    guest_call(c, slot);
                    return;
                }
            }
        }
        guest_fault(c, addr, "indirect call to untranslated address");
        return;
    }
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    /* Authorize only after both higher-priority namespaces rejected the key.
     * A collision, re-registration, or import-table change clears this edge
     * and naturally returns to the complete path above. */
    guest_dispatch_cache_authorize(addr, fn);
#endif
    fn(c);
}

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* The inline O(1) probe: frameless, one census increment, and a tail call
 * either into the resolved translated function or into guest_call_slow
 * (the complete classification above). */
void guest_call(CPU *__restrict c, uint32_t addr)
{
    const guest_dispatch_slot *slot;

#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = addr;
#endif
    /* The only hot-path census increment (see GuestPhaseProfileCounters). */
    GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
    slot = guest_dispatch_probe(addr);
    if (slot->key == addr) {
#ifdef __vita__
        KAGE_VITA_STALL_NOTE_DISPATCH(
            KAGE_VITA_STALL_DISPATCH_INDIRECT, image_rva(addr),
            image_rva(ld32(guest_stack_address(c, c->esp, 4U, addr))));
#endif
        slot->function(c);
        return;
    }
    guest_call_slow(c, addr);
}
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER)
# undef guest_call
/* Sampler build only: the publish/restore wrapper around guest_call_dispatch
 * (the inline probe with the dispatch table, the complete classification
 * without it); see the note above g_kage_guest_last_indirect_target. */
void guest_call(CPU *__restrict c, uint32_t addr)
{
    uint32_t sampler_enclosing_target = g_kage_guest_last_indirect_target;

    guest_call_dispatch(c, addr);
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
}
#endif

#if defined(ISAAC_VITA_IMPORT_DIRECT)
static void guest_import_direct_rebuild(int registration_ok)
{
    uint32_t i;

    memset(g_guest_import_direct, 0, sizeof g_guest_import_direct);
    if (!registration_ok || !g_imports ||
        g_import_count > GUEST_IMPORT_DIRECT_CAPACITY)
        return;
    for (i = 0; i < g_import_count; i++) {
        guest_import_family_fn fn = NULL;
        uint32_t local_index = 0U;

        /* The same endpoint/local pair guest_host_import_id() dispatches to:
         * both are derived from one binding function in host_vita_import_id.c,
         * so the two routes cannot disagree about a row. */
        if (!guest_host_import_id_direct_binding(i, &fn, &local_index) ||
            !fn)
            continue;
        g_guest_import_direct[i].fn = fn;
        g_guest_import_direct[i].local_index = local_index;
        g_guest_import_direct[i].slot_va =
            (uint32_t)(GUEST_IMAGE_BASE + g_imports[i].slot_rva);
    }
}

/* Generated `GUEST_IMPORT_CALL/JMP` sites (guest.h).  `target` is the word
 * the site read from its IAT slot; `import_id` the emitter's dense ID. */
void guest_import_call(CPU *__restrict c, uint32_t target, uint32_t import_id)
{
    const guest_import_direct *entry;

    /* Fail closed: an ID outside the table, a slot value other than the
     * validated token of exactly this ID (slot never tagged, table
     * re-registered and rejected, guest store into the IAT, emitter/ID drift)
     * or an entry without a family (unresolved row, family compiled out,
     * registration refused -- its token is 0, which a zeroed slot word would
     * match) all take the complete original path with no side effect from
     * this probe. */
    if (import_id >= GUEST_IMPORT_DIRECT_CAPACITY) {
        guest_call(c, target);
        return;
    }
    entry = &g_guest_import_direct[import_id];
    if (target != entry->slot_va || !entry->fn) {
        guest_call(c, target);
        return;
    }
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* Sampler (kage_vita_guest_sampler.c header): this route never enters
     * guest_call's wrapper, so publish the IAT slot ("ext") for the import's
     * duration and restore the enclosing target once the endpoint returns.
     * Without the restore every translated body that kept running after a
     * direct import was named "ext" until its enclosing guest_call returned. */
    uint32_t sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = target;
#endif
    /* The same logical import call as guest_call's validated branch: one
     * call census event, no lookup event, the per-import census note and
     * import coverage before either the endpoint or its loud unresolved
     * fault can transfer control (ph120.i/ph120.ik/ph120.ih count this route
     * exactly like the indexed path).  The stall-ring breadcrumb is
     * diagnostic and stays on the original path. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id);
    guest_coverage_import(import_id);
    if (entry->fn(c, entry->local_index, &g_host_import_calls)) {
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
        return;
    }
    /* guest_fault never returns to translated code (longjmp to the run scope
     * or abort): no live frame is left for the word to be restored for. */
    guest_fault(c, target, g_imports[import_id].name);
}
#endif

/* --------------------------------------------------- setjmp / longjmp --- */

enum {
    GUEST_JB_EBP          = 0x00,
    GUEST_JB_EBX          = 0x04,
    GUEST_JB_EDI          = 0x08,
    GUEST_JB_ESI          = 0x0c,
    GUEST_JB_ESP          = 0x10,
    GUEST_JB_EIP          = 0x14,
    GUEST_JB_REGISTRATION = 0x18,
    GUEST_JB_TRYLEVEL     = 0x1c,
    GUEST_JB_COOKIE       = 0x20,
    GUEST_JB_UNWINDFUNC   = 0x24
};

#define GUEST_JB_COOKIE_VALUE 0x56433230U

void guest_setjmp_prepare(CPU *__restrict c, guest_jump_site *site)
{
    uint32_t guest_env, count, guest_eip, fs, registration, try_level;

    if (!c || !site) abort();
    guest_env = ld32(guest_stack_address(c, c->esp + 4U, 4U, 0U));
    count = ld32(guest_stack_address(c, c->esp + 8U, 4U, 0U));
    guest_eip = ld32(guest_stack_address(c, c->esp, 4U, 0U));
    if (!guest_env) {
        guest_fault(c, 0U, "_setjmp3 received a null jump buffer");
        return;
    }
    /* The exact Isaac census contains two `_setjmp3(env, 0)` calls.  The
     * non-zero forms carry compiler-specific unwind metadata; accepting one
     * without executing that unwind would turn an error path into corruption. */
    if (count != 0U) {
        guest_fault(c, count, "_setjmp3 unwind arguments are unsupported");
        return;
    }

    fs = guest_fs_base(c);
    if (!fs) {
        guest_fault(c, 0U, "_setjmp3 could not obtain the guest TIB");
        return;
    }
    registration = ld32(fs);
    try_level = registration == 0xffffffffU
              ? 0xffffffffU : ld32(registration + 0x0cU);

    /* This is the current MSVC x86 _JUMP_BUFFER layout, confirmed against
     * the installed vcruntime140 `_setjmp3` bytes.  ESP still points at the
     * guest return word; finish pops only that word, preserving cdecl args. */
    st32(guest_env + GUEST_JB_EBP, c->ebp);
    st32(guest_env + GUEST_JB_EBX, c->ebx);
    st32(guest_env + GUEST_JB_EDI, c->edi);
    st32(guest_env + GUEST_JB_ESI, c->esi);
    st32(guest_env + GUEST_JB_ESP, c->esp);
    st32(guest_env + GUEST_JB_EIP, guest_eip);
    st32(guest_env + GUEST_JB_REGISTRATION, registration);
    st32(guest_env + GUEST_JB_TRYLEVEL, try_level);
    st32(guest_env + GUEST_JB_COOKIE, GUEST_JB_COOKIE_VALUE);
    st32(guest_env + GUEST_JB_UNWINDFUNC, 0U);
    /* `_setjmp3(env, 0)` deliberately leaves UnwindData[6] untouched. */

    site->guest_env = guest_env;
    site->guest_esp = c->esp;
    site->guest_eip = guest_eip;
    site->guest_registration = registration;
    if (c->jump_sites != site) {
        site->previous = c->jump_sites;
        c->jump_sites = site;
    }
}

void guest_setjmp_finish(CPU *__restrict c, guest_jump_site *site,
                         int resumed)
{
    uint32_t guest_eip;
    if (!c || !site) abort();
    if (c->jump_sites != site) {
        guest_fault(c, 0U, "_setjmp3 native site is not active");
        return;
    }
    if (resumed) {
        /* `_setjmp3` saved the pre-RET ESP.  MSVC x86 longjmp restores it as
         * `mov esp,[env+10h]; add esp,4; jmp [env+14h]`.  The word at the
         * saved ESP is a dead call-frame slot after the initial return and is
         * allowed to be reused -- the live libpng path puts longjmp's own
         * return there.  Native control already resumed at the saved generated
         * continuation, so reproduce the +4 without reading that stale word. */
        if (!guest_stack_adjust(c, 4U, site->guest_eip))
            return;
    } else {
        guest_eip = gpop_at(c, site->guest_eip);
        if (guest_eip != site->guest_eip) {
            guest_fault(c, guest_eip,
                        "_setjmp3 guest return address changed");
            return;
        }
    }
    c->eax = resumed ? c->jump_value : 0U;
    if (resumed) c->jump_value = 0U;
}

void guest_setjmp_leave(CPU *__restrict c, guest_jump_site *site)
{
    guest_jump_site *p;
    if (!c || !site) return;
    if (c->jump_sites == site) {
        c->jump_sites = site->previous;
        return;
    }
    /* A site pruned by a longjmp to an older destination is already gone.
     * Finding it below another live site is instead a broken non-LIFO owner
     * cleanup; rewiring live automatic records would make later native jumps
     * unsafe, so stop loudly. */
    for (p = c->jump_sites; p; p = p->previous) {
        if (p == site) {
            guest_fault(c, site->guest_eip,
                        "_setjmp3 owner left out of order");
            return;
        }
    }
}

GUEST_NORETURN void guest_longjmp(CPU *__restrict c, uint32_t guest_env,
                                  int32_t value)
{
    guest_jump_site *site;
    uint32_t fs, registration;

    if (!c) abort();
    for (site = c->jump_sites; site; site = site->previous)
        if (site->guest_env == guest_env) break;
    if (!site) {
        guest_fault(c, guest_env, "longjmp has no active guest setjmp");
        abort();
    }
    if (ld32(guest_env + GUEST_JB_ESP) != site->guest_esp ||
        ld32(guest_env + GUEST_JB_EIP) != site->guest_eip ||
        ld32(guest_env + GUEST_JB_REGISTRATION) !=
            site->guest_registration ||
        ld32(guest_env + GUEST_JB_COOKIE) != GUEST_JB_COOKIE_VALUE ||
        ld32(guest_env + GUEST_JB_UNWINDFUNC) != 0U) {
        guest_fault(c, guest_env, "longjmp received a corrupt guest jump buffer");
        abort();
    }

    fs = guest_fs_base(c);
    registration = fs ? ld32(fs) : 0U;
    if (!fs || registration != site->guest_registration) {
        /* Native C frames can be unwound now, but a different guest FS head
         * would also require running x86 SEH handlers/finally blocks. */
        guest_fault(c, registration,
                    "longjmp guest SEH unwind is unsupported");
        abort();
    }

    c->ebp = ld32(guest_env + GUEST_JB_EBP);
    c->ebx = ld32(guest_env + GUEST_JB_EBX);
    c->edi = ld32(guest_env + GUEST_JB_EDI);
    c->esi = ld32(guest_env + GUEST_JB_ESI);
    if (!guest_stack_set(c, site->guest_esp, site->guest_eip))
        abort();
    c->jump_value = value ? (uint32_t)value : 1U;
    c->jump_sites = site;       /* records above the target are now stale */
    longjmp(site->native_env, 1);
}

int guest_tls_process_attach(CPU *__restrict c)
{
    uint32_t i, fs, tls_array;
    if (!c) return -1;
    if (!g_image_mem) {
        guest_fault(c, 0U, "static TLS attach without a loaded image");
        return 1;
    }
    if (!g_tls_present) return 0;
    if (!g_tls_block || g_tls_index >= GUEST_TLS_SLOTS) {
        guest_fault(c, 0U, "static TLS metadata is incomplete");
        return 1;
    }
    if (g_tls_process_attached) return 0;
    fs = guest_fs_base(c);         /* binds the block into fs:[0x2c][index] */
    if (!fs) {
        guest_fault(c, 0U, "cannot allocate the guest TIB for static TLS");
        return 1;
    }
    tls_array = ld32(fs + 0x2CU);
    if (!tls_array || ld32(tls_array + g_tls_index * 4U) !=
                      (uint32_t)(uintptr_t)g_tls_block) {
        guest_fault(c, 0U, "static TLS block was not bound into the guest TIB");
        return 1;
    }
    for (i = 0; i < g_tls_callback_count; ++i) {
        uint32_t saved = c->esp;
        gpush(c, 0U);              /* Reserved */
        gpush(c, 1U);              /* DLL_PROCESS_ATTACH */
        gpush(c, g_image_base);    /* DllHandle */
        gpush(c, 0xFFF715A1U);     /* synthetic native-loader return */
        guest_call(c, g_tls_callbacks[i]);
        if (c->fault) return 1;
        if (c->esp != saved) {
            guest_fault(c, g_tls_callbacks[i],
                        "TLS callback did not clean its stdcall arguments");
            return 1;
        }
    }
    g_tls_process_attached = 1;
    return 0;
}

int guest_run_until_stop(CPU *__restrict c, guest_fn entry)
{
    guest_run_scope scope;
    if (!c || !entry || c->run_scope) return GUEST_RUN_INVALID;

    c->fault = NULL;
    c->fault_addr = 0;
    c->exit_api = NULL;
    c->exit_code = 0;
    c->stop_kind = GUEST_RUN_RETURNED;
    c->run_scope = &scope;

    /* ISO C only permits setjmp as a full controlling expression (or a small
     * set of equivalent forms). `jumped = setjmp(...)` happened to work in
     * MSVC x86 but is undefined C and is not a Vita/newlib contract. */
    if (setjmp(scope.env) == 0) {
        entry(c);
        if (c->jump_sites)
            guest_fault(c, 0U, "translated call returned with active setjmp");
    }

    /* A fault/exit boundary unwinds every generated native frame.  Any site
     * record left in one of those frames is invalid even though guest memory
     * remains available for diagnostics. */
    c->jump_sites = NULL;
    c->jump_value = 0U;
#if defined(ISAAC_VITA_LUA) && ISAAC_VITA_LUA
    /* guest_fault/guest_exit longjmp over native Lua and translated callback
     * activations.  Drop their process-local ownership before this CPU can be
     * run again; Lua state storage itself remains owned by lua_close. */
    isaac_vita_lua_abort_cpu(c);
#endif
    c->run_scope = NULL;
    return c->stop_kind;
}

/* ------------------------------------------------------------- faults --- */

unsigned g_int3_count, g_fault_count;
uint32_t g_guest_checkpoint_rva;

int guest_checkpoint(CPU *__restrict c, uint32_t rva)
{
    if (!g_guest_checkpoint_rva || rva != g_guest_checkpoint_rva)
        return 0;
    guest_fault(c, rva, "bring-up checkpoint");
    return 1;
}

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    guest_run_scope *scope;
    g_fault_count++;
    c->fault = what;
    c->fault_addr = addr;
    c->exit_api = NULL;
    c->stop_kind = GUEST_RUN_FAULT;
    fprintf(stderr, "guest: FAULT at %08x: %s\n", addr, what);
    scope = (guest_run_scope *)c->run_scope;
    if (scope)
        longjmp(scope->env, 1);

    /* Returning would let generated callers continue with a guest return
     * address still on ESP. That converts the useful first fault into an
     * unrelated memory violation. Unit and production entry points must use
     * guest_run_until_stop; an unguarded fault is a runtime contract breach. */
    abort();
}

void guest_exit(CPU *__restrict c, int32_t code, const char *api)
{
    guest_run_scope *scope;
    c->fault = NULL;
    c->fault_addr = 0U;
    c->exit_api = api;
    c->exit_code = code;
    c->stop_kind = GUEST_RUN_EXIT;
    fprintf(stderr, "guest: EXIT via %s with code %d\n",
            api ? api : "<unknown>", code);
    scope = (guest_run_scope *)c->run_scope;
    if (scope)
        longjmp(scope->env, 1);

    /* A guest noreturn API must never fall back into generated code. */
    abort();
}

void guest_int3(CPU *__restrict c, uint32_t addr)
{
    (void)c; (void)addr;
    g_int3_count++;      /* the guest's own deliberate trap; counted, not fatal */
}

/* fs:[0] is the TIB head, used by the SEH prologue. Nothing in the ladder
 * needs a real one, but it must resolve rather than silently return 0 -- so
 * hand out a small zeroed block and record that it was touched. */
unsigned g_fs_touches;
uint32_t g_guest_fs_base_cached;

uint32_t guest_fs_base(CPU *c)
{
    (void)c;
    g_fs_touches++;
    if (!g_tib) {
        uint32_t self, tls;
        g_tib = (unsigned char *)guest_low_alloc(4096);
        if (!g_tib) return 0;
        self = (uint32_t)(uintptr_t)g_tib;
        tls = self + 0x100U;        /* zeroed 64-entry TLS pointer array */
        *(uint32_t *)(g_tib + 0x00) = 0xFFFFFFFFu; /* SEH chain head */
        *(uint32_t *)(g_tib + 0x18) = self;        /* NT_TIB.Self */
#ifdef __vita__
        *(uint32_t *)(g_tib + 0x20) = (uint32_t)sceKernelGetProcessId();
        *(uint32_t *)(g_tib + 0x24) = (uint32_t)sceKernelGetThreadId();
#else
        *(uint32_t *)(g_tib + 0x20) = GetCurrentProcessId();
        *(uint32_t *)(g_tib + 0x24) = GetCurrentThreadId();
#endif
        *(uint32_t *)(g_tib + 0x2C) = tls;
                /* A new thread needs its own TLS instance (copied from the template) */
        if (g_tls_template && g_tls_block == g_tls_template) {
            void *nb = guest_low_alloc(g_tls_block_size ? g_tls_block_size : 1U);
            if (nb) {
                memcpy(nb, g_tls_template, g_tls_block_size);
                g_tls_block = nb;
            }
        }
        fprintf(stderr, "[guest] TIB allocated @ %p  tls=0x%08x tlsblk=0x%08x\n",
                (void *)g_tib, tls, (unsigned)(uintptr_t)g_tls_block);
    }
        /* StackBase/StackLimit belong to **this CPU's** guest stack. Use c's own bounds,
          * never a global -- under multithreading that would write another thread's stack. */
    *(uint32_t *)(g_tib + 0x04) = c ? c->stack_ceiling : g_stack_top;
    *(uint32_t *)(g_tib + 0x08) = c ? c->stack_floor   : g_stack_base;
    if (g_fs_touches <= 3)
        fprintf(stderr, "[guest] fs_base #%u -> 0x%08x (stack 0x%08x..0x%08x)\n",
                g_fs_touches, (unsigned)(uintptr_t)g_tib, g_stack_base, g_stack_top);
    if (g_tls_block && g_tls_index < GUEST_TLS_SLOTS) {
        uint32_t tls = *(uint32_t *)(g_tib + 0x2C);
        *(uint32_t *)(uintptr_t)(tls + g_tls_index * 4U) =
            (uint32_t)(uintptr_t)g_tls_block;
    }
    /* Every word written above is now current; generated units compiled with
     * GUEST_FS_BASE_INLINE answer from this until a writer above changes it. */
    g_guest_fs_base_cached = (uint32_t)(uintptr_t)g_tib;
    return (uint32_t)(uintptr_t)g_tib;
}
