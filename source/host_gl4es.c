/* host_gl4es.c -- OpenGL 1.x backend (gl4es over EGL + GLES2)
 *
 * ==========================================================================
 * 1. Why this layer exists
 *
 * The original assumption was that The Binding of Isaac: Repentance used
 * OpenGL 2.1 **fixed-function** calls: glBegin/glEnd, glMatrixMode/
 * glLoadIdentity, display lists, GL_QUADS. GLES only has a programmable
 * pipeline, and the gap is not "different function names" but "the thing does
 * not exist".
 *
 * gl4es exists precisely to close that gap: it translates fixed-function calls
 * into GLES2 shader + VBO calls. This project does not reinvent that layer.
 *
 * ★★ CORRECTION (2026-11-11, measured): the premise above is WRONG for this
 * game. Instrumented GL entry logging over a full run shows the game requests
 * glShaderSource / glVertexAttribPointer / glUniformMatrix4fv and **never**
 * calls glBegin / glEnd / glMatrixMode / glVertexPointer / glTexEnv. Isaac uses
 * shaders, not the fixed-function pipeline; gl4es's FPE path does not
 * participate at all. The layer is still needed (it bridges GL 2.1 to GLES 2.0
 * and converts the GLSL), but do not reason about performance as if FPE were
 * involved -- that mistake cost several days. See BUILD_SPEC_FOR_AGENTS.md.
 *
 * ==========================================================================
 * 2. The two ends of the bridge
 *
 * The guest side must see 32-bit addresses (x86 pointers are uint32_t) while
 * gl4es is arm64 code with 64-bit function pointers. So a token sits in between:
 *
 *   guest: call <token 0x7D......>
 *            | guest_call -> host_gl_dynamic
 *   host:  look the token up in a table -> read args off the x86 stack ->
 *          call gl4es using the arm64 ABI
 *
 * The hard part is **argument types**: on x86 every argument is on the stack;
 * on arm64 the first 8 integers go in x0-x7 and the first 8 floats in v0-v7.
 * Stuff the bit pattern of glVertex3f(1.0f,2.0f,3.0f)'s floats into x0 as
 * integers and gl4es receives 0x3F800000 instead of 1.0f.
 *
 * So every GL function needs an "argument kind" signature: i = integer/pointer,
 * f = float. Dispatch picks the matching static wrapper (C cannot call an
 * arbitrary signature dynamically).
 *
 * ==========================================================================
 * 3. Initialisation order (get it wrong and hardext probing returns null
 *    function pointers)
 *
 *   1. dlopen libEGL.so / libGLESv2.so
 *   2. EGL: getDisplay -> initialize -> chooseConfig -> createPbufferSurface
 *      -> createContext -> makeCurrent
 *   3. set_getprocaddress (our GLES name resolver)
 *      set_getmainfbsize (returns the pbuffer size)
 *   4. initialize_gl4es()   <- only here are GLES functions really called, and
 *      the context must already be current
 *
 * Steps 1-2 were independently verified in work/tools/egltest2.c
 * (Adreno 830, GLES 3.2).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

static void png_scan_heap_once(void);   /* diagnostic probe, defined at the end of the file */

#include "guest.h"

/* per-frame draw counters (defined at the end of the file; forward declaration here) */
extern unsigned g_frame_draws, g_frame_clears, g_frame_texbinds;

/* Texture-id histogram for glBindTexture (0..62 counted individually, 63 is the "larger" bucket).
 * Under FRAME_TRACE, SwapBuffers calls host_gl_tex_hist_report() to print and clear it. */
#define TEX_HIST_N 64
extern unsigned g_tex_hist[TEX_HIST_N];
extern unsigned g_cur_tex_id;
unsigned g_cur_tex_id;   /* id of the most recent glBindTexture */
static unsigned g_cur_active_unit;   /* unit currently selected by glActiveTexture */

/* ★★ 2026-11-10: record the arrays of the most recent TexCoordPointer / VertexPointer.
 *
 * Why not print on every pointer setup: at that moment we do not know which texture is about
 * to be drawn, nor whether the UVs are sane. Only at **draw time**, looking at [UV, texId, size]
 * together, can we tell whether "the UVs land in the wrong region of the atlas".
 *
 * This was the most important piece of evidence in this round:
 *   - UVs all in 0..1 with a 1024x1024 texture -> UVs are fine, the problem is the texture content
 *   - UVs are pixel coordinates (0..1024) but the texture is sampled normalised -> off by a /size
 *   - UVs never change -> the game did not update the array, or we read a wrong pointer
 */
typedef struct {
    uint32_t ptr;
    int      size;      /* components per vertex */
    int      stride;    /* bytes */
    int      valid;     /* 1 = readable float array */
    float    v[8];      /* raw values of the first 2 vertices */
} ptrprobe_t;
static ptrprobe_t g_uv_probe, g_vtx_probe;

/* ★★ 2026-11-12: last glVertexAttribPointer setup for each attribute slot.
 *
 * HUD text/icons all render as the same mu glyph; we suspect the UV array points at the
 * wrong place at draw time (or a wrong stride makes every vertex read the same UV).
 * Dumping this table at draw time shows what the UVs really are. */
typedef struct {
    uint32_t ptr, type, stride;
    int      size, norm, seen;
} attrslot_t;
static attrslot_t g_attr[16];

/* ★★ 2026-11-12: draw attribution statistics.
 *
 * The main menu is entirely blank on frames 360-600, yet [fbset] shows plenty of textures still being drawn.
 * We must separate "drawn into the offscreen RT" from "drawn to the screen":
 * if nothing goes to the screen for a while, the blankness is "not composited", not "not rendered". */
unsigned long g_draws_fb0, g_draws_fb1;   /* screen / offscreen RT */

/* ★ 2026-11-10: pending-print state for glGetAttribLocation.
 * The return value is written to eax only after the call, so this takes two steps:
 * store the name in the branch, print name + return value at the end. */
static char     g_gal_name[48];
static int      g_gal_pending;

/* ★★ 2026-11-10: uniform probe.
 *
 * The log proves the game uses only three uniform entry points:
 *   glGetUniformLocation / glUniformMatrix4fv / glUniform1i
 * there is no glUniform1f. So Exposure/Gamma are **vertex attributes** (always 1.0, which is fine).
 *
 * That makes glUniform1i highly suspicious: it is almost always used to set **the sampler2D texture unit**.
 * If the location gl4es returns does not match the real GLES program,
 * or the sampler is bound to an empty texture unit, the shader samples
 * the wrong (or an uninitialised) texture -- which shows up as "flat colour blocks / scrambled colours". */
static char     g_uni_name[48];
static int      g_uni_pending;
static int      g_uni_last_loc = -9999;
/* ★★ 2026-11-10: program of the current glUseProgram.
 *
 * The previous uniform-loc probe **deduplicated by name**, so "Texture0" only recorded
 * the query result of the first program. But the first program is the flat-colour shader
 * (gl_FragColor = Color0, which has no Texture0 at all), so returning -1 is perfectly normal.
 * Result: whether Texture0 is available in the real textured program, and whether Texture1 (the palette)
 * was ever queried, were both hidden by that dedup -- the conclusion "only Texture0, Texture1
 * never queried" was an artefact of the probe bug and must not be used for elimination.
 *
 * Deduplicating by (program, name) is what exposes each program's real uniform table. */
static uint32_t g_cur_prog;

/* ★ 2026-11-10: pending-print state for shader/program parameter queries.
 * glGetShaderiv / glGetProgramiv write their result into *params,
 * which can only be read after the call, so the branch stores the arguments first. */
static uint32_t g_iv_pname;
static uint32_t g_iv_ptr;
static int      g_iv_isprog;
static int      g_iv_pending;
#define TEX_INFO_N 2048
unsigned g_tex_w[TEX_INFO_N], g_tex_h[TEX_INFO_N];
unsigned char g_tex_seen[TEX_INFO_N];

/* ★★ 2026-11-10: the **final** content state of every texture.
 *
 * The old texstat counted all-zero by **call**, but the same id may first be uploaded as an empty placeholder
 * and get real data later -- so the "20% all zero" contained many intermediate states
 * and it was impossible to tell how many textures on screen were actually broken.
 *
 * Here the last result is recorded per id: 0=unknown 1=has content 2=all zero.
 * Periodically summarising this in SwapBuffers gives numbers that can be compared against the picture. */
unsigned char g_tex_zero[TEX_INFO_N];

/* ★★ 2026-11-10: keep the sampled colour of each texture for comparison at draw time.
 *
 * Why it must be stored: on device the game **only uses glDrawElements + VBO** (the log confirms
 * glTexCoordPointer is called 0 times), so no UV pointer can be obtained from the VBO --
 * the UV probe is useless.
 *
 * So switch to a probe that answers the question directly: at draw time look at **the currently bound texture
 * and what it really looks like**. That pairs "the texture used for the player sprite" with "its actual colour"
 * and immediately separates:
 *   - content right (skin/cloth colours present) -> the problem is UVs or blending
 *   - content wrong (all red / all white / all zero) -> the problem is decoding or upload
 */
unsigned char g_tex_rgb[TEX_INFO_N][3];

/* ★★ 2026-11-10: channel-distribution classification of the texture data.
 *
 * Why this is needed:
 *   On device the whole picture is **washed out, grey and pale**, while the background is fine. Looking at 6 textures by hand
 *   showed that the HUD batch (id=259/263/287/311/317/345, all 32x32) has
 *   sample means of R=35 G=0 B=0 -- G/B are **exactly 0**.
 *   Even a predominantly red real image has non-zero G/B from antialiasing/shadows.
 *   An exact 0 means **the other bytes were never written**; most likely the format is declared wrong:
 *   the game prepares single-channel (LUMINANCE / ALPHA) data but uploads it as RGBA,
 *   or the other way round.
 *
 *   6 samples are not enough to conclude, so this classifies and counts **all known textures**:
 *     0 = unclassified   1 = colour (all three channels have a distribution)
 *     2 = R only   3 = A only (greyscale/glyph textures, which is normal)
 *     4 = pure greyscale (R≈G≈B, all non-zero)
 * Only this ratio can show whether "the data layer is really broken". */
unsigned char g_tex_class[TEX_INFO_N];

/* ★ 2026-11-12: caller and all-zero verdict for each texture upload (diagnostic) */
unsigned long  g_upl_caller_ret[TEX_INFO_N];
unsigned char  g_upl_caller_zero[TEX_INFO_N];
uint32_t       g_upl_caller_id[TEX_INFO_N];
extern unsigned long g_tex_hist_total;
void host_gl_tex_hist_report(void);

/* ★★ 2026-11-10 (plan B): **upload-time arguments** of each texture, for re-upload at draw time.
 *
 * Evidence chain (see HANDOFF_20261110.md):
 *   - line 511  id=3  512x512 ptr=5a31f010 -> all zero at upload time
 *   - line 1121 id=49 512x512 ptr=5a31f010 -> the same pointer now holds content 07 06 06 09
 *   - glTexSubImage2D calls = 0
 *   => the game called glTexImage2D while the pixel data was **not written yet**.
 *     The buffer was filled later, but the texture had already been uploaded and nobody re-sent it.
 *
 * Plan B: keep a copy of the arguments and check at draw time
 *   - g_tex_zero[id] == 2 (confirmed all zero)
 *   - and the buffer pointed to by ptr is **now non-zero**
 * If so, re-upload once with the original arguments. No gl4es changes and no game code changes:
 * this only replays the one upload that failed because of the "timing mismatch".
 *
 * All fields keep the original values from upload time (including target / level / border / fmt / type),
 * so the re-upload is an **equivalent replay** and introduces no new format assumptions. */
typedef struct {
    uint32_t ptr;        /* guest pixel buffer address */
    unsigned w, h;
    int32_t  level, ifmt, border;
    uint32_t fmt, type, target;
    unsigned char unit;  /* texture unit active at upload time (g_cur_active_unit) */
    unsigned char valid; /* 1 = arguments complete, usable for a re-upload */
} texsrc_t;
static texsrc_t g_tex_src[TEX_INFO_N];

/* Probe each texture at most once per frame (draw can run thousands of times a second,
 * and there are hundreds of zero textures -- without throttling, sampling alone eats milliseconds).
 * Stores frame+1; 0 means "not probed yet this frame". */
static unsigned g_tex_repair_frame[TEX_INFO_N];

/* Re-upload stats: number of successful replays / probes that found the data still not ready.
 * [texfinal] in host_gl_audio.c reads both of these. */
unsigned g_tex_repairs, g_tex_repair_wait;

/* ==========================================================================
 * ★★ 2026-11-10 (corrected after measuring on device): re-upload is **off by default**.
 *
 * Measured conclusion (this version has already been run; do not retry it):
 *   the re-upload did fire for 10 textures, but the picture went from "HUD missing" to **the whole room in scrambled colours**.
 *
 * The cause is the data source, not the timing:
 *   ptr=60950010  is shared by 9 ids (11/13/15/17/33/35/37 ...)
 *   ptr=86b2c010  is shared by 3 ids (1015/1036/1038)
 *   ptr=86fd9010  is shared by 2 ids (999/1001)
 * All of them are zero at glTexImage2D time, and afterwards this **shared scratch area** holds only
 * the data of the last writer. Replaying that content for every id on the same ptr
 * = copying the last image onto all the others -> cross-wired atlas pages.
 *
 * So the reading of that "decisive evidence" in HANDOFF must be voided:
 *   "the same pointer uploaded twice, zero then non-zero" does not prove "this image would be fine a moment later";
 *   it only proves "this shared scratch area was written to afterwards". Same size + same ptr
 *   is the fingerprint of pool reuse, not of timing.
 *
 * Switch: an empty <game>/repair file or TEX_REPAIR=1 -> back on,
 * so it can be A/B tested on the few textures with an exclusive ptr. Same convention as <game>/exec and
 * <game>/fast in this project: touch one file to switch it.
 * ========================================================================== */
static int g_tex_repair_on = -1;   /* -1 = not probed yet */

int host_tex_repair_enabled(void)
{
    if (g_tex_repair_on >= 0) return g_tex_repair_on;
    g_tex_repair_on = 0;
    const char *env = getenv("TEX_REPAIR");
    if (env && env[0] && env[0] != '0') {
        g_tex_repair_on = 1;
    } else {
        static const char *const paths[] = {
            "/storage/emulated/0/Android/media/com.isaacport.launcher"
            "/files/game/repair",
            "/data/user/0/com.isaacport.launcher/files/game/repair",
            "repair",
            NULL
        };
        for (int i = 0; paths[i]; i++)
            /* ★ 2026-11-10: access() cannot be used here. On Android /storage/emulated
             * goes through FUSE, and access(F_OK) often returns EACCES -- even when fopen opens fine.
             * That is exactly how <game>/frame_dump failed on device (the file exists, the SO has the code,
             * yet the switch never took effect). Use an fopen probe instead, the same path the game uses for its log. */
            {
                FILE *probe = fopen(paths[i], "rb");
                if (probe) { fclose(probe); g_tex_repair_on = 1; break; }
            }
    }
    fprintf(stderr, "[repair] tex re-upload switch = %s (%s)\n",
            g_tex_repair_on ? "on" : "off",
            g_tex_repair_on ? "found game/repair or TEX_REPAIR"
                            : "off by default: ptr is a shared scratch pool, replaying it would cross-wire atlases");
    fflush(NULL);
    return g_tex_repair_on;
}

/* gl4es initialisation entry points (exported by libGL.a) */
void set_getprocaddress(void *(*fp)(const char *));
void set_getmainfbsize(void (*fp)(int *, int *));
void initialize_gl4es(void);
void *gl4es_GetProcAddress(const char *name);

