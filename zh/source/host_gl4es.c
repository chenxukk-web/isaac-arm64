/* host_gl4es.c —— OpenGL 1.x 后端（gl4es over EGL + GLES2）
 *
 * ==========================================================================
 * 一、为什么需要这一层
 *
 * 《以撒的结合：忏悔》用的是 OpenGL 2.1 的**固定管线**：glBegin/glEnd、
 * glMatrixMode/glLoadIdentity、显示列表、GL_QUADS。GLES 只有可编程管线，
 * 两者之间不是「函数名不同」的差距，而是「没有这个东西」的差距。
 *
 * gl4es 就是专门解决这个问题的：它把固定管线的调用翻译成 GLES2 的
 * shader + VBO 调用。本项目不再自己发明这一层。
 *
 * ==========================================================================
 * 二、桥的两端
 *
 * guest 侧看到的必须是 32 位地址（x86 指针是 uint32_t），而 gl4es 是
 * arm64 代码，函数指针 64 位。所以中间必须有 token：
 *
 *   guest: call <token 0x7D......>
 *            ↓ guest_call → host_gl_dynamic
 *   宿主:  按 token 查表 → 读 x86 栈上的参数 → 按 arm64 ABI 调用 gl4es
 *
 * 难点是**参数类型**：x86 上所有参数都在栈上，arm64 上前 8 个整数进
 * x0-x7、前 8 个浮点进 v0-v7。把 glVertex3f(1.0f,2.0f,3.0f) 的 float
 * 位模式当整数塞进 x0，gl4es 收到的是 0x3F800000 而不是 1.0f。
 *
 * 所以每个 GL 函数都要有一条「参数种类」签名：i = 整数/指针，f = float。
 * 分发时按签名选对应的静态 wrapper（C 不能动态调用任意签名）。
 *
 * ==========================================================================
 * 三、初始化顺序（顺序错了 hardext 探测会拿到空函数指针）
 *
 *   1. dlopen libEGL.so / libGLESv2.so
 *   2. EGL: getDisplay → initialize → chooseConfig → createPbufferSurface
 *      → createContext → makeCurrent
 *   3. set_getprocaddress(我们的 GLES 名字解析器)
 *      set_getmainfbsize(返回 pbuffer 尺寸)
 *   4. initialize_gl4es()   ← 到这里 GLES 函数才会被真正调用，上下文必须已在
 *
 * 1-2 已在 work/tools/egltest2.c 里独立验证过（Adreno 830, GLES 3.2）。
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

static void png_scan_heap_once(void);   /* 诊断探针，定义在文件尾 */

#include "guest.h"

/* 每帧绘制计数（定义在文件末尾，这里前置声明） */
extern unsigned g_frame_draws, g_frame_clears, g_frame_texbinds;

/* glBindTexture 的纹理 id 直方图（0..62 单独计数，63 是"更大"的桶）。
 * SwapBuffers 在 FRAME_TRACE 下调用 host_gl_tex_hist_report() 打印并清零。 */
#define TEX_HIST_N 64
extern unsigned g_tex_hist[TEX_HIST_N];
extern unsigned g_cur_tex_id;
unsigned g_cur_tex_id;   /* 最近一次 glBindTexture 的 id */
static unsigned g_cur_active_unit;   /* 当前 glActiveTexture 选中的单元 */

/* ★★ 2026-11-10：记录最近一次 TexCoordPointer / VertexPointer 的数组。
 *
 * 为什么不能在每次指针设置时打印：在那个时刻还不知道要画哪张纹理，
 * 也不知道 UV 到底合不合理。只有在 **draw 时**把 [UV, texId, 尺寸]
 * 放在一起看，才能判断是“UV 落到了图集的错误区域”。
 *
 * 这是本轮最关键的一项证据：
 *   · UV 全在 0..1 且 texture 是 1024x1024 → UV 正常，问题在纹理内容
 *   · UV 是像素坐标（0..1024）但纹理按归一化采样 → 差一个 /尺寸
 *   · UV 恒定不变 → 游戏没更新数组，或我们读到了错误的指针
 */
typedef struct {
    uint32_t ptr;
    int      size;      /* 每顶点分量数 */
    int      stride;    /* 字节 */
    int      valid;     /* 1 = 可读 float 数组 */
    float    v[8];      /* 前 2 组顶点的原始值 */
} ptrprobe_t;
static ptrprobe_t g_uv_probe, g_vtx_probe;

/* ★★ 2026-11-12：每个属性槽的最后一次 glVertexAttribPointer 设置。
 *
 * HUD 文字/图标全部渲染成同一个 µ 字形，怀疑 UV 数组在 draw 时指向了
 * 错误位置（或 stride 算错导致所有顶点读到同一个 UV）。
 * draw 时刻把这个表打出来，就能看出 UV 到底是什么。 */
typedef struct {
    uint32_t ptr, type, stride;
    int      size, norm, seen;
} attrslot_t;
static attrslot_t g_attr[16];

/* ★★ 2026-11-12：draw 归属统计。
 *
 * 主菜单在帧 360-600 是整片空白，但 [fbset] 显示大量纹理仍在画。
 * 必须分清「画到离屏 RT」和「画到屏幕」各多少次 ——
 * 如果某段时间完全没往屏幕画，空白就是"没合成"而不是"没渲染"。 */
unsigned long g_draws_fb0, g_draws_fb1;   /* 屏幕 / 离屏 RT */

/* ★ 2026-11-10：glGetAttribLocation 的待打印状态。
 * 返回值在函数调用后才写回 eax，所以分两步：
 * 分支里存名字，末尾打印名字+返回值。 */
static char     g_gal_name[48];
static int      g_gal_pending;

/* ★★ 2026-11-10：uniform 探针。
 *
 * 日志证明游戏只用三个 uniform 入口：
 *   glGetUniformLocation / glUniformMatrix4fv / glUniform1i
 * 没有 glUniform1f。所以 Exposure/Gamma 是**顶点属性**（值都是 1.0，正常）。
 *
 * 那 glUniform1i 就非常可疑：它几乎总是用来设 **sampler2D 的纹理单元**。
 * 如果 gl4es 返回的 location 与实际 GLES program 的不一致，
 * 或者 sampler 被绑到了空的纹理单元，shader 就会采样到
 * 错误的（或未初始化的）纹理 —— 正好表现为“纯色块/颜色错乱”。 */
static char     g_uni_name[48];
static int      g_uni_pending;
static int      g_uni_last_loc = -9999;
/* ★★ 2026-11-10：当前 glUseProgram 的 program。
 *
 * 上一版 uniform-loc 探针**按名字去重**，于是 "Texture0" 只记下了
 * 第一个 program 的查询结果。而第一个 program 是纯色 shader
 * （gl_FragColor = Color0，本来就没有 Texture0），返回 -1 完全正常。
 * 结果：真正的贴图 program 里 Texture0 是否可用、Texture1（调色板）
 * 是否被查询，全被这个去重掩盖了 —— 「只有 Texture0、从未查询
 * Texture1」这个结论是探针缺陷造成的假象，不能作为排除依据。
 *
 * 改成按 (program, name) 去重，才看得到每个 program 的真实 uniform 表。 */
static uint32_t g_cur_prog;

/* ★ 2026-11-10：shader/program 参数查询待打印状态。
 * glGetShaderiv / glGetProgramiv 的结果写进 *params，
 * 只能等调用后读，所以分支里先存参数。 */
static uint32_t g_iv_pname;
static uint32_t g_iv_ptr;
static int      g_iv_isprog;
static int      g_iv_pending;
#define TEX_INFO_N 2048
unsigned g_tex_w[TEX_INFO_N], g_tex_h[TEX_INFO_N];
unsigned char g_tex_seen[TEX_INFO_N];

/* ★★ 2026-11-10：每张纹理的**最终**内容状态。
 *
 * 原来的 texstat 按**调用次数**统计全零，而同一 id 可能先传空占位、
 * 再传真实数据 —— 于是 20% 的“全零”里包含大量中间状态，
 * 完全无法判断画面上到底有几张是坏的。
 *
 * 这里按 id 记最后一次结果：0=未知 1=有内容 2=全零。
 * 在 SwapBuffers 里定期汇总，才是真正能对照画面的数字。 */
unsigned char g_tex_zero[TEX_INFO_N];

/* ★★ 2026-11-10：保存每张纹理的采样颜色，供 draw 时对照。
 *
 * 为什么必须存：真机上游戏**只用 glDrawElements + VBO**（日志已确认
 * glTexCoordPointer 调用 0 次），UV 在 VBO 里拿不到指针 ——
 * UV 探针完全失效。
 *
 * 换成能直接回答问题的探针：在 draw 那一刻看**当前绑定的纹理
 * 到底长什么样**。这样能把“画玩家精灵用的纹理”和“它的实际颜色”
 * 对上，立刻区分：
 *   · 内容对（有皮肤色/衣服色） → 问题在 UV 或混合
 *   · 内容不对（纯红/纯白/全零） → 问题在解码或上传
 */
unsigned char g_tex_rgb[TEX_INFO_N][3];

/* ★★ 2026-11-10：纹理数据的通道分布分类。
 *
 * 为什么要这个：
 *   真机画面整体**褪色发灰发白**，而背景正常。手动看 6 张纹理
 *   发现 HUD 那批（id=259/263/287/311/317/345，全 32x32）的
 *   采样均值是 R=35 G=0 B=0 —— G/B 是**精确的 0**。
 *   真实图像即使是红色主调，G/B 也会有抗锯齿/阴影带来的非零值。
 *   精确 0 意味着**下半个字节根本没写入**，最可能是格式声明错了：
 *   游戏按单通道（LUMINANCE / ALPHA）准备数据，却按 RGBA 上传，
 *   或反之。
 *
 *   6 张样本不够下结论，所以这里对**全量已知纹理**分类统计：
 *     0 = 未分类   1 = 彩色（三通道都有分布）
 *     2 = 只有 R   3 = 只有 A（灰度/笔画纹理，正常现象）
 *     4 = 纯灰度（R≈G≈B 且都非 0）
 * 只有这个比例才能说明“数据层是不是真的坏了”。 */
