/* host_openal.c —— 把 OpenAL32.dll 的导入转发给真正的 OpenAL Soft
 *
 * ==========================================================================
 * 一、为什么要有这个文件
 *
 * 原来的 OpenAL 层是**纯记账桩**：alSourcePlay 只把状态标志改成
 * AL_PLAYING，alBufferData 只累加字节数、不保存数据，整个文件里没有
 * 任何音频输出后端。所以这个移植版**从来没有真正发出过声音**。
 *
 * 用户提供的 TBOI-Afterbirth-Plus apkvision APK 里带了
 * libopenalisaac.so = **OpenAL Soft 1.23.1**（OpenSL ES 后端）。
 * 实测在这台设备上可以正常打开设备、建上下文、播放 buffer：
 *     alcOpenDevice -> 0xb4000070a52e7010
 *     alBufferData 完成, alGetError=0
 *     alSourcePlay 已调用, alGetError=0     ← 真的出声
 *
 * 所以不再自己写音频后端，直接转发给 OpenAL Soft。
 *
 * ==========================================================================
 * 二、两个必须处理的技术问题
 *
 * 1. **指针宽度**。ALCdevice* / ALCcontext* 是 64 位指针
 *    （0xb4000070…），而 guest 只有 32 位寄存器。
 *    做法：句柄表。真指针存表，返回一个小 token（0x00D0xxxx）。
 *    guest 传回来时查表还原。
 *
 * 2. **浮点参数**。alSource3f / alSourcef / alListener3f / alListenerfv /
 *    alGetSourcef 的参数是 float，x86 调用方按 32 位压栈，
 *    我们读出来是 uint32 位模式，必须按位重新解释成 float，
 *    不能做数值转换。
 *
 * 3. **访客指针**。guest 内存是恒等映射到宿主低 4GB 的，
 *    所以 (void*)(uintptr_t)guest_ptr 直接可用（ALuint* / ALfloat*）。
 *
 * ==========================================================================
 * 三、失败时的行为
 *
 * 如果 dlopen 或 alcOpenDevice 失败，所有转发函数返回 0，
 * host_gl_audio.c 的旧桩照常工作（游戏能继续跑，只是没声音）。
 * 也就是说这一层是**纯增益**，不会引入新的崩溃点。
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "guest.h"

/* ---- stdcall/cdecl 小工具（与 host_gl_audio.c 一致）---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cret(CPU *__restrict c) { (void)gpop(c); }

/* 把栈上的 32 位位模式当 float 解释 */
static float Af(CPU *__restrict c, int i)
{
    uint32_t v = A(c, i);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}
/* 把 float 写成 guest 侧 32 位位模式 */
static uint32_t fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof v);
    return v;
}

/* ==========================================================================
 * 真 OpenAL 的函数指针
 * ========================================================================== */
typedef void ALCdevice;
typedef void ALCcontext;

static void *g_ol;
static int   g_ol_state;         /* 0 未尝试  1 可用  -1 不可用 */
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

/* ---- 句柄表：真 64 位指针 ↔ 32 位 token ---- */
#define OAL_MAX_HANDLES 16
static struct { uint32_t token; void *ptr; } g_h[OAL_MAX_HANDLES];
static uint32_t g_h_next = 0x00D00001u;

/* 统计（用来客观证明音频数据在流动，不靠"听"） */
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
 * 初始化
 * ========================================================================== */
static int oal_init(void)
{
    const char *cands[4];
    int i;

    if (g_ol_state) return g_ol_state > 0;

    /* 查找顺序：
     *   1) 环境变量（调试用）
     *   2) 默认库名 —— Android 链接器会在 app 的 nativeLibraryDir 里找
     *   3) 我们自己 .so 所在目录的同名文件（Java 把它拷到 files/lib/）
     */
    cands[0] = getenv("ISAAC_OPENAL");
    cands[1] = "libopenal.so";
    cands[2] = NULL;              /* 下面用 dladdr 补同目录路径 */
    cands[3] = NULL;

    /* 同目录兜底：如果 Java 把 libopenal.so 和 libisaac.so 都拷到了
     * files/lib/，用我们自己的路径推出它。dladdr 对已加载的 .so
     * 能拿到 dli_fname。 */
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

/* 打开设备 + 建上下文（幂等）。给 alcOpenDevice 用。 */
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
 * 转发分发
 *
 * 返回 1 = 已处理（含"真 OpenAL 不可用，但这个名字归我管"的情况？
 * 不 —— 返回 0 让旧桩处理，这样失败时行为完全退回原来）。
 * ========================================================================== */

/* 名字 → 转发 */
int host_openal_try(CPU *__restrict c, const char *name)
{
    const char *n;
    if (!c || !name) return 0;
    n = strchr(name, '!');
    n = n ? n + 1 : name;

    /* alc* 三个必须由我们管句柄，永远走这个分支（哪怕初始化失败也要
     * 返回 0 让旧桩答"成功"以免游戏卡在初始化）。 */
    if (strcmp(n, "alcOpenDevice") == 0) {
        uint32_t dummy = A(c, 0);           /* 设备名字符串，忽略 */
        (void)dummy;
        if (!oal_ensure_context()) return 0;
        /* 上下文已建好，设备句柄就是它所属的那个；用一个固定 token 表示 */
        c->eax = 0x00D00001u;
        cret(c);
        return 1;
    }
    if (strcmp(n, "alcCreateContext") == 0) {
        uint32_t dev = A(c, 0);
        if (!oal_ensure_context() || dev != 0x00D00001u) return 0;
        c->eax = 0x00D00002u;               /* 上下文 token */
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

    /* 其余 al* 都要求上下文已就绪 */
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

    return 0;   /* 不认识的交给旧桩 */
}