/* x86 argument access ---------------------------------------------------- */
static inline uint32_t gs32(CPU *c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static inline float gsf(CPU *c, int i)
{
    uint32_t v = gs32(c, i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline double gsd(CPU *c, int i)
{
    uint64_t lo = gs32(c, i), hi = gs32(c, i + 1);
    uint64_t raw = lo | (hi << 32);
    double d;
    memcpy(&d, &raw, 8);
    return d;
}
/* stdcall: pop the return address + clear n bytes of arguments */
static inline void gret(CPU *c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ======================================================================
 * EGL context
 * ====================================================================== */
typedef void *EGLDisplay_t;
typedef void *EGLConfig_t;
typedef void *EGLContext_t;
typedef void *EGLSurface_t;
typedef int32_t EGLint_t;
typedef uint32_t EGLBoolean_t;
typedef uint32_t EGLenum_t;

#define EGL_DEFAULT_DISPLAY        ((void *)0)
#define EGL_SURFACE_TYPE           0x3033
#define EGL_PBUFFER_BIT            0x0001
#define EGL_RENDERABLE_TYPE        0x3040
#define EGL_OPENGL_ES2_BIT         0x0004
#define EGL_RED_SIZE               0x3024
#define EGL_GREEN_SIZE             0x3023
#define EGL_BLUE_SIZE              0x3022
#define EGL_ALPHA_SIZE             0x3021
#define EGL_DEPTH_SIZE             0x3025
#define EGL_STENCIL_SIZE           0x3026
#define EGL_NONE                   0x3038
#define EGL_WIDTH                  0x3057
#define EGL_HEIGHT                 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
/* ★ 2026-11-06, new: constants needed for the in-process direct Surface path */
#define EGL_WINDOW_BIT             0x0004
#define EGL_RENDER_BUFFER          0x3086
#define EGL_BACK_BUFFER            0x3084

static void *g_egl_lib, *g_gles_lib;
static void *(*p_eglGetDisplay)(void *);
static EGLBoolean_t (*p_eglInitialize)(EGLDisplay_t, EGLint_t *, EGLint_t *);
static EGLBoolean_t (*p_eglChooseConfig)(EGLDisplay_t, const EGLint_t *,
                                         EGLConfig_t *, EGLint_t, EGLint_t *);
static EGLContext_t (*p_eglCreateContext)(EGLDisplay_t, EGLConfig_t,
                                          EGLContext_t, const EGLint_t *);
static EGLSurface_t (*p_eglCreatePbufferSurface)(EGLDisplay_t, EGLConfig_t,
                                                 const EGLint_t *);
static EGLSurface_t (*p_eglCreateWindowSurface)(EGLDisplay_t, EGLConfig_t,
                                                void *, const EGLint_t *);
static EGLBoolean_t (*p_eglSwapBuffers)(EGLDisplay_t, EGLSurface_t);
static EGLBoolean_t (*p_eglDestroySurface)(EGLDisplay_t, EGLSurface_t);
static EGLBoolean_t (*p_eglMakeCurrent)(EGLDisplay_t, EGLSurface_t,
                                        EGLSurface_t, EGLContext_t);
static void *(*p_eglGetProcAddress)(const char *);
static EGLint_t (*p_eglGetError)(void);

static EGLDisplay_t g_dpy;
static EGLSurface_t g_surf;
static EGLContext_t g_ctx;
static int          g_fb_w = 1280, g_fb_h = 720;
static int          g_gl_ready;      /* 0 not tried  1 ready  -1 failed */

/* ★ 2026-11-06: in-process window mode (built as a .so)
 *
 * The JNI layer gets an ANativeWindow from the SurfaceView Surface and, after dlopen but
 * before the game main, calls host_gl4es_set_window(). With a window we use
 * eglCreateWindowSurface + eglSwapBuffers to present directly: **zero copies, zero streaming**.
 *
 * Without a window (running directly in Termux, or when the Java side falls back to a child process)
 * we still use pbuffer + glReadPixels streaming, exactly as before.
 *
 * This is also what the iOS port does: the GL context is bound straight to a UIView CALayer. */
static void        *g_native_window;
static void        *p_ANativeWindow_release;
static void        *p_ANativeWindow_setBuffersGeometry;
static int          g_window_mode;   /* 1 = use a window surface, 0 = pbuffer */

void host_gl4es_set_window(void *win)
{
    g_native_window = win;
    g_window_mode = win ? 1 : 0;
    fprintf(stderr, "[gl4es] window mode = %s (ANativeWindow %p)\n",
            g_window_mode ? "on" : "off (pbuffer + stream)", win);
    fflush(NULL);
}

int host_gl4es_window_mode(void) { return g_window_mode; }
int host_gl4es_fb_w(void) { return g_fb_w; }
int host_gl4es_fb_h(void) { return g_fb_h; }

/* Lets the JNI layer tell the GL layer the real size before init.
 * Must be called before host_gl4es_init -- init runs only once (idempotent). */
void host_gl4es_set_fb_size(int w, int h)
{
    if (w > 0) g_fb_w = w;
    if (h > 0) g_fb_h = h;
}

static int gl_fail(const char *what)
{
    fprintf(stderr, "[gl4es] ✗ %s failed (EGL error 0x%04x)\n", what,
            p_eglGetError ? (unsigned)p_eglGetError() : 0u);
    g_gl_ready = -1;
    return -1;
}

/* Probe: is a page really mapped (mincore only inspects mappings, it does not fault) */
int probe_readable(unsigned long addr);
int probe_readable(unsigned long addr)
{
    unsigned char vec = 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    unsigned long page = addr & ~((unsigned long)pg - 1);
    return mincore((void *)page, (size_t)pg, &vec) == 0;
}

/* gl4es uses this to resolve GLES function names */
static int g_gles_proc_log = 40;

static void *gl4es_gles_proc(const char *name)
{
    if (!name || !g_gles_lib) return NULL;
    void *p = dlsym(g_gles_lib, name);
    if (!p && p_eglGetProcAddress) p = p_eglGetProcAddress(name);
    if (g_gles_proc_log > 0) {
        g_gles_proc_log--;
        fprintf(stderr, "[gl4es] gles_proc(\"%s\") -> %p%s\n",
                name, p, p ? "" : "   <- empty!");
    }
    return p;
}

static void gl4es_fbsize(int *w, int *h)
{
    if (w) *w = g_fb_w;
    if (h) *h = g_fb_h;
}

int host_gl4es_init(int width, int height)
{
    if (g_gl_ready) return g_gl_ready > 0 ? 0 : -1;   /* idempotent */
    /* ★ pass 0 = do not override (in window mode host_gl4es_set_fb_size already set the size) */
    if (width > 0)  g_fb_w = width;
    else if (g_fb_w <= 0) g_fb_w = 1280;
    if (height > 0) g_fb_h = height;
    else if (g_fb_h <= 0) g_fb_h = 720;

    g_egl_lib = dlopen("/system/lib64/libEGL.so", RTLD_NOW);
    if (!g_egl_lib) {
        fprintf(stderr, "[gl4es] ✗ cannot open libEGL.so: %s\n", dlerror());
        g_gl_ready = -1;
        return -1;
    }
    g_gles_lib = dlopen("/system/lib64/libGLESv2.so", RTLD_NOW);
    if (!g_gles_lib) {
        fprintf(stderr, "[gl4es] ✗ cannot open libGLESv2.so: %s\n", dlerror());
        g_gl_ready = -1;
        return -1;
    }

    p_eglGetDisplay          = dlsym(g_egl_lib, "eglGetDisplay");
    p_eglInitialize          = dlsym(g_egl_lib, "eglInitialize");
    p_eglChooseConfig        = dlsym(g_egl_lib, "eglChooseConfig");
    p_eglCreateContext       = dlsym(g_egl_lib, "eglCreateContext");
    p_eglCreatePbufferSurface= dlsym(g_egl_lib, "eglCreatePbufferSurface");
    p_eglCreateWindowSurface = dlsym(g_egl_lib, "eglCreateWindowSurface");
    p_eglSwapBuffers         = dlsym(g_egl_lib, "eglSwapBuffers");
    p_eglDestroySurface      = dlsym(g_egl_lib, "eglDestroySurface");
    p_eglMakeCurrent         = dlsym(g_egl_lib, "eglMakeCurrent");
    p_eglGetProcAddress      = dlsym(g_egl_lib, "eglGetProcAddress");
    p_eglGetError            = dlsym(g_egl_lib, "eglGetError");
    if (!p_eglGetDisplay || !p_eglInitialize || !p_eglChooseConfig ||
        !p_eglCreateContext || !p_eglCreatePbufferSurface || !p_eglMakeCurrent)
        return gl_fail("EGL symbol resolution");

    /* EGL_DEFAULT_DISPLAY is required: EGL_PLATFORM_SURFACELESS_MESA is unavailable on this device
     * (egltest2.c measured a 0 return). */
    g_dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g_dpy) return gl_fail("eglGetDisplay");

    EGLint_t maj = 0, min = 0;
    if (!p_eglInitialize(g_dpy, &maj, &min)) return gl_fail("eglInitialize");
    fprintf(stderr, "[gl4es] EGL %d.%d\n", maj, min);

    /* Window mode needs WINDOW_BIT, pbuffer mode needs PBUFFER_BIT. Requesting both
     * is legal too (Android configs usually support both), but requesting only the one
     * needed makes it easier to get a valid config on odd devices. */
    const EGLint_t surf_type =
        (g_window_mode && p_eglCreateWindowSurface) ? EGL_WINDOW_BIT
                                                    : EGL_PBUFFER_BIT;
    const EGLint_t cfg_attr[] = {
        EGL_SURFACE_TYPE,    surf_type,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig_t cfg = NULL;
    EGLint_t ncfg = 0;
    if (!p_eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1)
        return gl_fail("eglChooseConfig");

    if (g_window_mode && p_eglCreateWindowSurface && g_native_window) {
        const EGLint_t win_attr[] = { EGL_NONE };
        g_surf = p_eglCreateWindowSurface(g_dpy, cfg, g_native_window, win_attr);
        if (!g_surf) {
            fprintf(stderr, "[gl4es] ⚠ eglCreateWindowSurface failed; falling back to pbuffer\n");
            g_window_mode = 0;
        }
    }
    if (!g_window_mode) {
        const EGLint_t surf_attr[] = {
            EGL_WIDTH,  g_fb_w,
            EGL_HEIGHT, g_fb_h,
            EGL_NONE
        };
        g_surf = p_eglCreatePbufferSurface(g_dpy, cfg, surf_attr);
        if (!g_surf) return gl_fail("eglCreatePbufferSurface");
    }

    const EGLint_t ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = p_eglCreateContext(g_dpy, cfg, NULL, ctx_attr);
    if (!g_ctx) return gl_fail("eglCreateContext");

    if (!p_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx))
        return gl_fail("eglMakeCurrent");

    /* The GLES context is now in place. Only now may gl4es probe extensions. */
    set_getprocaddress(gl4es_gles_proc);
    set_getmainfbsize(gl4es_fbsize);
    initialize_gl4es();

    /* Verification: gl4es own glGetString must report 2.x */
    const unsigned char *(*p_glGetString)(uint32_t) =
        (const unsigned char *(*)(uint32_t))gl4es_GetProcAddress("glGetString");
    if (p_glGetString) {
        const char *ver = (const char *)p_glGetString(0x1F02);   /* GL_VERSION */
        const char *ren = (const char *)p_glGetString(0x1F01);   /* GL_RENDERER */
        fprintf(stderr, "[gl4es] ✓ GL_VERSION  = %s\n", ver ? ver : "(null)");
        fprintf(stderr, "[gl4es] ✓ GL_RENDERER = %s\n", ren ? ren : "(null)");
    } else {
        fprintf(stderr, "[gl4es] ✗ gl4es_GetProcAddress(\"glGetString\") returned NULL\n");
        g_gl_ready = -1;
        return -1;
    }

    fprintf(stderr, "[gl4es] ✓ backend ready %dx%d (%s)\n", g_fb_w, g_fb_h,
            g_window_mode ? "direct window surface" : "pbuffer + stream");
    g_gl_ready = 1;
    return 0;
}

int host_gl4es_ready(void) { return g_gl_ready; }

/* ★★ 2026-11-10: screen pixel probe.
 *
 * The user reports "it is red when switching rooms". This is the only way to **see directly**
 * what the GPU produced: read the centre pixel with GLES glReadPixels before the swap.
 *
 * Why read instead of stream:
 *   - our texture statistics read CPU memory, and FBO textures always read as zero from the CPU
 *   - "red" could be an uninitialised FBO, swapped channels, or a wrong clear colour
 *   - only a real screen read tells them apart
 *
 * Sample only over the first 60 frames after start-up (reading pixels forces a GPU sync, which costs time).
 * Also count the full-screen colour distribution: if R is always high and G/B always 0, the channels are wrong. */
static int g_cur_tex_id_pub;   /* for external queries only (unused placeholder) */

int host_gl4es_present_window(void)
{
    if (!g_window_mode || !p_eglSwapBuffers || !g_surf) return 0;

    {
        static int probed;
        static void (*p_glReadPixels)(int, int, int, int,
                                      unsigned, unsigned, void *);
        static int tried;
        if (!tried) {
            tried = 1;
            if (g_gles_lib)
                p_glReadPixels = (void (*)(int,int,int,int,unsigned,unsigned,void *))
                                 dlsym(g_gles_lib, "glReadPixels");
        }
        /* ★★ 2026-11-10: switched to **continuous** sampling.
         *
         * The old code sampled only the first 60 frames (`probed < 60`) = 1 second after start-up,
         * when the game is still loading and the picture is plain white -- so the log held
         * nothing but (255,255,255,255), which carries no information.
         *
         * Now: sample once every 120 frames (2 seconds) for the whole session.
         * glReadPixels forces a GPU sync, but once every 2 seconds is negligible. */
        if (p_glReadPixels) {
            static unsigned last_sample;
            extern unsigned host_gl_swap_count(void);
            unsigned fc = host_gl_swap_count();
            if (fc - last_sample < 120u) goto do_swap;
            last_sample = fc;
            /* Sample 5 points: centre + inset corners, so a single coincidental pixel cannot fool us. */
            unsigned char px[5][4];
            int sx[5], sy[5];
            int w = g_fb_w > 0 ? g_fb_w : 1;
            int h = g_fb_h > 0 ? g_fb_h : 1;
            sx[0] = w / 2; sy[0] = h / 2;
            sx[1] = w / 4; sy[1] = h / 4;
            sx[2] = w * 3 / 4; sy[2] = h / 4;
            sx[3] = w / 4; sy[3] = h * 3 / 4;
            sx[4] = w * 3 / 4; sy[4] = h * 3 / 4;
            for (int i = 0; i < 5; i++)
                p_glReadPixels(sx[i], sy[i], 1, 1, 0x1908 /*RGBA*/,
                               0x1401 /*UNSIGNED_BYTE*/, px[i]);

            /* Print only in the following cases, to avoid flooding the log:
             *   the first 5 frames (start-up screen)
             *   "reddish" detected (R clearly above G/B)
             *   a heartbeat every 15 frames */
            int reddish = 0;
            for (int i = 0; i < 5; i++)
                if (px[i][0] > 100 && px[i][1] < 40 && px[i][2] < 40) reddish = 1;

            if (reddish || (last_sample % 600u) == 0u) {
                fprintf(stderr, "[px] frame#%u center=(%d,%d,%d,%d) quarter=(%d,%d,%d) "
                        "three-quarter=(%d,%d,%d)%s\n",
                        fc,
                        px[0][0], px[0][1], px[0][2], px[0][3],
                        px[1][0], px[1][1], px[1][2],
                        px[4][0], px[4][1], px[4][2],
                        reddish ? "  ★ reddish" : "");
                fflush(NULL);
            }
        }
    }

do_swap:
    p_eglSwapBuffers(g_dpy, g_surf);
    return 1;
}

/* ======================================================================
 * 4. The guest token bridge
 *
 * The token space is 0x7D000000, separate from kernel32 dynamic exports at 0x7E000000.
 *
 * The signature table is generated by work/tools/gen_gl_bridge.py from the gl4es headers
 * (gl4es_bridge_gen.c). It is not hand-written: hand-writing always drops entries, and a dropped entry
 * makes the resolver return 0, the guest stores that 0 and calls 0 -- which reports as "indirect call to
 * address 0", very far from the real cause.
 * ====================================================================== */
#define GL_TOKEN_BASE 0x7D000000u
#define GL_MAX_ENTS   1024u

/* wglGetProcAddress must itself be callable from the guest, so use a sentinel at the end of the token space */
#define WGL_GETPROC_TOKEN 0x7DFFFF00u

/* ---- generated-table interface (gl4es_bridge_gen.c) ---- */
typedef void (*glbridge_fn)(CPU *, void *);
glbridge_fn gl_bridge_wrapper(int idx, int want_u32ret);
typedef struct { const char *name; short sig; char ret; } gl_bridge_decl;
const gl_bridge_decl *gl_bridge_lookup(const char *name);
int gl_bridge_count(void);

typedef struct {
    const char *name;
    void       *fn;
    int         sig;
    char        ret;      /* v=void  i=scalar  s=string  p=other pointer */
    uint32_t    token;
} glent;

static glent    g_gl_ents[GL_MAX_ENTS];
static unsigned g_gl_ent_n;
static char     g_gl_missing[128][48];
static unsigned g_gl_missing_n;

/* Strings returned by glGetString and friends live high in gl4es, unreachable by the guest uint32_t,
 * so they must be copied into the low 4GB before being returned. */
#define GL_STR_BASE 0x3A300000u
#define GL_STR_SIZE 0x10000u
static uint32_t g_gl_strbuf;

static uint32_t gl_strbuf_get(void)
{
    if (g_gl_strbuf) return g_gl_strbuf;
    void *p = mmap((void *)(uintptr_t)GL_STR_BASE, GL_STR_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)GL_STR_BASE) return 0;
    g_gl_strbuf = GL_STR_BASE;
    return g_gl_strbuf;
}

static uint32_t gl_publish_str(const char *s)
{
    if (!s) return 0;
    uint32_t base = gl_strbuf_get();
    if (!base) return 0;
    size_t n = strlen(s);
    if (n >= GL_STR_SIZE) n = GL_STR_SIZE - 1;
    memcpy((void *)(uintptr_t)base, s, n);
    *(char *)(uintptr_t)(base + (uint32_t)n) = 0;
    return base;
}

static void gl_note_missing(const char *name, const char *why)
{
    for (unsigned i = 0; i < g_gl_missing_n; i++)
        if (!strcmp(g_gl_missing[i], name)) return;
    if (g_gl_missing_n < 128) {
        snprintf(g_gl_missing[g_gl_missing_n], 48, "%s", name);
        g_gl_missing_n++;
    }
    fprintf(stderr, "[gl4es] ✗ rejected %s: %s\n", name, why);
}

/* Diagnostic: record every requested GL/WGL name and its result (deduplicated) */
static char     g_req_name[512][40];
static uint32_t g_req_ok[512];
static unsigned g_req_n;

static void gl_log_request(const char *name, uint32_t token)
{
    if (!name) return;
    for (unsigned i = 0; i < g_req_n; i++)
        if (!strcmp(g_req_name[i], name)) return;
    if (g_req_n < 512) {
        snprintf(g_req_name[g_req_n], 40, "%s", name);
        g_req_ok[g_req_n] = token;
        g_req_n++;
    }
    fprintf(stderr, "[glreq] %-40s -> %s%08x\n", name,
            token ? "token " : "0  ★ not provided", token);
}

uint32_t host_gl_proc_resolve(const char *name)
{
    if (!name || !name[0]) return 0;

    /* The game fetches the address of wglGetProcAddress itself via GetProcAddress */
    if (!strcmp(name, "wglGetProcAddress")) return WGL_GETPROC_TOKEN;

    {   /* log + run the original logic */
        uint32_t tok = 0;
        {
            for (unsigned i = 0; i < g_gl_ent_n; i++)
                if (!strcmp(g_gl_ents[i].name, name)) { tok = g_gl_ents[i].token; break; }
            if (!tok && g_gl_ready == 1) {
                const gl_bridge_decl *d = gl_bridge_lookup(name);
                void *fn = d ? gl4es_GetProcAddress(name) : NULL;
                if (d && fn) {
                    if (g_gl_ent_n < GL_MAX_ENTS) {
                        glent *e = &g_gl_ents[g_gl_ent_n];
                        e->name = d->name; e->fn = fn; e->sig = d->sig;
                        e->ret = d->ret;
                        e->token = GL_TOKEN_BASE + (uint32_t)g_gl_ent_n * 4u;
                        g_gl_ent_n++;
                        tok = e->token;
                    }
                } else if (!d) {
                    gl_note_missing(name, "this entry point is not in the gl4es headers");
                } else {
                    gl_note_missing(name, "gl4es does not export it");
                }
            }
        }
        gl_log_request(name, tok);
        return tok;
    }
}

static uint32_t host_gl_proc_resolve_orig(const char *name)
{
    if (!name || !name[0]) return 0;
    if (!strcmp(name, "wglGetProcAddress")) return WGL_GETPROC_TOKEN;

    for (unsigned i = 0; i < g_gl_ent_n; i++)
        if (!strcmp(g_gl_ents[i].name, name)) return g_gl_ents[i].token;

    if (g_gl_ready != 1) return 0;

    const gl_bridge_decl *d = gl_bridge_lookup(name);
    if (!d) { gl_note_missing(name, "this entry point is not in the gl4es headers"); return 0; }

    void *fn = gl4es_GetProcAddress(name);
    if (!fn) { gl_note_missing(name, "gl4es does not export it"); return 0; }

    if (g_gl_ent_n >= GL_MAX_ENTS) return 0;

    glent *e = &g_gl_ents[g_gl_ent_n];
    e->name  = d->name;          /* points at a static string in the generated table, never goes stale */
    e->fn    = fn;
    e->sig   = d->sig;
    e->ret   = d->ret;
    e->token = GL_TOKEN_BASE + (uint32_t)g_gl_ent_n * 4u;
    g_gl_ent_n++;

    fprintf(stderr, "[gl4es] + %s (signature[%d] ret=%c) -> %08x\n",
            e->name, e->sig, e->ret, e->token);
    return e->token;
}

/* ==========================================================================
 * ★★ 2026-11-10 (plan B): re-upload all-zero textures at draw time
 *
 * Why this must happen at draw time rather than as a retry during upload:
 *   at upload time the data really is zero, so an immediate retry is zero too; the buffer is
 *   only filled at some later moment. Draw is the moment "the image is about to be used" -- and the last
 *   safe point to re-upload (any later and it has already been drawn).
 *
 * Why not do it all in eglSwapBuffers: by then the binding state has changed,
 *   and gl4es glTexImage2D acts on the **currently bound** texture, so we would have to
 *   save and restore the binding. At draw time the bound texture is the one we want to fix: no extra state work.
 * ========================================================================== */
typedef void (*glteximage2d_fn)(uint32_t target, int32_t level,
                                int32_t internalformat, int32_t width,
                                int32_t height, int32_t border,
                                uint32_t format, uint32_t type,
                                const void *pixels);

/* 64-point uniform sampling; returns the number of points with non-zero bytes (0 = all zero).
 * Uses the same criterion as the statistics in the glTexImage2D branch: 4 bytes per pixel,
 * 64 sampled points -- the two conclusions must be directly comparable. */
static int tex_nz64(const unsigned char *q, uint32_t w, uint32_t h)
{
    uint64_t npix = (uint64_t)w * h;
    if (!q) return 0;
    if (npix == 0) return 0;
    int nz = 0;
    for (int k = 0; k < 64; k++) {
        uint64_t o = (uint64_t)k * npix / 64ull * 4ull;
        if (q[o] || q[o+1] || q[o+2] || q[o+3]) nz++;
    }
    return nz;
}

/* Channel classification (**identical** to the rule in the upload path; extracted so the two
 * criteria cannot diverge: the [texchan] statistics must cover both natively uploaded and re-uploaded textures).
 *   nz==0        -> 0 all zero
 *   R only       -> 2
 *   A only       -> 3 (the normal shape of shadows/outlines/fonts)
 *   R≈G≈B       -> 4 pure greyscale
 *   otherwise    -> 1 colour
 * The arguments are the **sums** over the 64 samples; the division by 64 happens inside. */
static unsigned char tex_classify(unsigned long sR, unsigned long sG,
                                  unsigned long sB, unsigned long sA, int nz)
{
    if (nz == 0) return 0;
    unsigned long mR = sR / 64, mG = sG / 64, mB = sB / 64, mA = sA / 64;
    if (mR >= 8 && mG < 3 && mB < 3) return 2;
    if (mR < 3 && mG < 3 && mB < 3 && mA >= 8) return 3;
    unsigned long mx = mR > mG ? (mR > mB ? mR : mB) : (mG > mB ? mG : mB);
    unsigned long mn = mR < mG ? (mR < mB ? mR : mB) : (mG < mB ? mG : mB);
    return (mx - mn <= 2) ? 4 : 1;
}

/* Re-sample one texture and refresh all derived state (zero/rgb/class).
 * Called after a re-upload -- otherwise [texfinal]/[texchan] keep reporting
 * the old "all zero" verdict and the log no longer matches the picture at all. */
static void tex_update_stats(unsigned id, const unsigned char *q,
                             uint32_t w, uint32_t h)
{
    if (id >= TEX_INFO_N) return;
    uint64_t npix = (uint64_t)w * h;
    unsigned long sR = 0, sG = 0, sB = 0, sA = 0;
    int nz = 0;
    if (q && npix) {
        for (int k = 0; k < 64; k++) {
            uint64_t o = (uint64_t)k * npix / 64ull * 4ull;
            sR += q[o]; sG += q[o+1]; sB += q[o+2]; sA += q[o+3];
            if (q[o] || q[o+1] || q[o+2] || q[o+3]) nz++;
        }
    }
    g_tex_zero[id] = nz ? 1 : 2;
    g_tex_rgb[id][0] = (unsigned char)(sR / 64);
    g_tex_rgb[id][1] = (unsigned char)(sG / 64);
    g_tex_rgb[id][2] = (unsigned char)(sB / 64);
    g_tex_class[id] = tex_classify(sR, sG, sB, sA, nz);
}

/* Fetch gl4es glTexImage2D. Prefer the entry the game already requested (that fn
 * has been proven to work), otherwise ask gl4es for one. */
static void *tex_proc_teximage2d(void)
{
    static void *cached;
    if (cached) return cached;
    for (unsigned i = 0; i < g_gl_ent_n; i++)
        if (!strcmp(g_gl_ents[i].name, "glTexImage2D")) {
            cached = g_gl_ents[i].fn;
            return cached;
        }
    cached = gl4es_GetProcAddress("glTexImage2D");
    return cached;
}

/* If the currently bound texture is confirmed all zero and its pixel buffer now has content,
 * re-upload it once with the original upload arguments. Returns 1 if it really re-uploaded.
 *
 * Key safety constraints:
 *   - only repair g_tex_zero==2 (NULL pointer placeholders / FBO attachments are left alone)
 *   - only repair when "the buffer is now non-zero" (otherwise the upload is wasted)
 *   - only repair when it matches the current active unit (otherwise unit1 arguments would upload unit0)
 *   - probe each texture only once per frame
 *   - the re-upload arguments are copied verbatim from the original call, zero format/type guessing
 *
 * Off by default; the reason is the measured conclusion above g_tex_repair_on.
 */
static int tex_repair_if_blank(unsigned id, unsigned frame)
{
    if (!host_tex_repair_enabled()) return 0;
    if (id == 0u || id >= TEX_INFO_N) return 0;
    if (g_tex_zero[id] != 2) return 0;              /* only repair "confirmed all zero" */
    texsrc_t *s = &g_tex_src[id];
    if (!s->valid) return 0;                        /* no usable arguments recorded */
    if (g_tex_repair_frame[id] == frame + 1u) return 0;   /* already probed this frame */
    g_tex_repair_frame[id] = frame + 1u;
    if (s->unit != (unsigned char)g_cur_active_unit) return 0;
    if (s->target != 0x0DE1u) return 0;              /* only repair GL_TEXTURE_2D */
    uint32_t p = s->ptr;
    if (p < 0x10000u || s->w == 0 || s->h == 0) return 0;

    const unsigned char *q = (const unsigned char *)(uintptr_t)p;
    if (tex_nz64(q, s->w, s->h) == 0) {             /* data still not ready */
        g_tex_repair_wait++;
        return 0;
    }

    glteximage2d_fn up = (glteximage2d_fn)tex_proc_teximage2d();
    if (!up) return 0;

    up(s->target, s->level, s->ifmt, (int32_t)s->w, (int32_t)s->h,
       s->border, s->fmt, s->type, (const void *)(uintptr_t)p);

    tex_update_stats(id, q, s->w, s->h);
    unsigned n = ++g_tex_repairs;
    if (n <= 20u || (n % 50u) == 0u) {
        fprintf(stderr,
            "[repair] id=%u %ux%u fmt=0x%x type=0x%x ptr=%08x "
            "first4=%02x%02x%02x%02x re-uploads=%u\n",
            id, s->w, s->h, s->fmt, s->type, p,
            q[0], q[1], q[2], q[3], n);
        fflush(NULL);
    }
    return 1;
}

/* ==========================================================================
 * ★★ 2026-11-10: **read-only** depth-state probe
 *
 * Why this exists: on-device logs show `glEnable(DEPTH_TEST)` enabled,
 * `glDepthFunc = GL_GREATER(0x204)`, and 3D vertices with `z = -1.000`.
 * Isaac is a 2D game that orders layers by z, and "monsters/HUD/bullets missing entirely" is exactly
 * what it looks like when depth rejects every later sprite.
 *
 * ★★ Iron rule: this probe **only reads guest stack arguments** (gs32); it never calls a GL function,
 * never calls gl4es_GetProcAddress and never changes any GL state.
 * The previous round corrupted gl4es state during loading precisely because the probe called
 * glGetProgramiv/glGetActiveUniform, showing up as std::terminate after glDeleteProgram.
 *
 * Output policy: one line only on a **state change**; a device run produces only a few dozen lines.
 * ========================================================================== */
static struct {
    int      depth_test;   /* -1 unknown / 0 off / 1 on */
    int      depth_func;   /* argument of the most recent glDepthFunc */
    int      depth_mask;   /* argument of the most recent glDepthMask */
    unsigned clear_mask;   /* mask of the most recent glClear */
    double   clear_depth;  /* argument of the most recent glClearDepth */
    int      have_clear_depth;
    unsigned changed;      /* change counter */
    unsigned draws_at_state;  /* draws seen while in the current state */
} g_depthst = { -1, -1, -1, 0, 0.0, 0, 0 };

/* ★★ 2026-11-10: framebuffer tracking (**strictly read-only**, calls no GL function).
 *
 * Purpose: the main menu only alternates between texId=1(8x8) and texId=987(1024x1024).
 * We need to determine what 987 really is:
 *   A. an offscreen render target -- render into it first, then blit it to the screen
 *   B. a real background texture
 * This decides whether "987 is all zero" is normal (RT content is filled by the GPU, so a CPU read is always zero).
 *
 * Approach: record only the glBindFramebuffer arguments and which FB each draw lands on.
 * Not a single GL call, zero side effects. */
static struct {
    uint32_t cur_fb;        /* currently bound framebuffer (0 = default/screen) */
    uint32_t fb_draws;      /* draws landing on the current FB */
    uint32_t last_tex;      /* texture used by the most recent draw */
    uint32_t switches;      /* switch count */
    uint32_t nseen;         /* [fbset] combinations recorded so far (stop scanning at 96) */
    uint32_t vp_x, vp_y, vp_w, vp_h;   /* most recent glViewport */
    uint32_t vp_seen;
} g_fbst = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

/* ★★ 2026-11-11: FBO attachment map (**strictly read-only**).
 *
 * The main menu blits tex=987(1024x1024) to the screen and it keeps being reported as "all zero".
 * We need to confirm whether 987 is:
 *   A. an FBO colour attachment (offscreen render target) -- always zero from the CPU, which is normal
 *   B. a real background texture -- all zero means a broken image
 *
 * Reading only the glFramebufferTexture2D arguments settles it:
 *   glFramebufferTexture2D(target, attachment, textarget, texture, level)
 *                                                        ^^^^^^^ gs32(c,3)
 *
 * Calls no GL function and changes no state. */
static struct {
    uint32_t tex[32];       /* texture ids attached as FBO attachments */
    uint32_t att[32];
    unsigned n;
} g_fbo_att;

static void depthst_report(const char *why)
{
    /* With DEPTH_TRACE=1 print everything; otherwise report only the first 40 changes. */
    static unsigned shown;
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("DEPTH_TRACE") ? 1 : 0;
    if (!verbose && shown >= 40u) return;
    shown++;
    fprintf(stderr,
        "[depthst] %-22s test=%s func=0x%x%s mask=%d clear=0x%x%s  draw(this state)=%u\n",
        why,
        g_depthst.depth_test == 1 ? "ON" : (g_depthst.depth_test == 0 ? "off" : "?"),
        (unsigned)g_depthst.depth_func,
        g_depthst.depth_func == 0x0204 ? "(GREATER)" :
        g_depthst.depth_func == 0x0203 ? "(LESS)" :
        g_depthst.depth_func == 0x0201 ? "(NEVER!)" :
        g_depthst.depth_func == 0x0207 ? "(ALWAYS)" : "",
        g_depthst.depth_mask,
        g_depthst.clear_mask,
        (g_depthst.clear_mask & 0x0100u) ? "(with DEPTH)" : "(no DEPTH!)",
        g_depthst.draws_at_state);
    if (g_depthst.have_clear_depth)
        fprintf(stderr, "[depthst]   |- glClearDepth(%.6f)  <- depth clear value\n",
                g_depthst.clear_depth);
    fflush(NULL);
}

/* ==========================================================================
 * ★★ 2026-11-11: ring buffer of recent GL calls (**pure recording**, for crash forensics)
 *
 * Why it is needed: on device it crashes during start-up loading (around frame 85),
 *   [glreq] glDeleteProgram -> token ...
 *   [host] std::terminate
 * But that `[glreq]` line is a GetProcAddress log entry and it is **not fflush'ed**,
 * so the buffer order before abort() is unreliable -- we cannot conclude the crash is in glDeleteProgram.
 *
 * This does one thing only: append the name of every GL entry actually dispatched (a static string pointer,
 * no copies, no allocation) to the ring buffer. If it really crashes, the terminate stub prints the last 64
 * entries, showing what the game was calling right before the crash.
 *
 * ★ Zero side effects: one pointer store plus one increment. No GL calls, no state changes.
 * ========================================================================== */
#define GL_RING_N 64
const char *g_gl_ring[GL_RING_N];
unsigned    g_gl_ring_n;
void host_gl_dump_ring(const char *why);

static inline void gl_ring_note(const char *name)
{
    g_gl_ring[g_gl_ring_n & (GL_RING_N - 1u)] = name;
    g_gl_ring_n++;
    /* Self-test hook: with GL_RING_AT=N dump once at the Nth GL call.
     * Used **locally** to verify the dump logic itself (no need to wait for a real crash).
     * Reads one cached long plus one comparison; the hot-path cost is negligible. */
    {
        static long at = -2;
        if (at == -2) {
            const char *e = getenv("GL_RING_AT");
            at = e ? atol(e) : -1;
        }
        if (at > 0 && g_gl_ring_n == (unsigned long)at)
            host_gl_dump_ring("GL_RING_AT self-test");
        /* Self-test: verify the terminate forensics path (including full-stack scan / UAF replay) does not crash. */
        {
            static long sel = -2;
            if (sel == -2) {
                const char *e = getenv("TERMINATE_SELFTEST");
                sel = e ? atol(e) : -1;
            }
            if (sel > 0 && g_gl_ring_n == (unsigned long)sel) {
                extern void host_terminate_selftest(void);
                host_terminate_selftest();
            }
        }
    }
}

/* Called by the terminate stub in host_crt.c: print the last N GL calls to stderr. */
void host_gl_dump_ring(const char *why)
{
    fprintf(stderr, "[glring] %s: last %u GL calls (oldest first)\n",
            why ? why : "", g_gl_ring_n < GL_RING_N ? g_gl_ring_n : GL_RING_N);
    unsigned start = (g_gl_ring_n > GL_RING_N) ? (g_gl_ring_n - GL_RING_N) : 0u;
    for (unsigned i = start; i < g_gl_ring_n; i++) {
        const char *nm = g_gl_ring[i & (GL_RING_N - 1u)];
        fprintf(stderr, "[glring]   #%u %s\n", i, nm ? nm : "(null)");
    }
    fflush(NULL);
}

/* Dynamic dispatch entry point for guest_call. 1 = handled. */
int host_gl_dynamic(CPU *__restrict c, uint32_t addr)
{
    /* wglGetProcAddress special case: it is itself an entry the guest can call */
    if (addr == WGL_GETPROC_TOKEN) {
        uint32_t np = gs32(c, 0);
        char name[64];
        name[0] = 0;
        if (np) {
            for (int i = 0; i < 63; i++) {
                char ch = *(const char *)(uintptr_t)(np + (uint32_t)i);
                if (!ch) break;
                name[i] = ch;
                name[i + 1] = 0;
            }
        }
        c->eax = name[0] ? host_gl_proc_resolve(name) : 0;
        gret(c, 4);
        return 1;
    }

    if (addr < GL_TOKEN_BASE || addr >= GL_TOKEN_BASE + GL_MAX_ENTS * 4u) return 0;
    uint32_t off = addr - GL_TOKEN_BASE;
    if (off % 4u) return 0;
    uint32_t idx = off / 4u;
    if (idx >= g_gl_ent_n) return 0;

    glent *e = &g_gl_ents[idx];

    /* ★ pure recording: see the g_gl_ring comment. Costs one pointer store. */
    gl_ring_note(e->name);

    /* Per-frame draw-call statistics: read and reset in SwapBuffers.
     * Used to decide "is the game drawing anything, or only clearing the screen". */
    if (e->name[0] == 'g' && e->name[1] == 'l') {
        if (!strncmp(e->name, "glDraw", 6)) {
            g_frame_draws++;
            /* ★ read-only counter: how many draws happened in the current depth state. The counter itself has no side effects. */
            g_depthst.draws_at_state++;
            /* ★★ draws with UI/font-sized textures: dump the attribute table and UVs at that moment.
             * The whole HUD renders as mu; either the texture or the UVs are wrong, and this shows both at once. */
            {
                unsigned tid = g_cur_tex_id;
                unsigned tw = tid < TEX_INFO_N ? g_tex_w[tid] : 0u;
                unsigned th = tid < TEX_INFO_N ? g_tex_h[tid] : 0u;
                static unsigned ui_shown;
                /* ★ 2026-11-12: exclude 8x8/1x1 placeholder white textures (they would eat the quota),
                 * and sample only after frame 200, so what we catch really is main-menu/HUD drawing. */
                extern unsigned host_gl_swap_count(void);
                if (tw >= 32u && th >= 32u && tw <= 512u && th <= 512u &&
                    (uint64_t)tw * th <= 131072ull &&
                    host_gl_swap_count() > 200u && ui_shown < 30u) {
                    ui_shown++;
                    fprintf(stderr,
                        "[uidraw] #%u tex=%u(%ux%u) fb=%u\n",
                        ui_shown, tid, tw, th, g_fbst.cur_fb);
                    for (int ai = 0; ai < 8; ai++) {
                        if (!g_attr[ai].seen) continue;
                        fprintf(stderr, "[uidraw]   idx=%d size=%d type=0x%x stride=%u ptr=%08x",
                                ai, g_attr[ai].size, g_attr[ai].type,
                                g_attr[ai].stride, g_attr[ai].ptr);
                        /* for size==2 floats, read the UVs of the first 4 vertices using the stride */
                        if (g_attr[ai].size == 2 && g_attr[ai].type == 0x1406u) {
                            const char *base =
                                (const char *)(uintptr_t)g_attr[ai].ptr;
                            int st = (int)g_attr[ai].stride;
                            if (st <= 0) st = 8;
                            /* ★★ Validation: the stride must be a sane small value, and
                             * the span of ptr + 4 vertices must fall inside a valid guest address.
                             * Without these two checks a garbage stride reads unmapped pages --
                             * the first time this probe was added locally it SEGV'd at 0xbe3bf. */
                            int ok = (g_attr[ai].ptr > 0x10000u) &&
                                     (st > 0 && st <= 256) &&
                                     ((uint64_t)g_attr[ai].ptr +
                                      4ull * (uint64_t)st + 8ull < 0xC0000000ull);
                            if (ok) {
                                fprintf(stderr, "  UV=");
                                for (int v = 0; v < 4; v++) {
                                    const float *fp =
                                        (const float *)(const void *)(base + (size_t)v * (size_t)st);
                                    fprintf(stderr, " (%.4f,%.4f)", (double)fp[0], (double)fp[1]);
                                }
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                    fflush(NULL);
                }
            }
            g_fbst.fb_draws++;
            if (g_fbst.cur_fb == 0u) g_draws_fb0++; else g_draws_fb1++;
            g_fbst.last_tex = g_cur_tex_id;
            /* Annotate which framebuffer the first 60 draws land on -- answering
             * "is the main menu drawing into 987, or blitting 987 to the screen".
             * Starts only after frame 200 (the main menu comes earlier). */
            /* ★ Count deduplicated by (framebuffer, texture) combination:
             * only look at "which textures were used to draw into the RT" and "which were used to blit to the screen",
             * which directly answers "was the main menu spritesheet ever drawn". */
            if (g_fbst.nseen < 96u) {   /* ★ skip entirely when full, no pointless scanning in the hot path */
                static struct { uint32_t fb, tex; unsigned long n; } seen[96];
                unsigned k;
                for (k = 0; k < g_fbst.nseen; k++)
                    if (seen[k].fb == g_fbst.cur_fb &&
                        seen[k].tex == g_cur_tex_id) { seen[k].n++; break; }
                if (k == g_fbst.nseen) {
                    seen[g_fbst.nseen].fb = g_fbst.cur_fb;
                    seen[g_fbst.nseen].tex = g_cur_tex_id;
                    seen[g_fbst.nseen].n = 1;
                    g_fbst.nseen++;
                    fprintf(stderr, "[fbset] new combination fb=%u%s tex=%u\n",
                            g_fbst.cur_fb,
                            g_fbst.cur_fb ? "(RT)" : "(screen)",
                            g_cur_tex_id);
                    fflush(NULL);
                }
            }
            /* ★★ 2026-11-10 (plan B): before drawing, re-upload textures whose data "was not ready at the time".
             *
             * Placed here rather than retrying right after glTexImage2D: at upload time
             * the buffer really is zero and a retry then is still zero; only at this draw moment
             * may it already be filled, and draw is the last moment before it is needed.
             *
             * Right now the bound texture is the one this draw will sample, so the re-upload
             * needs no rebinding / unit switch -- zero state disturbance.
             * Each texture is probed once per frame; if the probe fails (still all zero), wait for the next frame.
             */
            {
                extern unsigned host_gl_swap_count(void);
                tex_repair_if_blank(g_cur_tex_id, host_gl_swap_count());
            }
            /* Print the arguments of the first 6 draws: count/type immediately separate
             * "one full-screen quad" from "tens of thousands of triangles". */
            /* Print the **currently bound texture id** at draw time --
             * the most direct step in judging "is it drawing a blank texture". First 40 all, then one every 200. */
            static unsigned shown;
            shown++;
            /* ★★ 2026-11-10: sample only **after the game really starts rendering**.
             *
             * The old condition `shown <= 40` all fell into the start-up phase (the loading screen),
             * where those 40 draws were all white textures + an FBO filling the screen,
             * which is worthless for diagnosing "why are the in-game HUD/sprites missing".
             *
             * Now: sample only after frame 500 (roughly once start-up is done and a room is entered).
             * The first 60 record real post-start-up drawing, then one heartbeat every 2000. */
            extern unsigned host_gl_swap_count(void);
            static unsigned after;
            if (host_gl_swap_count() > 500u) {
                after++;
                if (after <= 60u || (after % 2000u) == 0u) {
                    fprintf(stderr,
                        "[draw] %s  a0=%u a1=%u a2=%u a3=%u  texId=%u(%ux%u)%s  total#%u\n",
                        e->name, gs32(c,0), gs32(c,1), gs32(c,2), gs32(c,3),
                        g_cur_tex_id,
                        g_cur_tex_id < TEX_INFO_N ? g_tex_w[g_cur_tex_id] : 0,
                        g_cur_tex_id < TEX_INFO_N ? g_tex_h[g_cur_tex_id] : 0,
                        (g_cur_tex_id < TEX_INFO_N && g_tex_seen[g_cur_tex_id]) ? "" : "(not uploaded)",
                        g_frame_draws);            
                    fflush(NULL);
                }
            }
            /* ★★ 2026-11-10: texture-content snapshot at draw time.
             *
             * The original idea was to probe UVs, but on-device logs prove the game **only uses glDrawElements + VBO**
             * (glTexCoordPointer called 0 times), so no UV pointer can be obtained.
             *
             * So probe this instead: when this texture is drawn, **what does it actually look like**.
             * This line puts [draw number, texture id, size, sampled RGB, content state]
             * all together and directly answers "is the texture used for the player sprite correct".
             */
            {
                static unsigned snap;
                snap++;
                if ((snap <= 60u && host_gl_swap_count() > 500u)
                        || (snap % 3000u) == 0u) {
                    unsigned id = g_cur_tex_id;
                    unsigned tw = id < TEX_INFO_N ? g_tex_w[id] : 0u;
                    unsigned th = id < TEX_INFO_N ? g_tex_h[id] : 0u;
                    const char *st = "unknown";
                    unsigned r=0,g=0,b=0;
                    if (id < TEX_INFO_N) {
                        st = g_tex_zero[id] == 2 ? "all zero"
                           : (g_tex_zero[id] == 1 ? "has data" : "placeholder/FBO");
                        r = g_tex_rgb[id][0];
                        g = g_tex_rgb[id][1];
                        b = g_tex_rgb[id][2];
                    }
                    fprintf(stderr,
                        "[snap] #%u %s tex=%u(%ux%u) %s RGB≈(%u,%u,%u)\n",
                        snap, e->name, id, tw, th, st, r, g, b);
                    fflush(NULL);
                }
            }
        }
        else if (!strcmp(e->name, "glClear")) {
            g_frame_clears++;
            /* ★ read-only: clear mask. Without GL_DEPTH_BUFFER_BIT(0x100)
             * depth survives across frames and last frame's depth rejects this frame's sprites. */
            unsigned mask = (unsigned)gs32(c, 0);
            if (mask != g_depthst.clear_mask) {
                g_depthst.clear_mask = mask;
                g_depthst.changed++;
                depthst_report("glClear mask changed");
            }
        }
        /* ================================================================
         * ★★ 2026-11-10: vertex/texture-coordinate probe.
         *
         * On-device symptom (confirmed by screen recording): the room background is fine, but **every sprite is wrong**.
         *   - HUD hearts -> solid red rectangles
         *   - stat icons -> solid white rectangles
         *   - monsters/bullets -> missing entirely
         *   - room switch -> red blocks
         * All of these are small regions of an atlas, selected by UV.
         * Sampling a "flat colour block" is exactly **the UVs landing in a blank/wrong region of the atlas**.
         *
         * Here we read the array pointer from guest memory directly and check whether the UVs are sane:
         *   - normal: 0..1 (normalised) or 0..texture width/height (pixel coordinates)
         *   - abnormal: huge values, NaN, or a constant 0
         *
         * glTexCoordPointer(size, type, stride, ptr) -- 4 arguments
         * glVertexPointer  (size, type, stride, ptr) -- same
         * ================================================================ */
        else if (!strcmp(e->name, "glTexCoordPointer") ||
                 !strcmp(e->name, "glVertexPointer")) {
            /* ★ 2026-11-10: only **record**, do not print here.
             * The reason is in the ptrprobe_t comment: we do not know yet which texture will be drawn. */
            ptrprobe_t *pp = (e->name[2] == 'T') ? &g_uv_probe : &g_vtx_probe;
            uint32_t size   = gs32(c, 0);
            uint32_t type   = gs32(c, 1);
            uint32_t stride = gs32(c, 2);
            uint32_t ptr    = gs32(c, 3);
            pp->ptr = ptr;
            pp->size = (int)size;
            pp->stride = (int)stride;
            pp->valid = 0;
            /* type: 0x1406=GL_FLOAT, 0x1403=GL_SHORT, 0x1400=GL_BYTE */
            if (type == 0x1406u && ptr > 0x10000u && size >= 1u && size <= 4u) {
                const float *f = (const float *)(uintptr_t)ptr;
                int comp = (int)size;
                pp->valid = 1;
                for (int v = 0; v < 2; v++)
                    for (int k = 0; k < comp; k++)
                        pp->v[v * 4 + k] = f[v * comp + k];
            }
        }
        /* ================================================================
         * ★★ 2026-11-10: shader vertex-attribute probe.
         *
         * On-device logs prove the game uses **custom GLSL shaders**
         *   glCreateProgram / glGetAttribLocation / glVertexAttribPointer
         * and **does not use glTexCoordPointer** (called 0 times).
         *
         * So the UVs come from a shader attribute array. If some attribute's
         * index/size/stride/ptr is wrong, the shader samples with wrong UVs,
         * the result being **the whole quad sampling one point of the texture** -> a flat-colour rectangle.
         * This matches the screen recording exactly (hearts/icons turning into flat colour blocks).
         *
         * glVertexAttribPointer(index, size, type, normalized, stride, ptr)
         *   -- 6 arguments (cdecl)
         * ================================================================ */
        else if (!strncmp(e->name, "glVertexAttribPointer", 21)) {
            /* ★ persistent record (not affected by the print limit) */
            {
                uint32_t ai = (uint32_t)gs32(c, 0);
                if (ai < 16u) {
                    g_attr[ai].ptr    = (uint32_t)gs32(c, 5);
                    g_attr[ai].size   = (int)gs32(c, 1);
                    g_attr[ai].type   = (uint32_t)gs32(c, 2);
                    g_attr[ai].norm   = (int)gs32(c, 3);
                    g_attr[ai].stride = (uint32_t)gs32(c, 4);
                    g_attr[ai].seen   = 1;
                }
            }
            static unsigned vai;
            vai++;
            if (vai <= 30u) {
                uint32_t idx  = gs32(c, 0);
                uint32_t size = gs32(c, 1);
                uint32_t type = gs32(c, 2);
                uint32_t norm = gs32(c, 3);
                uint32_t strd = gs32(c, 4);
                uint32_t ptr  = gs32(c, 5);
                /* ★ 2026-11-10: read vertices by **stride**, not contiguously by size.
                 * Earlier this ignored stride (this shader uses 28 bytes = 7 floats),
                 * so the "second vertex" actually read a neighbouring attribute's value
                 * and printed bogus data (e.g. Gamma=426.667 was really Position.x).
                 * stride=0 in GL means "tightly packed", and only then does it equal size*sizeof(type). */
                char buf[200];
                buf[0] = 0;
                if (type == 0x1406u && ptr > 0x10000u && size >= 1u && size <= 4u) {
                    const char *base = (const char *)(uintptr_t)ptr;
                    int st = (int)strd;
                    if (st <= 0) st = (int)size * 4;
                    const float *v0 = (const float *)(const void *)base;
                    const float *v1 = (const float *)(const void *)(base + st);
                    snprintf(buf, sizeof buf,
                             " v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f)",
                             (double)v0[0],
                             (double)(size > 1 ? v0[1] : 0.0f),
                             (double)(size > 2 ? v0[2] : 0.0f),
                             (double)v1[0],
                             (double)(size > 1 ? v1[1] : 0.0f),
                             (double)(size > 2 ? v1[2] : 0.0f));
                }
                fprintf(stderr,
                    "[attrib] #%u idx=%u size=%u type=0x%x norm=%u stride=%u ptr=%08x%s\n",
                    vai, idx, size, type, norm, strd, ptr, buf);
                fflush(NULL);
            }
        }
        else if (!strncmp(e->name, "glGetAttribLocation", 19)) {
            /* ★ 2026-11-10: the return value is in c->eax, but **the function has not been called yet**
             * (the real call is at the end of this function, w(c, e->fn)).
             * So only store the attribute name here and print it after the call. */
            uint32_t pname = gs32(c, 1);
            g_gal_name[0] = 0;
            if (pname > 0x10000u) {
                for (int i = 0; i < 31; i++) {
                    uint32_t a = pname + (uint32_t)i;
                    char ch = (char)(ld32(a & ~3u) >> ((a & 3u) * 8u));
                    if (!ch) break;
                    g_gal_name[i] = ch;
                    g_gal_name[i + 1] = 0;
                }
            }
            g_gal_pending = 1;
        }
        else if (!strcmp(e->name, "glUseProgram")) {
            g_cur_prog = gs32(c, 0);
        }
        else if (!strncmp(e->name, "glGetUniformLocation", 20)) {
            /* Same as glGetAttribLocation: the return value is readable only after the call,
             * so only the name is stored here. */
            uint32_t pname = gs32(c, 1);
            g_uni_name[0] = 0;
            if (pname > 0x10000u) {
                for (int i = 0; i < 47; i++) {
                    uint32_t a = pname + (uint32_t)i;
                    char ch = (char)(ld32(a & ~3u) >> ((a & 3u) * 8u));
                    if (!ch) break;
                    g_uni_name[i] = ch;
                    g_uni_name[i + 1] = 0;
                }
            }
            g_uni_pending = 1;
        }
        else if (!strncmp(e->name, "glUniform1i", 11)) {
            /* ★ key: location + value. The sampler2D texture unit is set right here. */
            /* ★ 2026-11-10: deduplicate by (loc,val) instead of printing only the first 20. */
            static int seen_l[32], seen_v[32];
            static int nseen;
            int L = (int)gs32(c, 0), V = (int)gs32(c, 1);
            int dup = 0;
            for (int i = 0; i < nseen; i++)
                if (seen_l[i] == L && seen_v[i] == V) { dup = 1; break; }
            if (!dup && nseen < 32) {
                seen_l[nseen] = L; seen_v[nseen] = V; nseen++;
                fprintf(stderr, "[uniform] glUniform1i(loc=%d, val=%d)\n", L, V);
                fflush(NULL);
            }
        }
        else if (!strncmp(e->name, "glGetShaderiv", 13) ||
                 !strncmp(e->name, "glGetProgramiv", 14)) {
            /* ★★ 2026-11-10: shader compile/link status.
             *
             * Previously this was never checked -- if a shader fails to compile or link,
             * gl4es falls back to default behaviour, and if the status code the game gets is
             * "failure" it may take another path; if it is a fake "success",
             * it renders with a broken program -- which is exactly
             * the "flat-coloured sprites / wrong UV sampling" shape.
             *
             * The return value is written into *params (the int the 3rd argument points to),
             * so it is read **after** the call. Record it here first. */
            g_iv_pname = gs32(c, 1);
            g_iv_ptr   = gs32(c, 2);
            g_iv_isprog = (e->name[9] == 'P');
            g_iv_pending = 1;
        }
        else if (!strncmp(e->name, "glActiveTexture", 15)) {
            /* ★ 2026-11-10: record the currently active texture unit.
             * GL_TEXTURE0=0x84C0, so unit = enum value - 0x84C0. */
            static int shown_at[4];
            unsigned e2 = gs32(c, 0);
            unsigned unit = (e2 >= 0x84C0u) ? (e2 - 0x84C0u) : 0u;
            g_cur_active_unit = unit;
            if (unit < 4u && shown_at[unit] < 1) {
                shown_at[unit] = 1;
                fprintf(stderr, "[activeTexture] switched to unit%u (0x%x)\n", unit, e2);
                fflush(NULL);
            }
        }
        else if (!strncmp(e->name, "glBlendFunc", 11)) {
            /* ★★ 2026-11-10: blend mode.
             *
             * On-device symptom: sprites become **flat-colour rectangles** instead of transparent artwork.
             * Besides UV/texture problems, another common cause is wrong blend factors.
             * Two typical cases:
             *   - premultiplied alpha data used with non-premultiplied maths -> dark/bright fringes
             *   - source factor wrongly ONE -> transparent areas also write colour -> the whole block is opaque
             * Printing the actual arguments here makes it easy to judge against the GL constants. */
            static unsigned bf;
            bf++;
            if (bf <= 12u) {
                if (!strcmp(e->name, "glBlendFuncSeparate")) {
                    fprintf(stderr, "[blend] FuncSeparate(srcRGB=0x%x dstRGB=0x%x"
                            " srcA=0x%x dstA=0x%x)\n",
                            gs32(c,0), gs32(c,1), gs32(c,2), gs32(c,3));
                } else {
                    fprintf(stderr, "[blend] %s(src=0x%x dst=0x%x)\n",
                            e->name, gs32(c,0), gs32(c,1));
                }
                fflush(NULL);
            }
        }
        else if (!strncmp(e->name, "glEnable", 8) ||
                 !strncmp(e->name, "glDisable", 9)) {
            /* Only GL_BLEND(0xBE2) / GL_DEPTH_TEST(0xB71) / GL_ALPHA_TEST matter here */
            uint32_t cap = gs32(c, 0);
            /* ★ read-only: track the depth-test switch, calling no GL function. */
            if (cap == 0x0B71u) {
                int on = (e->name[2] == 'E');   /* glEnable / glDisable */
                if (g_depthst.depth_test != on) {
                    g_depthst.depth_test = on;
                    g_depthst.draws_at_state = 0;
                    depthst_report(on ? "glEnable(DEPTH_TEST)"
                                      : "glDisable(DEPTH_TEST)");
                }
            }
            if (cap == 0x0BE2u || cap == 0x0B71u || cap == 0x0BC0u) {
                static unsigned en;
                en++;
                if (en <= 16u) {
                    fprintf(stderr, "[state] %s 0x%x (%s)\n", e->name, cap,
                            cap == 0x0BE2u ? "BLEND"
                          : cap == 0x0B71u ? "DEPTH_TEST"
                                          : "ALPHA_TEST");
                    fflush(NULL);
                }
            }
        }
        /* ★★ read-only depth-state probe: reads only gs32(c,0) and calls no GL function.
         * Note: handling glEnable must go into the **existing** branch (it matches earlier),
         * a separate else-if is never reached -- this trap was already hit once this round. */
        else if (!strcmp(e->name, "glDepthFunc")) {
            int f = (int)gs32(c, 0);
            if (f != g_depthst.depth_func) {
                g_depthst.depth_func = f;
                depthst_report("glDepthFunc");
            }
        }
        else if (!strcmp(e->name, "glClearDepth") ||
                 !strcmp(e->name, "glClearDepthf")) {
            /* ★ read-only: depth clear value. On x86 glClearDepth is a double (8 bytes)
             * and glClearDepthf is a float (4 bytes). After the bridge fix both are passed correctly;
             * this only records the arguments as evidence. */
            double d = !strcmp(e->name, "glClearDepth")
                       ? gsd(c, 0) : (double)gsf(c, 0);
            g_depthst.clear_depth = d;
            g_depthst.have_clear_depth = 1;
            depthst_report("glClearDepth");
        }
        else if (!strcmp(e->name, "glFramebufferTexture2D")) {
            /* ★ strictly read-only: which texture is attached as an FBO attachment.
             * Arguments: target, attachment, textarget, texture, level */
            uint32_t tex = (uint32_t)gs32(c, 3);
            uint32_t att = (uint32_t)gs32(c, 1);
            /* ★ Note: **never return** here. This is a branch inside the dispatch function,
             * and returning would skip gl_bridge_wrapper below --
             * i.e. swallow glFramebufferTexture2D, so the game never gets an FBO.
             * Just wrap it in an if. */
            if (tex < 0x10000u) {
                static unsigned shown;
                unsigned k;
                for (k = 0; k < g_fbo_att.n; k++)
                    if (g_fbo_att.tex[k] == tex) break;
                if (k == g_fbo_att.n && g_fbo_att.n < 32u) {
                    g_fbo_att.tex[g_fbo_att.n] = tex;
                    g_fbo_att.att[g_fbo_att.n] = att;
                    g_fbo_att.n++;
                    if (shown++ < 16u) {
                        fprintf(stderr,
                            "[fbatt] tex=%u attached as FBO attachment attach=0x%x%s\n",
                            tex, att,
                            att == 0x8CE0u ? "(COLOR0)" :
                            att == 0x8D00u ? "(DEPTH)" : "");
                        fflush(NULL);
                    }
                }
            }
        }
        else if (!strcmp(e->name, "glViewport")) {
            /* ★ strictly read-only: viewport. The main-menu RT is 1024x1024 but the logical picture is 960x540,
             * so a wrong viewport draws the content into a corner or clips it. */
            g_fbst.vp_x = (uint32_t)gs32(c, 0);
            g_fbst.vp_y = (uint32_t)gs32(c, 1);
            g_fbst.vp_w = (uint32_t)gs32(c, 2);
            g_fbst.vp_h = (uint32_t)gs32(c, 3);
            g_fbst.vp_seen = 1u;
            static unsigned shown_vp;
            static uint32_t last[4] = { 0xFFFFFFFFu, 0, 0, 0 };
            if (shown_vp < 20u &&
                (last[0] != g_fbst.vp_x || last[1] != g_fbst.vp_y ||
                 last[2] != g_fbst.vp_w || last[3] != g_fbst.vp_h)) {
                shown_vp++;
                last[0] = g_fbst.vp_x; last[1] = g_fbst.vp_y;
                last[2] = g_fbst.vp_w; last[3] = g_fbst.vp_h;
                fprintf(stderr, "[viewport] fb=%u %ux%u @%u,%u\n",
                        g_fbst.cur_fb, g_fbst.vp_w, g_fbst.vp_h,
                        g_fbst.vp_x, g_fbst.vp_y);
                fflush(NULL);
            }
        }
        else if (!strcmp(e->name, "glBindFramebuffer")) {
            /* ★ strictly read-only: only record the new binding, calling no GL function. */
            uint32_t fb = (uint32_t)gs32(c, 1);
            if (fb != g_fbst.cur_fb) {
                g_fbst.cur_fb = fb;
                g_fbst.fb_draws = 0u;
                g_fbst.switches++;
                static unsigned shown_sw;
                if (shown_sw < 24u) {
                    shown_sw++;
                    fprintf(stderr, "[fb] bound framebuffer=%u%s  total switches=%u\n",
                            fb, fb ? "(offscreen RT)" : "(default/screen)",
                            g_fbst.switches);
                    fflush(NULL);
                }
            }
        }
        else if (!strcmp(e->name, "glAlphaFunc")) {
            /* ★★★ read-only: (func, ref) of the alpha test.
             *
             * This is the most critical argument of this round. The shader only has
             *   #ifdef GL_ES
             *       if (gl_FragColor.a == 0.0) discard;
             *   #endif
             * i.e. fully transparent pixels are discarded automatically **only under GLSL ES**.
             * On the non-ES compile path, transparent pixels are decided by glAlphaFunc.
             *
             * Before the bridge fix glAlphaFunc was sig='ii', so the second argument ref
             * (GLclampf, a float) was stuffed into x1 as an integer and gl4es read it from s0 -> garbage.
             * ref too high -> every pixel is discarded by the alpha test -> **sprites/monsters/HUD vanish entirely**
             * ref too low -> transparent pixels pass too -> **hearts become solid red rectangles**
             * Both symptoms match the screen recording exactly. */
            static unsigned shown_af;
            if (shown_af < 12u) {
                shown_af++;
                fprintf(stderr, "[alpha] glAlphaFunc(func=0x%x %s, ref=%.6f)\n",
                        gs32(c, 0),
                        gs32(c, 0) == 0x0201 ? "NEVER" :
                        gs32(c, 0) == 0x0204 ? "GREATER" :
                        gs32(c, 0) == 0x0203 ? "LESS" :
                        gs32(c, 0) == 0x0207 ? "ALWAYS" :
                        gs32(c, 0) == 0x0206 ? "LEQUAL" :
                        gs32(c, 0) == 0x0202 ? "EQUAL" : "",
                        (double)gsf(c, 1));
                fflush(NULL);
            }
        }
        else if (!strcmp(e->name, "glDepthRange") ||
                 !strcmp(e->name, "glDepthRangef")) {
            /* ★ read-only: depth range. This is the second key quantity for judging whether depth is sane.
             * Windows glDepthRange takes 2 doubles; GLES glDepthRangef takes 2 floats.
             * Before the bridge fix this received garbage ('ii'), and gl4es glDepthRangef
             * clamped the garbage to some value in [0,1] -- a wrong depth range equally makes
             * GL_GREATER reject a whole class of fragments. */
            int isd = !strcmp(e->name, "glDepthRange");
            double n = isd ? gsd(c, 0) : (double)gsf(c, 0);
            double f2 = isd ? gsd(c, 2) : (double)gsf(c, 1);
            static unsigned shown_dr;
            if (shown_dr < 12u) {
                shown_dr++;
                fprintf(stderr, "[depthst]   |- %s(%.6f, %.6f)\n",
                        e->name, n, f2);
                fflush(NULL);
            }
        }
        else if (!strcmp(e->name, "glDepthMask")) {
            int m = (int)gs32(c, 0);
            if (m != g_depthst.depth_mask) {
                g_depthst.depth_mask = m;
                depthst_report("glDepthMask");
            }
        }
        else if (!strncmp(e->name, "glBindTexture", 13)) {
            g_frame_texbinds++;
            g_cur_tex_id = gs32(c, 1);
            /* ★★ 2026-11-10: record "which unit this texture id is bound to".
             *
             * The palette shader needs two samplers:
             *     vec4 PaletteIndex = texture2D(Texture0, TexCoord0);
             *     vec4 OutputColor  = texture2D(Texture1, PaletteIndex.xy) * Color0;
             * If both samplers point at unit 0, the palette lookup will
             * sample **the index texture itself** -> colours completely scrambled -> exactly "scrambled textures".
             *
             * So we must know whether the game ever binds the palette to unit 1. */
            {
                static unsigned shown_bind[4];
                unsigned unit = g_cur_active_unit;
                if (unit < 4u && shown_bind[unit] < 6u) {
                    shown_bind[unit]++;
                    fprintf(stderr, "[bind] unit%u <- tex=%u\n",
                            unit, g_cur_tex_id);
                    fflush(NULL);
                }
            }
            /* Histogram: which texture is bound most often.
             * A plain white screen typically means "every draw binds the same 8x8 white texture",
             * which is obvious at a glance -- far more useful than printing every bind. */
            {
                uint32_t id = gs32(c, 1);
                if (id < TEX_HIST_N) g_tex_hist[id]++;
                else g_tex_hist[TEX_HIST_N - 1]++;
                g_tex_hist_total++;
            }
            static int tshow;
            if (tshow < 8) {
                tshow++;
                fprintf(stderr, "[tex] glBindTexture(target=%u, id=%u)\n",
                        gs32(c,0), gs32(c,1));
                fflush(NULL);
            }
        }
        /* ================================================================
         * Texture upload probe (2026-11-04)
         *
         * Earlier, "GetProcAddress(glTexImage2D) appears only once" was used to infer
         * "no texture was ever really uploaded" -- that inference was wrong: once the game has the function pointer
         * it caches it and calls it directly, so GetProcAddress should only ever appear once.
         * To decide whether uploads really happen, count real calls at the dispatch point.
         *
         * glTexImage2D(GLenum target, GLint level, GLint internalformat,
         *              GLsizei w, GLsizei h, GLint border,
         *              GLenum format, GLenum type, const void *pixels)
         * 9 cdecl arguments, gs32(c,0..8).
         * ================================================================ */
        else if (!strcmp(e->name, "glTexImage2D")) {
            static unsigned long n_teximg, n_empty;
            uint32_t px = gs32(c, 8);
            n_teximg++;
            /* ★★ 2026-11-10 (plan B): record **all arguments** of this upload.
             *
             * "Data not ready at upload time" is a real timing mismatch (evidence in HANDOFF_20261110.md:
             * the same pointer ptr=5a31f010 uploads all zero first and content later),
             * so we must be able to replay this upload verbatim at draw time.
             * The replay needs the complete set of arguments; missing one breaks it:
             *   target/level/ifmt/w/h/border/fmt/type/px
             *
             * Only record the level==0 case with a non-empty px:
             *   - level>0 is a mipmap, replaying level 0 has nothing to do with it
             *   - px==NULL is an FBO/placeholder allocation; replaying NULL only clears the image
             */
            {
                uint32_t sid = g_cur_tex_id;
                uint32_t lvl = gs32(c, 1);
                if (sid > 0u && sid < TEX_INFO_N && px >= 0x10000u && lvl == 0u
                        && gs32(c, 0) == 0x0DE1u   /* GL_TEXTURE_2D only */
                        && gs32(c, 3) > 0 && gs32(c, 4) > 0) {
                    texsrc_t *s = &g_tex_src[sid];
                    s->ptr    = px;
                    s->w      = gs32(c, 3);
                    s->h      = gs32(c, 4);
                    s->target = gs32(c, 0);
                    s->level  = (int32_t)lvl;
                    s->ifmt   = (int32_t)gs32(c, 2);
                    s->border = (int32_t)gs32(c, 5);
                    s->fmt    = gs32(c, 6);
                    s->type   = gs32(c, 7);
                    s->unit   = (unsigned char)g_cur_active_unit;
                    s->valid  = 1;
                    /* A new upload -> allow probing again this frame (the content may have changed) */
                    g_tex_repair_frame[sid] = 0u;
                }
            }
            /* Record "bound texture id -> size", so the texId printed at draw time
             * maps straight to a size and we can tell whether it is that 8x8 white texture. */
            /* Print the guest return address to locate "who created this texture slot" */
            /* ★ 2026-11-06 start-up speed: this used to print a line for every id>=900 texture,
             * and hundreds of large textures late in loading meant thousands of fprintf+fflush lines,
             * each one also a Java -> external-storage write. Keep only the first few. */
            if (n_teximg <= 24 && g_cur_tex_id >= 900 && g_cur_tex_id < TEX_INFO_N &&
                !g_tex_seen[g_cur_tex_id]) {
                fprintf(stderr, "[teximg] id=%u creator ret=%08x\n",
                        g_cur_tex_id, ld32(c->esp));
            }
            {
                uint32_t id = g_cur_tex_id;
                if (id < TEX_INFO_N) {
                    g_tex_w[id] = gs32(c, 3);
                    g_tex_h[id] = gs32(c, 4);
                    g_tex_seen[id] = 1;
                    /* ★ 2026-11-06 start-up speed: the "first-pixel sampling" below did 64
                     * cross-page random reads per id>=900 texture (1024x1024 = 4MB,
                     * every one a cache miss), so hundreds of large images meant tens of thousands of
                     * misses + thousands of log lines. Do it only for the first few textures and when
                     * TEX_TRACE is explicitly enabled. */
                    static int tex_trace = -1;
                    if (tex_trace < 0) tex_trace = getenv("TEX_TRACE") != NULL;
                    if (n_teximg <= 24 || (id >= 900 && tex_trace)) {
                        fprintf(stderr, "[teximg] id=%u %ux%u",
                                id, gs32(c,3), gs32(c,4));
                        /* id>=900 means a large texture created late -- print the first pixel and
                         * one random mid-point pixel to judge whether it is plain white/empty. */
                        if (id >= 900 && px) {
                            uint32_t w = gs32(c,3), hh = gs32(c,4);
                            unsigned long sum = 0; int nz = 0;
                            /* coarse 64-point sampling: count the non-zero and non-0xFF ratios */
                            for (int k = 0; k < 64; k++) {
                                uint32_t o = (uint32_t)((unsigned long)k *
                                    (unsigned long)(w * hh) / 64ul) * 4u;
                                uint8_t v = *(uint8_t *)(uintptr_t)(px + o);
                                sum += v; if (v) nz++;
                            }
                            fprintf(stderr, " first=R%uG%uB%uA%u sample mean=%lu nonzero=%d/64",
                                    *(uint8_t*)(uintptr_t)px,
                                    *(uint8_t*)(uintptr_t)(px+1),
                                    *(uint8_t*)(uintptr_t)(px+2),
                                    *(uint8_t*)(uintptr_t)(px+3),
                                    sum/64, nz);
                        }
                        fprintf(stderr, "\n");
                        fflush(NULL);
                    }
                }
            }
            if (!px) n_empty++;
            /* ★★ 2026-11-10: raw pixel snapshot export.
             *
             * A text log only gives means and hides pixel-format details.
             * So the first 1KB of raw bytes of the first 12 typical textures is written as hex,
             * which locally allows us to determine precisely:
             *   - whether the four RGBA channels really interleave as R,G,B,A
             *   - whether it is BGRA (B/G swapped)
             *   - whether it is single-channel LUMINANCE (only 1 of every 4 bytes non-zero)
             *   - whether alpha is premultiplied
             * Only the first 12 are written, under 24KB of text in total. */
            if (px && px >= 0x10000u) {
                /* ★★ 2026-11-10 bug fix: the two branches must each use their own counter.
                 *
                 * texdump (full export for <=64x64) and texraw (only 64 bytes for >64x64)
                 * shared txdumped, so:
                 *   id=1  8x8    -> texdump, txdumped=1
                 *   id=3  512x512 -> texraw,  txdumped=2
                 *   id=5  512x512 -> texraw,  txdumped=3
                 *   id=259 32x32  -> txdumped=3, so the texdump condition failed!
                 * As a result none of the 32x32 HUD textures we actually wanted were exported,
                 * and the log only ever held the 8x8 all-white id=1 image. */
                static unsigned txdumped;    /* for texdump only */
                static unsigned txrawed;     /* for texraw only */
                /* ★★ 2026-11-10: export **complete** small textures (not just 64 bytes).
                 *
                 * On-device symptom: HUD hearts/stat icons became flat-colour rectangles, while in the reference
                 * images they are shaded artwork. texchan showed that batch of 32x32 textures
                 * (id=259/263/287/311/317/345) "has only the R channel".
                 *
                 * 64 bytes cannot prove anything (not even one row of a 32x32 image).
                 * So 32x32 and smaller textures are exported **in full** as hex,
                 * which can be rebuilt into a PNG locally to see what they are.
                 *
                 * Limit: only <=64x64 and only the first 3, so the total stays controllable. */
                uint32_t dw = (uint32_t)gs32(c, 3), dh = (uint32_t)gs32(c, 4);
                uint64_t nbytes = (uint64_t)dw * dh * 4ull;
                /* ★ 2026-11-10: limit 3 -> 6, and prefer 32x32 (the HUD icon batch).
                 * The 32x32 textures are the main source of the "R channel only" warning,
                 * so their complete data is required to tell a data bug from a rendering bug. */
                if (txdumped < 6u && dw > 0 && dh > 0 &&
                    dw <= 64u && dh <= 64u && nbytes <= 16384ull) {
                    txdumped++;
                    fprintf(stderr, "[texdump] id=%u %ux%u bytes=%llu fmt=0x%x\n",
                            g_cur_tex_id, dw, dh, (unsigned long long)nbytes,
                            gs32(c, 6));
                    const unsigned char *q2 = (const unsigned char *)(uintptr_t)px;
                    for (uint64_t off = 0; off < nbytes; off += 64) {
                        fprintf(stderr, "[texdump] %06llu ",
                                (unsigned long long)off);
                        uint64_t lim = (nbytes - off < 64) ? (nbytes - off) : 64;
                        for (uint64_t k = 0; k < lim; k++)
                            fprintf(stderr, "%02x", (unsigned)q2[off + k]);
                        fprintf(stderr, "\n");
                    }
                    fflush(NULL);
                }
                /* ★ font/UI atlas export: forensics for the garbled HUD */
                if (px >= 0x10000u && dw >= 64u && dw <= 512u &&
                    dh >= 16u && dh <= 512u &&
                    (uint64_t)dw * dh <= 512ull * 512ull) {
                    extern void host_tex_dump_ppm(uint32_t, uint32_t, uint32_t,
                                                  uint32_t, const unsigned char *);
                    host_tex_dump_ppm(g_cur_tex_id, dw, dh, gs32(c, 6),
                                      (const unsigned char *)(uintptr_t)px);
                }
                if (txrawed < 12u && dw > 64u) {
                    txrawed++;
                    fprintf(stderr, "[texraw] id=%u %ux%u ifmt=0x%x fmt=0x%x type=0x%x ptr=%08x\n",
                            g_cur_tex_id, dw, dh,
                            gs32(c, 2), gs32(c, 6), gs32(c, 7), px);
                    fprintf(stderr, "[texraw]   ");
                    for (int k = 0; k < 64; k++)
                        fprintf(stderr, "%02x", (unsigned)((const unsigned char *)(uintptr_t)px)[k]);
                    fprintf(stderr, "\n");
                    fflush(NULL);
                }
            }
            /* ★★ 2026-11-10: separate "placeholder allocation" from "real upload".
             *
             * This was the biggest error in the previous version's statistics:
             *   an FBO attachment texture (e.g. id=987) calls glTexImage2D with data=NULL,
             *   which only allocates VRAM; the GPU renders the content into it.
             *   Sampling it from the CPU always yields zero -- that is not a broken image, it is invisible.
             *
             * Result: texfinal's "19% all zero" mixed in many FBO placeholders
             * and did not match the "main menu/monsters missing" the user saw at all.
             *
             * Now:
             *   px == NULL          -> g_tex_zero = 0 (unknown, excluded from numerator and denominator)
             *   px != NULL all zero -> g_tex_zero = 2 (a genuinely broken image)
             *   px != NULL with content -> g_tex_zero = 1 (good)
             */
            /* ★ full-count statistics (not sampling): look at the first 16 bytes of every texture
             * to decide "how many really carry content this round". Reading 16 bytes is negligible. */
            /* ★★ 2026-11-12: record the **caller return address** of each texture upload + whether it is all zero.
             *
             * id=9/83/95/97 are known to upload all zero, and id=97(256x256) and id=105(256x256)
             * **share the same ptr=65312010**, one all zero and one with content.
             * We must tell "which loader uploaded it" -- the 6 SetTexelData call sites
             * correspond to ImagePng / solid fill / 4 other unknown paths.
             * Strictly read-only c->esp, calling no GL. */
            {
                uint32_t ret = ld32(c->esp);
                uint32_t pw = (uint32_t)gs32(c, 3), ph = (uint32_t)gs32(c, 4);
                int is_zero = 0;
                if (px >= 0x10000u && pw && ph &&
                    (uint64_t)pw * ph <= 512ull * 512ull) {
                    const unsigned char *q0 = (const unsigned char *)(uintptr_t)px;
                    uint64_t tot = (uint64_t)pw * ph * 4ull;
                    uint64_t nzb = 0;
                    for (uint64_t i = 0; i < tot; i++) if (q0[i]) { nzb++; if (nzb > 8) break; }
                    is_zero = (nzb == 0);
                }
                extern unsigned long g_upl_caller_ret[];
                extern unsigned char g_upl_caller_zero[];
                extern uint32_t g_upl_caller_id[];
                if (g_cur_tex_id < TEX_INFO_N) {
                    g_upl_caller_ret[g_cur_tex_id]  = ret;
                    g_upl_caller_zero[g_cur_tex_id] = (unsigned char)is_zero;
                    g_upl_caller_id[g_cur_tex_id]   = px;
                }
            }
            {
                static unsigned long n_blank;
                /* ★ criterion fix: looking only at the first 16 bytes misleads -- the top-left of a spritesheet
                 * is often transparent. Use 64-point uniform sampling of the non-zero ratio instead. */
                if (!px || px < 0x10000u) {
                    n_blank++;
                    /* ★ 2026-11-10: px==NULL is an FBO/placeholder allocation, not a broken image.
                     * It used to be set to 2 here, which inflated the all-zero statistics. */
                    if (g_cur_tex_id < TEX_INFO_N) g_tex_zero[g_cur_tex_id] = 0;
                } else {
                    uint32_t w = (uint32_t)gs32(c, 3), hh = (uint32_t)gs32(c, 4);
                    const unsigned char *q = (const unsigned char *)(uintptr_t)px;
                    uint64_t npix = (uint64_t)w * hh;
                    int nz = 0;
                    /* ★ 2026-11-10: accumulate the four channels separately.
                     * Used to judge whether "red when switching rooms" is swapped channels:
                     *   - a normal texture has similar R/G/B means
                     *   - if only R is high and G/B are always 0 -> BGRA/RGBA reversed
                     *   - if R/G/B are all 0 and A has values -> alpha used as colour */
                    unsigned long sR = 0, sG = 0, sB = 0, sA = 0;
                    if (npix > 0) {
                        for (int k = 0; k < 64; k++) {
                            uint64_t o = (uint64_t)k * npix / 64ull * 4ull;
                            sR += q[o]; sG += q[o+1]; sB += q[o+2]; sA += q[o+3];
                            if (q[o] || q[o+1] || q[o+2] || q[o+3]) { nz++; }
                        }
                    }
                    if (nz == 0) {
                        n_blank++;
                        /* ★★ 2026-11-10: **call-site probe**.
                         *
                         * Confirmed both on device and locally: with the same ptr and the same size,
                         * some ids succeed and others are all zero ("same pointer, id=49 has content,
                         * id=3/5/7 all zero"). So the problem is not memory, not the pool,
                         * not the size, but **some decodes really produce zero**.
                         *
                         * But we must tell "which path": ImagePng / ImagePcx /
                         * ProceduralImageBase may all allocate this texture, and
                         * only ImagePng goes through libpng. The return address settles it at once.
                         *
                         * ld32(c->esp) = the return address pushed by the call (guest RVA).
                         * Note this function runs **before the real gl4es call**,
                         * so esp still points at the return address. */
                        extern int fread_covers(uint32_t ptr);
                        static unsigned zshown;
                        if (zshown < 40u) {
                            zshown++;
                            /* Full scan: separate "nothing written" from "only a little written".
                             * 64-point sampling can miss a small amount of writing, but there are
                             * only a few dozen all-zero textures, so one full scan is affordable (local diagnostics). */
                            uint64_t tot = npix * 4ull, nzb = 0;
                            if (tot > (64ull << 20)) tot = 64ull << 20;  /* 64MB cap */
                            for (uint64_t i = 0; i < tot; i++) if (q[i]) nzb++;
                            fprintf(stderr,
                                "[zerotex] id=%u %ux%u ptr=%08x ret=%08x "
                                "nonzero bytes=%llu/%llu first16="
                                "%02x%02x%02x%02x%02x%02x%02x%02x"
                                "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                g_cur_tex_id, gs32(c,3), gs32(c,4), px,
                                ld32(c->esp),
                                (unsigned long long)nzb, (unsigned long long)tot,
                                q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],
                                q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]);
                            fflush(NULL);
                        }
                        /* The old zerotex (with the fread check) is kept below,
                         * but it prints only when the quota above did not use it up. */
                        static unsigned zshown2;
                        /* ★★ 2026-11-12: **decisive check** -- the alpha channel of all-zero textures.
                         *
                         * The font atlas uses "RGB=255 white + alpha mask", and locally measured
                         * alpha=0 accounts for 64% and alpha=255 for 17%. Premultiply only multiplies
                         * RGB by alpha and **never touches alpha itself**.
                         *
                         * Therefore:
                         *   alpha all zero too -> decoding wrote nothing at all (ImagePng failed)
                         *   alpha has non-zeros -> it was written; premultiply cleared the RGB
                         * The two paths need completely different fixes, so they must be separated first. */
                        if (zshown2 < 8u) {
                            uint64_t an = 0, rn = 0;
                            uint64_t tot2 = npix * 4ull;
                            if (tot2 > (8ull << 20)) tot2 = 8ull << 20;
                            for (uint64_t i = 0; i + 3 < tot2; i += 4) {
                                if (q[i] || q[i+1] || q[i+2]) rn++;
                                if (q[i+3]) an++;
                            }
                            fprintf(stderr,
                                "[alphachk] id=%u %ux%u ptr=%08x rgb_nonzero=%llu alpha_nonzero=%llu"
                                " -> %s\n",
                                g_cur_tex_id, gs32(c,3), gs32(c,4), px,
                                (unsigned long long)rn, (unsigned long long)an,
                                an ? "written; premultiply cleared RGB"
                                   : "decoder wrote nothing (all zero)");
                            fflush(NULL);
                        }
                        if (zshown2 < 4u) {
                            zshown2++;
                            fprintf(stderr,
                                "[zerotex] id=%u %ux%u ptr=%08x fread=%s "
                                "first4=%02x%02x%02x%02x\n",
                                g_cur_tex_id, gs32(c,3), gs32(c,4), px,
                                fread_covers(px) ? "yes" : "no",
                                q[0], q[1], q[2], q[3]);
                            fflush(NULL);
                        }
                    }
                    /* Only report the genuinely suspicious: high R with G/B exactly 0 (wrong channel order).
                     *
                     * ★ 2026-11-10: drop the only_a warning.
                     * "R=G=B=0 but A has values" is **normal** -- shadows, glows and
                     * font outlines are all pure-alpha textures (black with transparency).
                     * They used to be reported as anomalies, flooding the log with false positives. */
                    {
                        static unsigned long shown_rgba;
                        int r_big = (sR >= 64 * 8 && sG == 0 && sB == 0);
                        if (shown_rgba < 6UL && r_big) {
                            shown_rgba++;
                            fprintf(stderr, "[rgb] ⚠ id=%u %ux%u mean R=%lu G=%lu B=%lu A=%lu"
                                    "  <- only R (suspected BGRA/RGBA channel order bug)\n",
                                    g_cur_tex_id, w, hh,
                                    sR / 64, sG / 64, sB / 64, sA / 64);
                            fflush(NULL);
                        }
                    }
                    if (g_cur_tex_id < TEX_INFO_N) {
                        g_tex_zero[g_cur_tex_id] = (nz == 0) ? 2 : 1;
                        /* Keep the mean colour for comparison at draw time (see the g_tex_rgb comment) */
                        g_tex_rgb[g_cur_tex_id][0] = (unsigned char)(sR / 64);
                        g_tex_rgb[g_cur_tex_id][1] = (unsigned char)(sG / 64);
                        g_tex_rgb[g_cur_tex_id][2] = (unsigned char)(sB / 64);

                        /* ★ 2026-11-10: channel classification (see the g_tex_class comment).
                         * The criterion is extracted into tex_classify() and must use **the same** rule
                         * as the draw-time re-upload path, or the [texchan] distribution splits. */
                        g_tex_class[g_cur_tex_id] =
                            tex_classify(sR, sG, sB, sA, nz);
                    }

                    /* ★★ 2026-11-10: channel-order detection.
                     *
                     * The clearest difference against the recording (bug.jpg): the game picture **fades to red/white**.
                     * The typical cause is **BGRA data uploaded as RGBA** (or the reverse).
                     * Isaac runs on Windows there, so its pixel data is quite likely
                     * BGRA to begin with (DIB format).
                     *
                     * Criterion: interpret the same block of data as RGBA and as BGRA
                     * and see which looks more like an image:
                     *   - a real image rarely has R=G=B for every pixel (greyscale)
                     *   - if read as RGBA the G/B means are near 0, while read as BGRA
                     *     all three channels have a distribution, then it is BGRA. */
                    {
                        static unsigned long shown_ch;
                        if (shown_ch < 6UL && w >= 32u && hh >= 32u) {
                            unsigned long eR=0,eG=0,eB=0;
                            for (int k = 0; k < 64; k++) {
                                uint64_t o = (uint64_t)k * npix / 64ull * 4ull;
                                eR += q[o]; eG += q[o+1]; eB += q[o+2];
                            }
                            /* Read as RGBA: G and B are almost 0 while R clearly has values */
                            int rgb_degenerate = (eG / 64 < 3UL && eB / 64 < 3UL
                                                  && eR / 64 > 10UL);
                            if (rgb_degenerate) {
                                shown_ch++;
                                fprintf(stderr,
                                    "[chan] ⚠ id=%u %ux%u  read as RGBA: R=%lu G=%lu B=%lu (G/B~0)"
                                    " read as BGRA: R=%lu G=%lu B=%lu\n",
                                    g_cur_tex_id, w, hh,
                                    eR/64, eG/64, eB/64,
                                    eB/64, eG/64, eR/64);
                                fflush(NULL);
                            }
                        }
                    }
                }
                /* Pixel address of non-zero textures: 0x30xxxxxx = built into the PE,
                 * 0x48..-0xB0.. = our guest heap = loaded from the archive.
                 * This distinction decides whether the problem is "decoding" or "archive reading". */
                if (px && px >= 0x10000u) {
                    const unsigned char *q = (const unsigned char *)(uintptr_t)px;
                    uint32_t w2 = (uint32_t)gs32(c, 3), h2 = (uint32_t)gs32(c, 4);
                    uint64_t np2 = (uint64_t)w2 * h2;
                    int z = 1;
                    if (np2 > 0) {
                        for (int k = 0; k < 64; k++) {
                            uint64_t o = (uint64_t)k * np2 / 64ull * 4ull;
                            if (q[o]) { z = 0; break; }
                        }
                    }
                    if (!z) {
                        static unsigned long n_ok_tex;
                        /* ★ 2026-11-10: limit 12 -> 40, and **prefer large ids**.
                         * Real rendering uses ids like 1038/1074/1091/1149/1155,
                         * and the old "first 12 only" all landed on small start-up textures,
                         * so the ones actually used on screen were never visible. */
                        if (n_ok_tex++ < 40UL || g_cur_tex_id >= 1000u)
                            fprintf(stderr, "[texstat] non-zero texture #%lu id=%u %dx%d px=%08x "
                                    "ret=%08x %02x %02x %02x %02x\n",
                                    n_ok_tex, g_cur_tex_id, gs32(c,3), gs32(c,4), px,
                                    ld32(c->esp),
                                    q[0], q[1], q[2], q[3]);
                    }
                }
                if ((n_teximg % 20UL) == 0UL) {
                    fprintf(stderr, "[texstat] %lu textures total, %lu of them all zero\n",
                            n_teximg, n_blank);
                    fflush(NULL);
                }
            }
            if (n_teximg <= 12UL || (n_teximg % 64UL) == 0UL) {
                fprintf(stderr,
                    "[teximg] #%lu target=%u level=%u ifmt=%u %ux%u border=%u "
                    "fmt=%u type=%u pixels=%08x (null pointers total %lu)\n",
                    n_teximg, gs32(c,0), gs32(c,1), gs32(c,2),
                    gs32(c,3), gs32(c,4), gs32(c,5),
                    gs32(c,6), gs32(c,7), px, n_empty);
                /* 2026-11-04: knowing "it uploaded" is not enough -- we also need **the uploaded bytes**.
                 * If the pixels are all 0 or all 0xFF, PNG decoding actually failed
                 * and a blank buffer was uploaded, so the problem is decoding, not rendering. */
                if (px) {
                    /* first 16 bytes all zero -> trigger one heap scan to see whether the decompressed output is PNG */
                    {
                        const unsigned char *q = (const unsigned char *)(uintptr_t)px;
                        int z = 1;
                        for (int k = 0; k < 16; k++) if (q[k]) { z = 0; break; }
                        if (z) png_scan_heap_once();
                    }
                    unsigned char b[16];
                    for (int k = 0; k < 16; k++)
                        b[k] = *(unsigned char *)(uintptr_t)(px + (uint32_t)k);
                    fprintf(stderr,
                        "[teximg]     first 16 bytes: %02x %02x %02x %02x  %02x %02x %02x %02x"
                        "  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                        b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
                }
                fflush(NULL);
            }
        }
        /* ★ 2026-11-06: glTexSubImage2D is the key path for "build an empty atlas, then fill it",
         * and it had no logging at all -- with all-zero textures we could not tell "not called" from "called with zero data".
         * 9 cdecl arguments: target,level,x,y,w,h,format,type,pixels */
        else if (!strcmp(e->name, "glTexSubImage2D")) {
            static unsigned long n_sub, n_sub_zero;
            uint32_t spx = gs32(c, 8);
            n_sub++;
            {
                int z = (spx == 0);
                if (!z) {
                    const unsigned char *q = (const unsigned char *)(uintptr_t)spx;
                    z = 1;
                    for (int k = 0; k < 16; k++) if (q[k]) { z = 0; break; }
                }
                if (z) n_sub_zero++;
            }
            if (n_sub <= 8UL || (n_sub % 200UL) == 0UL) {
                fprintf(stderr,
                    "[teximg] SubImage #%lu tex=%u %dx%d @%d,%d fmt=%u type=%u "
                    "px=%08x (all-zero total %lu)\n",
                    n_sub, g_cur_tex_id, gs32(c, 4), gs32(c, 5),
                    gs32(c, 2), gs32(c, 3), gs32(c, 6), gs32(c, 7),
                    spx, n_sub_zero);
                fflush(NULL);
            }
        }
        else if (!strcmp(e->name, "glClearColor")) {
            /* When the picture is plain white, the first thing to rule out is "cleared to white".
             * The arguments are 4 floats, read as f32. */
            static int ccshow;
            if (ccshow < 12) {
                ccshow++;
                float f[4];
                for (int k = 0; k < 4; k++) {
                    uint32_t bits = gs32(c, k);
                    float v; memcpy(&v, &bits, 4); f[k] = v;
                }
                fprintf(stderr, "[clear] glClearColor(%.3f, %.3f, %.3f, %.3f)\n",
                        f[0], f[1], f[2], f[3]);
                fflush(NULL);
            }
        }
        else if (!strcmp(e->name, "glCheckFramebufferStatus")) {
            /* The return value is printed after the common dispatch (see w(c, e->fn) below). */
        }
        else if (!strcmp(e->name, "glGenTextures")) {
            static unsigned long n_gen;
            n_gen++;
            if (n_gen <= 8UL) {
                fprintf(stderr, "[gentex] #%lu n=%u\n", n_gen, gs32(c,0));
                fflush(NULL);
            }
        }
    }

    /* (1) String return: get the full 64-bit pointer, then copy it into the low 4GB.
     *    (It cannot go through a u32ret wrapper -- that would truncate the pointer into garbage.) */
    if (e->ret == 's') {
        const char *s = NULL;
        if (e->sig == 1 && 0) { /* placeholder, keeps the structure clear */ }
        /* Handle the two known signatures: glGetString(GLenum), glGetStringi(GLenum,GLuint) */
        extern int gl_bridge_sig_is_1(int);
        (void)gl_bridge_sig_is_1;
        /* Take the signature length straight from the generated table */
        {
            /* sig is an index and the wrapper decides the length; use the argument count here */
            /* glGetString: 1 argument; glGetStringi: 2 */
        }
        if (!strcmp(e->name, "glGetStringi")) {
            s = ((const char *(*)(uint32_t, uint32_t))e->fn)(gs32(c, 0), gs32(c, 1));
            gret(c, 8);
        } else {
            s = ((const char *(*)(uint32_t))e->fn)(gs32(c, 0));
            gret(c, 4);
        }
        c->eax = gl_publish_str(s);
        return 1;
    }

    /* (2) Other pointer returns (glMapBuffer and the like): they point into gl4es internal buffers
     *    unreachable by the guest 32-bit pointers. Return 0 honestly and report it. */
    if (e->ret == 'p') {
        gl_note_missing(e->name, "returned a pointer that cannot be mapped in the low 4GB; not wired up");
        glbridge_fn w = gl_bridge_wrapper(e->sig, 0);   /* still execute, so state keeps advancing */
        if (w) w(c, e->fn);
        c->eax = 0;
        return 1;
    }

    /* (2b) Pointer-array arguments: the guest char** holds 32-bit pointers, while gl4es
     *     reads a host array of 64-bit pointers, gluing two guest pointers into one garbage address.
     *     That is exactly where the measured fault_addr came from. The host-side array must be rebuilt. */
    if (!strcmp(e->name, "glShaderSource")) {
        uint32_t shader = gs32(c, 0);
        uint32_t count  = gs32(c, 1);
        uint32_t strp   = gs32(c, 2);
        uint32_t lenp   = gs32(c, 3);
        if (count > 64) count = 64;          /* conservative limit */
        const char *host_strs[64];
        int32_t     host_lens[64];
        uint32_t i;
        for (i = 0; i < count; i++) {
            uint32_t gp = ld32(guest_stack_address(c, strp + i * 4u, 4u, 0u));
            host_strs[i] = gp ? (const char *)(uintptr_t)gp : "";
            host_lens[i] = lenp
                ? (int32_t)ld32(guest_stack_address(c, lenp + i * 4u, 4u, 0u))
                : -1;
        }

        /* ★★ 2026-11-10: the shader source must be copied **before the call**.
         *
         * The previous version read host_strs[k] after the call, so the log showed
         *   [shader #1.0 length 265]
         *   an empty line
         * -- the length was right but the content was all \0.
         * The cause is that gl4es copies the source internally and releases/reuses that guest memory,
         * so after the call the source pointer points at invalid content.
         *
         * Now we copy quickly into a local stack buffer first, and print after the call. */
        static char snap_src[6][912];
        static int  snap_len[6];
        int snap_n = 0;
        {
            static unsigned ns_pre;
            if (ns_pre < 6u) {
                for (uint32_t k = 0; k < count && snap_n < 6; k++) {
                    const char *t = host_strs[k] ? host_strs[k] : "";
                    int32_t L = host_lens[k];
                    int n = 0;
                    if (L > 0) n = (L < 900) ? (int)L : 900;
                    else while (n < 900 && t[n]) n++;
                    if (n > 0) memcpy(snap_src[snap_n], t, (size_t)n);
                    snap_len[snap_n] = n;
                    snap_n++;
                }
            }
        }

        ((void (*)(uint32_t, uint32_t, const char *const *, const int32_t *))e->fn)
            (shader, count, host_strs, lenp ? host_lens : NULL);
        gret(c, 16);

        /* ★★ 2026-11-10: print the source **copied before the call**.
         * This is the most direct evidence: it shows how the GLSL really computes UVs and colours.
         * On device Isaac uses custom shaders (not the fixed pipeline),
         * so "faded sprites / sampling the wrong cell" can only be explained from the GLSL. */
        if (snap_n > 0) {
            static unsigned ns;
            ns++;
            for (int k = 0; k < snap_n; k++) {
                fprintf(stderr, "[shader #%u.%d length=%d]\n", ns, k, snap_len[k]);
                if (snap_len[k] > 0)
                    fwrite(snap_src[k], 1, (size_t)snap_len[k], stderr);
                fprintf(stderr, "\n---\n");
            }
            fflush(NULL);
        }
        return 1;
    }

    /* (3) Scalar return vs void */
    glbridge_fn w = gl_bridge_wrapper(e->sig, e->ret == 'i' ? 1 : 0);
    if (!w) return 0;
    w(c, e->fn);

    /* ★ 2026-11-10: the glGetAttribLocation return value is readable only now.
     * The attribute name was stored in the branch; print the name + the actual index here. */
    if (g_gal_pending) {
        /* ★★ 2026-11-10: likewise switched to per-name dedup.
         * The limit of 16 hid later new attribute names, so the full layout stayed invisible. */
        static char seen[64][48];
        static int nseen;
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            if (!strcmp(seen[i], g_gal_name)) { dup = 1; break; }
        if (!dup && nseen < 64) {
            snprintf(seen[nseen], sizeof seen[0], "%s", g_gal_name);
            nseen++;
            fprintf(stderr, "[attrib-loc] \"%s\" -> %d\n",
                    g_gal_name, (int)c->eax);
            fflush(NULL);
        }
        g_gal_pending = 0;
    }

    /* ★ 2026-11-10: the glGetUniformLocation return value.
     * Returning -1 means the name does not exist in the shader (optimised away by the compiler, or a wrong name),
     * and after that every glUniform silently fails -- including the sampler texture-unit binding. */
    if (g_uni_pending) {
        /* ★★ 2026-11-10: changed from "only the first 20" to "once per distinct name".
         * The old limit of 20 was exactly filled by 8xTexture0 + 12xTransform,
         * so later queries of Texture1 were never recorded -- and
         * Texture1 is exactly the palette shader second sampler, the most critical evidence.
         * Deduplicating with a static table neither floods the log nor drops new names. */
        static char seen[256][56];
        static int nseen;
        char key[56];
        snprintf(key, sizeof key, "%u:%s", g_cur_prog, g_uni_name);
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            if (!strcmp(seen[i], key)) { dup = 1; break; }
        if (!dup && nseen < 256) {
            snprintf(seen[nseen], sizeof seen[0], "%s", key);
            nseen++;
            g_uni_last_loc = (int)c->eax;
            fprintf(stderr, "[uniform-loc] prog=%u \"%s\" -> %d%s\n",
                    g_cur_prog, g_uni_name, (int)c->eax,
                    (int)c->eax < 0 ? "  ★ not found (the uniform will be ignored)" : "");
            fflush(NULL);
        }
        g_uni_pending = 0;
    }

    /* ★ 2026-11-10: shader / program status.
     * The pnames that matter:
     *   0x8B81 GL_COMPILE_STATUS   0x8B82 GL_LINK_STATUS
     *   0x8B84 GL_INFO_LOG_LENGTH  0x8B86 GL_ACTIVE_UNIFORMS
     *   0x8B89 GL_ACTIVE_ATTRIBUTES
     * The game usually notices a compile/link failure itself; but if gl4es lies about success,
     * it renders with a broken program -- the prime suspect. */
    if (g_iv_pending) {
        static unsigned iv;
        if (iv < 24u && g_iv_ptr) {
            iv++;
            int val = (int)ld32(g_iv_ptr);
            const char *nm = NULL;
            switch (g_iv_pname) {
                case 0x8B81u: nm = "COMPILE_STATUS"; break;
                case 0x8B82u: nm = "LINK_STATUS";    break;
                case 0x8B84u: nm = "INFO_LOG_LEN";   break;
                case 0x8B86u: nm = "ACTIVE_UNIFORMS";break;
                case 0x8B89u: nm = "ACTIVE_ATTRIBS"; break;
                default: break;
            }
            if (nm) {
                int fail = ((g_iv_pname == 0x8B81u || g_iv_pname == 0x8B82u)
                            && val == 0);
                fprintf(stderr, "[shaderstat] %s %s = %d%s\n",
                        g_iv_isprog ? "program" : "shader", nm, val,
                        fail ? "  ★★ FAILED!" : "");
                fflush(NULL);
            }
        }
        g_iv_pending = 0;
    }

    /* FBO completeness: 0x8CD5 = GL_FRAMEBUFFER_COMPLETE.
     * When incomplete the game silently skips offscreen rendering and the render-target texture stays all 0 --
     * exactly the "cutscene logic runs but the screen is all white" shape. */
    if (!strcmp(e->name, "glCheckFramebufferStatus")) {
        static int n;
        if (n++ < 12) {
            fprintf(stderr, "[fbo] glCheckFramebufferStatus -> %08x  %s\n",
                    c->eax, c->eax == 0x8CD5u ? "COMPLETE" : "★ incomplete");
            fflush(NULL);
        }
    }
    return 1;
}

int host_gl_bridge_count(void) { return gl_bridge_count(); }

/* Per-frame counters (read and reset by SwapBuffers in host_gl_audio.c) */
unsigned g_frame_draws, g_frame_clears, g_frame_texbinds;

unsigned g_tex_hist[TEX_HIST_N];
unsigned long g_tex_hist_total;

void host_gl_tex_hist_report(void)
{
    if (!g_tex_hist_total) return;
    fprintf(stderr, "[texhist] total binds %lu:", g_tex_hist_total);
    int shown = 0;
    for (int i = 0; i < TEX_HIST_N - 1; i++) {
        if (!g_tex_hist[i]) continue;
        fprintf(stderr, " id%d=%u", i, g_tex_hist[i]);
        if (++shown >= 12) { fprintf(stderr, " ..."); break; }
    }
    if (g_tex_hist[TEX_HIST_N - 1])
        fprintf(stderr, " id>=%d=%u", TEX_HIST_N - 1, g_tex_hist[TEX_HIST_N - 1]);
    fprintf(stderr, "\n");
    fflush(NULL);
    memset(g_tex_hist, 0, sizeof g_tex_hist);
    g_tex_hist_total = 0;
}

/* ============================================================================
 * Frame capture: read the current frame out of the pbuffer and save it as PPM
 *
 * The EGL surface is a pbuffer (offscreen) now, so nothing is displayed -- but that also means we can
 * glReadPixels exactly "what the game rendered". That beats any log: one glance tells
 * a black screen from a flat colour from a real title screen.
 *
 * Caller: SwapBuffers in host_gl_audio.c (which has the frame counter).
 * Switch: FRAME_DUMP_EVERY=N  -> save one frame each at N, 2N, 3N... (at most 5)
 *       FRAME_DUMP_AT=N     -> save one frame only at frame N
 * Output: ./frames/frame_<N>.ppm (P6, uncompressed; convert to png with ffmpeg/ImageMagick)
 * ==========================================================================*/
void glFinish(void);
void glReadPixels(int x, int y, int width, int height,
                  unsigned int format, unsigned int type, void *pixels);

int host_gl_dump_frame(const char *path)
{
    if (!g_gl_ready) return -1;
    int w = g_fb_w, h = g_fb_h;
    if (w <= 0 || h <= 0) return -1;

    /* ★★ 2026-11-11: **capture only on the default framebuffer**.
     *
     * Measured evidence: the capture was plain white (255,255,255) while the game glClearColor is
     * (0,0,0,1) black -- so glReadPixels is not reading the screen.
     *
     * The cause is that glReadPixels reads the content of the **currently bound** framebuffer.
     * The game switches between fb=1 (offscreen RT, 1024x1024) and fb=0 (screen, 960x540)
     * several times per frame; if the capture moment lands on the RT, reading 960x540 gets
     * a region of the RT (uninitialised white), not the screen.
     *
     * Fix: use **our own shadow state** g_fbst.cur_fb to decide the current binding,
     * skip this frame when it is not 0 and wait for the next SwapBuffers.
     * ★ Pure read of one unsigned: no GL queries, no state changes. */
    if (g_fbst.cur_fb != 0u) {
        fprintf(stderr, "[frame] skipped (current fb=%u, not the screen)\n",
                g_fbst.cur_fb);
        fflush(NULL);
        return -2;
    }

    size_t stride = (size_t)w * 3u;
    unsigned char *buf = (unsigned char *)malloc(stride * (size_t)h);
    if (!buf) return -1;

    glFinish();                     /* gl4es batches lazily, so flush it first */
    glReadPixels(0, 0, w, h, 0x1907 /*GL_RGB*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* OpenGL origin is bottom-left, PPM is top-left -> write the rows in reverse */
    for (int y = h - 1; y >= 0; y--)
        fwrite(buf + (size_t)y * stride, 1, stride, f);
    fclose(f);
    free(buf);
    fprintf(stderr, "[frame] saved %s (%dx%d)\n", path, w, h);
    fflush(NULL);
    return 0;
}

/* ==========================================================================
 * Frame streaming (2026-11-04)
 *
 * Background: the APK MainActivity starts a **separate process** with ProcessBuilder to run this ELF.
 * Android Surface / ANativeWindow can only be used in the process that holds it
 * (ANativeWindow_fromSurface needs a JNIEnv), so the child process cannot get it.
 * So the picture can only be "pushed": each frame RGBA goes to Java over TCP:7138,
 * and Java puts it on screen with a SurfaceView.
 *
 * Why RGBA instead of RGB: Android Bitmap.Config.ARGB_8888 in memory
 * is byte-ordered R,G,B,A (little-endian 0xAABBGGRR), byte-for-byte identical
 * to glReadPixels(GL_RGBA) output -- Java can feed it straight to Bitmap.copyPixelsFromBuffer
 * without writing a single pixel-conversion line. RGB would make Java build alpha itself: slow and error-prone.
 *
 * Protocol (fixed-size header + raw pixels):
 *      u32 magic 'IFRM' (0x4D524649)
 *      u32 width
 *      u32 height
 *      u32 bytes  (= width*height*4)
 *      [bytes] RGBA, top to bottom row by row (already flipped)
 *
 * The flip happens **here**: OpenGL origin is bottom-left, Bitmap is top-left. Flipping natively is far
 * faster than flipping in Java, and Java can keep a pure copyPixelsFromBuffer.
 * ========================================================================== */
#define STREAM_PORT 7138
/* ★ 2026-11-05: volatile is required.
 * These variables are read and written across threads (the listener writes g_stream_fd, the render thread reads it);
 * without volatile the compiler hoists the g_stream_fd < 0 test out of the loop or caches it as a constant,
 * which shows up as "Java is connected but native never sends a frame" -- exactly the on-device symptom. */
static volatile int g_stream_fd = -1;
static volatile int g_stream_every = 0;   /* 0 = off; N = push one frame every N frames */
static volatile unsigned g_stream_tick;
static volatile unsigned g_stream_sent;
static volatile unsigned g_stream_dropped;
static unsigned char *g_stream_buf;
static size_t g_stream_cap;
static unsigned char *g_stream_pack;
static size_t g_stream_pack_cap;

static void *gl_stream_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        fprintf(stderr, "[stream] ✗ socket() failed errno=%d (%s)\n",
                errno, strerror(errno));
        fflush(NULL);
        return NULL;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(STREAM_PORT);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) {
        /* ★ the most common cause: Android gave no INTERNET permission and SELinux refuses outright. */
        fprintf(stderr, "[stream] ✗ bind(%d) failed errno=%d (%s)\n",
                STREAM_PORT, errno, strerror(errno));
        fflush(NULL);
        close(ls);
        return NULL;
    }
    if (listen(ls, 1) != 0) {
        fprintf(stderr, "[stream] ✗ listen failed errno=%d (%s)\n",
                errno, strerror(errno));
        fflush(NULL);
        close(ls);
        return NULL;
    }
    fprintf(stderr, "[stream] ✓ listening on 127.0.0.1:%d\n", STREAM_PORT);
    fflush(NULL);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { usleep(200000); continue; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fprintf(stderr, "[stream] ✓ client connected fd=%d\n", fd);
        fflush(NULL);
        g_stream_fd = fd;

        /* Keep the connection: poll to see whether the peer closed.
         * Note that a blocking recv must not hold this thread -- then, when the child process exits, the thread
         * sits in recv and accept no longer answers reconnects. */
        unsigned ticks = 0;
        for (;;) {
            char c;
            ssize_t n = recv(fd, &c, 1, MSG_DONTWAIT);
            if (n == 0) break;                       /* peer closed */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (g_stream_fd != fd) break;            /* the render thread already gave up */
            usleep(150000);
            /* Report a heartbeat about every 10 seconds.
             * 0 frames means the game has not entered its render loop yet (most likely loading archives);
             * a rising frame count means streaming is fine. The user can tell "hung" from "busy" at a glance. */
            if (++ticks % 66u == 0u) {
                /* Also report memory: when Android OOM-kills the child process it leaves no
                 * output at all (SIGKILL), so we can only infer from "how far RSS had risen".
                 * Read /proc/self/status directly, one line every 10 seconds. */
                char mem[160] = "";
                FILE *f = fopen("/proc/self/status", "r");
                if (f) {
                    char ln[256];
                    while (fgets(ln, sizeof ln, f)) {
                        if (!strncmp(ln, "VmRSS:", 6) || !strncmp(ln, "VmSize:", 7)
                            || !strncmp(ln, "VmPeak:", 7)) {
                            size_t L = strlen(ln);
                            while (L && (ln[L-1] == '\n' || ln[L-1] == ' ')) ln[--L] = 0;
                            const char *q = ln;
                            while (*q == ' ' || *q == '\t') q++;
                            strncat(mem, q, sizeof mem - strlen(mem) - 2);
                            strncat(mem, " ", sizeof mem - strlen(mem) - 2);
                        }
                    }
                    fclose(f);
                }
                fprintf(stderr, "[stream] heartbeat: sent %u frames, %u render callbacks | %s\n",
                        g_stream_sent, g_stream_tick, mem);
                fflush(NULL);
            }
        }
        fprintf(stderr, "[stream] client disconnected (%u frames sent total)\n", g_stream_sent);
        fflush(NULL);
        if (g_stream_fd == fd) g_stream_fd = -1;
        close(fd);
    }
    return NULL;
}

void host_gl_stream_init(void)
{
    static int inited;
    if (inited) return;
    inited = 1;
    const char *e = getenv("FRAME_STREAM");
    if (!e) return;
    g_stream_every = atoi(e);
    if (g_stream_every <= 0) { g_stream_every = 0; return; }
    pthread_t t;
    if (pthread_create(&t, NULL, gl_stream_thread, NULL) == 0) pthread_detach(t);
    fprintf(stderr, "[stream] frame push thread created (one every %d frames), waiting for bind result\n",
            g_stream_every);
    fflush(NULL);
}

/* Non-blocking send: dropping frames beats blocking the render thread.
 *
 * ★ 2026-11-05: on device the frame rate fell from 54.6 to 14.6 fps, longest frame 7055 ms.
 * The cause was this function using a blocking send -- Java does
 * copyPixelsFromBuffer + drawBitmap to a Canvas per frame, far slower than native,
 * so once the socket buffer fills, send blocks and the whole render thread stops.
 *
 * Fix: MSG_DONTWAIT plus giving up on a partial send. On-device streaming only needs to be watchable,
 * and dropping a few frames beats dragging 54 fps down to 14 fps.
 *
 * Returns 0 = whole frame sent; 1 = only part was sent (the caller must reset the connection);
 * -1 = complete failure. */
static int send_all_nonblock(int fd, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t sent = 0;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Buffer full: this frame was not fully sent. Returning 1 means "partial send",
                 * and the caller drops the rest and reconnects (the stream is already misaligned). */
                return 1;
            }
            return -1;
        }
        if (w == 0) return -1;
        b += w; n -= (size_t)w; sent += (size_t)w;
    }
    return 0;
}