unsigned char g_tex_class[TEX_INFO_N];

/* ★ 2026-11-12：每张纹理上传时的调用者与全零判定（诊断） */
unsigned long  g_upl_caller_ret[TEX_INFO_N];
unsigned char  g_upl_caller_zero[TEX_INFO_N];
uint32_t       g_upl_caller_id[TEX_INFO_N];
extern unsigned long g_tex_hist_total;
void host_gl_tex_hist_report(void);

/* ★★ 2026-11-10（方案 B）：每张纹理**上传时的参数**，供 draw 时补传。
 *
 * 证据链（见 HANDOFF_20261110.md）：
 *   · 行 511  id=3  512x512 ptr=5a31f010 → 上传时全零
 *   · 行 1121 id=49 512x512 ptr=5a31f010 → 同一指针此时有内容 07 06 06 09
 *   · glTexSubImage2D 调用 = 0 次
 *   ⇒ 游戏是在像素数据**还没写好**的时候就调了 glTexImage2D。
 *     缓冲区后来才被填充，但纹理已经传完、再没人补传。
 *
 * 方案 B 的做法：把参数留一份，在 draw 那一刻检查
 *   · g_tex_zero[id] == 2（确认为全零）
 *   · 而且 ptr 指向的缓冲区**现在已经非零**
 * 满足就按原参数补传一次。不改 gl4es、不动游戏代码，
 * 只把“时序错位”这次失败的上传重放一遍。
 *
 * 字段全部按上传时的原始值保存（含 target / level / border / fmt / type），
 * 补传是一次**等价重放**，不引入任何新的格式假设。 */
typedef struct {
    uint32_t ptr;        /* guest 像素缓冲地址 */
    unsigned w, h;
    int32_t  level, ifmt, border;
    uint32_t fmt, type, target;
    unsigned char unit;  /* 上传时 active 的纹理单元（g_cur_active_unit） */
    unsigned char valid; /* 1 = 参数齐全，可用于补传 */
} texsrc_t;
static texsrc_t g_tex_src[TEX_INFO_N];

/* 每帧每张纹理最多探测一次（draw 可能每秒几千次，
 * 而零纹理有上百张 —— 不节流的话光采样就吃掉几毫秒）。
 * 存 frame+1，0 表示“本帧还没探过”。 */
static unsigned g_tex_repair_frame[TEX_INFO_N];

/* 补传统计：成功重放的张数 / 探到但数据仍未就绪的探测次数。
 * host_gl_audio.c 的 [texfinal] 会读这两个数。 */
unsigned g_tex_repairs, g_tex_repair_wait;

/* ==========================================================================
 * ★★ 2026-11-10（真机实测后修正）：补传**默认关闭**。
 *
 * 实测结论（这一版已经跑过，不要重复试）：
 *   补传正确触发了 10 张，但画面从“HUD 缺失”变成**整个房间彩色错乱**。
 *
 * 原因在数据源，不在时序：
 *   ptr=60950010  被 9 个 id 共用（11/13/15/17/33/35/37 …）
 *   ptr=86b2c010  被 3 个 id 共用（1015/1036/1038）
 *   ptr=86fd9010  被 2 个 id 共用（999/1001）
 * 它们在 glTexImage2D 那一刻全部是零，之后这块**共享暂存地**只留下
 * 最后一个写入者的数据。把这份内容重放给同一 ptr 上的每个 id
 * = 把最后一张图复制给其余所有图 → 串图。
 *
 * 所以 HANDOFF 里那条“决定性证据”的解读必须作废：
 *   “同一指针两次上传，先零后非零”并不证明“这张图晚一会就好了”，
 *   只证明“这块共用暂存地后来被写过”。尺寸相同 + 同 ptr
 *   是池复用的指纹，不是时序指纹。
 *
 * 开关：<game>/repair 空文件 或 TEX_REPAIR=1 → 重新开启，
 * 便于在独占 ptr 的少数纹理上做对照实验。与项目里 <game>/exec、
 * <game>/fast 同一约定，touch 一个文件即可切。
 * ========================================================================== */
static int g_tex_repair_on = -1;   /* -1 = 未探测 */

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
            /* ★ 2026-11-10：不能用 access()。Android 的 /storage/emulated
             * 走 FUSE，access(F_OK) 常常返回 EACCES —— 即使 fopen 完全能打开。
             * 实测 <game>/frame_dump 就是这么失效的（文件在、SO 里有代码、
             * 但开关永不生效）。改用 fopen 探测，和游戏自己写日志同一个路径。 */
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

/* gl4es 的初始化入口（libGL.a 的导出符号） */
void set_getprocaddress(void *(*fp)(const char *));
void set_getmainfbsize(void (*fp)(int *, int *));
void initialize_gl4es(void);
void *gl4es_GetProcAddress(const char *name);

/* x86 参数读取 --------------------------------------------------------- */
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
/* stdcall：弹返回地址 + 清 n 字节参数 */
static inline void gret(CPU *c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ======================================================================
 * EGL 上下文
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
/* ★ 2026-11-06 新增：进程内直连 Surface 需要的常量 */
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
static int          g_gl_ready;      /* 0 未尝试  1 就绪  -1 失败 */

/* ★ 2026-11-06：进程内窗口模式（.so 化）
 *
 * JNI 层从 SurfaceView 的 Surface 拿到 ANativeWindow，在 dlopen 之后、
 * 游戏 main 之前调用 host_gl4es_set_window()。有 window 就用
 * eglCreateWindowSurface + eglSwapBuffers 直连上屏，**零拷贝、零推流**。
 *
 * 没有 window（Termux 里直接跑、或 Java 侧走子进程 fallback）时
 * 仍用 pbuffer + glReadPixels 推流，行为和以前完全一致。
 *
 * 这也是 iOS 移植版的做法：GL 上下文直接绑在 UIView 的 CALayer 上。 */
static void        *g_native_window;
static void        *p_ANativeWindow_release;
static void        *p_ANativeWindow_setBuffersGeometry;
static int          g_window_mode;   /* 1 = 用 window surface，0 = pbuffer */

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

/* 让 JNI 层在 init 之前告诉 GL 层真实尺寸。
 * 必须在 host_gl4es_init 之前调用 —— init 只会跑一次（幂等）。 */
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

/* 探针：判断一页是否真映射（mincore 只看映射，不触发缺页） */
int probe_readable(unsigned long addr);
int probe_readable(unsigned long addr)
{
    unsigned char vec = 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    unsigned long page = addr & ~((unsigned long)pg - 1);
    return mincore((void *)page, (size_t)pg, &vec) == 0;
}

/* gl4es 用这个解析 GLES 函数名 */
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
    if (g_gl_ready) return g_gl_ready > 0 ? 0 : -1;   /* 幂等 */
    /* ★ 传 0 = 不要覆盖（窗口模式下尺寸已由 host_gl4es_set_fb_size 设好） */
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

    /* 必须用 EGL_DEFAULT_DISPLAY：EGL_PLATFORM_SURFACELESS_MESA 在本机不可用
     * （egltest2.c 实测返回 0）。 */
    g_dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g_dpy) return gl_fail("eglGetDisplay");

    EGLint_t maj = 0, min = 0;
    if (!p_eglInitialize(g_dpy, &maj, &min)) return gl_fail("eglInitialize");
    fprintf(stderr, "[gl4es] EGL %d.%d\n", maj, min);

    /* 窗口模式要 WINDOW_BIT，pbuffer 模式要 PBUFFER_BIT。两个都请求
     * 也合法（Android 的 config 通常两种都支持），但只请求需要的那一种
     * 更容易在奇怪设备上拿到有效 config。 */
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

    /* 到这里 GLES 上下文已在。现在才允许 gl4es 去探测扩展。 */
    set_getprocaddress(gl4es_gles_proc);
    set_getmainfbsize(gl4es_fbsize);
    initialize_gl4es();

    /* 验证：gl4es 自己的 glGetString 必须能报出 2.x */
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

/* ★★ 2026-11-10：屏幕像素探测。
 *
 * 用户报“切换房间时是红色”。这是唯一能**直接看到** GPU 侧结果的
 * 手段：在 swap 之前用 GLES 的 glReadPixels 读中心像素。
 *
 * 为什么必须读，而不是推：
 *   · 我们的纹理统计读的是 CPU 内存，FBO 纹理在 CPU 侧永远读零
 *   · “红色”可能是 FBO 未初始化、通道错位、或清屏色错
 *   · 只有真读屏幕才能分清是哪种
 *
 * 只在启动后前 60 帧采样（读像素会强制 GPU 同步，有开销）。
 * 同时统计全屏颜色分布：如果 R 恒高 G/B 恒 0，就是通道错。 */
