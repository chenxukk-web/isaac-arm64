/* host_msvcpp.c —— MSVCP140 的 C++ 流最小实现（阶段一：让它不崩）
 *
 * ==========================================================================
 * 一、为什么需要这个，以及为什么是「最小」实现
 *
 * 游戏用 std::stringstream 按分隔符切分字符串（典型用途：解析版本号
 * "1.2.3"、控制台命令参数）。这是 MSVC 运行库的私有 ABI：`basic_ios` /
 * `basic_iostream` / `basic_streambuf` 的成员布局是编译器内部约定，
 * 跨编译器复现困难。
 *
 * 从游戏代码反推出来的布局（RVA 0x25c990，basic_stringstream 构造）：
 *
 *     [stream + 0x00]  vtable = 0xb3ee54，其 [1] = 0x68
 *     [stream + 0x18]  stringbuf 子对象
 *     [stream + 0x68]  basic_ios 子对象（lea ecx,[ebx+0x68]; call basic_ios）
 *     [stream + 0xa0]  _Mystrbuf = 0x68 + 0x38
 *
 * 而 getline 内联体（RVA 0x25d680）取 streambuf 的方式是：
 *     mov eax,[stream]      ; vtable
 *     mov eax,[eax+4]       ; = 0x68（虚基类偏移）
 *     mov ecx,[eax+stream+0x38]   ; = [stream+0xa0]
 * 与上面完全一致 —— 布局确认无误。
 *
 * 阶段一的目标是**让游戏越过这里而不崩**：所有读操作立即返回 EOF，
 * 于是 getline 循环一次都不执行，切分结果是空数组。
 *
 * 阶段一的已知代价：字符串不会被真正切分。如果后续出现「版本号解析
 * 失败」「控制台命令参数为空」之类的逻辑错误，就是这里 —— 那时要升级
 * 到阶段二（按 MSVC 布局真实实现 stringbuf 的读写指针）。
 * 这样分阶段的好处是：先用最小代价确认「这条路通不通」，
 * 而不是一头扎进完整的 C++ 流实现。
 * ==========================================================================
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
/* MSVC x86 的成员函数是 __thiscall：this 在 ecx，参数在栈，被调方清参数
 * （其实是调用方清，但 COM/成员函数都按 stdcall 的 pop 数量由被调方处理
 *  — 这里统一用「弹返回地址 + 清 n 字节」。 */
static int g_msvcp_trace = 0;   /* 调到 8 可逐条看 esp 进出 */
static const char *g_msvcp_cur;

static void R(CPU *__restrict c, uint32_t n)
{
    uint32_t before = c->esp;
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
    if (g_msvcp_trace > 0) {
        g_msvcp_trace--;
        fprintf(stderr, "[msvcp-trace] %-46s esp %08x -> %08x  popped %u\n",
                g_msvcp_cur ? g_msvcp_cur : "?", before, c->esp, n);
    }
}

/* MSVC 流状态位（std::ios_base::iostate） */
#define GOODBIT  0x00
#define EOFLBIT  0x01
#define FAILBIT  0x02
#define BADBIT   0x04

/* 布局（从 RVA 0x25c990 / 0x25d680 反推）：
 *   stream + 0x68           basic_ios 子对象（它的 this）
 *   basic_ios_this + 0x0c   状态位 iostate
 *   basic_ios_this + 0x38   _Mystrbuf
 * 而 getline 检查状态用的是 `test byte [0x68 + stream + 0xc]`
 *  —— 正是 basic_ios_this + 0xc。 */
#define IOS_OFF       0x68u
#define IOS_STATE_OFF 0x0cu      /* 相对 basic_ios 的 this */
#define MYSTRBUF_OFF  0xa0u      /* 相对 stream */

static int g_msvcp_log = 0;   /* 调到 8 可看构造顺序 */

/* ---- 构造 ---- */

/* `basic_ios::basic_ios()`  (RVA 调用点 0x65c9d4, this = stream+0x68)
 * 职责：清空 _Mystrbuf，把状态位设成 good。
 * vtable 由游戏代码在调用前后自己写，我们不动它。 */
