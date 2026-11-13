/* 游戏自带的字符串切分工具（SplitAppend / SplitConstruct）。
 *
 * 原始实现是：构造 MSVC 的 basic_stringstream，循环调用游戏自己的
 * std::getline 包装（RVA 0x25d680 一带），再把 token 逐个 push_back
 * 进 vector<string>，最后销毁 stream。
 *
 * 我们移植了 iostream 的骨架（basic_ios/basic_streambuf/basic_iostream
 * 的构造析构、_Ipfx、sgetc/uflow/sbumpc），但 getline 那一层依赖的是
 * MSVCP140 内部的完整状态机。实测结果：source 有 21 个字符，
 * SplitConstruct 却产出 0 个元素 —— vector 三指针全 0，调用方紧接着
 * 取 last-0x18 做析构，读到 0xfffffffc 崩掉。
 *
 * 这两个函数的临时 stream 对象从不外泄，所以不必复现 iostream 的
 * 逐字节行为：直接按分隔符切分，保持 guest 可见的 std::string /
 * std::vector ABI 即可。下面每一步 guest 调用都做 esp 校验，
 * ABI 一旦漂移立刻报错而不是带着错栈跑下去。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "guest.h"

/* ---- std::string / std::vector 的 guest 侧布局（MSVC x86）---- */
#define GUEST_STRING_BYTES       24u   /* char buf[16] + size + capacity */
#define GUEST_STRING_LENGTH      0x10u
#define GUEST_STRING_CAPACITY    0x14u
#define GUEST_STRING_SSO_CAPACITY 15u

#define SPLIT_APPEND_RVA    0x00259650u
#define SPLIT_CONSTRUCT_RVA 0x002597b0u

/* 被调用的 guest 函数（都在 gen/ 里，声明照抄） */
void sub_000079f0(CPU *__restrict c);   /* string copy-construct */
void sub_00007c80(CPU *__restrict c);   /* string assign(ptr,len) */
void sub_00007be0(CPU *__restrict c);   /* string::~string */
void sub_0025ceb0(CPU *__restrict c);   /* vector::_Emplace_reallocate */

static int split_fault(CPU *__restrict c, uint32_t rva, const char *message)
{
    guest_fault(c, rva, message);
    return 0;
}

static int exact_return(CPU *__restrict c, uint32_t saved, uint32_t rva,
                        const char *operation)
{
    if (c->fault)
        return 0;
    if (c->esp != saved)
        return split_fault(c, rva, operation);
    return 1;
}

static int string_copy_construct(CPU *__restrict c, uint32_t dst,
                                 uint32_t src, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, src);
    gpush(c, 0xabc079f0u);
    c->ecx = dst;
    sub_000079f0(c);
    return exact_return(c, saved, rva, "split: string copy ABI drift");
}

static int string_assign_bytes(CPU *__restrict c, uint32_t dst,
                               uint32_t bytes, uint32_t length, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, length);
    gpush(c, bytes);
    gpush(c, 0xabc07c80u);
    c->ecx = dst;
    sub_00007c80(c);
    return exact_return(c, saved, rva, "split: string assign ABI drift");
}

static int string_destroy(CPU *__restrict c, uint32_t string, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, 0xabc07be0u);
    c->ecx = string;
    sub_00007be0(c);
    return exact_return(c, saved, rva, "split: string dtor ABI drift");
}

static int validate_vector(CPU *__restrict c, uint32_t vector, uint32_t rva)
{
    uint32_t begin = ld32(vector);
    uint32_t end = ld32(vector + 4u);
    uint32_t capacity = ld32(vector + 8u);

    if ((begin == 0u && (end != 0u || capacity != 0u)) ||
        begin > end || end > capacity ||
        (end - begin) % GUEST_STRING_BYTES != 0u ||
        (capacity - begin) % GUEST_STRING_BYTES != 0u)
        return split_fault(c, rva, "split: malformed vector<string>");
    return 1;
}