static int g_cur_tex_id_pub;   /* 仅供外部查询（未使用，占位） */

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
        /* ★★ 2026-11-10：改成**持续**采样。
         *
         * 旧代码 `probed < 60` 只采前 60 帧 = 启动后 1 秒，
         * 那时游戏还在加载，画面就是纯白 —— 所以日志里
         * 全是 (255,255,255,255)，毫无信息量。
         *
         * 现在：每 120 帧（2 秒）采一次，持续整个会话。
         * glReadPixels 会强制 GPU 同步，但 2 秒一次的开销可忽略。 */
        if (p_glReadPixels) {
            static unsigned last_sample;
            extern unsigned host_gl_swap_count(void);
            unsigned fc = host_gl_swap_count();
            if (fc - last_sample < 120u) goto do_swap;
            last_sample = fc;
            /* 采样 5 个点：中心 + 四角内缩，避免只看一个像素被巧合骗过。 */
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

            /* 只在下列情况打印，避免刷屏：
             *   前 5 帧（启动画面）
             *   检测到“偏红”（R 明显高于 G/B）
             *   每 15 帧一次心跳 */
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
 * 四、guest token 桥
 *
 * token 空间 0x7D000000，和 kernel32 动态导出的 0x7E000000 分开。
 *
 * 签名表由 work/tools/gen_gl_bridge.py 从 gl4es 的头文件生成
 * （gl4es_bridge_gen.c）。不手写：手写必漏项，漏项的后遗症是运行时
 * 返回 0、guest 存下这个 0 再 call 0 —— 报出来是「indirect call to
 * address 0」，离真正原因很远。
 * ====================================================================== */
#define GL_TOKEN_BASE 0x7D000000u
#define GL_MAX_ENTS   1024u

/* wglGetProcAddress 自己也要能被 guest 调用，用 token 空间末尾的哨兵值 */
#define WGL_GETPROC_TOKEN 0x7DFFFF00u

/* ---- 生成表的接口（gl4es_bridge_gen.c） ---- */
typedef void (*glbridge_fn)(CPU *, void *);
glbridge_fn gl_bridge_wrapper(int idx, int want_u32ret);
typedef struct { const char *name; short sig; char ret; } gl_bridge_decl;
const gl_bridge_decl *gl_bridge_lookup(const char *name);
int gl_bridge_count(void);

typedef struct {
    const char *name;
    void       *fn;
    int         sig;
    char        ret;      /* v=void  i=标量  s=字符串  p=其他指针 */
    uint32_t    token;
} glent;

static glent    g_gl_ents[GL_MAX_ENTS];
static unsigned g_gl_ent_n;
static char     g_gl_missing[128][48];
static unsigned g_gl_missing_n;

/* glGetString 等返回的字符串在 gl4es 的高地址，guest 的 uint32_t 读不到，
 * 必须拷进低 4GB 再返回。 */
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

/* 诊断：记录每一个被请求的 GL/WGL 名字及其结果（去重） */
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

    /* 游戏会通过 GetProcAddress 拿 wglGetProcAddress 自己的地址 */
    if (!strcmp(name, "wglGetProcAddress")) return WGL_GETPROC_TOKEN;

    {   /* 记录 + 走原逻辑 */
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
    e->name  = d->name;          /* 指向生成表里的静态字符串，不会失效 */
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
 * ★★ 2026-11-10（方案 B）：draw 时补传全零纹理
 *
 * 为什么必须在 draw 时做、而不是在上传时重试：
 *   上传发生时数据确实是零，立即重试还是零；只有“后来某一时刻”
 *   缓冲区才被填好。draw 是“马上要用这张图”的时刻 —— 也是最后一个
 *   安全的补传点（再晚就画完了）。
 *
 * 为什么不能在 eglSwapBuffers 里统一做：那时的绑定状态已经变了，
 *   而 gl4es 的 glTexImage2D 作用于**当前绑定**的纹理，得额外
 *   记住并恢复绑定。draw 时当前绑定的就是我们想修的那张，零多余状态操作。
 * ========================================================================== */
typedef void (*glteximage2d_fn)(uint32_t target, int32_t level,
                                int32_t internalformat, int32_t width,
                                int32_t height, int32_t border,
                                uint32_t format, uint32_t type,
                                const void *pixels);

/* 64 点均匀采样，返回有非零字节的点数（0 = 全零）。
 * 与 glTexImage2D 分支里的统计保持同一判据：按 4 字节/像素、
 * 采样 64 个点 —— 两者结论必须能直接对照。 */
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

/* 通道分类（与上传路径里的规则**完全一致**，抽出来是为了不让两处
 * 判据分叉：[texchan] 的统计必须同时覆盖“原生上传”和“补传”的纹理）。
 *   nz==0        → 0 全零
 *   只有 R       → 2
 *   只有 A       → 3（阴影/描边/字体的正常形态）
 *   R≈G≈B       → 4 纯灰度
 *   否则         → 1 彩色
 * 参数是 64 点采样的**累加和**，内部再除 64。 */
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

/* 重新采样一张纹理并刷新全部派生状态（zero/rgb/class）。
 * 补传之后调用 —— 否则 [texfinal]/[texchan] 会一直沿用
 * “全零”的旧结论，日志和画面完全对不上。 */
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

/* 拿到 gl4es 的 glTexImage2D。优先用已经被游戏请求过的入口（那份 fn
 * 已经被验证过可用），否则问 gl4es 要一个。 */
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

/* 若当前绑定的这张纹理确认是全零、而它的像素缓冲区现在已经有内容，
 * 就按上传时的原参数补传一次。返回 1 表示真的重传了。
 *
 * 关键安全约束：
 *   · 只补 g_tex_zero==2 的（NULL 指针占位 / FBO 附件不动）
 *   · 只补“缓冲区现在非零”的（否则白花一次上传）
 *   · 只补与当前 active unit 一致的（否则会拿 unit1 的参数去传 unit0）
 *   · 每帧每张只探一次
 *   · 重传参数完全照搬原调用，格式/类型零猜测
 *
 * 默认关闭，原因见 g_tex_repair_on 上方的实测结论。
 */
static int tex_repair_if_blank(unsigned id, unsigned frame)
{
    if (!host_tex_repair_enabled()) return 0;
    if (id == 0u || id >= TEX_INFO_N) return 0;
    if (g_tex_zero[id] != 2) return 0;              /* 只修“确认为全零” */
    texsrc_t *s = &g_tex_src[id];
    if (!s->valid) return 0;                        /* 没留下可用参数 */
    if (g_tex_repair_frame[id] == frame + 1u) return 0;   /* 本帧已探过 */
    g_tex_repair_frame[id] = frame + 1u;
    if (s->unit != (unsigned char)g_cur_active_unit) return 0;
    if (s->target != 0x0DE1u) return 0;              /* 只补 GL_TEXTURE_2D */
    uint32_t p = s->ptr;
    if (p < 0x10000u || s->w == 0 || s->h == 0) return 0;

    const unsigned char *q = (const unsigned char *)(uintptr_t)p;
    if (tex_nz64(q, s->w, s->h) == 0) {             /* 数据仍未就绪 */
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
 * ★★ 2026-11-10：深度状态**只读**探针
 *
 * 为什么要有这个：真机日志显示 `glEnable(DEPTH_TEST)` 开着、
 * `glDepthFunc = GL_GREATER(0x204)`、3D 顶点 `z = -1.000`。
 * Isaac 是 2D 游戏靠 z 排图层，「怪物/HUD/子弹整类缺失」正是
 * 深度把后画精灵全挡掉的形态。
 *
 * ★★ 铁律：本探针**只读 guest 栈参数**（gs32），绝不调用任何 GL 函数、
 * 绝不调用 gl4es_GetProcAddress、绝不改任何 GL 状态。
 * 上一轮就是因为探针里调了 glGetProgramiv/glGetActiveUniform，
 * 在加载期把 gl4es 状态搅坏，表现为 glDeleteProgram 后 std::terminate。
 *
 * 输出策略：只在**状态变化**时打一行，真机一轮只有几十行。
 * ========================================================================== */
static struct {
    int      depth_test;   /* -1 未知 / 0 关 / 1 开 */
    int      depth_func;   /* 最近一次 glDepthFunc 参数 */
    int      depth_mask;   /* 最近一次 glDepthMask 参数 */
    unsigned clear_mask;   /* 最近一次 glClear 的 mask */
    double   clear_depth;  /* 最近一次 glClearDepth 的实参 */
    int      have_clear_depth;
    unsigned changed;      /* 变化计数 */
    unsigned draws_at_state;  /* 处于当前状态时发生过的 draw 次数 */
} g_depthst = { -1, -1, -1, 0, 0.0, 0, 0 };

/* ★★ 2026-11-10：framebuffer 跟踪（**纯只读**，不调用任何 GL 函数）。
 *
 * 目的：主界面只在 texId=1(8x8) 和 texId=987(1024x1024) 之间交替。
 * 需要判定 987 到底是
 *   A. 离屏 render target —— 先渲染进去，再贴到屏幕
 *   B. 真实背景纹理
 * 这决定「987 全零」是不是正常的（RT 内容由 GPU 填，CPU 读必零）。
 *
 * 做法：只记录 glBindFramebuffer 的参数，以及每次 draw 落在哪个 FB 上。
 * 一个 GL 函数都不调，零副作用。 */
static struct {
    uint32_t cur_fb;        /* 当前绑定的 framebuffer（0 = 默认/屏幕） */
    uint32_t fb_draws;      /* 落在当前 FB 上的 draw 次数 */
    uint32_t last_tex;      /* 最近一次 draw 用的纹理 */
    uint32_t switches;      /* 切换次数 */
    uint32_t nseen;         /* [fbset] 已记录组合数（满 96 后停止扫描） */
    uint32_t vp_x, vp_y, vp_w, vp_h;   /* 最近一次 glViewport */
    uint32_t vp_seen;
} g_fbst = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

/* ★★ 2026-11-11：FBO 附件映射表（**纯只读**）。
 *
 * 主界面用 tex=987(1024x1024) 贴屏，而它一直被报「全零」。
 * 需要确认 987 到底是：
 *   A. FBO 的颜色附件（离屏 render target）—— CPU 侧读必零，正常
 *   B. 真的背景纹理 —— 全零就是坏图
 *
 * 只读 glFramebufferTexture2D 的参数就能分清：
 *   glFramebufferTexture2D(target, attachment, textarget, texture, level)
 *                                                        ^^^^^^^ gs32(c,3)
 *
 * 不调用任何 GL 函数，不改任何状态。 */
static struct {
    uint32_t tex[32];       /* 被挂成 FBO 附件的纹理 id */
    uint32_t att[32];
    unsigned n;
} g_fbo_att;

static void depthst_report(const char *why)
{
    /* DEPTH_TRACE=1 时全量输出；否则只报前 40 条变化。 */
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
 * ★★ 2026-11-11：最近 GL 调用环形缓冲（**纯记录**，为崩溃取证）
 *
 * 为什么需要：真机在启动加载期崩（帧 85 左右），
 *   [glreq] glDeleteProgram -> token ...
 *   [host] std::terminate
 * 但 `[glreq]` 那条是 GetProcAddress 的日志，而且**没有 fflush**，
 * abort() 之前缓冲顺序不可靠 —— 不能据此断定崩溃就在 glDeleteProgram。
 *
 * 这里只做一件事：把每次真正分发到的 GL 入口名字（静态字符串指针，
 * 不拷贝、不分配）写进环形缓冲。真崩了，terminate 桩把最后 64 条
 * 打出来，就能看到游戏在崩之前到底在调什么。
 *
 * ★ 零副作用：一次指针写 + 一次自增。不调任何 GL、不改任何状态。
 * ========================================================================== */
#define GL_RING_N 64
const char *g_gl_ring[GL_RING_N];
unsigned    g_gl_ring_n;
void host_gl_dump_ring(const char *why);

static inline void gl_ring_note(const char *name)
{
    g_gl_ring[g_gl_ring_n & (GL_RING_N - 1u)] = name;
    g_gl_ring_n++;
    /* 自检钩子：GL_RING_AT=N 时在第 N 次 GL 调用处 dump 一次。
     * 用于**本地**验证 dump 逻辑本身对（不必等真崩）。
     * 只读一个缓存的 long + 一次比较，热路径开销可忽略。 */
    {
        static long at = -2;
        if (at == -2) {
            const char *e = getenv("GL_RING_AT");
            at = e ? atol(e) : -1;
        }
        if (at > 0 && g_gl_ring_n == (unsigned long)at)
            host_gl_dump_ring("GL_RING_AT self-test");
        /* 自测：验证 terminate 取证路径（含全栈扫描 / UAF 回放）不会崩。 */
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

/* 供 host_crt.c 的 terminate 桩调用：把最后 N 条 GL 调用打到 stderr。 */
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

/* guest_call 的动态分发入口。1 = 已处理。 */
int host_gl_dynamic(CPU *__restrict c, uint32_t addr)
{
    /* wglGetProcAddress 特判：它自己也是一个可被 guest call 的入口 */
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

    /* ★ 纯记录：见 g_gl_ring 的注释。开销是一次指针写。 */
    gl_ring_note(e->name);

    /* 每帧绘制调用统计：SwapBuffers 里读走并归零。
     * 用来判定"游戏到底在画东西，还是只清了个屏"。 */
    if (e->name[0] == 'g' && e->name[1] == 'l') {
        if (!strncmp(e->name, "glDraw", 6)) {
            g_frame_draws++;
            /* ★ 只读计数：当前深度状态下发生了多少次 draw。计数本身无副作用。 */
            g_depthst.draws_at_state++;
            /* ★★ UI/字体尺寸纹理的绘制：dump 当时的属性表与 UV 值。
             * HUD 全部渲染成 µ，要么纹理错、要么 UV 错，这里一次看清。 */
            {
                unsigned tid = g_cur_tex_id;
                unsigned tw = tid < TEX_INFO_N ? g_tex_w[tid] : 0u;
                unsigned th = tid < TEX_INFO_N ? g_tex_h[tid] : 0u;
                static unsigned ui_shown;
                /* ★ 2026-11-12：排除 8x8/1x1 这类占位白纹理（它们会占满名额），
                 * 只在帧号 > 200 之后采样，确保抓到的是真正的主菜单/HUD 绘制。 */
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
                        /* size==2 且 float 的，按 stride 读前 4 个顶点的 UV */
                        if (g_attr[ai].size == 2 && g_attr[ai].type == 0x1406u) {
                            const char *base =
                                (const char *)(uintptr_t)g_attr[ai].ptr;
                            int st = (int)g_attr[ai].stride;
                            if (st <= 0) st = 8;
                            /* ★★ 校验：stride 必须是合理的小值，且
                             * ptr + 4 个顶点跨度必须落在合法 guest 地址内。
                             * 没有这两条会拿垃圾 stride 去读无映射页 ——
                             * 本地第一次加这个探针就 SEGV 在 0xbe3bf。 */
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
            /* 前 60 次 draw 标注它落在哪个 framebuffer 上 —— 回答
             * 「主界面是在往 987 里画，还是在把 987 贴屏幕」。
             * 帧号 > 200 才开始（主界面在此之前）。 */
            /* ★ 按 (framebuffer, 纹理) 组合去重统计：
             * 只看「往 RT 里画用了哪些纹理」和「贴屏幕用了哪些纹理」，
             * 这直接回答「主界面的 spritesheet 到底有没有被画」。 */
            if (g_fbst.nseen < 96u) {   /* ★ 满了就整个跳过，别在热路径上白扫 */
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
            /* ★★ 2026-11-10（方案 B）：画之前先把“当时数据还没就绪”的纹理补传。
             *
             * 放在这里而不是 glTexImage2D 之后立即重试：上传发生时
             * 缓冲区确实是零，当时重试必然还是零；只有 draw 这一刻
             * 才可能已经填好，而 draw 也正是“马上就要用到”的最后时机。
             *
             * 此刻当前绑定的就是这次 draw 要采样的纹理，所以补传
             * 不需要改绑定 / 切换单元 —— 零状态扰动。
             * 每帧每张只探一次，探不到（仍全零）就等下一帧。
             */
            {
                extern unsigned host_gl_swap_count(void);
                tex_repair_if_blank(g_cur_tex_id, host_gl_swap_count());
            }
            /* 前 6 次绘制打印实参：count/type 能立刻区分
             * "画了一个全屏 quad" vs "画了几万个三角形"。 */
            /* 打印 draw 时**当前绑定的纹理 id** —— 判断"画的是空白纹理"
             * 最直接的一步。前 40 次全打，之后每 200 次打一条。 */
            static unsigned shown;
            shown++;
            /* ★★ 2026-11-10：只在**游戏真正开始渲染之后**采样。
             *
             * 旧条件 `shown <= 40` 全落在启动阶段（loading 画面），
             * 那 40 条 draw 全是白纹理 + FBO 占满屏，
             * 对诊断“游戏内 HUD/精灵为何缺失”零价值。
             *
             * 现在：帧号 > 500 才采样（大约启动完成、进房间后）。
             * 前 60 条录完启动后的真实绘制，之后每 2000 条一条心跳。 */
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
            /* ★★ 2026-11-10：draw 时刻的纹理内容快照。
             *
             * 原本想探 UV，但真机日志证明游戏**只用 glDrawElements + VBO**
             * （glTexCoordPointer 调用 0 次），拿不到 UV 指针。
             *
             * 改探这个：画这张纹理时，它**实际长什么样**。
             * 这一行把 [draw 序号, 纹理id, 尺寸, 采样RGB, 内容状态]
             * 全部放在一起，直接回答“画玩家精灵的那张纹理对不对”。
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
            /* ★ 只读：清屏掩码。没有 GL_DEPTH_BUFFER_BIT(0x100) 时，
             * 深度会跨帧残留，上一帧的深度挡住这一帧的精灵。 */
            unsigned mask = (unsigned)gs32(c, 0);
            if (mask != g_depthst.clear_mask) {
                g_depthst.clear_mask = mask;
                g_depthst.changed++;
                depthst_report("glClear mask changed");
            }
        }
        /* ================================================================
         * ★★ 2026-11-10：顶点/纹理坐标探针。
         *
         * 真机现象（录屏确认）：房间背景正常，但**所有精灵都错**。
         *   · HUD 红心 → 纯红色矩形
         *   · 属性图标 → 纯白色矩形
         *   · 怪物/子弹 → 直接缺失
         *   · 切房间 → 红色块
         * 而这些图都是图集（atlas）里的小区域，用 UV 选取。
         * 采样到“纯色块”正是 **UV 落到了图集的空白/错误区域** 的典型形态。
         *
         * 这里直接读 guest 内存里的数组指针，看 UV 值是否合理：
         *   · 正常：0..1（归一化）或 0..纹理宽高（像素坐标）
         *   · 异常：极大值、NaN、或恒定 0
         *
         * glTexCoordPointer(size, type, stride, ptr) —— 4 个参数
         * glVertexPointer  (size, type, stride, ptr) —— 同上
         * ================================================================ */
        else if (!strcmp(e->name, "glTexCoordPointer") ||
                 !strcmp(e->name, "glVertexPointer")) {
            /* ★ 2026-11-10：只**记录**，不在此时打印。
             * 理由见 ptrprobe_t 的注释：此刻还不知道要画哪张纹理。 */
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
         * ★★ 2026-11-10：shader 顶点属性探针。
         *
         * 真机日志证明：游戏用**自定义 GLSL shader**
         *   glCreateProgram / glGetAttribLocation / glVertexAttribPointer
         * 而**不用 glTexCoordPointer**（调用 0 次）。
         *
         * 也就是说 UV 走的是 shader 属性数组。如果某个属性的
         * index/size/stride/ptr 错了，shader 就会用错误的 UV 采样，
         * 结果是**整块采到纹理的同一点** → 纯色矩形。
         * 这与录屏完全一致（红心/图标变成纯色块）。
         *
         * glVertexAttribPointer(index, size, type, normalized, stride, ptr)
         *   —— 6 个参数（cdecl）
         * ================================================================ */
        else if (!strncmp(e->name, "glVertexAttribPointer", 21)) {
            /* ★ 持久化记录（不受打印上限影响） */
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
                /* ★ 2026-11-10：按 **stride** 读顶点，不能按 size 连续读。
                 * 之前没考虑 stride（这个 shader 是 28 字节 = 7 floats），
                 * 于是“第二个顶点”读到的其实是相邻属性的值，
                 * 打出来的是虚假数据（例如 Gamma=426.667 其实是 Position.x）。
                 * stride=0 在 GL 里表示“紧密排列”，此时才等于 size*sizeof(type)。 */
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
            /* ★ 2026-11-10：返回值在 c->eax，但此时**函数还没被调用**
             * （真正的调用在本函数末尾的 w(c, e->fn)）。
             * 所以这里只把属性名存起来，等调用再打印。 */
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
            /* 同 glGetAttribLocation：返回值要等调用后才可读，
             * 这里只存名字。 */
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
            /* ★ 关键：location + 值。sampler2D 的纹理单元就在这里设。 */
            /* ★ 2026-11-10：按 (loc,val) 去重，而不是只打前 20 条。 */
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
            /* ★★ 2026-11-10：shader 编译/链接状态。
             *
             * 之前完全不检查 —— 如果 shader 编译或链接失败，
             * gl4es 会回退到默认行为，而游戏拿到的状态码如果是
             * “失败”，它可能自己换路径；如果是假的“成功”，
             * 就会用一个有缺陷的 program 渲染 —— 正好是
             * “精灵纯色/UV 采样错”的形态。
             *
             * 返回值写进 *params（第 3 个参数指向的 int），
             * 所以在调用**之后**读。这里先记下来。 */
            g_iv_pname = gs32(c, 1);
            g_iv_ptr   = gs32(c, 2);
            g_iv_isprog = (e->name[9] == 'P');
            g_iv_pending = 1;
        }
        else if (!strncmp(e->name, "glActiveTexture", 15)) {
            /* ★ 2026-11-10：记录当前活跃纹理单元。
             * GL_TEXTURE0=0x84C0，所以 unit = 枚举值 - 0x84C0。 */
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
            /* ★★ 2026-11-10：混合模式。
             *
             * 真机现象：精灵变成**纯色矩形**，而不是带透明的图案。
             * 除了 UV/纹理问题，还有一个常见原因：混合因子错了。
             * 典型两种：
             *   · 预乘 alpha 数据用非预乘公式 → 边缘发黑/发亮
             *   · 源因子错成 ONE → 透明区域也写颜色 → 整块不透明
             * 这里把实际参数打出来，对照 GL 常量即可判断。 */
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
            /* 只关心 GL_BLEND(0xBE2) / GL_DEPTH_TEST(0xB71) / GL_ALPHA_TEST */
            uint32_t cap = gs32(c, 0);
            /* ★ 只读：跟踪深度测试开关，不调用任何 GL 函数。 */
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
        /* ★★ 只读深度状态探针：只读 gs32(c,0)，不调用任何 GL 函数。
         * 注意：glEnable 的处理必须并进**已有**那条分支（它在前面先命中），
         * 单独再开一条 else-if 永远走不到 —— 这个坑本轮已经踩过一次。 */
        else if (!strcmp(e->name, "glDepthFunc")) {
            int f = (int)gs32(c, 0);
            if (f != g_depthst.depth_func) {
                g_depthst.depth_func = f;
                depthst_report("glDepthFunc");
            }
        }
        else if (!strcmp(e->name, "glClearDepth") ||
                 !strcmp(e->name, "glClearDepthf")) {
            /* ★ 只读：深度清除值。x86 上 glClearDepth 是 double（8 字节），
             * glClearDepthf 是 float（4 字节）。桥修好后两者都会被正确传递，
             * 这里只是为了把实参记下来做证据。 */
            double d = !strcmp(e->name, "glClearDepth")
                       ? gsd(c, 0) : (double)gsf(c, 0);
            g_depthst.clear_depth = d;
            g_depthst.have_clear_depth = 1;
            depthst_report("glClearDepth");
        }
        else if (!strcmp(e->name, "glFramebufferTexture2D")) {
            /* ★ 纯只读：哪个纹理被挂成 FBO 附件。
             * 参数：target, attachment, textarget, texture, level */
            uint32_t tex = (uint32_t)gs32(c, 3);
            uint32_t att = (uint32_t)gs32(c, 1);
            /* ★ 注意：这里**绝不能 return**。这是分发函数内部的分支，
             * return 会让后面的 gl_bridge_wrapper 不执行 ——
             * 等于把 glFramebufferTexture2D 吞掉，游戏永远拿不到 FBO。
             * 用 if 包住即可。 */
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
            /* ★ 纯只读：视口。主界面 RT 是 1024x1024，但逻辑画面是 960x540，
             * 视口错了内容就会画到角落或被裁掉。 */
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
            /* ★ 纯只读：只记下新的绑定，不调用任何 GL 函数。 */
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
            /* ★★★ 只读：alpha 测试的 (func, ref)。
             *
             * 这是本轮最关键的实参。shader 里只有
             *   #ifdef GL_ES
             *       if (gl_FragColor.a == 0.0) discard;
             *   #endif
             * 也就是**只在 GLSL ES 下**才自动丢弃全透明像素。
             * 非 ES 编译路径下，透明像素靠 glAlphaFunc 判定。
             *
             * 桥修好之前 glAlphaFunc 是 sig='ii'，第二个参数 ref
             * （GLclampf，浮点）被当整数塞进 x1，gl4es 从 s0 读 → 垃圾。
             * ref 偏大 → 所有像素被 alpha test 丢弃 → **精灵/怪物/HUD 整类消失**
             * ref 偏小 → 透明像素也通过 → **红心变成纯红矩形**
             * 两种现象都与录屏完全吻合。 */
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
            /* ★ 只读：深度范围。这是判定「深度是否合理」的第二个关键量。
             * Windows 的 glDepthRange 是 2×double，GLES 的 glDepthRangef 是 2×float。
             * 桥修好之前这里收的是垃圾（'ii'），gl4es 的 glDepthRangef 会把
             * 垃圾 clamp 成 [0,1] 里某个值 —— 深度范围错位同样会让
             * GL_GREATER 把整类片段挡掉。 */
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
            /* ★★ 2026-11-10：记录"这个纹理 id 绑到哪个单元"。
             *
             * 调色板着色器需要两个 sampler：
             *     vec4 PaletteIndex = texture2D(Texture0, TexCoord0);
             *     vec4 OutputColor  = texture2D(Texture1, PaletteIndex.xy) * Color0;
             * 如果两个 sampler 都指向 unit 0，调色板查表就会
             * 去采样**索引图自己** → 颜色完全乱 → 正是“贴图错乱”。
             *
             * 所以必须知道：游戏到底有没有把调色板绑到 unit 1。 */
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
            /* 直方图：哪张纹理被绑得最多。
             * 纯白屏的典型形态是"所有 draw 都在绑同一张 8x8 白纹理"，
             * 一眼就能看出来 —— 比逐个打印 bind 有效得多。 */
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
         * 纹理上传探针（2026-11-04）
         *
         * 之前用 "GetProcAddress(glTexImage2D) 只出现一次" 推断
         * "从没真正上传过纹理" —— 这个推断是错的：游戏拿到函数指针后
         * 会缓存起来直接 call，GetProcAddress 本来就只该出现一次。
         * 要判定到底上传没上传，必须在分发点上数真实调用。
         *
         * glTexImage2D(GLenum target, GLint level, GLint internalformat,
         *              GLsizei w, GLsizei h, GLint border,
         *              GLenum format, GLenum type, const void *pixels)
         * 9 个 cdecl 参数，gs32(c,0..8)。
         * ================================================================ */
        else if (!strcmp(e->name, "glTexImage2D")) {
            static unsigned long n_teximg, n_empty;
            uint32_t px = gs32(c, 8);
            n_teximg++;
            /* ★★ 2026-11-10（方案 B）：记下这次上传的**全部参数**。
             *
             * 因为存在“上传时数据未就绪”的时序错位（证据见 HANDOFF_20261110.md：
             * 同一指针 ptr=5a31f010 先传全零、后一次传有内容），
             * 所以要在 draw 时能把这笔上传原样重放一遍。
             * 重放需要完整的一组参数，少一个都不行：
             *   target/level/ifmt/w/h/border/fmt/type/px
             *
             * 只记 level==0 且 px 非空的情况：
             *   · level>0 是 mipmap，重放 level 0 与它无关
             *   · px==NULL 是 FBO/占位分配，重放 NULL 只会把图清空
             */
            {
                uint32_t sid = g_cur_tex_id;
                uint32_t lvl = gs32(c, 1);
                if (sid > 0u && sid < TEX_INFO_N && px >= 0x10000u && lvl == 0u
                        && gs32(c, 0) == 0x0DE1u   /* 只认 GL_TEXTURE_2D */
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
                    /* 新一次上传 → 本帧允许重新探测（内容可能已变） */
                    g_tex_repair_frame[sid] = 0u;
                }
            }
            /* 记录"当前绑定的纹理 id → 尺寸"，这样 draw 时打印的 texId
             * 能直接对应到尺寸，判断是不是那张 8x8 白图。 */
            /* 打印 guest 返回地址，定位"谁创建了这个纹理槽" */
            /* ★ 2026-11-06 启动提速：原来对每张 id>=900 的纹理都打一行，
             * 加载后期有几百张大纹理 → 上千行 fprintf+fflush，
             * 而且每行都要走一次 Java → 外部存储的写入。只留前几张。 */
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
                    /* ★ 2026-11-06 启动提速：下面那段"首像素采样"对每张
                     * id>=900 的纹理做 64 次跨页随机读（1024x1024 = 4MB，
                     * 每次都是 cache miss），加载几百张大图就是几万次
                     * miss + 上千行日志。只在最前面几张和显式开启
                     * TEX_TRACE 时才做。 */
                    static int tex_trace = -1;
                    if (tex_trace < 0) tex_trace = getenv("TEX_TRACE") != NULL;
                    if (n_teximg <= 24 || (id >= 900 && tex_trace)) {
                        fprintf(stderr, "[teximg] id=%u %ux%u",
                                id, gs32(c,3), gs32(c,4));
                        /* id>=900 是后期创建的大纹理 —— 把首像素和
                         * 一个随机中点像素打出来，判断是不是纯白/纯空。 */
                        if (id >= 900 && px) {
                            uint32_t w = gs32(c,3), hh = gs32(c,4);
                            unsigned long sum = 0; int nz = 0;
                            /* 粗采样 64 点，统计非零与非 0xFF 的比例 */
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
            /* ★★ 2026-11-10：原始像素快照导出。
             *
             * 纯文本日志只能给均值，看不出像素格式细节。
             * 这里把前 12 张较典型纹理的前 1KB 原始字节写成十六进制，
             * 拿到本地可以精确判断：
             *   · RGBA 四通道是否真的是 R,G,B,A 交错
             *   · 是不是 BGRA（B/G 位置互换）
             *   · 是不是单通道 LUMINANCE（每 4 字节只有 1 个有值）
             *   · 是不是预乘 alpha
             * 只写前 12 张，总量 < 24KB 文本。 */
            if (px && px >= 0x10000u) {
                /* ★★ 2026-11-10 bug 修复：两个分支必须各用各的计数器。
                 *
                 * 原来 texdump（<=64x64 全量导出）和 texraw（>64x64 只导 64 字节）
                 * 共用 txdumped，结果：
                 *   id=1  8x8    → texdump，txdumped=1
                 *   id=3  512x512 → texraw， txdumped=2
                 *   id=5  512x512 → texraw， txdumped=3
                 *   id=259 32x32  → txdumped=3，texdump 条件失败！
                 * 于是真正想看的 32x32 HUD 纹理一张都没导出，
                 * 日志里永远只有 id=1 那张 8x8 全白图。 */
                static unsigned txdumped;    /* texdump 专用 */
                static unsigned txrawed;     /* texraw 专用 */
                /* ★★ 2026-11-10：把**完整**小纹理导出（不只 64 字节）。
                 *
                 * 真机现象：HUD 红心/属性图标变成纯色矩形，而对照图里
                 * 它们是有明暗的图案。texchan 显示那批 32x32 纹理
                 * （id=259/263/287/311/317/345）“只有 R 通道”。
                 *
                 * 64 字节看不出结论（不够一个 32x32 图的一行）。
                 * 这里把 32x32 及更小的纹理**全量**导出为十六进制，
                 * 拿到本地可以直接重建成 PNG 看是什么。
                 *
                 * 限制：只导 <=64x64 且只导前 3 张，总量可控。 */
                uint32_t dw = (uint32_t)gs32(c, 3), dh = (uint32_t)gs32(c, 4);
                uint64_t nbytes = (uint64_t)dw * dh * 4ull;
                /* ★ 2026-11-10：上限 3 → 6，并优先抓 32x32（HUD 图标那批）。
                 * 320x32 的纹理是“只有 R 通道”告警的主力，
                 * 必须拿到它们的完整数据才能判断是数据错还是渲染错。 */
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
                /* ★ 字体/UI 图集导出：HUD 乱码取证 */
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
            /* ★★ 2026-11-10：区分「占位分配」和「真实上传」。
             *
             * 这是上一版统计最大的错误：
             *   FBO 附件纹理（如 id=987）调用 glTexImage2D 时 data=NULL，
             *   只分配显存空间，内容由 GPU 渲染填充。
             *   CPU 侧去采样它必然是零 —— 那不是坏图，是看不见。
             *
             * 结果：texfinal 报的“19% 全零”里混了大量 FBO 占位，
             * 和用户看到的“主界面/怪物缺失”完全对不上。
             *
             * 现在：
             *   px == NULL          → g_tex_zero = 0（未知，不计入分子分母）
             *   px != NULL 且全零 → g_tex_zero = 2（真正的坏图）
             *   px != NULL 有内容 → g_tex_zero = 1（好）
             */
            /* ★ 全量统计（不是采样）：每张纹理都看前 16 字节，
             * 判断"这一轮到底有多少张真正带内容"。只读 16 字节，开销可忽略。 */
            /* ★★ 2026-11-12：记录每张纹理上传的**调用者返回地址** + 是否全零。
             *
             * 已知 id=9/83/95/97 上传全零，而 id=97(256x256) 和 id=105(256x256)
             * **共用同一个 ptr=65312010**，一个全零一个有内容。
             * 这必须分清是「哪个加载器传的」——6 个 SetTexelData 调用点
             * 对应 ImagePng / 纯色填充 / 另外 4 个未知路径。
             * 纯只读 c->esp，不调任何 GL。 */
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
                /* ★ 判据修正：只看前 16 字节会误判 —— spritesheet 左上角
                 * 常常就是透明的。改为 64 点均匀采样统计非零比例。 */
                if (!px || px < 0x10000u) {
                    n_blank++;
                    /* ★ 2026-11-10：px==NULL 是 FBO/占位分配，不算坏图。
                     * 之前这里写 2，导致全零统计虚高。 */
                    if (g_cur_tex_id < TEX_INFO_N) g_tex_zero[g_cur_tex_id] = 0;
                } else {
                    uint32_t w = (uint32_t)gs32(c, 3), hh = (uint32_t)gs32(c, 4);
                    const unsigned char *q = (const unsigned char *)(uintptr_t)px;
                    uint64_t npix = (uint64_t)w * hh;
                    int nz = 0;
                    /* ★ 2026-11-10：四通道分别累加。
                     * 用来判断“切换房间时红色”是不是通道错位：
                     *   · 正常纹理 R/G/B 均值接近
                     *   · 如果只有 R 高、G/B 恒为0 → BGRA/RGBA 搞反
                     *   · 如果 R/G/B 都为 0、A 有值 → alpha 当颜色用 */
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
                        /* ★★ 2026-11-10：**调用点探针**。
                         *
                         * 真机 + 本地双重确认：同一个 ptr、同一个尺寸下
                         * 有的 id 成功有的全零（"同一指针，id=49 有内容、
                         * id=3/5/7 全零"）。所以问题不是内存、不是池、
                         * 不是尺寸，而是**某些次解码真的产出零**。
                         *
                         * 但要区分“哪条路径”：ImagePng / ImagePcx /
                         * ProceduralImageBase 都可能分配这块纹理，而
                         * 只有 ImagePng 走 libpng。返回地址一下就分清了。
                         *
                         * ld32(c->esp) = call 压的返回地址（guest RVA）。
                         * 注意本函数在**真正调用 gl4es 之前**执行，
                         * 所以 esp 仍指向返回地址。 */
                        extern int fread_covers(uint32_t ptr);
                        static unsigned zshown;
                        if (zshown < 40u) {
                            zshown++;
                            /* 全量扫描：区分“完全没写”和“只写了一点”。
                             * 64 点采样可能漏掉少部分写入，但全零纹理
                             * 总共几十张，全扫一次开销可接受（本地诊断）。 */
                            uint64_t tot = npix * 4ull, nzb = 0;
                            if (tot > (64ull << 20)) tot = 64ull << 20;  /* 上限 64MB */
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
                        /* 旧的 zerotex（带 fread 判定）保留在后面，
                         * 但只在没被上面吃掉名额时打。 */
                        static unsigned zshown2;
                        /* ★★ 2026-11-12：**决定性判定** —— 全零纹理的 alpha 通道。
                         *
                         * 字体图集是「RGB=255 白 + alpha 遮罩」格式，本地实测
                         * alpha=0 占 64%、alpha=255 占 17%。premultiply 只把
                         * RGB 乘以 alpha，**不会动 alpha 本身**。
                         *
                         * 所以：
                         *   alpha 也全零  → 解码根本没写入（ImagePng 失败）
                         *   alpha 有非零  → 写入了，是 premultiply 把 RGB 清了
                         * 这两条路的修法完全不同，必须先分开。 */
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
                    /* 只报“真正可疑”的：R 高而 G/B 恰好为 0（通道序错）。
                     *
                     * ★ 2026-11-10：去掉 only_a 报警。
                     * “R=G=B=0 但 A 有值”是**正常的** —— 阴影、光晕、
                     * 字体描边都是纯 alpha 纹理（黑色带透明度）。
                     * 之前把它们当异常报，日志里刷出一堆假阳性。 */
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
                        /* 保存颜色均值，供 draw 时对照（见 g_tex_rgb 注释） */
                        g_tex_rgb[g_cur_tex_id][0] = (unsigned char)(sR / 64);
                        g_tex_rgb[g_cur_tex_id][1] = (unsigned char)(sG / 64);
                        g_tex_rgb[g_cur_tex_id][2] = (unsigned char)(sB / 64);

                        /* ★ 2026-11-10：通道分类（见 g_tex_class 注释）。
                         * 判据已抽成 tex_classify()，必须与 draw 时补传路径
                         * 用**同一份**规则，否则 [texchan] 的分布会分裂。 */
                        g_tex_class[g_cur_tex_id] =
                            tex_classify(sR, sG, sB, sA, nz);
                    }

                    /* ★★ 2026-11-10：通道序检测。
                     *
                     * 录像对比（bug.jpg）最明显的差异：游戏画面**褪色成红/白**。
                     * 典型原因是 **BGRA 数据被当成 RGBA 上传**（或反之）。
                     * 真机上 Isaac 跑在 Windows 上，它的像素数据很可能
                     * 本来就是 BGRA（DIB 格式）。
                     *
                     * 判据：分别按 RGBA / BGRA 解释同一块数据，
                     * 看哪种“更像图像”：
                     *   · 真实图像很少所有像素恰好 R=G=B（灰度）
                     *   · 如果按 RGBA 解释时 G/B 均值近 0，而按 BGRA 解释时
                     *     三个通道都有分布，那就是 BGRA。 */
                    {
                        static unsigned long shown_ch;
                        if (shown_ch < 6UL && w >= 32u && hh >= 32u) {
                            unsigned long eR=0,eG=0,eB=0;
                            for (int k = 0; k < 64; k++) {
                                uint64_t o = (uint64_t)k * npix / 64ull * 4ull;
                                eR += q[o]; eG += q[o+1]; eB += q[o+2];
                            }
                            /* 按 RGBA 解释：G 和 B 都几乎为 0，而 R 明显有值 */
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
                /* 非零纹理的像素地址：落在 0x30xxxxxx = PE 内置，
                 * 落在 0x48..-0xB0.. = 我们的 guest 堆 = 来自归档。
                 * 这个区分决定了问题在“解码”还是“归档读取”。 */
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
                        /* ★ 2026-11-10：上限 12 → 40，而且**优先报大 id**。
                         * 实际渲染用的是 id=1038/1074/1091/1149/1155 这些，
                         * 旧的“只报前 12 张”全落在启动阶段的小纹理上，
                         * 根本看不到画面里真正在用的那几张。 */
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
                /* 2026-11-04：光知道"上传了"不够 —— 还要看**上传的字节**。
                 * 如果像素全是 0 或全是 0xFF，说明 PNG 解码其实失败了，
                 * 传上去的是一块空白缓冲区，那问题在解码不在渲染。 */
                if (px) {
                    /* 首 16 字节全零 → 触发一次堆扫描，确认解压产物是不是 PNG */
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
        /* ★ 2026-11-06：glTexSubImage2D 是"建空图集再填内容"的关键路径，
         * 之前完全没日志 —— 纹理全零时无法区分"没调用"和"调用了但数据是零"。
         * 9 个 cdecl 参数：target,level,x,y,w,h,format,type,pixels */
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
            /* 画面纯白时，第一个要排除的就是"被清成白色"。
             * 参数是 4 个 float，按 f32 读。 */
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
            /* 返回值在统一分发之后打印（见下方 w(c, e->fn) 处）。 */
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

    /* ① 返回字符串：拿到完整 64 位指针再拷进低 4GB。
     *    （不能走 u32ret wrapper —— 那会把指针截断成垃圾。） */
    if (e->ret == 's') {
        const char *s = NULL;
        if (e->sig == 1 && 0) { /* 占位，保持结构清晰 */ }
        /* 按已知的两个签名处理：glGetString(GLenum)、glGetStringi(GLenum,GLuint) */
        extern int gl_bridge_sig_is_1(int);
        (void)gl_bridge_sig_is_1;
        /* 直接从生成表拿签名长度 */
        {
            /* sig 字段是索引，长度由 wrapper 决定；这里用参数个数判断 */
            /* glGetString: 1 个参数；glGetStringi: 2 个 */
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

    /* ② 其他指针返回值（glMapBuffer 之类）：指向 gl4es 内部缓冲，
     *    guest 的 32 位指针访问不到。诚实返回 0，并报告。 */
    if (e->ret == 'p') {
        gl_note_missing(e->name, "returned a pointer that cannot be mapped in the low 4GB; not wired up");
        glbridge_fn w = gl_bridge_wrapper(e->sig, 0);   /* 仍要执行，保持状态推进 */
        if (w) w(c, e->fn);
        c->eax = 0;
        return 1;
    }

    /* ②b 指针数组参数：guest 的 char** 里存的是 32 位指针，而 gl4es
     *     按宿主 64 位指针数组读，会把两个 guest 指针拼成一个垃圾地址。
     *     实测 fault_addr 就是这么来的。必须重建宿主侧数组。 */
    if (!strcmp(e->name, "glShaderSource")) {
        uint32_t shader = gs32(c, 0);
        uint32_t count  = gs32(c, 1);
        uint32_t strp   = gs32(c, 2);
        uint32_t lenp   = gs32(c, 3);
        if (count > 64) count = 64;          /* 保守上限 */
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

        /* ★★ 2026-11-10：必须在**调用之前**拷贝 shader 源码。
         *
         * 上一版在调用后才读 host_strs[k]，结果日志里
         *   [shader #1.0 长度265]
         *   空行
         * —— 长度对、内容却全是 \0。
         * 原因是 gl4es 内部会把源串拷走并让出/重用那块 guest 内存，
         * 调用结束后源指针就成了无效内容。
         *
         * 现在先快手拷到栈上的本地缓冲区，调用后再打印。 */
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

        /* ★★ 2026-11-10：打印**调用前拷贝**的源码。
         * 这是最直接的证据：能看到 GLSL 到底怎么算 UV 和颜色。
         * 真机上 Isaac 用自定义 shader（不用固定管线），
         * 所以"精灵褪色 / 采到错误格子"只能从 GLSL 里找原因。 */
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

    /* ③ 标量返回 vs void */
    glbridge_fn w = gl_bridge_wrapper(e->sig, e->ret == 'i' ? 1 : 0);
    if (!w) return 0;
    w(c, e->fn);

    /* ★ 2026-11-10：glGetAttribLocation 的返回值现在才可读。
     * 属性名在分支里已存好，这里打印名字 + 实际 index。 */
    if (g_gal_pending) {
        /* ★★ 2026-11-10：同样改成按名字去重。
         * 上限 16 会把后面的新属性名挡掉，看不到完整布局。 */
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

    /* ★ 2026-11-10：glGetUniformLocation 的返回值。
     * 返回 -1 说明名字在 shader 里不存在（被编译器优化掉，或名字错），
     * 那之后所有 glUniform 都会静默失效 —— 包括 sampler 的纹理单元绑定。 */
    if (g_uni_pending) {
        /* ★★ 2026-11-10：从"只打前 20 条"改成"每个不同名字打一次"。
         * 上一版上限 20 刚好被 8×Texture0 + 12×Transform 占满，
         * 于是后面查询 Texture1 完全没被记录 —— 而
         * Texture1 正是调色板着色器的第二个 sampler，最关键的证据。
         * 用静态表去重：既不会刷屏，也不会漏掉新名字。 */
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

    /* ★ 2026-11-10：shader / program 状态。
     * 关心的 pname：
     *   0x8B81 GL_COMPILE_STATUS   0x8B82 GL_LINK_STATUS
     *   0x8B84 GL_INFO_LOG_LENGTH  0x8B86 GL_ACTIVE_UNIFORMS
     *   0x8B89 GL_ACTIVE_ATTRIBUTES
     * 编译/链接失败时游戏通常会自己发现；但如果 gl4es 谎报成功，
     * 就会用一个有缺陷的 program 渲染 —— 这是重点怀疑。 */
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

    /* FBO 完整性：0x8CD5 = GL_FRAMEBUFFER_COMPLETE。
     * 不完整时游戏会静默跳过离屏渲染，渲染目标纹理保持全 0 ——
     * 正是"cutscene 逻辑在跑、画面全白"的形态。 */
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

/* 每帧计数（由 host_gl_audio.c 的 SwapBuffers 读走并归零） */
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
 * 帧抓取：把 pbuffer 里的当前帧读回来存成 PPM
 *
 * 现在 EGL surface 是 pbuffer（离屏），画面不显示 —— 但也因此可以直接
 * glReadPixels 拿到"游戏到底渲染了什么"。这比任何日志都强：一眼能看出
 * 是黑屏、是纯色、还是真的标题画面。
 *
 * 调用方：host_gl_audio.c 的 SwapBuffers（那里有帧计数）。
 * 开关：FRAME_DUMP_EVERY=N  → 在第 N、2N、3N… 帧各存一张（最多 5 张）
 *       FRAME_DUMP_AT=N     → 只在第 N 帧存一张
 * 输出：./frames/frame_<N>.ppm（P6，未压缩；用 ffmpeg/ImageMagick 转 png）
 * ==========================================================================*/
void glFinish(void);
void glReadPixels(int x, int y, int width, int height,
                  unsigned int format, unsigned int type, void *pixels);

int host_gl_dump_frame(const char *path)
{
    if (!g_gl_ready) return -1;
    int w = g_fb_w, h = g_fb_h;
    if (w <= 0 || h <= 0) return -1;

    /* ★★ 2026-11-11：**只在默认 framebuffer 上抓帧**。
     *
     * 实测证据：抓到的是纯白 (255,255,255)，而游戏 glClearColor 是
     * (0,0,0,1) 黑色 —— 说明 glReadPixels 读的不是屏幕。
     *
     * 原因是 glReadPixels 读**当前绑定的** framebuffer 的内容。
     * 游戏在 fb=1(离屏 RT, 1024x1024) 和 fb=0(屏幕, 960x540) 之间
     * 每帧切换多次；如果抓帧时刻正好停在 RT 上，读 960x540 读到的
     * 是 RT 的一块区域（未初始化的白色），不是屏幕画面。
     *
     * 修法：用**我们自己的影子状态** g_fbst.cur_fb 判断当前绑定，
     * 不是 0 就跳过这一帧，等下一个 SwapBuffers。
     * ★ 纯读一个 unsigned，不调任何 GL 查询、不改任何状态。 */
    if (g_fbst.cur_fb != 0u) {
        fprintf(stderr, "[frame] skipped (current fb=%u, not the screen)\n",
                g_fbst.cur_fb);
        fflush(NULL);
        return -2;
    }

    size_t stride = (size_t)w * 3u;
    unsigned char *buf = (unsigned char *)malloc(stride * (size_t)h);
    if (!buf) return -1;

    glFinish();                     /* gl4es 是延迟批处理的，必须先冲干净 */
    glReadPixels(0, 0, w, h, 0x1907 /*GL_RGB*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* OpenGL 原点在左下，PPM 在左上 → 逐行倒着写 */
    for (int y = h - 1; y >= 0; y--)
        fwrite(buf + (size_t)y * stride, 1, stride, f);
    fclose(f);
    free(buf);
    fprintf(stderr, "[frame] saved %s (%dx%d)\n", path, w, h);
    fflush(NULL);
    return 0;
}

/* ==========================================================================
 * 帧推流（2026-11-04）
 *
 * 背景：APK 的 MainActivity 用 ProcessBuilder 起一个**独立进程**跑这个 ELF。
 * Android 的 Surface / ANativeWindow 只能在持有它的进程里用
 * （ANativeWindow_fromSurface 需要 JNIEnv），子进程拿不到。
 * 所以画面只能"推"过去：这里把每帧的 RGBA 通过 TCP:7138 发给 Java，
 * Java 用 SurfaceView 贴到屏幕上。
 *
 * 为什么用 RGBA 而不是 RGB：Android 的 Bitmap.Config.ARGB_8888 在内存里
 * 就是 R,G,B,A 字节序（小端 0xAABBGGRR），和 glReadPixels(GL_RGBA) 的
 * 输出逐字节一致 —— Java 侧可以 Bitmap.copyPixelsFromBuffer 直接吃，
 * 一个像素的转换代码都不用写。RGB 反而要它自己拼 alpha，慢且容易错。
 *
 * 协议（定长头 + 裸像素）：
 *      u32 magic 'IFRM' (0x4D524649)
 *      u32 width
 *      u32 height
 *      u32 bytes  (= width*height*4)
 *      [bytes] RGBA, 从上到下逐行（已翻转）
 *
 * 翻转在**这里**做：OpenGL 原点在左下，Bitmap 在左上。让 native 翻比让
 * Java 翻快得多，而且 Java 侧能保持一个纯 copyPixelsFromBuffer。
 * ========================================================================== */
#define STREAM_PORT 7138
/* ★ 2026-11-05：必须 volatile。
 * 这几个变量跨线程读写（监听线程写 g_stream_fd，渲染线程读），
 * 不加 volatile 时编译器会把 g_stream_fd < 0 的判断提升到循环外/缓存成常量，
 * 表现就是"Java 已连接、native 永远不发帧" —— 真机上正是这个症状。 */
static volatile int g_stream_fd = -1;
static volatile int g_stream_every = 0;   /* 0 = 关闭；N = 每 N 帧推一张 */
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
        /* ★ 最常见的原因：Android 没给 INTERNET 权限，SELinux 直接拒绝。 */
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

        /* 保持连接：轮询看对端是否关闭。
         * 注意不能用阻塞 recv 占住这个线程 —— 那样子进程退出时线程
         * 卡在 recv 里，accept 也不再响应重连。 */
        unsigned ticks = 0;
        for (;;) {
            char c;
            ssize_t n = recv(fd, &c, 1, MSG_DONTWAIT);
            if (n == 0) break;                       /* 对端关闭 */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (g_stream_fd != fd) break;            /* 渲染线程已判定失败 */
            usleep(150000);
            /* 每 ~10 秒报一次心跳。
             * 帧数为 0 说明游戏还没进渲染循环（多半在加载归档）；
             * 帧数在涨说明推流正常。用户一眼能分清"卡死"和"在忙"。 */
            if (++ticks % 66u == 0u) {
                /* 同时报内存：Android 上子进程被 OOM 杀掉时不会留下任何
                 * 输出（SIGKILL），只能靠"崩前 RSS 涨到多少"来推断。
                 * 直接读 /proc/self/status，每 10 秒一条。 */
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

/* 非阻塞发送：宁可丢帧也不能阻塞渲染线程。
 *
 * ★ 2026-11-05：真机上帧率从 54.6 掉到 14.6 fps，最大帧 7055 ms。
 * 原因就是这个函数用的是阻塞 send —— Java 侧每帧要
 * copyPixelsFromBuffer + drawBitmap 到 Canvas，比 native 慢得多，
 * 一旦 socket 缓冲区满，send 就卡住，整个渲染线程跟着停。
 *
 * 改法：MSG_DONTWAIT + 部分发送即放弃。真机推流本来就是"能看就行"，
 * 丢几帧远好过把 54 fps 拖成 14 fps。
 *
 * 返回 0 = 整帧发完；1 = 只发了一部分（调用方要重置连接状态）；
 * -1 = 完全失败。 */
static int send_all_nonblock(int fd, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t sent = 0;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 缓冲区满：这一帧没发完。返回 1 表示“部分发送”，
                 * 调用方会丢弃剩余内容并断开重连（流已错位）。 */
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
    /* 诊断：前几次调用把三个前置条件都打出来。
     * "已连接但收不到帧"只有三种可能：没被调用、条件不满足、发送失败。 */
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
    /* ★ 2026-11-06：改推 RGB565（每像素 2 字节，带宽减半）。
     *
     * 原来推 RGBA8888 = 960*540*4 = 2.07 MB/帧。真机上 Java 侧每帧要
     * copyPixelsFromBuffer + drawBitmap，2MB 全屏拷贝 + Surface 锁，
     * 推流一快就把渲染线程堵住 → 只能靠 FRAME_STREAM=8 降频，
     * 结果**画面只有 7 fps**（游戏本身 55.9 fps）。
     *
     * RGB565 = 1.04 MB/帧，且 Java 侧 Bitmap.Config.RGB_565 的
     * copyPixelsFromBuffer 是直接 memcpy，比 ARGB_8888 快一半。
     * 这个游戏本来就是像素风，565 的色彩损失肉眼几乎看不出。 */
    size_t src_need = (size_t)w * (size_t)h * 3u;   /* GL_RGB 读回 */
    size_t need     = (size_t)w * (size_t)h * 2u;   /* 转成 565 发送 */

    /* ★ fd 和最外层两个缓冲的分配都必须放在读像素之前。
     * 否则「没有客户端」时还会白做一次 glReadPixels + 565 转换
     * （960x540 大约 2~3 ms），而且 g_stream_pack 未分配就写会崩。 */
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

    /* RGB → RGB565，同时上下翻转（GL 原点在左下，屏幕在左上）。
     *
     * 565 位布局（小端 16 位）：RRRRRGGG GGGBBBBB
     *   r5 = R>>3, g6 = G>>2, b5 = B>>3
     *   v  = (r5<<11) | (g6<<5) | b5
     * 直接写 uint16_t，按小端存就是 Android 要的顺序。 */
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

    /* magic 换成 'IFR5'，Java 侧据此选 Bitmap.Config.RGB_565。 */
    uint32_t hdr[4] = { 0x35524649u, (uint32_t)w, (uint32_t)h, (uint32_t)need };
    int rh = send_all_nonblock(fd, hdr, sizeof hdr);
    if (rh < 0) {
        fprintf(stderr, "[stream] ✗ send failed errno=%d (%s); disconnecting\n",
                errno, strerror(errno));
        fflush(NULL);
        g_stream_fd = -1;
        return;
    }
    if (rh > 0) { g_stream_dropped++; return; }   /* 头都没发完，丢帧 */

    int rp = send_all_nonblock(fd, g_stream_pack, need);
    if (rp < 0) {
        fprintf(stderr, "[stream] ✗ send failed errno=%d (%s); disconnecting\n",
                errno, strerror(errno));
        fflush(NULL);
        g_stream_fd = -1;
        return;
    }
    if (rp > 0) {
        /* 像素只发了一部分 —— 流已错位，必须断开让 Java 重连，
         * 否则后面每一帧都会被解析成垃圾。 */
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
 * 堆扫描探针（2026-11-06）
 *
 * 已知事实链：
 *   1. fread 读了 1.1GB，但「PNG 签名块 = 0」→ 归档里是压缩数据
 *   2. glTexImage2D 的像素缓冲全零，且 HEAP_POISON 下不是 0xCD
 *      → 有人把它清零了，然后没填回内容
 *
 * 还没确定：**解压出来到底是不是 PNG**。直接在 guest 堆里搜签名：
 *   · 搜到 89 50 4E 47 → 数据在，问题在 libpng 解码
 *   · 搜不到           → 解压输出根本不是 PNG（或解压失败）
 *
 * 用 /proc/self/maps 限制范围，绝不碰未映射页。只跑一次。
 * ========================================================================== */
void png_scan_heap_once_pub(void);
static void png_scan_heap_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    /* 默认关：扫 400MB 堆要 2 秒，真机上会看到一次明显卡顿。
     * 需要时设 PNG_SCAN=1。 */
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
 * ★★ 2026-11-12：纹理导出为 PPM（真机取证用）
 *
 * 目的：HUD 数字全部渲染成同一个 µ 字形，怀疑字体图集（.fnt 的 _0.png）
 * 上传内容不对。日志里的 hex 看不出形状，必须把纹理写成图片看。
 *
 * 只导出"像字体图集"的尺寸（128..512 的方形/扁矩形），最多 16 张，
 * 直接写 PPM（P6）到 <game>/frames/tex_<id>.ppm，本地转 PNG 查看。
 * 纯只读：只读 guest 内存，不调任何 GL。
 * ========================================================================== */
void host_tex_dump_ppm(uint32_t id, uint32_t w, uint32_t h,
                       uint32_t fmt, const unsigned char *px)
{
    static unsigned n;
    if (n >= 32u) return;
    if (!px || w < 16u || h < 16u) return;
    /* ★ 只导出"字体/UI 图集"尺寸，排除大背景图：
     *   128x128(16384)  256x256(65536)  512x128(65536)  256x128(32768)
     *   512x256(131072) 也保留（pftempestasevencondensed 就是这个尺寸）
     * 512x512 及以上的贴图页不要（会把名额占满）。 */
    if ((uint64_t)w * h > 131072ull) return;

    const char *dir = "/storage/emulated/0/Android/media/com.isaacport"
                      ".launcher/files/game/frames";
    char path[256];
    snprintf(path, sizeof path, "%s/tex_%u.ppm", dir, id);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    /* guest 里是 RGBA8888，PPM 要 RGB。fmt 0x1908 = RGBA */
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




/* ★★ 2026-11-12：SetTexelData 调用者归因（本地诊断）。
 * 6 个调用点：0x5a153b=ImagePng, 0x5a0b1d, 0x5a1a18, 0x5a1e9e,
 *             0x5b606e=纯色填充, 0x5b61bb
 * 字体图集全零，必须知道它走哪条路。 */
void host_settexel_probe(uint32_t callret, uint32_t img)
{
    static const uint32_t sites[6] = { 0x5a153bU, 0x5a0b1dU, 0x5a1a18U,
                                       0x5a1e9eU, 0x5b606eU, 0x5b61bbU };
    static const char *names[6] = { "ImagePng", "5a0b1d", "5a1a18",
                                    "5a1e9e", "solid fill", "5b61bb" };
    uint32_t w = img >= 0x10000u ? ld32((uint32_t)(img + 0x80U)) : 0u;
    uint32_t h = img >= 0x10000u ? ld32((uint32_t)(img + 0x84U)) : 0u;
    /* 只关心 UI/字体尺寸，避免刷屏 */
    if (!((uint64_t)w * h <= 131072ull && w >= 16u && h >= 16u)) return;
    int k = -1;
    for (int i = 0; i < 6; i++) if (sites[i] == callret) { k = i; break; }
    fprintf(stderr, "[settexel] %ux%u from %s (ret=%08x)\n",
            w, h, k >= 0 ? names[k] : "unknown", callret);
    fflush(NULL);
}
