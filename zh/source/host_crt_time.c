/* Android CRT 层 —— 时间族与宽字符转换
 *
 * 两个坑：
 *
 * ① Windows 的 struct tm 是 9 个 int（36 字节），字段顺序与 POSIX 一致，
 *    但 Android 的 struct tm 多了 tm_gmtoff / tm_zone，sizeof 不一样。
 *    所以宿主 localtime_r 的结果**不能**直接把指针交给 guest —— 必须
 *    在低 4GB 里另开一块 36 字节按 Windows 布局写。游戏会读 [tm+0x10]
 *    (tm_mon)、[tm+0x14] (tm_year)，偏移必须对。
 *
 * ② Windows 的 wchar_t 是 16 位 UTF-16，Android 的是 32 位。mbstowcs 不
 *    能直接转交，得自己解码 UTF-8 / 编码 UTF-16。游戏用 0x104(260) 的
 *    缓冲，是典型的 MAX_PATH 风格调用。
 *
 * 所有参数量都从调用点实测，注释里标了 RVA。
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }
static char *G(uint32_t a) { return (char *)(uintptr_t)a; }

/* ---- 低 4GB 的 guest 侧缓冲 ---- */
#define TBASE 0x3A200000u
#define TSIZE 0x1000u
static uint32_t g_tbase;

static uint32_t tbase(void)
{
    if (g_tbase) return g_tbase;
    void *p = mmap((void *)(uintptr_t)TBASE, TSIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)TBASE) return 0;
    g_tbase = TBASE;
    return g_tbase;
}

/* 三块 36 字节的 struct tm：localtime / gmtime / 备用，避免互相踩 */
#define TM_LOCAL 0u
#define TM_GMT   1u
#define TM_TEMP  2u

static uint32_t tm_guest(unsigned slot)
{
    uint32_t b = tbase();
    return b ? b + slot * 64u : 0;
}

/* Windows struct tm（9 个 int，共 36 字节） */
static uint32_t tm_write(unsigned slot, const struct tm *t)
{
    uint32_t g = tm_guest(slot);
    if (!g || !t) return 0;
    st32(g + 0,  (uint32_t)t->tm_sec);
    st32(g + 4,  (uint32_t)t->tm_min);
    st32(g + 8,  (uint32_t)t->tm_hour);
    st32(g + 12, (uint32_t)t->tm_mday);
    st32(g + 16, (uint32_t)t->tm_mon);
    st32(g + 20, (uint32_t)t->tm_year);
    st32(g + 24, (uint32_t)t->tm_wday);
    st32(g + 28, (uint32_t)t->tm_yday);
    st32(g + 32, (uint32_t)t->tm_isdst);
    return g;
}

static void tm_read(uint32_t g, struct tm *t)
{
    memset(t, 0, sizeof *t);
    if (!g) return;
    t->tm_sec   = (int)ld32(g + 0);
    t->tm_min   = (int)ld32(g + 4);
    t->tm_hour  = (int)ld32(g + 8);
    t->tm_mday  = (int)ld32(g + 12);
    t->tm_mon   = (int)ld32(g + 16);
    t->tm_year  = (int)ld32(g + 20);
    t->tm_wday  = (int)ld32(g + 24);
    t->tm_yday  = (int)ld32(g + 28);
    t->tm_isdst = (int)ld32(g + 32);
}

static void ret64(CPU *c, int64_t v)
{
    c->eax = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    c->edx = (uint32_t)((uint64_t)v >> 32);
}

/* ---- _time64(__time64_t *ptime)  RVA 0x884681：cdecl 4 ---- */
static void h_time64(CPU *c)
{
    uint32_t p = A(c, 0);
    int64_t now = (int64_t)time(NULL);
    if (p) {
        st32(p, (uint32_t)((uint64_t)now & 0xFFFFFFFFu));
        st32(p + 4, (uint32_t)((uint64_t)now >> 32));
    }
    ret64(c, now);
    cdecl_ret(c);
}