static void msvcp_basic_ios_ctor(CPU *c)
{
    uint32_t self = c->ecx;              /* __thiscall: this 在 ecx */
    if (self) {
        st32(self + 0x38u, 0u);                    /* _Mystrbuf = null */
        st8(self + IOS_STATE_OFF, (uint8_t)GOODBIT); /* 状态 = good */
        st32(self + 0x34u, 0u);                    /* _Tiestr = null */
    }
    if (g_msvcp_log > 0) {
        g_msvcp_log--;
        fprintf(stderr, "[msvcp] basic_ios::ctor(this=%08x)\n", self);
    }
    R(c, 0);
}

/* `basic_iostream::basic_iostream(streambuf*)`
 * 调用点 RVA 0x65c9f0。注意调用点压了**两个** push：
 *     0065c9da  push 0        ; 第 2 个参数
 *     0065c9e6  push edi      ; 第 1 个参数 = streambuf
 *     0065c9e7  mov ecx, ebx  ; this = stream
 * 所以清理 8 字节而不是 4 —— 少清 4 字节会让调用者的栈失衡，
 * 之后每个 [ebp-x] 都读到错位的值（实测症状：memcpy 的目标地址变成
 * 0xfffffffe，即把某个 -2 的返回值当成指针）。 */
static void msvcp_basic_iostream_ctor(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t sb = A(c, 0);
    if (self) st32(self + MYSTRBUF_OFF, sb);
    if (g_msvcp_log > 0) {
        g_msvcp_log--;
        fprintf(stderr, "[msvcp] basic_iostream::ctor(this=%08x, sb=%08x, arg1=%08x)\n",
                self, sb, A(c, 1));
    }
    R(c, 8);
}

static void msvcp_basic_ios_dtor(CPU *c)
{
    uint32_t self = c->ecx;
    if (self) st32(self + 0x38u, 0u);
    R(c, 0);
}
static void msvcp_basic_iostream_dtor(CPU *c)
{
    (void)c->ecx;
    R(c, 0);
}
/* `basic_streambuf::basic_streambuf()` (RVA 调用点 0x65ca1a, this = stringbuf)
 *
 * ★ 这个构造函数不是空操作。从 sub_0025c990 的后续代码可以看到，游戏在
 *   构造完之后直接写六个「指针的指针」字段：
 *       0x25ca74  mov ecx,[edi+0xc]; mov [ecx],esi
 *       0x25ca79  mov eax,[edi+0x1c]; mov [eax],esi
 *       0x25ca7e  mov eax,[edi+0x2c]; mov [eax],edx
 *       0x25ca83  mov eax,[edi+0x10]; mov [eax],esi
 *       0x25ca8d  mov eax,[edi+0x20]; mov [eax],esi
 *       ...
 *   也就是说 [this+0xc] 等六个槽必须**已经指向可写的内存**。如果构造
 *   什么也不做，这六格是栈上的垃圾（实测是 -2），下一步就写 0xfffffffe。
 *
 *   这里让它们都指向对象内部的保留区（+0x40..+0x58）并清零。
 *   stringbuf 子对象在 stream+0x18，而 stream 至少 0xa4 字节，
 *   所以 +0x40..+0x58 落在对象内，不会踩到别处。 */
static void msvcp_streambuf_ctor(CPU *c)
{
    uint32_t self = c->ecx;
    if (self) {
        static const uint32_t offs[6] = { 0x0cu, 0x1cu, 0x2cu, 0x10u, 0x20u, 0x30u };
        for (unsigned i = 0; i < 6; i++) {
            uint32_t slot = self + 0x40u + i * 4u;
            st32(slot, 0u);
            st32(self + offs[i], slot);
        }
        st32(self + 0x38u, 0u);   /* 缓冲尾 */
        st32(self + 0x3cu, 0u);
    }
    if (g_msvcp_log > 0) {
        g_msvcp_log--;
        fprintf(stderr, "[msvcp] basic_streambuf::ctor(this=%08x)\n", self);
    }
    R(c, 0);
}
static void msvcp_streambuf_dtor(CPU *c) { (void)c->ecx; R(c, 0); }
static void msvcp_lock(CPU *c)   { (void)c->ecx; R(c, 0); }
static void msvcp_unlock(CPU *c) { (void)c->ecx; R(c, 0); }
static void msvcp_setbuf(CPU *c) { (void)c->ecx; c->eax = 0; R(c, 8); }
static void msvcp_sync(CPU *c)   { (void)c->ecx; c->eax = 0; R(c, 0); }
static void msvcp_imbue(CPU *c)  { (void)c->ecx; R(c, 4); }