static int vector_append(CPU *__restrict c, uint32_t vector,
                         uint32_t string, uint32_t rva)
{
    uint32_t end, capacity, saved;
    if (!validate_vector(c, vector, rva))
        return 0;
    end = ld32(vector + 4u);
    capacity = ld32(vector + 8u);
    saved = c->esp;

    if (end != capacity) {
        if (capacity - end < GUEST_STRING_BYTES)
            return split_fault(c, rva, "split: partial vector capacity");
        gpush(c, string);
        gpush(c, 0xabc079f0u);
        c->ecx = end;
        sub_000079f0(c);
        if (!exact_return(c, saved, rva, "split: vector copy ABI drift"))
            return 0;
        st32(vector + 4u, end + GUEST_STRING_BYTES);
        return 1;
    }

    /* ====================================================================
     * 自己做扩容，不用 vector::_Emplace_reallocate(RVA 0x25ceb0)。
     *
     * 2026-11-04 实测：调用它之后 vector 三指针会指向互相重叠的块 ——
     * 三次追加后 begin 从 63436010 → 63438010 → 63436010，
     * 而 end - begin 只有 48（2 个元素），容量却是 72（3 个）。
     * 于是切分出来的第 3 段（"ScoreMenu.anm2"）丢失，
     * anm2 的 spritesheet 路径变成空串 → png 从不加载 → 画面纯白。
     *
     * 这个函数的内部状态机依赖 MSVC vector 的完整语义，我们只在
     * 这一个场景用到它，自己实现更可控：按 2 倍增长分配新块、
     * 逐个复制元素、释放旧块、更新三指针。全程走 guest 的
     * malloc/free（IAT 0x606504 / 0x6064fc），保证块在 guest 堆上。
     * ==================================================================== */
    {
        uint32_t begin = ld32(vector);
        uint32_t count = (end - begin) / GUEST_STRING_BYTES;
        uint32_t newcap = capacity - begin;
        uint32_t want = (count + 1u) * GUEST_STRING_BYTES;
        uint32_t fresh, i;

        if (newcap < want) {
            newcap = newcap ? newcap * 2u : GUEST_STRING_BYTES;
            while (newcap < want) newcap *= 2u;
        }
        /* guest malloc */
        {
            uint32_t es = c->esp;
            gpush(c, newcap);
            gpush(c, 0xabcd0001u);
            guest_call(c, ld32(0x3c606504u));
            fresh = c->eax;
            c->esp = es;
            if (c->fault) return split_fault(c, rva, "split: guest malloc fault");
        }
        if (!fresh) return split_fault(c, rva, "split: guest malloc failed");

        /* 逐个复制既有元素（copy-construct，SSO/堆块由 guest 代码处理） */
        for (i = 0; i < count; i++) {
            uint32_t es = c->esp;
            gpush(c, begin + i * GUEST_STRING_BYTES);
            gpush(c, 0xabcd0002u);
            c->ecx = fresh + i * GUEST_STRING_BYTES;
            sub_000079f0(c);
            c->esp = es;
            if (c->fault) return split_fault(c, rva, "split: copy fault");
        }
        /* 追加新元素 */
        {
            uint32_t es = c->esp;
            gpush(c, string);
            gpush(c, 0xabcd0003u);
            c->ecx = fresh + count * GUEST_STRING_BYTES;
            sub_000079f0(c);
            c->esp = es;
            if (c->fault) return split_fault(c, rva, "split: append fault");
        }
        /* 释放旧块（若存在） */
        if (begin) {
            uint32_t es = c->esp;
            gpush(c, begin);
            gpush(c, 0xabcd0004u);
            guest_call(c, ld32(0x3c6064fcu));
            c->esp = es;
        }
        st32(vector, fresh);
        st32(vector + 4u, fresh + (count + 1u) * GUEST_STRING_BYTES);
        st32(vector + 8u, fresh + newcap);
        {
            static int on4 = -1;
            if (on4 < 0) on4 = getenv("SPLIT_TRACE") != NULL;
            if (on4) {
                fprintf(stderr, "[split]   grow2: count=%u newcap=%u -> begin=%08x "
                                "end=%08x cap=%08x\n", count, newcap,
                        fresh, ld32(vector + 4u), ld32(vector + 8u));
                fflush(NULL);
            }
        }
        return 1;
    }
}