void host_gl_stream_frame(void)
{
    /* Diagnostic: print all three preconditions on the first few calls.
     * "connected but no frames" has only three causes: not called, condition unmet, send failure. */
    static int dbg;
    if (dbg < 6) {
        dbg++;
        fprintf(stderr, "[stream] probe: every=%d fd=%d ready=%d tick=%u\n",
                g_stream_every, g_stream_fd, g_gl_ready, g_stream_tick);
        fflush(NULL);
    }
    if (!g_stream_every || g_stream_fd < 0 || !g_gl_ready) return;
    if (++g_stream_tick % (unsigned)g_stream_every) return;

    int w = g_fb_w, h = g_fb_h;
    if (w <= 0 || h <= 0) return;
    /* ★ 2026-11-06: switch to pushing RGB565 (2 bytes per pixel, half the bandwidth).
     *
     * It used to push RGBA8888 = 960*540*4 = 2.07 MB per frame. On device Java had to do
     * copyPixelsFromBuffer + drawBitmap per frame: a 2MB full-screen copy plus a Surface lock,
     * so a fast stream blocked the render thread -> the only option was FRAME_STREAM=8 to slow it down,
     * leaving **the picture at only 7 fps** (the game itself runs at 55.9 fps).
     *
     * RGB565 = 1.04 MB per frame, and Java Bitmap.Config.RGB_565
     * copyPixelsFromBuffer is a plain memcpy, twice as fast as ARGB_8888.
     * The game is pixel art anyway, so the 565 colour loss is barely visible. */
    size_t src_need = (size_t)w * (size_t)h * 3u;   /* read back as GL_RGB */
    size_t need     = (size_t)w * (size_t)h * 2u;   /* converted to 565 for sending */

    /* ★ Both the fd and the two top-level buffer allocations must happen before reading pixels.
     * Otherwise, with no client, we would still waste a glReadPixels + 565 conversion
     * (about 2-3 ms at 960x540), and writing g_stream_pack before it is allocated would crash. */
    int fd = g_stream_fd;
    if (fd < 0) return;
    if (g_stream_cap < src_need) {
        unsigned char *nb = (unsigned char *)realloc(g_stream_buf, src_need);
        if (!nb) return;
        g_stream_buf = nb; g_stream_cap = src_need;
    }
    if (g_stream_pack_cap < need) {
        unsigned char *nb = (unsigned char *)realloc(g_stream_pack, need);
        if (!nb) return;
        g_stream_pack = nb; g_stream_pack_cap = need;
    }

    glFinish();
    glReadPixels(0, 0, w, h, 0x1907 /*GL_RGB*/, 0x1401, g_stream_buf);

    /* RGB -> RGB565, flipping vertically at the same time (GL origin bottom-left, screen top-left).
     *
     * 565 bit layout (little-endian 16-bit): RRRRRGGG GGGBBBBB
     *   r5 = R>>3, g6 = G>>2, b5 = B>>3
     *   v  = (r5<<11) | (g6<<5) | b5
     * Writing uint16_t directly gives, in little-endian, exactly the order Android wants. */
    {
        uint16_t *dst = (uint16_t *)g_stream_pack;
        const unsigned char *src = g_stream_buf;
        for (int y = 0; y < h; y++) {
            const unsigned char *row = src + (size_t)(h - 1 - y) * (size_t)w * 3u;
            uint16_t *orow = dst + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                unsigned r = row[x * 3 + 0], g = row[x * 3 + 1], b = row[x * 3 + 2];
                orow[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }

    /* magic is now 'IFR5', so Java picks Bitmap.Config.RGB_565. */
    uint32_t hdr[4] = { 0x35524649u, (uint32_t)w, (uint32_t)h, (uint32_t)need };
    int rh = send_all_nonblock(fd, hdr, sizeof hdr);
    if (rh < 0) {
        fprintf(stderr, "[stream] ✗ send failed errno=%d (%s); disconnecting\n",
                errno, strerror(errno));
        fflush(NULL);
        g_stream_fd = -1;
        return;
    }
    if (rh > 0) { g_stream_dropped++; return; }   /* the header alone was incomplete; drop the frame */

    int rp = send_all_nonblock(fd, g_stream_pack, need);
    if (rp < 0) {
        fprintf(stderr, "[stream] ✗ send failed errno=%d (%s); disconnecting\n",
                errno, strerror(errno));
        fflush(NULL);
        g_stream_fd = -1;
        return;
    }
    if (rp > 0) {
        /* Only part of the pixels went out -- the stream is misaligned, so disconnect and let Java reconnect,
         * otherwise every following frame is parsed as garbage. */
        g_stream_dropped++;
        fprintf(stderr, "[stream] dropping frames due to congestion (%u total); disconnecting to reconnect\n", g_stream_dropped);
        fflush(NULL);
        g_stream_fd = -1;
        return;
    }
    unsigned n_sent = ++g_stream_sent;
    if (n_sent == 1 || (n_sent % 60u) == 0u) {
        fprintf(stderr, "[stream] sent %u frames (%dx%d)\n", n_sent, w, h);
        fflush(NULL);
    }
}

int host_gl_fb_width(void)  { return g_fb_w; }
int host_gl_fb_height(void) { return g_fb_h; }


/* ==========================================================================
 * Heap scan probe (2026-11-06)
 *
 * Known chain of facts:
 *   1. fread read 1.1GB, but "PNG signature blocks = 0" -> the archive holds compressed data
 *   2. the glTexImage2D pixel buffer is all zero, and under HEAP_POISON it is not 0xCD
 *      -> something zeroed it and never filled the content back in
 *
 * Not yet settled: **is the decompressed output really PNG**. Search the guest heap for signatures:
 *   - 89 50 4E 47 found -> the data is there and the problem is libpng decoding
 *   - not found        -> the decompressed output is not PNG at all (or decompression failed)
 *
 * /proc/self/maps bounds the scan so unmapped pages are never touched. Runs once only.
 * ========================================================================== */
void png_scan_heap_once_pub(void);
static void png_scan_heap_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    /* Off by default: scanning 400MB of heap takes 2 seconds and causes a visible hitch on device.
     * Set PNG_SCAN=1 when needed. */
    if (!getenv("PNG_SCAN")) return;

    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[512];
    unsigned long n_png = 0, n_idat = 0, n_xml = 0, scanned = 0, n_region = 0;
    while (fgets(line, sizeof line, m)) {
        unsigned long lo, hi;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        if (perm[0] != 'r') continue;
        if (n_region < 24)
            fprintf(stderr, "[pngscan]   map %08lx-%08lx %s %s\n",
                    lo, hi, perm, line + 73 > line ? "" : "");
        if (lo < 0x10000000UL || lo >= 0xC0000000UL) continue;
        if (hi > 0xB0000000UL) hi = 0xB0000000UL;
        if (hi <= lo) continue;
        {
            const unsigned char *p = (const unsigned char *)(uintptr_t)lo;
            size_t n = (size_t)(hi - lo);
            for (size_t i = 0; i + 8 < n; i += 4) {
                if (p[i] == 0x89 && p[i+1] == 'P' && p[i+2] == 'N' && p[i+3] == 'G') {
                    if (n_png < 5)
                        fprintf(stderr, "[pngscan] PNG signature @%08lx\n", lo + (unsigned long)i);
                    n_png++;
                    i += 4096;
                } else if (p[i] == '<' && p[i+1] == '?' && p[i+2] == 'x' && p[i+3] == 'm') {
                    if (n_xml < 5)
                        fprintf(stderr, "[pngscan] XML header @%08lx\n", lo + (unsigned long)i);
                    n_xml++;
                    i += 4096;
                } else if (p[i] == 'I' && p[i+1] == 'D' && p[i+2] == 'A' && p[i+3] == 'T') {
                    n_idat++;
                    i += 4096;
                }
            }
        }
        scanned += (unsigned long)(hi - lo);
        n_region++;
    }
    fclose(m);
    fprintf(stderr, "[pngscan] scanned %.1f MB of heap: PNG=%lu IDAT=%lu XML headers=%lu\n",
            (double)scanned / 1048576.0, n_png, n_idat, n_xml);
    fflush(NULL);
}

void png_scan_heap_once_pub(void) { png_scan_heap_once(); }

/* ==========================================================================
 * ★★ 2026-11-12: export textures as PPM (for on-device forensics)
 *
 * Purpose: HUD numbers all render as the same mu glyph; we suspect the font atlas (the .fnt _0.png)
 * is uploaded with wrong content. Hex in the log does not reveal shapes, so the texture must be written as an image.
 *
 * Export only "font-atlas-like" sizes (squares/wide rectangles of 128..512), at most 16 textures,
 * written straight as PPM (P6) to <game>/frames/tex_<id>.ppm and converted to PNG locally for viewing.
 * Strictly read-only: reads guest memory only, calls no GL.
 * ========================================================================== */
void host_tex_dump_ppm(uint32_t id, uint32_t w, uint32_t h,
                       uint32_t fmt, const unsigned char *px)
{
    static unsigned n;
    if (n >= 32u) return;
    if (!px || w < 16u || h < 16u) return;
    /* ★ Export only "font/UI atlas" sizes and exclude large background images:
     *   128x128(16384)  256x256(65536)  512x128(65536)  256x128(32768)
     *   512x256(131072) is kept too (pftempestasevencondensed is that size)
     * 512x512 and larger pages are excluded (they would use up the quota). */
    if ((uint64_t)w * h > 131072ull) return;

    const char *dir = "/storage/emulated/0/Android/media/com.isaacport"
                      ".launcher/files/game/frames";
    char path[256];
    snprintf(path, sizeof path, "%s/tex_%u.ppm", dir, id);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    /* the guest holds RGBA8888, PPM needs RGB. fmt 0x1908 = RGBA */
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * (size_t)w * 4u;
        for (uint32_t x = 0; x < w; x++) {
            fputc(row[x * 4 + 0], f);
            fputc(row[x * 4 + 1], f);
            fputc(row[x * 4 + 2], f);
        }
    }
    fclose(f);
    n++;
    fprintf(stderr, "[texppm] id=%u %ux%u fmt=0x%x -> %s\n", id, w, h, fmt, path);
    fflush(NULL);
}




