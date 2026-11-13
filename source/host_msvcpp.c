/* host_msvcpp.c -- minimal C++ stream implementation for MSVCP140 (stage one: stop it crashing)
 *
 * ==========================================================================
 * 1. Why this is needed, and why the implementation is "minimal"
 *
 * The game uses std::stringstream to split strings on a delimiter (typical use:
 * parsing the version string "1.2.3", console command arguments). This is a private
 * ABI of the MSVC runtime: the member layout of `basic_ios` /
 * `basic_iostream` / `basic_streambuf` is an internal compiler convention,
 * and reproducing it under another compiler is difficult.
 *
 * Layout reverse-engineered from the game code (RVA 0x25c990, basic_stringstream ctor):
 *
 *     [stream + 0x00]  vtable = 0xb3ee54, its [1] = 0x68
 *     [stream + 0x18]  stringbuf subobject
 *     [stream + 0x68]  basic_ios subobject (lea ecx,[ebx+0x68]; call basic_ios)
 *     [stream + 0xa0]  _Mystrbuf = 0x68 + 0x38
 *
 * And the inlined getline body (RVA 0x25d680) obtains the streambuf like this:
 *     mov eax,[stream]      ; vtable
 *     mov eax,[eax+4]       ; = 0x68 (virtual base offset)
 *     mov ecx,[eax+stream+0x38]   ; = [stream+0xa0]
 * Exactly matching the above -- the layout is confirmed.
 *
 * The goal of stage one is to **let the game pass through here without crashing**: all
 * reads return EOF at once, so the getline loop never runs and the split result is empty.
 *
 * Known cost: the string is never really split. If "version parsing failed" / "console
 * argument empty" style bugs appear later, this is the spot -- then upgrade to stage two
 * (real stringbuf read/write pointers per the MSVC layout) rather than diving head-first
 * into a complete C++ stream implementation.
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
/* MSVC x86 member functions are __thiscall: this in ecx, arguments on the stack, and
 * the callee cleans them (really the caller, but COM/member functions follow the
 * stdcall pop count handled by the callee -- here "pop return address + clear n bytes". */
static int g_msvcp_trace = 0;   /* set to 8 to trace esp in/out call by call */
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

/* MSVC stream state bits (std::ios_base::iostate) */
#define GOODBIT  0x00
#define EOFLBIT  0x01
#define FAILBIT  0x02
#define BADBIT   0x04

/* Layout (reverse-engineered from RVA 0x25c990 / 0x25d680):
 *   stream + 0x68           basic_ios subobject (its this)
 *   basic_ios_this + 0x0c   state bits iostate
 *   basic_ios_this + 0x38   _Mystrbuf
 * And getline checks the state with `test byte [0x68 + stream + 0xc]`
 *  -- exactly basic_ios_this + 0xc. */
#define IOS_OFF       0x68u
#define IOS_STATE_OFF 0x0cu      /* relative to basic_ios's this */
#define MYSTRBUF_OFF  0xa0u      /* relative to stream */

static int g_msvcp_log = 0;   /* set to 8 to see the construction order */

/* ---- construction ---- */

/* `basic_ios::basic_ios()`  (RVA call site 0x65c9d4, this = stream+0x68)
 * Responsibility: clear _Mystrbuf, set the state bits to good.
 * The vtable is written by the game code around the call; we leave it alone. */
static void msvcp_basic_ios_ctor(CPU *c)
{
    uint32_t self = c->ecx;              /* __thiscall: this in ecx */
    if (self) {
        st32(self + 0x38u, 0u);                    /* _Mystrbuf = null */
        st8(self + IOS_STATE_OFF, (uint8_t)GOODBIT); /* state = good */
        st32(self + 0x34u, 0u);                    /* _Tiestr = null */
    }
    if (g_msvcp_log > 0) {
        g_msvcp_log--;
        fprintf(stderr, "[msvcp] basic_ios::ctor(this=%08x)\n", self);
    }
    R(c, 0);
}