static int validate_source(CPU *__restrict c, uint32_t source, uint32_t rva)
{
    uint32_t length, capacity, bytes;
    if (!source)
        return split_fault(c, rva, "split: null source string");
    length = ld32(source + GUEST_STRING_LENGTH);
    capacity = ld32(source + GUEST_STRING_CAPACITY);
    if (length > 0x7fffffffu)
        return split_fault(c, rva, "split: source exceeds MSVC string max");
    if (capacity < length)
        return split_fault(c, rva, "split: malformed source string");
    bytes = capacity < 16u ? source : ld32(source);
    if (length && !bytes)
        return split_fault(c, rva, "split: null source storage");
    return 1;
}

static int split_bytes(CPU *__restrict c, uint32_t source, uint32_t vector,
                       uint8_t delimiter, int construct_vector, uint32_t rva)
{
    uint32_t entry_sp = c->esp;
    uint32_t saved_ebp = c->ebp, saved_ebx = c->ebx;
    uint32_t saved_esi = c->esi, saved_edi = c->edi;
    uint32_t snapshot, token, bytes, length, position;

    if (!vector)
        return split_fault(c, rva, "split: null output vector");
    if (entry_sp < 2u * GUEST_STRING_BYTES)
        return split_fault(c, rva, "split: guest stack underflow");

    /* 这是 vector 构造函数而不是赋值：原始代码在构造 stream 之前
     * 就把三个指针清零了。 */
    if (construct_vector) {
        st32(vector, 0u);
        st32(vector + 4u, 0u);
        st32(vector + 8u, 0u);
    }
    if (!validate_source(c, source, rva) ||
        (!construct_vector && !validate_vector(c, vector, rva)))
        return 0;

    /* 原始 stringstream 会先把整个 source 拷一份快照再开始切 ——
     * 当 source 是 vector 自己的元素、而 push_back 引起扩容时，
     * 这个快照是必需的。照抄。 */
    snapshot = entry_sp - 2u * GUEST_STRING_BYTES;
    token = snapshot + GUEST_STRING_BYTES;
    if (!guest_stack_set(c, snapshot, rva))
        return 0;
    if (!string_copy_construct(c, snapshot, source, rva))
        return 0;

    st32(token, 0u);
    st32(token + GUEST_STRING_LENGTH, 0u);
    st32(token + GUEST_STRING_CAPACITY, GUEST_STRING_SSO_CAPACITY);
    st8(token, 0u);

    length = ld32(snapshot + GUEST_STRING_LENGTH);
    bytes = ld32(snapshot + GUEST_STRING_CAPACITY) < 16u
        ? snapshot : ld32(snapshot);
    /* SPLIT_TRACE=1：打印源字符串和切分结果，确认长度是否被读错。 */
    {
        static int on = -1;
        if (on < 0) on = getenv("SPLIT_TRACE") != NULL;
        if (on) {
            char t[96] = {0};
            for (uint32_t z = 0; z < 90u && z < length; z++)
                t[z] = (char)ld8(bytes + z);
            fprintf(stderr, "[split] source@%08x len=%u cap=%u \"%s\" delim=%c\n",
                    source, length, ld32(snapshot + GUEST_STRING_CAPACITY), t,
                    delimiter >= 0x20 ? (char)delimiter : '?');
            fflush(NULL);
        }
    }
    position = 0u;
    for (;;) {
        uint32_t start = position;
        while (position < length && ld8(bytes + position) != delimiter)
            ++position;
        {
            static int on2 = -1;
            if (on2 < 0) on2 = getenv("SPLIT_TRACE") != NULL;
            int ok_a = string_assign_bytes(c, token, bytes + start,
                                           position - start, rva);
            if (on2) {
                char tk[64] = {0};
                uint32_t tl = ld32(token + GUEST_STRING_LENGTH);
                for (uint32_t z = 0; z < 60u && z < tl; z++)
                    tk[z] = (char)ld8(ld32(token + GUEST_STRING_CAPACITY) < 16u
                                      ? token : ld32(token) + z * 0);
                fprintf(stderr, "[split]   seg[%u] start=%u end=%u assign=%d fault=%d\n",
                        (unsigned)(start), start, position, ok_a, c->fault);
                fflush(NULL);
            }
            if (!ok_a) return 0;
            if (!vector_append(c, vector, token, rva)) {
                if (on2) { fprintf(stderr, "[split]   vector_append failed\n"); fflush(NULL); }
                return 0;
            }
        }
        if (position >= length)
            break;
        ++position;                 /* getline 吃掉分隔符 */
        if (position > length)
            break;
    }

    if (!string_destroy(c, token, rva) ||
        !string_destroy(c, snapshot, rva))
        return 0;

    c->ebp = saved_ebp;
    c->ebx = saved_ebx;
    c->esi = saved_esi;
    c->edi = saved_edi;
    if (!guest_stack_set(c, entry_sp, rva))
        return 0;
    c->eax = vector;
    (void)gpop(c);                  /* 两个包装都是裸 RET */
    return 1;
}

