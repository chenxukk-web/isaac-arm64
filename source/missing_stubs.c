/* Functions that are called but have no translated definition -- loud stubs as
 * per the repentogxm principle.
 *
 * From the original text (gen_all.py header):
 *   "Untranslated functions become loud stubs, never empty ones. A stub that
 *    returns quietly is a wrong program that looks like a working one. Each
 *    one calls guest_fault with its own address and the mnemonic that blocked it."
 *
 * Update 2026-11-02: the 11 symbols formerly here (sub_00259650 ... sub_005700e0)
 * were not "non-code" but real functions misjudged by bounds analysis -- pure
 * C++ virtual functions appearing only in vtables, invisible to recursive
 * descent. Each has since been translated by recomp/build_one.py (all
 * UNSUPPORTED = 0), bodies in gen_extra/, registration in guest_extra_table.c.
 * Those 11 loud stubs are gone and no longer impersonate them.
 *
 * The two that remain are still called but untranslated; reaching them faults
 * explicitly.
 */
#include <stdint.h>
#include "guest.h"

#define LOUD_STUB(name, rva) \
    void name(CPU *__restrict c) { guest_fault(c, (uint32_t)rva, "stub: not translated"); }


/* Two more found later (missed by a whole-tree call/def diff; surfaced at link) */
LOUD_STUB(sub_00560c60, 0x00560c60)
LOUD_STUB(sub_00560e30, 0x00560e30)