/* `basic_iostream::basic_iostream(streambuf*)`
 * Call site RVA 0x65c9f0. Note that the call site pushes **two** values:
 *     0065c9da  push 0        ; 2nd argument
 *     0065c9e6  push edi      ; 1st argument = streambuf
 *     0065c9e7  mov ecx, ebx  ; this = stream
 * so clean 8 bytes, not 4 -- cleaning only 4 unbalances the caller's stack, and then
 * every [ebp-x] reads a misaligned value (observed symptom: the memcpy destination turns
 * into 0xfffffffe, i.e. some -2 return value is treated as a pointer). */
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
/* `basic_streambuf::basic_streambuf()` (RVA call site 0x65ca1a, this = stringbuf)
 *
 * ★ This constructor is not a no-op. The code following sub_0025c990 shows the game
 *   writing six "pointer to pointer" fields right after construction:
 *       0x25ca74  mov ecx,[edi+0xc]; mov [ecx],esi
 *       0x25ca79  mov eax,[edi+0x1c]; mov [eax],esi
 *       0x25ca7e  mov eax,[edi+0x2c]; mov [eax],edx
 *       0x25ca83  mov eax,[edi+0x10]; mov [eax],esi
 *       0x25ca8d  mov eax,[edi+0x20]; mov [eax],esi
 *       ...
 *   In other words the six slots such as [this+0xc] must **already point at writable
 *   memory**; doing nothing leaves stack garbage (-2) and the next step writes 0xfffffffe.
 *
 *   Here they all point at a reserved area inside the object (+0x40..+0x58), zeroed.
 *   The stringbuf subobject sits at stream+0x18, and stream is at least 0xa4 bytes,
 *   so +0x40..+0x58 falls inside the object and does not step on anything else. */
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
        st32(self + 0x38u, 0u);   /* buffer end */
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
 * sentry: checks the stream state, returns true only when good. Stage one is always good. */
static void msvcp_Ipfx(CPU *c)
{
    (void)c->ecx; (void)A(c, 0);
    c->eax = 1;                          /* true */
    R(c, 4);
}

/* ---- streambuf reads: stage one returns EOF for all of them ---- */

/* `basic_streambuf::sgetc()` -- returns the current character without advancing; EOF is
 * expressed by traits' eof() = -1. */
/* These streambuf methods are **unimplemented** (always EOF / always 0).
 * With SB_TRACE=1 the call counts are printed, to tell whether the game really takes
 * this path -- if it does, anything using stream reads silently gets EOF. */
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
/* `basic_streambuf::sbumpc()` -- take the current character and advance */
static void msvcp_sbumpc(CPU *c)
{
    sb_note(1, "sbumpc");
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;
    R(c, 0);
}
/* `basic_streambuf::snextc()` -- advance, then take the character */
static void msvcp_snextc(CPU *c)
{
    sb_note(2, "snextc");
    (void)c->ecx;
    c->eax = 0xFFFFFFFFu;
    R(c, 0);
}
/* `basic_streambuf::uflow()` -- virtual function, called when the buffer is empty */
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
    c->eax = 0;                          /* 0 bytes read */
    c->edx = 0;
    R(c, 8);
}

/* ★★ 2026-11-10: `basic_streambuf::sputn(const char*, streamsize)`.
 *
 * Why it must be added: on real hardware **the pause key always crashes**; the site is
 *     [MISS] MSVCP140.dll!?sputn@?$basic_streambuf@DU...QAE_JPBD_J@Z
 *     [SEGV] sig=6  pc belongs to libc.so (symbol abort)
 * i.e. this import had no handler → the IAT slot was never filled in → the first call lands in the fallback abort.
 *
 * Why the pause key gets here: on ESC the game writes logs / refreshes the debug console
 * output, and that path goes through ostream → streambuf::sputn.
 *
 * Semantic choice: return n (claim everything was written) instead of 0.
 * The existing streambuf read stubs all return EOF, which shows there is no real buffer;
 * returning 0 would make the caller set failbit/badbit, possibly triggering another
 * exception path. Returning n lets the caller assume the write finished and continue.
 * Cost: this output really is discarded (it was a stub anyway), but game logic is unaffected.
 *
 * Layout (thiscall: this in ecx):
 *   [esp+4]  s (char*)          4 bytes
 *   [esp+8]  n (streamsize)     8 bytes (__int64)
 *   returns edx:eax = n
 * so clean 4 + 8 = 12 bytes. Do not use A(c,1) as the whole n -- it reads only the low 32 bits. */
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
 * Records the state bits. The sentry reads them afterwards to decide whether the stream is usable. */
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

/* ================= dispatch =================
 * The names are MSVC decorated (mangled) names; matching ignores the exact std::char_traits
 * parameterization -- keyword substrings are used, because full names are very long and easy to mistype. */
