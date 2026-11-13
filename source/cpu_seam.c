/* guest_cpuid / guest_xgetbv -- the virtual x86 contract.
 *
 * Mirrors the implementation in host_vita_first_fault.c (which is itself
 * "Copy of the working PC virtual-CPU contract"). This describes the x86 code
 * paths the translator supports and has nothing to do with the ARM host, so
 * both platforms should agree.
 *
 * Key point: SSE/SSE2 support must be declared, otherwise the game's own CPU
 * probe picks the pure software rendering path.
 */
#include "guest.h"

void guest_cpuid(CPU *__restrict c)
{
    uint32_t leaf = c->eax;

    c->eax = c->ebx = c->ecx = c->edx = 0U;
    if (leaf == 0U) {
        c->eax = 1U;
        c->ebx = 0x756e6547U;     /* "Genu" */
        c->edx = 0x49656e69U;     /* "ineI" */
        c->ecx = 0x6c65746eU;     /* "ntel" */
    } else if (leaf == 1U) {
        c->eax = 0x00000663U;
        c->edx = (1U << 0)  |     /* x87 */
                 (1U << 8)  |     /* CMPXCHG8B */
                 (1U << 15) |     /* CMOV */
                 (1U << 24) |     /* FXSAVE/FXRSTOR */
                 (1U << 25) |     /* SSE */
                 (1U << 26);      /* SSE2 */
    } else if (leaf == 0x80000000U) {
        c->eax = 0x80000000U;
    }
}

void guest_xgetbv(CPU *__restrict c)
{
    c->eax = c->ecx == 0U ? 3U : 0U;
    c->edx = 0U;
}
