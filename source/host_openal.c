/* host_openal.c —— forward the OpenAL32.dll imports to real OpenAL Soft
 *
 * ==========================================================================
 * 1. Why this file has to exist
 *
 * The original OpenAL layer was a **pure bookkeeping stub**: alSourcePlay only
 * flipped a state flag to AL_PLAYING, alBufferData only accumulated a byte
 * count without keeping the data, and the whole file had no audio output
 * backend at all. So that port **never actually made a sound**.
 *
 * The user-supplied TBOI-Afterbirth-Plus apkvision APK ships
 * libopenalisaac.so = **OpenAL Soft 1.23.1** (OpenSL ES backend).
 * Measured on this device, it opens the device, creates a context and plays
 * buffers normally:
 *     alcOpenDevice -> 0xb4000070a52e7010
 *     alBufferData done, alGetError=0
 *     alSourcePlay called, alGetError=0     ← genuinely audible
 *
 * So we no longer write our own audio backend; we forward to OpenAL Soft.
 *
 * ==========================================================================
 * 2. Two technical problems that must be handled
 *
 * 1. **Pointer width**. ALCdevice* / ALCcontext* are 64-bit pointers
 *    (0xb4000070…), while the guest only has 32-bit registers.
 *    Approach: a handle table. The real pointer stays in the table and a
 *    small token (0x00D0xxxx) is returned. When the guest passes it back,
 *    we look it up and restore the pointer.
 *
 * 2. **Float arguments**. The parameters of alSource3f / alSourcef /
 *    alListener3f / alListenerfv / alGetSourcef are float, and the x86
 *    caller pushes them as 32-bit values; what we read out is a uint32 bit
 *    pattern, so it must be reinterpreted bitwise as float,
 *    not numerically converted.
 *
 * 3. **Guest pointers**. Guest memory is identity-mapped into the host's low
 *    4GB, so (void*)(uintptr_t)guest_ptr is directly usable
 *    (ALuint* / ALfloat*).
 *
 * ==========================================================================
 * 3. Behaviour on failure
 *
 * If dlopen or alcOpenDevice fails, every forwarding function returns 0 and
 * the old stub in host_gl_audio.c keeps working as before (the game keeps
 * running, just without sound). So this layer is **purely additive** and
 * introduces no new crash points.
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "guest.h"

/* ---- stdcall/cdecl helpers (same as host_gl_audio.c) ---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cret(CPU *__restrict c) { (void)gpop(c); }

/* reinterpret the 32-bit bit pattern on the stack as float */
static float Af(CPU *__restrict c, int i)
{
    uint32_t v = A(c, i);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}
/* write a float as a 32-bit bit pattern on the guest side */
static uint32_t fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof v);
    return v;
}

/* ==========================================================================
 * Function pointers into real OpenAL
 * ========================================================================== */
typedef void ALCdevice;
typedef void ALCcontext;

static void *g_ol;
static int   g_ol_state;         /* 0 not tried  1 usable  -1 unavailable */
static int   g_ctx_ready;

static ALCdevice  *(*p_alcOpenDevice)(const char *);
static void        (*p_alcCloseDevice)(ALCdevice *);
static ALCcontext *(*p_alcCreateContext)(ALCdevice *, const int *);
static int         (*p_alcMakeContextCurrent)(ALCcontext *);
static void        (*p_alcDestroyContext)(ALCcontext *);
static void        (*p_alcProcessContext)(ALCcontext *);