/* `basic_istream::_Ipfx(bool noskipws)` (RVA 0x65d6dd)
 * sentry：检查流状态，good 才返回 true。阶段一总是 good。 */
static void msvcp_Ipfx(CPU *c)
{
    (void)c->ecx; (void)A(c, 0);
    c->eax = 1;                          /* true */
    R(c, 4);
}

/* ---- streambuf 读取：阶段一全部 EOF ---- */

/* `basic_streambuf::sgetc()` —— 返回当前字符但不前进；EOF 用 traits 的
 * eof() = -1 表示。 */
/* streambuf 的这几个是**未实现**（恒 EOF / 恒 0）。
 * SB_TRACE=1 时按调用次数打印，用来判断游戏是否真的走这条路 ——
 * 如果走了，任何用到流式读取的功能都会静默拿到 EOF。 */
static unsigned long g_sb[5];
static void sb_note(int i, const char *n)
{
    static int on = -1;
    if (on < 0) on = getenv("SB_TRACE") != NULL;
    g_sb[i]++;
    if (on && g_sb[i] <= 6)
        fprintf(stderr, "[sb] %s call #%lu (ecx=%08x)\n",
                n, g_sb[i], 0u);
}
void sb_report(void)
{
    fprintf(stderr, "[sb] streambuf fake-stub call counts: "
                    "sgetc=%lu sbumpc=%lu snextc=%lu uflow=%lu xsgetn=%lu\n",
            g_sb[0], g_sb[1], g_sb[2], g_sb[3], g_sb[4]);
    fflush(NULL);
}

static void msvcp_sgetc(CPU *c)
{
    sb_note(0, "sgetc");
    { static int shown;
      if (shown++ < 10)
          fprintf(stderr, "[sb] ★ sgetc called ecx=%08x (EOF stub)\n", c->ecx); }
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;                /* eof */
    R(c, 0);
}
/* `basic_streambuf::sbumpc()` —— 取当前字符并前进 */
static void msvcp_sbumpc(CPU *c)
{
    sb_note(1, "sbumpc");
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;
    R(c, 0);
}
/* `basic_streambuf::snextc()` —— 前进后取字符 */
static void msvcp_snextc(CPU *c)
{
    sb_note(2, "snextc");
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;
    R(c, 0);
}
/* `basic_streambuf::uflow()` —— 虚函数，缓冲区空时被调用 */
static void msvcp_uflow(CPU *c)
{
    sb_note(3, "uflow");
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;
    R(c, 0);
}
/* `basic_streambuf::xsgetn(char*, streamsize)` */
static void msvcp_xsgetn(CPU *c)
{
    sb_note(4, "xsgetn");
    (void)c->ecx; (void)A(c, 0); (void)A(c, 1);
    c->eax = 0;                          /* 读到 0 个 */
    c->edx = 0;
    R(c, 8);
}