void sub_00259650(CPU *__restrict c)
{
    uint32_t source;
    uint32_t vector;
    uint8_t delimiter;
    source = c->ecx;
    vector = ld32(guest_stack_address(c, c->esp + 4u, 4U, SPLIT_APPEND_RVA));
    delimiter = (uint8_t)c->edx;
    (void)split_bytes(c, source, vector, delimiter, 0, SPLIT_APPEND_RVA);
}

void sub_002597b0(CPU *__restrict c)
{
    uint32_t vector;
    uint32_t source;
    uint8_t delimiter;
    vector = c->ecx;
    source = c->edx;
    delimiter = ld8(guest_stack_address(c, c->esp + 4u, 1U, SPLIT_CONSTRUCT_RVA));
    (void)split_bytes(c, source, vector, delimiter, 1, SPLIT_CONSTRUCT_RVA);
}

/* ============================================================================
 * sub_002599c0 —— 路径规范化（接管的第二处）
 *
 * 原始实现是 402 条翻译指令：把 anm2 路径按 '/' 切成 vector<string>，
 * 过滤掉 ".." 段，再用 '/' 重新拼起来，最后与 spritesheet 的相对路径
 * 合并写入 dst。
 *
 * 2026-11-04 实测：翻译版切分出的 3 段在过滤循环后只剩 2 段，
 * dst 最终是空串 —— 于是 anm2 的 spritesheet 路径为 ""，
 * png 从不加载，画面纯白。切分本身（host_split 的 SplitConstruct）
 * 是正确的，丢在 0x2599c0 内部的过滤/重组上。
 *
 * 这个函数的语义已经很清楚，且临时对象不外泄，所以直接接管：
 *   入参  ecx = anm2 路径 char*      ("gfx/ui/ScoreMenu.anm2")
 *         edx = 相对路径 char*       ("ScoreMenu.png")
 *         [esp+4] = dst std::string* （24 字节，栈上）
 *   输出  dst 里写合并后的完整路径
 *
 * ABI 与原本一致：裸 ret，调用方用 add esp,8 清两个参数。
 * ==========================================================================*/
#define PATH_JOIN_RVA 0x002599c0u

/* 读 guest 的 C 字符串到本地缓冲，返回长度（截断时返回 -1） */
static int read_guest_cstr(uint32_t ptr, char *out, int cap)
{
    int i = 0;
    if (!ptr) { out[0] = 0; return 0; }
    for (; i < cap - 1; i++) {
        char ch = (char)ld8(ptr + (uint32_t)i);
        out[i] = ch;
        if (!ch) return i;
    }
    out[cap - 1] = 0;
    return -1;
}