/* ---- _localtime64(const __time64_t *ptime)  RVA 0x884697：cdecl 4 ---- */
static void h_localtime64(CPU *c)
{
    uint32_t p = A(c, 0);
    time_t t = 0;
    if (p) {
        uint64_t v = (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
        t = (time_t)(int64_t)v;
    } else {
        t = time(NULL);
    }
    struct tm out;
    if (!localtime_r(&t, &out)) { c->eax = 0; cdecl_ret(c); return; }
    c->eax = tm_write(TM_LOCAL, &out);
    cdecl_ret(c);
}

/* ---- _gmtime64(const __time64_t *ptime)  RVA 0x41fcda：cdecl 4 ---- */
static void h_gmtime64(CPU *c)
{
    uint32_t p = A(c, 0);
    time_t t = 0;
    if (p) {
        uint64_t v = (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
        t = (time_t)(int64_t)v;
    } else {
        t = time(NULL);
    }
    struct tm out;
    if (!gmtime_r(&t, &out)) { c->eax = 0; cdecl_ret(c); return; }
    c->eax = tm_write(TM_GMT, &out);
    cdecl_ret(c);
}

/* ---- _mkgmtime64(struct tm *t)  RVA 0x41fcbf：cdecl 4 ----
 * 把 guest 的 struct tm 当成 UTC 转回 time_t。timegm 是 GNU 扩展。 */
static void h_mkgmtime64(CPU *c)
{
    uint32_t p = A(c, 0);
    if (!p) { ret64(c, -1); cdecl_ret(c); return; }
    struct tm t;
    tm_read(p, &t);
    time_t v = timegm(&t);
    ret64(c, (int64_t)v);
    cdecl_ret(c);
}

/* ---- strftime(buf, max, fmt, tm)  RVA 0x7f71d6：cdecl 16 ---- */
static void h_strftime(CPU *c)
{
    uint32_t buf = A(c, 0), mx = A(c, 1), fmtp = A(c, 2), tmp = A(c, 3);
    if (!buf || !mx || !fmtp || !tmp) { c->eax = 0; cdecl_ret(c); return; }

    struct tm t;
    tm_read(tmp, &t);
    /* 宿主 strftime 写进临时缓冲，再拷到 guest —— 格式串是 guest 指针，
     * 但内容是普通 ASCII，宿主可以直接读。 */
    char tmpbuf[1024];
    size_t n = strftime(tmpbuf, sizeof tmpbuf, G(fmtp), &t);
    if (n == 0) { c->eax = 0; cdecl_ret(c); return; }
    if (n >= mx) n = mx - 1;
    memcpy(G(buf), tmpbuf, n);
    G(buf)[n] = 0;
    c->eax = (uint32_t)n;
    cdecl_ret(c);
}

/* ---- mbstowcs_s(&ret, wcstr, sizeInWords, mbstr, count)  RVA 0x962f4c ----
 * cdecl 20。UTF-8 → UTF-16。返回值是 errno_t（0 = 成功）。 */
static void h_mbstowcs_s(CPU *c)
{
    uint32_t pret  = A(c, 0);
    uint32_t wcstr = A(c, 1);
    uint32_t words = A(c, 2);
    uint32_t mbstr = A(c, 3);
    uint32_t count = A(c, 4);
    (void)count;

    if (!mbstr) {
        if (pret) st32(pret, 0);
        if (wcstr && words) st16(wcstr, 0);
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    const unsigned char *s = (const unsigned char *)(uintptr_t)mbstr;
    uint32_t written = 0;          /* 已写 16 位单元数（不含 \0） */
    uint32_t cap = (wcstr && words) ? words - 1u : 0u;   /* 预留结尾 L'\0' */

    while (*s) {
        uint32_t cp;
        if (*s < 0x80) { cp = *s++; }
        else if ((*s & 0xE0) == 0xC0) { cp = (uint32_t)(*s++ & 0x1F) << 6;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F); }
        else if ((*s & 0xF0) == 0xE0) { cp = (uint32_t)(*s++ & 0x0F) << 12;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F) << 6;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F); }
        else if ((*s & 0xF8) == 0xF0) { cp = (uint32_t)(*s++ & 0x07) << 18;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F) << 12;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F) << 6;
                                        if (*s) cp |= (uint32_t)(*s++ & 0x3F); }
        else { cp = 0xFFFD; s++; }

        if (cp >= 0x10000u) {
            /* 代理对 */
            if (written + 2u > cap) break;
            cp -= 0x10000u;
            st16(wcstr + written * 2u, (uint16_t)(0xD800u + (cp >> 10)));
            st16(wcstr + written * 2u + 2u, (uint16_t)(0xDC00u + (cp & 0x3FFu)));
            written += 2u;
        } else {
            if (written + 1u > cap) break;
            st16(wcstr + written * 2u, (uint16_t)cp);
            written += 1u;
        }
    }
    if (wcstr && words) st16(wcstr + written * 2u, 0);
    if (pret) st32(pret, written);
    c->eax = 0;               /* S_OK */
    cdecl_ret(c);
}