typedef struct { const char *needle; void (*fn)(CPU *); } msvcp_entry;

/* std::uncaught_exception(): returns __uncaught_exceptions() != 0.
 *
 * In guest code it is only used by RAII guards to decide "are we currently unwinding".
 * Our port has no real SEH unwinding (_CxxThrowException only diagnoses and then aborts),
 * so a "currently unwinding" state never exists -- returning false is the accurate answer,
 * not a fake stub that swallows errors. */
static void msvcp_uncaught_exception(CPU *c)  { c->eax = 0u; R(c, 0); }

/* basic_ostream::_Osfx -- "flush the buffer out" on destruction.
 * Our ostream is a stub anyway and has no buffer of its own, so there is nothing to do;
 * this is a semantic no-op, not error swallowing. */
static void msvcp_Osfx(CPU *c)  { R(c, 0); }

/* ★★ 2026-11-10: `basic_ostream<char>::operator<<(unsigned long)`.
 *
 * After adding sputn, the pause key moved to a different crash point:
 *     [MISS] MSVCP140.dll!??6?$basic_ostream@DU?$char_traits@D@std@@@std@@
 *                        QAEAAV01@K@Z
 * `??6` is the decorated-name prefix of operator<<; the trailing `K` = unsigned long.
 *
 * So the game formats output with `oss << someUnsignedLong` (writing logs / the debug
 * console). This is a **separate export** that does not go through the sputn IAT slot,
 * so adding sputn is not enough -- it must be provided separately.
 *
 * Semantics: __thiscall, this in ecx, one DWORD argument, returns ostream& (i.e. this).
 * We have no real buffer, so the value is simply discarded; returning this keeps `<<` chains going.
 *   [esp+4]  unsigned long value   4 bytes
 *   returns eax = this
 *
 * ★ Why an **exact** needle instead of "??6?$basic_ostream@":
 * operator<< has 20+ overloads (char/int/unsigned/long long/float/double/const char*...),
 * each cleaning a different number of stack bytes. A broad match would clean the wrong
 * size and silently misalign esp -- harder to debug than a crash. Prefer MISS, and add
 * new types one at a time as they show up. */
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

/* `basic_ostream<char>::operator<<(int)` -- same as above, 4-byte argument. */
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

/* `basic_ostream<char>::operator<<(char)` -- char is passed as 4 bytes (x86 promotion). */
static void msvcp_ostream_shl_char(CPU *c)
{
    uint32_t self = c->ecx;
    (void)A(c, 0);
    c->eax = self;
    R(c, 4);
}