/* ★★ 2026-11-10：`basic_streambuf::sputn(const char*, streamsize)`。
 *
 * 为什么必须补：真机上**按暂停键必崩**，现场是
 *     [MISS] MSVCP140.dll!?sputn@?$basic_streambuf@DU...QAE_JPBD_J@Z
 *     [SEGV] sig=6  pc 属于 libc.so (符号 abort)
 * 即：这个导入没人处理 → IAT 槽没回填 → 游戏一调就进兜底 abort。
 *
 * 为什么暂停键会走到这里：按 ESC 时游戏会写日志 / 刷新调试控制台
 * 输出，那条路径经过 ostream → streambuf::sputn。
 *
 * 语义选择：返回 n（声称全部写入成功），而不是 0。
 * 现有 streambuf 读取桩一律返回 EOF，说明我们没有真实缓冲；
 * 若返回 0，调用方会把流置为 failbit/badbit，可能触发另一条
 * 异常路径。返回 n 让调用方认为写完了，继续正常执行。
 * 代价：这些输出确实被丢弃（本来也是桩），但不影响游戏逻辑。
 *
 * 布局（thiscall：this 在 ecx）：
 *   [esp+4]  s (char*)          4 字节
 *   [esp+8]  n (streamsize)     8 字节（__int64）
 *   返回 edx:eax = n
 * 所以清 4 + 8 = 12 字节。不能用 A(c,1) 当完整 n —— 它只读低 32 位。 */
static void msvcp_sputn(CPU *c)
{
    uint32_t s = A(c, 0);
    uint32_t nLo = A(c, 1);
    uint32_t nHi = A(c, 2);
    {
        static unsigned shown;
        if (shown++ < 8u) {
            fprintf(stderr, "[sb] sputn(s=%08x, n=%u) -> claims written\n",
                    s, nLo);
            fflush(NULL);
        }
    }
    c->eax = nLo;
    c->edx = nHi;
    R(c, 12);
}

/* `basic_ios::setstate(iostate)` (RVA 0x65d788)
 * 记录状态位。sentry 随后会读它判断流是否可用。 */
static void msvcp_setstate(CPU *c)
{
    uint32_t self = c->ecx;              /* this = stream+0x68 */
    uint32_t st = A(c, 0);
    (void)A(c, 1);                       /* rethrow */
    if (self) {
        uint8_t cur = ld8(self + IOS_STATE_OFF);
        st8(self + IOS_STATE_OFF, (uint8_t)(cur | (uint8_t)st));
    }
    R(c, 8);
}
/* `basic_ios::clear(iostate)` */
static void msvcp_clear(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t st = A(c, 0);
    if (self) st8(self + IOS_STATE_OFF, (uint8_t)st);
    R(c, 4);
}

/* ================= 分发 =================
 * 名字是 MSVC 的修饰名（mangled），匹配时忽略 std::char_traits 的具体
 * 参数化写法 —— 用关键字子串判断，因为完整名字极长且易抄错。 */
typedef struct { const char *needle; void (*fn)(CPU *); } msvcp_entry;

/* std::uncaught_exception()：返回 __uncaught_exceptions() != 0。
 *
 * guest 代码里它只用来给 RAII 守卫判断「现在是不是处于异常展开中」。
 * 我们的移植没有真正的 SEH 展开（_CxxThrowException 只做诊断后 abort），
 * 任何时刻都不存在「正在展开」的状态 —— 返回 false 是准确答案，
 * 不是把错误吞掉的假桩。 */
static void msvcp_uncaught_exception(CPU *c)  { c->eax = 0u; R(c, 0); }

/* basic_ostream::_Osfx —— 析构时「把缓冲冲出去」。
 * 我们的 ostream 本来就是桩，没有自己的缓冲，所以无事可做；
 * 这是语义上的空操作，不是吞错误。 */
static void msvcp_Osfx(CPU *c)  { R(c, 0); }

/* ★★ 2026-11-10：`basic_ostream<char>::operator<<(unsigned long)`。
 *
 * 补 sputn 之后，暂停键换了一个崩点：
 *     [MISS] MSVCP140.dll!??6?$basic_ostream@DU?$char_traits@D@std@@@std@@
 *                        QAEAAV01@K@Z
 * `??6` 是 operator<< 的修饰名前缀，末尾的 `K` = unsigned long。
 *
 * 即游戏在用 `oss << someUnsignedLong` 做格式化输出（写日志/调试
 * 控制台）。这是一个**独立导出**，不经过 sputn 的 IAT 槽，所以补了
 * sputn 也不够 —— 必须单独提供。
 *
 * 语义：__thiscall，this 在 ecx，参数 1 个 DWORD，返回 ostream&（即 this）。
 * 我们没真实缓冲，数值直接丢弃；返回 this 让链式调用 `<<` 继续。
 *   [esp+4]  unsigned long value   4 字节
 *   返回 eax = this
 *
 * ★ 为什么用**精确** needle 而不是 "??6?$basic_ostream@"：
 * operator<< 有二十多个重载（char/int/unsigned/long long/float/
 * double/const char*…），清栈字节数各不相同。泛匹配会按错的大小
 * 清栈，让 esp 静默错位 —— 那比崩溃更难查。宁可 MISS。
 * 新类型出现时再逐条补。 */
