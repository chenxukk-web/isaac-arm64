/* Android CRT 层 —— UCRT 的 printf 家族
 *
 * 只有 __stdio_common_vsprintf 这一个导入，但它是整个 UI 的文本出口：
 * 游戏所有 sprintf/snprintf 最终都落到这里。返回 0 会让界面全是空串，
 * 而那正是「看起来在跑、其实全错」的假桩，纪律不允许。
 *
 * 调用约定（从 RVA 0x11f8e 的调用点实测）：
 *   cdecl，28 字节，调用方 add esp,0x1c 清理
 *     +0   _Options 低 32     (ecx|1)
 *     +4   _Options 高 32     ([acrt_locale+4])
 *     +8   _Buffer
 *     +12  _BufferCount
 *     +16  _Format
 *     +20  _Locale           (0)
 *     +24  _ArgList          ← x86 va_list 就是指向 guest 参数区的 uint32
 *
 * 关键约束：_ArgList 是 **x86 的 va_list**，而我们是 arm64 宿主。宿主
 * 的 va_list 是结构体，语义对不上，不能直接转交。所以这里自己按 x86
 * 规则推进参数指针，再逐项交给宿主 snprintf 做实际格式化 —— 浮点、
 * 大整数这些难写的部分因此不必重写。
 *
 * x86 cdecl 的参数推进规则：所有参数 4 字节对齐；int/pointer 占 4；
 * long long / double 占 8（低 32 在前）。
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }

/* ---- x86 va_list 推进 ---- */
static uint32_t va32(CPU *__restrict c, uint32_t *p)
{
    uint32_t v = ld32(guest_stack_address(c, *p, 4u, 0u));
    *p += 4u;
    return v;
}
static uint64_t va64(CPU *__restrict c, uint32_t *p)
{
    uint64_t lo = ld32(guest_stack_address(c, *p, 4u, 0u));
    uint64_t hi = ld32(guest_stack_address(c, *p + 4u, 4u, 0u));
    *p += 8u;
    return lo | (hi << 32);
}
static double va_f64(CPU *__restrict c, uint32_t *p)
{
    uint64_t raw = va64(c, p);
    double d;
    memcpy(&d, &raw, sizeof d);
    return d;
}

static int g_vsprintf_log = 12;   /* 前若干次打印格式串，便于验证 */

#define SPEC_MAX 96
static void spec_add(char *spec, unsigned *sn, char ch)
{
    if (*sn < SPEC_MAX - 1) spec[(*sn)++] = ch;
    spec[*sn] = 0;
}