static void (*p_alGenBuffers)(int, unsigned *);
static void (*p_alDeleteBuffers)(int, const unsigned *);
static void (*p_alBufferData)(unsigned, int, const void *, int, int);
static void (*p_alGenSources)(int, unsigned *);
static void (*p_alDeleteSources)(int, const unsigned *);
static void (*p_alSourcePlay)(unsigned);
static void (*p_alSourceStop)(unsigned);
static void (*p_alSourcePause)(unsigned);
static void (*p_alSourceRewind)(unsigned);
static void (*p_alSourcef)(unsigned, int, float);
static void (*p_alSource3f)(unsigned, int, float, float, float);
static void (*p_alSourcei)(unsigned, int, int);
static void (*p_alGetSourcei)(unsigned, int, int *);
static void (*p_alGetSourcef)(unsigned, int, float *);
static void (*p_alSourceQueueBuffers)(unsigned, int, const unsigned *);
static void (*p_alSourceUnqueueBuffers)(unsigned, int, unsigned *);
static void (*p_alListenerf)(int, float);
static void (*p_alListener3f)(int, float, float, float);
static void (*p_alListenerfv)(int, const float *);
static int  (*p_alGetError)(void);
static void (*p_alDistanceModel)(int);
static void (*p_alDopplerFactor)(float);

/* ---- handle table: real 64-bit pointer ↔ 32-bit token ---- */
#define OAL_MAX_HANDLES 16
static struct { uint32_t token; void *ptr; } g_h[OAL_MAX_HANDLES];
static uint32_t g_h_next = 0x00D00001u;

/* counters (objective proof that audio data is flowing, not by "listening") */
static unsigned long g_n_bufdata, g_n_play, g_n_gen_src, g_n_gen_buf;
static unsigned long long g_bytes_pcm;

static void oal_stats(const char *why)
{
    fprintf(stderr, "[openal] stats(%s): buffer=%lu source=%lu alBufferData=%lu(%llu MB) "
            "alSourcePlay=%lu\n",
            why, g_n_gen_buf, g_n_gen_src, g_n_bufdata,
            g_bytes_pcm / (1024ull * 1024ull), g_n_play);
    fflush(NULL);
}

void host_openal_report(void) { if (g_ol_state > 0) oal_stats("heartbeat"); }

static uint32_t h_store(void *p)
{
    int i;
    if (!p) return 0;
    for (i = 0; i < OAL_MAX_HANDLES; i++) {
        if (!g_h[i].ptr) {
            g_h[i].token = g_h_next++;
            g_h[i].ptr = p;
            return g_h[i].token;
        }
    }
    return 0;
}
static void *h_get(uint32_t tok)
{
    int i;
    for (i = 0; i < OAL_MAX_HANDLES; i++)
        if (g_h[i].ptr && g_h[i].token == tok) return g_h[i].ptr;
    return NULL;
}
static void h_drop(uint32_t tok)
{
    int i;
    for (i = 0; i < OAL_MAX_HANDLES; i++)
        if (g_h[i].ptr && g_h[i].token == tok) { g_h[i].ptr = NULL; return; }
}

/* ==========================================================================
 * Initialization
 * ========================================================================== */