static void msvcp_ostream_shl_uint(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t v = A(c, 0);
    {
        static unsigned shown;
        if (shown++ < 6u) {
            fprintf(stderr, "[sb] ostream<<(uint=%u) -> discarded, returns this\n", v);
            fflush(NULL);
        }
    }
    c->eax = self;
    R(c, 4);
}

/* `basic_ostream<char>::operator<<(int)` —— 同上，参数 4 字节。 */
static void msvcp_ostream_shl_int(CPU *c)
{
    uint32_t self = c->ecx;
    int32_t v = (int32_t)A(c, 0);
    {
        static unsigned shown;
        if (shown++ < 6u) {
            fprintf(stderr, "[sb] ostream<<(int=%d) -> discarded, returns this\n", v);
            fflush(NULL);
        }
    }
    c->eax = self;
    R(c, 4);
}

/* `basic_ostream<char>::operator<<(char)` —— char 按 4 字节传（x86 提升）。 */
static void msvcp_ostream_shl_char(CPU *c)
{
    uint32_t self = c->ecx;
    (void)A(c, 0);
    c->eax = self;
    R(c, 4);
}

/* `basic_ostream<char>::operator<<(const char*)` —— 指针 4 字节。 */
static void msvcp_ostream_shl_str(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t p = A(c, 0);
    {
        static unsigned shown;
        if (shown++ < 6u) {
            /* 尽量把字符串本身也打出来，方便确认是哪条日志 */
            char tmp[80];
            int i = 0;
            if (p) {
                for (; i < (int)sizeof tmp - 1; i++) {
                    uint32_t a = p + (uint32_t)i;
                    char ch = (char)(ld32(a & ~3u) >> ((a & 3u) * 8u));
                    if (!ch) break;
                    tmp[i] = ch;
                }
            }
            tmp[i] = 0;
            fprintf(stderr, "[sb] ostream<<(\"%s\") -> discarded\n", tmp);
            fflush(NULL);
        }
    }
    c->eax = self;
    R(c, 4);
}

/* ==========================================================================
 * 2026-11-10：ostream / streambuf 剩余方法的批量补齐。
 *
 * 历史教训：这块一直在“挤牙膏”。每次补上一个，游戏就往前走一步、
 * 撞到下一个 [MISS]，又崩一次：
 *   sputn        → 崩       → 补
 *   operator<<K  → 崩       → 补
 *   flush        → 崩       → 补（本次）
 * 与其反复真机试错，这次直接把 PE 导入表里**全部**相关导出一次做完。
 *
 * 用 objdump -p 从 isaac-ng.exe.unpacked 里查到的完整清单（共 15 个）：
 *   ?sgetc  ?sbumpc  ?snextc  ?uflow  ?xsgetn   ← 已有
 *   ?sputn  ?xsputn  ?sputc   ?showmanyc        ← sputn 已有
 *   ?flush  ?put     ?widen   ?setstate       ← setstate 已有
 *   ?setbuf ?sync    ?imbue                     ← 已有
 *
 * 修饰名解码：
 *   QAE/QBE = public __thiscall（BE 额外表示 const）
 *   MAE     = protected __thiscall（虚函数，vtable 调用）
 *   返回值  H=int  D=char  _J=__int64  AAV<..>@=类引用
 * ========================================================================== */