/* `basic_ostream<char>::operator<<(const char*)` -- pointer, 4 bytes. */
static void msvcp_ostream_shl_str(CPU *c)
{
    uint32_t self = c->ecx;
    uint32_t p = A(c, 0);
    {
        static unsigned shown;
        if (shown++ < 6u) {
            /* Dump the string itself as far as possible, to identify which log line it is */
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
 * 2026-11-10: bulk completion of the remaining ostream / streambuf methods.
 *
 * History lesson: this area was being squeezed out one drop at a time. Each addition moved the game one
 * step forward, hit the next [MISS], and crashed again:
 *   sputn        → crash     → add
 *   operator<<K  → crash     → add
 *   flush        → crash     → add (this time)
 * Instead of repeated trial and error on real hardware, this time every related export is done in one pass.
 *
 * Full list obtained with objdump -p on isaac-ng.exe.unpacked (15 in total):
 *   ?sgetc  ?sbumpc  ?snextc  ?uflow  ?xsgetn   ← already present
 *   ?sputn  ?xsputn  ?sputc   ?showmanyc        ← sputn already present
 *   ?flush  ?put     ?widen   ?setstate       ← setstate already present
 *   ?setbuf ?sync    ?imbue                     ← already present
 *
 * Decorated-name decoding:
 *   QAE/QBE = public __thiscall (BE additionally means const)
 *   MAE     = protected __thiscall (virtual function, vtable call)
 *   return  H=int  D=char  _J=__int64  AAV<..>@=class reference
 * ========================================================================== */

/* `basic_ostream::flush()` -- no arguments, returns ostream&. */
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

/* `basic_ostream::put(char)` -- returns ostream&.
 * On x86 the char argument occupies a 4-byte slot. */
static void msvcp_ostream_put(CPU *c)
{
    uint32_t self = c->ecx;
    (void)A(c, 0);
    c->eax = self;
    R(c, 4);
}

/* `basic_streambuf::xsputn(const char*, streamsize)` -- virtual function.
 * Returns the number of elements actually written. Like sputn, it claims success (returns n).
 *   [esp+4]  s (const char*)   4 bytes
 *   [esp+8]  n (streamsize)    8 bytes
 *   returns edx:eax = n
 * cleans 4 + 8 = 12 bytes. */
static void msvcp_xsputn(CPU *c)
{
    uint32_t nLo = A(c, 1);
    uint32_t nHi = A(c, 2);
    (void)A(c, 0);
    c->eax = nLo;
    c->edx = nHi;
    R(c, 12);
}

/* `basic_streambuf::sputc(char)` -- writes one character, returns int (traits::int_type).
 * On success it returns the character itself, EOF(-1) means failure. We have no real buffer,
 * so we return the character value and let the caller believe the write succeeded.
 *   [esp+4]  c (char)  4-byte slot
 *   cleans 4 bytes. */
static void msvcp_sputc(CPU *c)
{
    uint32_t ch = A(c, 0);
    c->eax = ch & 0xFFu;
    R(c, 4);
}

/* `basic_streambuf::showmanyc()` -- virtual function, returns __int64.
 * "how many characters can still be read". We have no readable data at all, so return 0.
 * No arguments, cleans 0 bytes. */
static void msvcp_showmanyc(CPU *c)
{
    c->eax = 0;
    c->edx = 0;
    R(c, 0);
}

/* `basic_ios::widen(char)` -- const member, returns char.
 * For the char version widen is the identity mapping (the locale does not change a single byte).
 *   [esp+4]  c (char)  4-byte slot
 *   cleans 4 bytes. */
static void msvcp_widen(CPU *c)
{
    c->eax = A(c, 0) & 0xFFu;
    R(c, 4);
}

/* ★★ 2026-11-10: `basic_ostream<char>::operator<<(unsigned long)`.
 *
 * After adding sputn, the pause key moved to a different crash point:
 *     [MISS] MSVCP140.dll!??6?$basic_ostream@DU?$char_traits@D@std@@@std@@
 *                        QAEAAV01@K@Z
 * `??6` is the decorated-name prefix of operator<<; K = unsigned long.
 *
 * So the game formats output with `oss << someUnsignedLong` (writing logs / the debug
 * console). This is a **separate export** that does not go through the sputn IAT slot,
 * so adding sputn is not enough -- it must be provided separately.
 *
 * Semantics: __thiscall, this in ecx, one DWORD argument, returns ostream& (i.e. this).
 * We have no real buffer, so the value is simply discarded; returning this keeps `<<` chains going.
 *   [esp+4]  unsigned long value
 *   returns eax = this
 *   cleans 4 bytes */
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
    /* ★ 2026-11-10: the remaining streambuf write methods in the PE import table. */
    { "?xsputn@?$basic_streambuf@",   msvcp_xsputn },
    { "?sputc@?$basic_streambuf@",    msvcp_sputc },
    { "?showmanyc@?$basic_streambuf@", msvcp_showmanyc },
    { "?widen@?$basic_ios@",          msvcp_widen },
    /* ★ 2026-11-10: root cause of the pause-key crash. See the msvcp_sputn comment. */
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
    /* ★ 2026-11-10: the latest pause-key crash point. flush is the last stage of the logging path. */
    { "?flush@?$basic_ostream@",   msvcp_ostream_flush },
    { "?put@?$basic_ostream@",     msvcp_ostream_put },
    /* ★ 2026-11-10: the second pause-key crash point.
     * The needle contains the **full argument encoding**, so overloads with different argument
     * sizes do not match by mistake. Prefix `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@`
     *   - QAE = public __thiscall, returns AAV01 = ostream&
     * The suffix holds only the argument encoding, tolerating minor VS version differences. */
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@K@Z",
                                      msvcp_ostream_shl_uint },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@H@Z",
                                      msvcp_ostream_shl_int },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@D@Z",
                                      msvcp_ostream_shl_char },
    { "??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@PBD@Z",
                                      msvcp_ostream_shl_str },
    /* ★ 2026-11-10: the second pause-key crash point. Note that it must come before the
     * broad match (the table is strstr'd in order, so the more specific entries come first). */
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
