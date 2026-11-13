/* 被调用但没有翻译定义的函数 —— 按 repentogxm 的原则生成 loud stub。
 *
 * 原文（gen_all.py 头部）：
 *   "Untranslated functions become loud stubs, never empty ones. A stub that
 *    returns quietly is a wrong program that looks like a working one. Each
 *    one calls guest_fault with its own address and the mnemonic that blocked it."
 *
 * 2026-11-02 更新：原来这里的 11 个（sub_00259650 … sub_005700e0）不是
 * 「非代码」，而是被 bounds 分析误判的**真实函数** —— 它们是纯 C++ 虚函数，
 * 只出现在 vtable 里，递归下降看不见。现在已用 recomp/build_one.py 逐个
 * 翻译（全部 UNSUPPORTED = 0），函数体在 gen_extra/，注册在
 * guest_extra_table.c。这 11 条 loud stub 已删除，不再冒充它们。
 *
 * 保留的两条仍是被调用但没有翻译定义的地址，运行到会明确 fault。
 */
#include <stdint.h>
#include "guest.h"

#define LOUD_STUB(name, rva) \
    void name(CPU *__restrict c) { guest_fault(c, (uint32_t)rva, "stub: not translated"); }


/* 后发现的另外两个（全仓扫 call/def 差异时漏掉，链接时才暴露） */
LOUD_STUB(sub_00560c60, 0x00560c60)
LOUD_STUB(sub_00560e30, 0x00560e30)