/* 把 s 写入 guest 的 std::string（MSVC 32 位：buf[16] + size + capacity） */
static void guest_string_set(CPU *__restrict c, uint32_t dst,
                             const char *s, int len)
{
    if (!dst || len < 0) return;
    if (len < 16) {
        for (int i = 0; i < len; i++) st8(dst + (uint32_t)i, (uint8_t)s[i]);
        st8(dst + (uint32_t)len, 0u);
        st32(dst + GUEST_STRING_LENGTH, (uint32_t)len);
        st32(dst + GUEST_STRING_CAPACITY, GUEST_STRING_SSO_CAPACITY);
        return;
    }
    {
        uint32_t saved = c->esp, buf;
        gpush(c, (uint32_t)len + 1u);
        gpush(c, 0xabc599c0u);
        guest_call(c, ld32(0x3c606504u));       /* guest malloc */
        c->esp = saved;
        if (c->fault) return;
        buf = c->eax;
        if (!buf) return;
        for (int i = 0; i < len; i++) st8(buf + (uint32_t)i, (uint8_t)s[i]);
        st8(buf + (uint32_t)len, 0u);
        st32(dst, buf);
        st32(dst + GUEST_STRING_LENGTH, (uint32_t)len);
        st32(dst + GUEST_STRING_CAPACITY, (uint32_t)len);
    }
}

void sub_002599c0(CPU *__restrict c)
{
    uint32_t dst = ld32(guest_stack_address(c, c->esp + 4u, 4u, PATH_JOIN_RVA));
    uint32_t anmp = c->ecx;
    uint32_t relp = c->edx;

    char anm[512], rel[512];
    char out[1024];
    int anl = read_guest_cstr(anmp, anm, (int)sizeof anm);
    int rll = read_guest_cstr(relp, rel, (int)sizeof rel);
    int o = 0;
    int trace = getenv("PNC_TRACE") != NULL;

    if (anl < 0) anl = (int)sizeof anm - 1;
    if (rll < 0) rll = (int)sizeof rel - 1;

    /* 1) 取 anm2 路径的目录部分（最后一个 '/' 之前，含 '/'） */
    {
        int cut = -1;
        for (int i = 0; i < anl; i++) if (anm[i] == '/' || anm[i] == '\\') cut = i;
        for (int i = 0; i <= cut && o < (int)sizeof out - 1; i++) {
            char ch = anm[i];
            out[o++] = (ch == '\\') ? '/' : ch;
        }
    }
    /* 2) 拼上相对路径 */
    for (int i = 0; i < rll && o < (int)sizeof out - 1; i++) {
        char ch = rel[i];
        out[o++] = (ch == '\\') ? '/' : ch;
    }
    out[o] = 0;

    /* 3) 规范化：逐段处理 "." 与 ".." */
    {
        char norm[1024];
        int n = 0;
        int i = 0, abs = (out[0] == '/');
        if (abs) norm[n++] = '/';
        while (i < o) {
            int seg = i;
            while (i < o && out[i] != '/') i++;
            {
                int len = i - seg;
                if (len == 0) { /* 连续斜杠 */ }
                else if (len == 1 && out[seg] == '.') { /* "." 跳过 */ }
                else if (len == 2 && out[seg] == '.' && out[seg + 1] == '.') {
                    /* ".." 回退一节 */
                    while (n > (abs ? 1 : 0) && norm[n - 1] != '/') n--;
                    if (n > (abs ? 1 : 0)) n--;
                } else {
                    if (n > 0 && norm[n - 1] != '/') norm[n++] = '/';
                    for (int k = 0; k < len; k++) norm[n++] = out[seg + k];
                }
            }
            if (i < o && out[i] == '/') i++;
        }
        norm[n] = 0;
        guest_string_set(c, dst, norm, n);
        if (trace) {
            fprintf(stderr, "[pnc2] anm2=\"%s\" rel=\"%s\" -> \"%s\" (dst=%08x)\n",
                    anm, rel, norm, dst);
            fflush(NULL);
        }
    }
    c->eax = dst;
    (void)gpop(c);          /* 裸 ret */
}