/* ---- wcstombs_s —— UTF-16 → UTF-8（表里有，但游戏没直接调用） ---- */
int host_crt_time_wcstombs_s(CPU *c)
{
    uint32_t pret  = A(c, 0);
    uint32_t mbstr = A(c, 1);
    uint32_t bytes = A(c, 2);
    uint32_t wcstr = A(c, 3);
    uint32_t count = A(c, 4);
    (void)count;

    if (!wcstr) {
        if (pret) st32(pret, 0);
        if (mbstr && bytes) st8(mbstr, 0);
        c->eax = 0; cdecl_ret(c); return 1;
    }
    uint32_t cap = (mbstr && bytes) ? bytes - 1u : 0u;
    uint32_t o = 0;
    for (uint32_t i = 0; ; i++) {
        uint32_t u = ld16(wcstr + i * 2u);
        if (!u) break;
        if (u >= 0xD800u && u <= 0xDBFFu) {
            uint32_t lo = ld16(wcstr + (i + 1) * 2u);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            }
        }
        char b[4]; int n;
        if (u < 0x80) { b[0] = (char)u; n = 1; }
        else if (u < 0x800) { b[0] = (char)(0xC0 | (u >> 6));
                              b[1] = (char)(0x80 | (u & 0x3F)); n = 2; }
        else if (u < 0x10000) { b[0] = (char)(0xE0 | (u >> 12));
                                b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
                                b[2] = (char)(0x80 | (u & 0x3F)); n = 3; }
        else { b[0] = (char)(0xF0 | (u >> 18));
               b[1] = (char)(0x80 | ((u >> 12) & 0x3F));
               b[2] = (char)(0x80 | ((u >> 6) & 0x3F));
               b[3] = (char)(0x80 | (u & 0x3F)); n = 4; }
        if (o + (uint32_t)n > cap) break;
        for (int k = 0; k < n; k++) st8(mbstr + o + (uint32_t)k, (uint8_t)b[k]);
        o += (uint32_t)n;
    }
    if (mbstr && bytes) st8(mbstr + o, 0);
    if (pret) st32(pret, o);
    c->eax = 0;
    cdecl_ret(c);
    return 1;
}

/* ---- _get_osfhandle(fd)  RVA 0x9965db：cdecl 4 ----
 * 直接把 CRT 的 fd 当 Win32 HANDLE 返回。KERNEL32 的 ReadFile/WriteFile
 * 需要接受这种值 —— 见 host_android.c 里的说明。 */
static void h_get_osfhandle(CPU *c)
{
    int32_t fd = (int32_t)A(c, 0);
    c->eax = (uint32_t)fd;
    cdecl_ret(c);
}

int host_crt_time_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    const char *n = strchr(name, '!');
    n = n ? n + 1 : name;
    if      (!strcmp(n, "_time64"))       { h_time64(c);       return 1; }
    else if (!strcmp(n, "_localtime64"))  { h_localtime64(c);  return 1; }
    else if (!strcmp(n, "_gmtime64"))     { h_gmtime64(c);     return 1; }
    else if (!strcmp(n, "_mkgmtime64"))   { h_mkgmtime64(c);   return 1; }
    else if (!strcmp(n, "strftime"))      { h_strftime(c);     return 1; }
    else if (!strcmp(n, "mbstowcs_s"))    { h_mbstowcs_s(c);   return 1; }
    else if (!strcmp(n, "wcstombs_s"))    { return host_crt_time_wcstombs_s(c); }
    else if (!strcmp(n, "_get_osfhandle")){ h_get_osfhandle(c);return 1; }
    return 0;
}

int host_crt_time_count(void) { return 8; }