static int oal_init(void)
{
    const char *cands[4];
    int i;

    if (g_ol_state) return g_ol_state > 0;

    /* Lookup order:
     *   1) environment variable (for debugging)
     *   2) default library name —— the Android linker looks it up in the
     *      app's nativeLibraryDir
     *   3) a file of the same name next to our own .so (Java copies it into
     *      files/lib/)
     */
    cands[0] = getenv("ISAAC_OPENAL");
    cands[1] = "libopenal.so";
    cands[2] = NULL;              /* filled in below via dladdr with the sibling path */
    cands[3] = NULL;

    /* Same-directory fallback: if Java copied both libopenal.so and
     * libisaac.so into files/lib/, derive it from our own path. dladdr
     * yields dli_fname for a loaded .so. */
    {
        static char selfbuf[512];
        Dl_info di;
        if (dladdr((void *)(uintptr_t)&oal_init, &di) && di.dli_fname) {
            const char *slash = strrchr(di.dli_fname, '/');
            if (slash) {
                size_t pre = (size_t)(slash - di.dli_fname) + 1u;
                if (pre < sizeof selfbuf - 16u) {
                    memcpy(selfbuf, di.dli_fname, pre);
                    strcpy(selfbuf + pre, "libopenal.so");
                    cands[2] = selfbuf;
                }
            }
        }
    }

    for (i = 0; i < 3; i++) {
        if (!cands[i]) continue;
        g_ol = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
        if (g_ol) {
            fprintf(stderr, "[openal] ✓ loaded %s\n", cands[i]);
            break;
        }
    }
    if (!g_ol) {
        fprintf(stderr, "[openal] ✗ cannot find libopenal.so: %s\n", dlerror());
        fprintf(stderr, "[openal]   audio stays silent (old stub behaviour)\n");
        g_ol_state = -1;
        return 0;
    }

#define SYM(var, name) \
    do { *(void **)(&var) = dlsym(g_ol, name); \
         if (!var) { fprintf(stderr, "[openal] ✗ missing symbol %s\n", name); \
                     g_ol_state = -1; return 0; } } while (0)

    SYM(p_alcOpenDevice,        "alcOpenDevice");
    SYM(p_alcCloseDevice,       "alcCloseDevice");
    SYM(p_alcCreateContext,     "alcCreateContext");
    SYM(p_alcMakeContextCurrent,"alcMakeContextCurrent");
    SYM(p_alcDestroyContext,    "alcDestroyContext");
    SYM(p_alcProcessContext,    "alcProcessContext");
    SYM(p_alGenBuffers,         "alGenBuffers");
    SYM(p_alDeleteBuffers,      "alDeleteBuffers");
    SYM(p_alBufferData,         "alBufferData");
    SYM(p_alGenSources,         "alGenSources");
    SYM(p_alDeleteSources,      "alDeleteSources");
    SYM(p_alSourcePlay,         "alSourcePlay");
    SYM(p_alSourceStop,         "alSourceStop");
    SYM(p_alSourcePause,        "alSourcePause");
    SYM(p_alSourcef,            "alSourcef");
    SYM(p_alSource3f,           "alSource3f");
    SYM(p_alSourcei,            "alSourcei");
    SYM(p_alGetSourcei,         "alGetSourcei");
    SYM(p_alGetSourcef,         "alGetSourcef");
    SYM(p_alSourceQueueBuffers, "alSourceQueueBuffers");
    SYM(p_alSourceUnqueueBuffers,"alSourceUnqueueBuffers");
    SYM(p_alListenerf,          "alListenerf");
    SYM(p_alListener3f,         "alListener3f");
    SYM(p_alListenerfv,         "alListenerfv");
    SYM(p_alGetError,           "alGetError");
    p_alDistanceModel = dlsym(g_ol, "alDistanceModel");
    p_alDopplerFactor = dlsym(g_ol, "alDopplerFactor");
    p_alSourceRewind  = dlsym(g_ol, "alSourceRewind");
#undef SYM

    g_ol_state = 1;
    fprintf(stderr, "[openal] ✓ OpenAL Soft ready (real audio)\n");
    return 1;
}

/* Open the device and create the context (idempotent). Used by alcOpenDevice. */
static int oal_ensure_context(void)
{
    ALCdevice *d;
    ALCcontext *ctx;
    if (g_ctx_ready) return 1;
    if (!oal_init()) return 0;

    d = p_alcOpenDevice(NULL);
    if (!d) {
        fprintf(stderr, "[openal] ✗ alcOpenDevice failed\n");
        g_ol_state = -1;
        return 0;
    }
    ctx = p_alcCreateContext(d, NULL);
    if (!ctx || !p_alcMakeContextCurrent(ctx)) {
        fprintf(stderr, "[openal] ✗ failed to create/activate the context\n");
        g_ol_state = -1;
        return 0;
    }
    g_ctx_ready = 1;
    fprintf(stderr, "[openal] ✓ device and context ready\n");
    return 1;
}

/* ==========================================================================
 * Forwarding dispatch
 *
 * Returns 1 = handled (including the case "real OpenAL is unavailable but
 * this name is mine to answer"? No —— return 0 and let the old stub handle
 * it, so on failure behaviour falls back exactly to the original).
 * ========================================================================== */

