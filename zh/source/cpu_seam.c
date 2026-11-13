/* guest_cpuid / guest_xgetbv —— 虚拟 x86 契约。
 *
 * 照抄 host_vita_first_fault.c 的实现（那份本身就是 "Copy of the working
 * PC virtual-CPU contract"）。这段描述的是翻译器支持的 x86 代码路径，
 * 与 ARM 宿主无关，所以两个平台应当一致。
 *
 * 关键点：必须声明 SSE/SSE2 支持，否则游戏自己的 CPU 检测会选到
 * 纯软件渲染路径。
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