/* `basic_ostream::flush()` —— 无参数，返回 ostream&。 */
static void msvcp_ostream_flush(CPU *c)
{
    uint32_t self = c->ecx;
    {
        static unsigned shown;
        if (shown++ < 6u) {
            fprintf(stderr, "[sb] ostream::flush() -> returns this\n");
            fflush(NULL);
        }
    }
    c->eax = self;
    R(c, 0);
}

/* `basic_ostream::put(char)` —— 返回 ostream&。
 * x86 下 char 参数占 4 字节槽位。 */
static void msvcp_ostream_put(CPU *c)
{
    uint32_t self = c->ecx;
    (void)A(c, 0);
    c->eax = self;
    R(c, 4);
}

/* `basic_streambuf::xsputn(const char*, streamsize)` —— 虚函数。
 * 返回实际写入个数。同 sputn，声称全部写成功（返回 n）。
 *   [esp+4]  s (const char*)   4 字节
 *   [esp+8]  n (streamsize)    8 字节
 *   返回 edx:eax = n
 * 清 4 + 8 = 12 字节。 */
static void msvcp_xsputn(CPU *c)
{
    uint32_t nLo = A(c, 1);
    uint32_t nHi = A(c, 2);
    (void)A(c, 0);
    c->eax = nLo;
    c->edx = nHi;
    R(c, 12);
}

/* `basic_streambuf::sputc(char)` —— 写单个字符，返回 int（traits::int_type）。
 * 成功时返回字符本身，EOF(-1) 表示失败。我们没有真实缓冲，
 * 返回字符值让调用方认为写成功了。
 *   [esp+4]  c (char)  4 字节槽位
 *   清 4 字节。 */
static void msvcp_sputc(CPU *c)
{
    uint32_t ch = A(c, 0);
    c->eax = ch & 0xFFu;
    R(c, 4);
}

/* `basic_streambuf::showmanyc()` —— 虚函数，返回 __int64。
 * “还能读多少字符”。我们没有任何可读数据，返回 0。
 * 无参数，清 0 字节。 */
static void msvcp_showmanyc(CPU *c)
{
    c->eax = 0;
    c->edx = 0;
    R(c, 0);
}

/* `basic_ios::widen(char)` —— const 成员，返回 char。
 * char 版本下 widen 就是恒等映射（locale 不改变单字节）。
 *   [esp+4]  c (char)  4 字节槽位
 *   清 4 字节。 */
static void msvcp_widen(CPU *c)
{
    c->eax = A(c, 0) & 0xFFu;
    R(c, 4);
}

/* ★★ 2026-11-10：`basic_ostream<char>::operator<<(unsigned long)`。
 *
 * 补 sputn 之后，暂停键换了一个崩点：
 *     [MISS] MSVCP140.dll!??6?$basic_ostream@DU?$char_traits@D@std@@@std@@
 *                        QAEAAV01@K@Z
 * `??6` 是 operator<< 的修饰名前缀，K = unsigned long。
 *
 * 即游戏在用 `oss << someUnsignedLong` 做格式化输出（写日志/调试
 * 控制台）。这是一个**独立导出**，不经过 sputn 的 IAT 槽，所以补了
 * sputn 也不够 —— 必须单独提供。
 *
 * 语义：__thiscall，this 在 ecx，参数 1 个 DWORD，返回 ostream&（即 this）。
 * 我们没真实缓冲，数值直接丢弃；返回 this 让链式调用 `<<` 继续。
 *   [esp+4]  unsigned long value
 *   返回 eax = this
 *   清 4 字节 */
static void msvcp_ostream_shl_ulong(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t v = A(c, 0);
    {
        static unsigned shown;
        if (shown++ < 6u) {
            fprintf(stderr, "[sb] ostream<<(ulong=%u) this=%08x -> discarded\n",
                    v, self);
            fflush(NULL);
        }
    }
    c->eax = self;
    R(c, 4);
}