/* name → forward */
int host_openal_try(CPU *__restrict c, const char *name)
{
    const char *n;
    if (!c || !name) return 0;
    n = strchr(name, '!');
    n = n ? n + 1 : name;

    /* The three alc* entry points must have their handles managed by us, so
     * they always take this branch (even if initialization failed we still
     * return 0 so the old stub can answer "success" and the game does not
     * hang in init). */
    if (strcmp(n, "alcOpenDevice") == 0) {
        uint32_t dummy = A(c, 0);           /* device name string, ignored */
        (void)dummy;
        if (!oal_ensure_context()) return 0;
        /* the context is already up, so the device handle is the one it
         * belongs to; a fixed token denotes it */
        c->eax = 0x00D00001u;
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcCreateContext") == 0) {
        uint32_t dev = A(c, 0);
        if (!oal_ensure_context() || dev != 0x00D00001u) return 0;
        c->eax = 0x00D00002u;               /* context token */
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcMakeContextCurrent") == 0) {
        (void)A(c, 0);
        if (!g_ctx_ready) return 0;
        c->eax = 1u;                        /* ALC_TRUE */
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcProcessContext") == 0) {
        if (!g_ctx_ready) return 0;
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcDestroyContext") == 0) {
        (void)A(c, 0);
        if (!g_ctx_ready) return 0;
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcCloseDevice") == 0) {
        (void)A(c, 0);
        if (!g_ol_state) return 0;
        cret(c);
        return 1;
    }

    /* all remaining al* calls require a ready context */
    if (!g_ctx_ready) return 0;

    if (strcmp(n, "alGenBuffers") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 0);
        uint32_t out = A(c, 1);
        if (cnt <= 0 || cnt > 64 || !out) { c->eax = 0; cret(c); return 1; }
        p_alGenBuffers(cnt, ids);
        g_n_gen_buf += (unsigned long)cnt;
        for (int i = 0; i < cnt; i++) st32(out + (uint32_t)i * 4u, ids[i]);
        cret(c); return 1;
    }
    if (strcmp(n, "alDeleteBuffers") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 0);
        uint32_t p = A(c, 1);
        if (cnt <= 0 || cnt > 64 || !p) { cret(c); return 1; }
        for (int i = 0; i < cnt; i++) ids[i] = ld32(p + (uint32_t)i * 4u);
        p_alDeleteBuffers(cnt, ids);
        cret(c); return 1;
    }
    if (strcmp(n, "alBufferData") == 0) {
        unsigned buf = A(c, 0);
        int fmt = (int)A(c, 1);
        uint32_t data = A(c, 2);
        int size = (int)A(c, 3);
        int freq = (int)A(c, 4);
        p_alBufferData(buf, fmt, (const void *)(uintptr_t)data, size, freq);
        if (g_n_bufdata < 6UL)
            fprintf(stderr, "[openal] alBufferData#%lu buf=%u fmt=0x%x %d bytes %d Hz err=%d\n",
                    g_n_bufdata + 1, buf, fmt, size, freq, p_alGetError());
        g_n_bufdata++;
        if (size > 0) g_bytes_pcm += (unsigned long long)size;
        cret(c); return 1;
    }
    if (strcmp(n, "alGenSources") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 0);
        uint32_t out = A(c, 1);
        if (cnt <= 0 || cnt > 64 || !out) { cret(c); return 1; }
        p_alGenSources(cnt, ids);
        g_n_gen_src += (unsigned long)cnt;
        for (int i = 0; i < cnt; i++) st32(out + (uint32_t)i * 4u, ids[i]);
        cret(c); return 1;
    }
    if (strcmp(n, "alDeleteSources") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 0);
        uint32_t p = A(c, 1);
        if (cnt <= 0 || cnt > 64 || !p) { cret(c); return 1; }
        for (int i = 0; i < cnt; i++) ids[i] = ld32(p + (uint32_t)i * 4u);
        p_alDeleteSources(cnt, ids);
        cret(c); return 1;
    }
    if (strcmp(n, "alSourcePlay") == 0) {
        p_alSourcePlay(A(c, 0));
        if (g_n_play < 6UL)
            fprintf(stderr, "[openal] alSourcePlay#%lu src=%u err=%d\n",
                    g_n_play + 1, A(c, 0), p_alGetError());
        if (++g_n_play % 200UL == 0UL) oal_stats("200 playbacks");
        cret(c); return 1;
    }
    if (strcmp(n, "alSourceStop") == 0)    { p_alSourceStop(A(c,0)); cret(c); return 1; }
    if (strcmp(n, "alSourcePause") == 0)   { p_alSourcePause(A(c,0)); cret(c); return 1; }
    if (strcmp(n, "alSourceRewind") == 0)  {
        unsigned s = A(c,0);
        if (p_alSourceRewind) p_alSourceRewind(s);
        else p_alSourceStop(s);
        cret(c); return 1;
    }
    if (strcmp(n, "alSourcef") == 0) {
        p_alSourcef(A(c,0), (int)A(c,1), Af(c,2));
        cret(c); return 1;
    }
    if (strcmp(n, "alSource3f") == 0) {
        p_alSource3f(A(c,0), (int)A(c,1), Af(c,2), Af(c,3), Af(c,4));
        cret(c); return 1;
    }
    if (strcmp(n, "alSourcei") == 0) {
        p_alSourcei(A(c,0), (int)A(c,1), (int)A(c,2));
        cret(c); return 1;
    }
    if (strcmp(n, "alGetSourcei") == 0) {
        int v = 0;
        uint32_t out = A(c, 2);
        p_alGetSourcei(A(c,0), (int)A(c,1), &v);
        if (out) st32(out, (uint32_t)v);
        c->eax = 0;
        cret(c); return 1;
    }
    if (strcmp(n, "alGetSourcef") == 0) {
        float v = 0.0f;
        uint32_t out = A(c, 2);
        p_alGetSourcef(A(c,0), (int)A(c,1), &v);
        if (out) st32(out, fbits(v));
        c->eax = 0;
        cret(c); return 1;
    }
    if (strcmp(n, "alSourceQueueBuffers") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 1);
        uint32_t p = A(c, 2);
        if (cnt <= 0 || cnt > 64 || !p) { cret(c); return 1; }
        for (int i = 0; i < cnt; i++) ids[i] = ld32(p + (uint32_t)i * 4u);
        p_alSourceQueueBuffers(A(c,0), cnt, ids);
        cret(c); return 1;
    }
    if (strcmp(n, "alSourceUnqueueBuffers") == 0) {
        unsigned ids[64];
        int cnt = (int)A(c, 1);
        uint32_t p = A(c, 2);
        if (cnt <= 0 || cnt > 64 || !p) { cret(c); return 1; }
        p_alSourceUnqueueBuffers(A(c,0), cnt, ids);
        for (int i = 0; i < cnt; i++) st32(p + (uint32_t)i * 4u, ids[i]);
        cret(c); return 1;
    }
    if (strcmp(n, "alListenerf") == 0) {
        p_alListenerf((int)A(c,0), Af(c,1));
        cret(c); return 1;
    }
    if (strcmp(n, "alListener3f") == 0) {
        p_alListener3f((int)A(c,0), Af(c,1), Af(c,2), Af(c,3));
        cret(c); return 1;
    }
    if (strcmp(n, "alListenerfv") == 0) {
        p_alListenerfv((int)A(c,0), (const float *)(uintptr_t)A(c,1));
        cret(c); return 1;
    }
    if (strcmp(n, "alGetError") == 0) {
        c->eax = (uint32_t)p_alGetError();
        cret(c); return 1;
    }
    if (strcmp(n, "alDistanceModel") == 0) {
        if (p_alDistanceModel) p_alDistanceModel((int)A(c,0));
        cret(c); return 1;
    }
    if (strcmp(n, "alDopplerFactor") == 0) {
        if (p_alDopplerFactor) p_alDopplerFactor(Af(c,0));
        cret(c); return 1;
    }

    return 0;   /* unknown names go to the old stub */
}