static int fmt_vsprintf(CPU *c, char *buf, uint32_t cap, const char *fmt,
                        uint32_t arglist)
{
    uint32_t arg = arglist;
    uint32_t out = 0;                       /* 已产出字节数（不含 \0） */
    uint32_t limit = cap ? cap - 1u : 0u;   /* 预留结尾 \0 */

#define PUT(ch) do { if (buf && out < limit) buf[out] = (char)(ch); out++; } while (0)

    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        if (*f == '%') { PUT('%'); continue; }
        if (!*f) { PUT('%'); break; }

        char spec[SPEC_MAX];
        unsigned sn = 0;
        spec_add(spec, &sn, '%');

        /* ---- flags ---- */
        for (;; f++) {
            if (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0')
                spec_add(spec, &sn, *f);
            else break;
        }

        /* ---- width（可能是 *） ---- */
        if (*f == '*') {
            int w = (int32_t)va32(c, &arg);
            if (w < 0) { spec_add(spec, &sn, '-'); w = -w; }
            if (w > 4096) w = 4096;
            char t[16];
            snprintf(t, sizeof t, "%d", w);
            for (char *q = t; *q; q++) spec_add(spec, &sn, *q);
            f++;
        } else {
            while (*f >= '0' && *f <= '9') { spec_add(spec, &sn, *f); f++; }
        }

        /* ---- precision ---- */
        if (*f == '.') {
            spec_add(spec, &sn, '.');
            f++;
            if (*f == '*') {
                int p = (int32_t)va32(c, &arg);
                if (p < 0) p = 0;
                if (p > 4096) p = 4096;
                char t[16];
                snprintf(t, sizeof t, "%d", p);
                for (char *q = t; *q; q++) spec_add(spec, &sn, *q);
                f++;
            } else {
                while (*f >= '0' && *f <= '9') { spec_add(spec, &sn, *f); f++; }
            }
        }

        /* ---- length（吃掉，转换前统一提成 32/64 位） ----
         * 0 = int, 1 = 64 位（ll/l/z/j/t）, 2 = short/char, 3 = long double */
        int lng = 0;
        for (;; f++) {
            if (*f == 'l') { lng = 1; if (f[1] == 'l') f++; }
            else if (*f == 'h') { lng = 2; if (f[1] == 'h') f++; }
            else if (*f == 'L') { lng = 3; }
            else if (*f == 'z' || *f == 'j' || *f == 't') { lng = 1; }
            else break;
        }

        char conv = *f;
        char tmp[1024];
        int n = 0;

        switch (conv) {
        case 'd': case 'i': {
            long long v = (lng == 1) ? (long long)(int64_t)va64(c, &arg)
                                     : (long long)(int32_t)va32(c, &arg);
            if (lng == 2) v = (long long)(int32_t)(int16_t)v;   /* h → short */
            spec_add(spec, &sn, 'l'); spec_add(spec, &sn, 'l');
            spec_add(spec, &sn, 'd');
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = (lng == 1)
                ? va64(c, &arg)
                : (unsigned long long)va32(c, &arg);
            if (lng == 2) v &= 0xFFFFu;
            spec_add(spec, &sn, 'l'); spec_add(spec, &sn, 'l');
            spec_add(spec, &sn, conv);
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'c': {
            int v = (int32_t)va32(c, &arg);
            spec_add(spec, &sn, 'c');
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 's': {
            uint32_t sp = va32(c, &arg);
            const char *s = sp ? (const char *)(uintptr_t)sp : "(null)";
            spec_add(spec, &sn, 's');
            n = snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case 'p': {
            uint32_t v = va32(c, &arg);
            /* Windows 的 %p 是 8 位大写十六进制、带前导 0 */
            n = snprintf(tmp, sizeof tmp, "%08X", (unsigned)v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        case 'a': case 'A': {
            double v;
            if (lng == 3) {
                /* x86 80 位 long double 占 10 字节。游戏用不到，按 double
                 * 取值并把指针按 10 字节推进，避免后续参数错位。 */
                uint64_t raw = va64(c, &arg);
                arg += 2u;
                memcpy(&v, &raw, sizeof v);
            } else {
                v = va_f64(c, &arg);
            }
            spec_add(spec, &sn, conv);
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'n': {
            uint32_t np = va32(c, &arg);
            if (np) st32(np, out);
            continue;
        }
        default:
            /* 未知转换：原样输出，别静默吞掉 */
            PUT('%');
            if (conv) PUT(conv);
            continue;
        }

        if (n < 0) { PUT('?'); continue; }
        if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
        for (int i = 0; i < n; i++) PUT(tmp[i]);
    }
#undef PUT

    if (buf && cap) {
        uint32_t end = (out < limit) ? out : limit;
        buf[end] = 0;
    }
    return (int)out;
}

static void h_stdio_common_vsprintf(CPU *c)
{
    uint32_t opt_lo = A(c, 0);
    uint32_t opt_hi = A(c, 1);
    uint32_t buf    = A(c, 2);
    uint32_t cap    = A(c, 3);
    uint32_t fmtp   = A(c, 4);
    uint32_t locale = A(c, 5);
    uint32_t arglst = A(c, 6);
    (void)opt_lo; (void)opt_hi; (void)locale;

    if (!fmtp) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    const char *fmt = (const char *)(uintptr_t)fmtp;
    if (g_vsprintf_log > 0) {
        g_vsprintf_log--;
        fprintf(stderr, "[crt] vsprintf buf=%08x cap=%u fmt=\"%.120s\"\n",
                buf, cap, fmt);
    }

    int n = fmt_vsprintf(c, buf ? (char *)(uintptr_t)buf : NULL, cap, fmt, arglst);
    if (g_vsprintf_log < 12 && buf && n >= 0 && n < (int)cap) {
        /* 头几次把结果也打出来，直接看格式化对不对 */
        static int shown = 0;
        if (shown < 6) {
            shown++;
            fprintf(stderr, "[crt]   -> \"%.120s\"\n", (const char *)(uintptr_t)buf);
        }
    }
    c->eax = (uint32_t)n;
    cdecl_ret(c);
}


/* ====================================================================
 * __stdio_common_vfprintf
 *
 * 调用约定（RVA 0x7f8767 实测）：cdecl 24 字节，调用方 add esp,0x18
 *   +0   _Options 低     +4  _Options 高
 *   +8   _Stream         +12 _Format
 *   +16  _Locale         +20 _ArgList
 *
 * 复用上面的 x86 va_list 解析器：先按 cap=0 求长度，再分配、再格式化，
 * 所以输出不会被截断（截断会让日志和 UI 文本静默出错）。
 * ==================================================================== */
extern int host_crt_stdio_write(uint32_t guest_fp, const void *buf, uint32_t n);

/* 可选的输出重定向：调试期把 printf 转到宿主 stderr，便于观察 */
static int g_echo_stdout = 1;

static void h_stdio_common_vfprintf(CPU *c)
{
    uint32_t opt_lo = A(c, 0);
    uint32_t opt_hi = A(c, 1);
    uint32_t stream = A(c, 2);
    uint32_t fmtp   = A(c, 3);
    uint32_t locale = A(c, 4);
    uint32_t arglst = A(c, 5);
    (void)opt_lo; (void)opt_hi; (void)locale;

    if (!fmtp) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    const char *fmt = (const char *)(uintptr_t)fmtp;

    /* 第一遍：只求长度 */
    int need = fmt_vsprintf(c, NULL, 0, fmt, arglst);
    if (need < 0) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    uint32_t cap = (uint32_t)need + 1u;
    char *buf = malloc(cap);
    if (!buf) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    int n = fmt_vsprintf(c, buf, cap, fmt, arglst);
    if (n < 0) n = 0;

    int wrote = host_crt_stdio_write(stream, buf, (uint32_t)n);

    if (g_echo_stdout && wrote >= 0 && n > 0) {
        static int echoes = 0;
        if (echoes < 200) {
            echoes++;
            fprintf(stderr, "[guest printf] %.*s", n, buf);
        }
    }
    free(buf);

    /* 失败返回负数（UCRT 语义） */
    c->eax = (wrote < 0) ? (uint32_t)-1 : (uint32_t)n;
    cdecl_ret(c);
}

/* ---- __stdio_common_vsprintf_s ----
 * 安全版 vsprintf：多一个 _BufferCount 之后的 size 参数，语义上遇到
 * 截断要返回 -1 并清空缓冲。参数布局与 vsprintf 相同（UCRT 的 _s 变体
 * 在 __stdio_common 层面共用同一形态）。 */
static void h_stdio_common_vsprintf_s(CPU *c)
{
    uint32_t opt_lo = A(c, 0), opt_hi = A(c, 1);
    uint32_t buf = A(c, 2), cap = A(c, 3), fmtp = A(c, 4);
    uint32_t locale = A(c, 5), arglst = A(c, 6);
    (void)opt_lo; (void)opt_hi; (void)locale;

    if (!fmtp || !buf) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    int n = fmt_vsprintf(c, (char *)(uintptr_t)buf, cap,
                         (const char *)(uintptr_t)fmtp, arglst);
    if (n < 0 || (uint32_t)n >= cap) {
        if (cap) ((char *)(uintptr_t)buf)[0] = 0;   /* 截断即清空 */
        c->eax = (uint32_t)-1;
    } else {
        c->eax = (uint32_t)n;
    }
    cdecl_ret(c);
}

/* ---- __stdio_common_vsscanf ----
 * 参数：_Options(8), _Buffer, _BufferCount, _Format, _Locale, _ArgList
 * 游戏用它解析配置文件。实现 %d %u %x %f %s %c 这些常用转换。 */
static int fmt_vsscanf(CPU *c, const char *buf, const char *fmt, uint32_t arglist)
{
    uint32_t arg = arglist;
    int assigned = 0;
    const char *s = buf;

    for (const char *f = fmt; *f; f++) {
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            continue;
        }
        if (*f != '%') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (*s != *f) return assigned;
            s++;
            continue;
        }
        f++;
        if (*f == '%') {
            while (*s == ' ' || *s == '\t') s++;
            if (*s != '%') return assigned;
            s++;
            continue;
        }
        /* 宽度 */
        int width = 0;
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        /* 长度 */
        int lng = 0;
        for (;; f++) {
            if (*f == 'l') { lng = 1; if (f[1] == 'l') f++; }
            else if (*f == 'h') { lng = 2; if (f[1] == 'h') f++; }
            else if (*f == 'z' || *f == 'j' || *f == 't') { lng = 1; }
            else break;
        }
        char conv = *f;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) return assigned;

        switch (conv) {
        case 'd': case 'i': {
            char *end; long long v = strtoll(s, &end, (conv == 'i') ? 0 : 10);
            if (end == s) return assigned;
            s = end;
            uint32_t p = va32(c, &arg);
            if (!p) break;
            if (lng == 1) { st32(p, (uint32_t)v); st32(p + 4, (uint32_t)((uint64_t)v >> 32)); }
            else if (lng == 2) st16(p, (uint16_t)v);
            else st32(p, (uint32_t)v);
            assigned++;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            char *end;
            int base = (conv == 'o') ? 8 : ((conv == 'u') ? 10 : 16);
            unsigned long long v = strtoull(s, &end, base);
            if (end == s) return assigned;
            s = end;
            uint32_t p = va32(c, &arg);
            if (!p) break;
            if (lng == 1) { st32(p, (uint32_t)v); st32(p + 4, (uint32_t)(v >> 32)); }
            else if (lng == 2) st16(p, (uint16_t)v);
            else st32(p, (uint32_t)v);
            assigned++;
            break;
        }
        case 'f': case 'e': case 'g': {
            char *end; double v = strtod(s, &end);
            if (end == s) return assigned;
            s = end;
            uint32_t p = va32(c, &arg);
            if (!p) break;
            uint64_t raw; memcpy(&raw, &v, 8);
            st32(p, (uint32_t)raw); st32(p + 4, (uint32_t)(raw >> 32));
            assigned++;
            break;
        }
        case 's': {
            uint32_t p = va32(c, &arg);
            if (!p) break;
            int i = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
                   (width == 0 || i < width)) {
                st8(p + (uint32_t)i, (uint8_t)*s);
                i++; s++;
            }
            st8(p + (uint32_t)i, 0);
            assigned++;
            break;
        }
        case 'c': {
            uint32_t p = va32(c, &arg);
            if (!p) break;
            st8(p, (uint8_t)*s);
            s++;
            assigned++;
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

static void h_stdio_common_vsscanf(CPU *c)
{
    uint32_t opt_lo = A(c, 0), opt_hi = A(c, 1);
    uint32_t buf = A(c, 2), cap = A(c, 3), fmtp = A(c, 4);
    uint32_t locale = A(c, 5), arglst = A(c, 6);
    (void)opt_lo; (void)opt_hi; (void)cap; (void)locale;

    if (!buf || !fmtp) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }
    c->eax = (uint32_t)fmt_vsscanf(c, (const char *)(uintptr_t)buf,
                                   (const char *)(uintptr_t)fmtp, arglst);
    cdecl_ret(c);
}

/* 返回 1 = 已处理 */
int host_crt_stdio_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    const char *n = strchr(name, '!');
    n = n ? n + 1 : name;
    if      (!strcmp(n, "__stdio_common_vsprintf"))   { h_stdio_common_vsprintf(c);   return 1; }
    else if (!strcmp(n, "__stdio_common_vsprintf_s")) { h_stdio_common_vsprintf_s(c); return 1; }
    else if (!strcmp(n, "__stdio_common_vfprintf"))   { h_stdio_common_vfprintf(c);   return 1; }
    else if (!strcmp(n, "__stdio_common_vsscanf"))    { h_stdio_common_vsscanf(c);    return 1; }
    return 0;
}

int host_crt_stdio_count(void) { return 4; }