/* ★★ 2026-11-12: SetTexelData caller attribution (local diagnostics).
 * 6 call sites: 0x5a153b=ImagePng, 0x5a0b1d, 0x5a1a18, 0x5a1e9e,
 *             0x5b606e=solid fill, 0x5b61bb
 * The font atlas is all zero; we must know which path it takes. */
void host_settexel_probe(uint32_t callret, uint32_t img)
{
    static const uint32_t sites[6] = { 0x5a153bU, 0x5a0b1dU, 0x5a1a18U,
                                       0x5a1e9eU, 0x5b606eU, 0x5b61bbU };
    static const char *names[6] = { "ImagePng", "5a0b1d", "5a1a18",
                                    "5a1e9e", "solid fill", "5b61bb" };
    uint32_t w = img >= 0x10000u ? ld32((uint32_t)(img + 0x80U)) : 0u;
    uint32_t h = img >= 0x10000u ? ld32((uint32_t)(img + 0x84U)) : 0u;
    /* Only UI/font sizes matter; this avoids flooding the log */
    if (!((uint64_t)w * h <= 131072ull && w >= 16u && h >= 16u)) return;
    int k = -1;
    for (int i = 0; i < 6; i++) if (sites[i] == callret) { k = i; break; }
    fprintf(stderr, "[settexel] %ux%u from %s (ret=%08x)\n",
            w, h, k >= 0 ? names[k] : "unknown", callret);
    fflush(NULL);
}