static const msvcp_entry s_msvcp[] = {
    { "??0?$basic_ios@",              msvcp_basic_ios_ctor },
    { "??1?$basic_ios@",              msvcp_basic_ios_dtor },
    { "??0?$basic_iostream@",         msvcp_basic_iostream_ctor },
    { "??1?$basic_iostream@",         msvcp_basic_iostream_dtor },
    { "??0?$basic_streambuf@",        msvcp_streambuf_ctor },
    { "??1?$basic_streambuf@",        msvcp_streambuf_dtor },
    { "?_Ipfx@",                      msvcp_Ipfx },
    { "?sgetc@?$basic_streambuf@",    msvcp_sgetc },
    { "?sbumpc@?$basic_streambuf@",   msvcp_sbumpc },
    { "?snextc@?$basic_streambuf@",   msvcp_snextc },
    { "?uflow@?$basic_streambuf@",    msvcp_uflow },
    { "?xsgetn@?$basic_streambuf@",   msvcp_xsgetn },
    /* ★ 2026-11-10：PE 导入表里剩下的 streambuf 写方法。 */
    { "?xsputn@?$basic_streambuf@",   msvcp_xsputn },
    { "?sputc@?$basic_streambuf@",    msvcp_sputc },
    { "?showmanyc@?$basic_streambuf@", msvcp_showmanyc },
    { "?widen@?$basic_ios@",          msvcp_widen },
    /* ★ 2026-11-10：暂停键崩溃的根因。见 msvcp_sputn 的注释。 */
    { "?sputn@?$basic_streambuf@",    msvcp_sputn },
    { "?setstate@?$basic_ios@",       msvcp_setstate },
    { "?clear@?$basic_ios@",          msvcp_clear },
    { "?_Lock@?$basic_streambuf@",    msvcp_lock },
    { "?_Unlock@?$basic_streambuf@",  msvcp_unlock },
    { "?setbuf@?$basic_streambuf@",   msvcp_setbuf },
    { "?sync@?$basic_streambuf@",     msvcp_sync },
    { "?imbue@?$basic_streambuf@",    msvcp_imbue },
    { "?uncaught_exception@std@@",    msvcp_uncaught_exception },
    { "?_Osfx@?$basic_ostream@",   msvcp_Osfx },
    /* ★ 2026-11-10：暂停键最新崩点。flush 是写日志路径的最后一道。 */
    { "?flush@?$basic_ostream@",   msvcp_ostream_flush },
    { "?put@?$basic_ostream@",     msvcp_ostream_put },
    /* ★ 2026-11-10：暂停键第二个崩点。
     * needle 包含**完整参数编码**，避免误匹配到参数大小不同的重载。
     * 前缀 `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@`
     *   - QAE = public __thiscall，返回 AAV01 = ostream&
     * 后缀只写参数编码，容下 VS 版本的细微差异。 */
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@K@Z",
                                      msvcp_ostream_shl_uint },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@H@Z",
                                      msvcp_ostream_shl_int },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@D@Z",
                                      msvcp_ostream_shl_char },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@PBD@Z",
                                      msvcp_ostream_shl_str },
    /* ★ 2026-11-10：暂停键第二个崩点。注意必须排在泛匹配之前
     * （表是按顺序 strstr 的，越具体的越靠前）。 */
    { "??6?$basic_ostream@",       msvcp_ostream_shl_ulong },
};

int host_msvcp_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    const char *bang = strchr(name, '!');
    if (!bang || strncmp(name, "MSVCP140.dll", 12) != 0) return 0;
    const char *sym = bang + 1;
    { static int on = -1;
      if (on < 0) on = getenv("MSVCP_TRACE") != NULL;
      if (on) {
          static unsigned long n;
          fprintf(stderr, "[msvcp] #%lu %s\n", ++n, sym);
          fflush(NULL);
      } }
    for (unsigned i = 0; i < sizeof s_msvcp / sizeof s_msvcp[0]; i++) {
        if (strstr(sym, s_msvcp[i].needle) != NULL) {
            g_msvcp_cur = s_msvcp[i].needle;
            s_msvcp[i].fn(c);
            g_msvcp_cur = NULL;
            return 1;
        }
    }
    return 0;
}

int host_msvcp_count(void)
{
    return (int)(sizeof s_msvcp / sizeof s_msvcp[0]);
}
