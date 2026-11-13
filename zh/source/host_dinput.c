/* host_dinput.c —— 最小可用的 IDirectInput8 假对象
 *
 * ==========================================================================
 * 一、为什么必须有它
 *
 * 游戏启动时会：
 *     DirectInput8Create(..., &g_pDI)          // g_pDI @ 0x803010
 *     ...
 *     mov byte [0xc007de], 1                   // ← 无条件置 1
 *     call sub_0059d3b0
 *        └─ if ([0xc007de]) { eax = [0x803010];
 *                             ecx = [eax];            // ← NULL 解引用
 *                             call [ecx+0x10]; }      // EnumDevices
 *
 * 也就是说：**游戏假设 DirectInput8Create 永远成功**，失败标志位照样置 1。
 * 这是游戏的假设，不是我们能改的（gen/ 不可改）。
 *
 * 在真机上这个假设成立：DINPUT8.dll 是 Windows 系统组件，
 * DirectInput8Create 只在极端情况下失败。所以正确做法是**提供这个对象**，
 * 让创建成功，然后由 EnumDevices 诚实地报告「没有设备」。
 *
 * 这也正是真机上「拔掉手柄」时的行为：接口存在，枚举结果为空。
 *
 * （注：gen 里其实有一段 `#if defined(__vita__)` 的分支把 0xc007de 强制
 *  置 0 来绕过这个问题。我们没有定义 __vita__，也不想为了绕过一个宿主
 *  缺失去改编译宏 —— 那会让别处的 Vita 专用路径也被打开。）
 *
 * ==========================================================================
 * 二、对象布局（全部在低 4GB）
 *
 *   [g_pDI] ──► obj:  obj[0] = &vtable[0]
 *                     obj[4] = 引用计数
 *
 *   vtable（COM 标准顺序，槽里放的是 guest 可 call 的 token）：
 *     +0x00 QueryInterface   +0x10 EnumDevices
 *     +0x04 AddRef           +0x14 GetDeviceStatus
 *     +0x08 Release          +0x18 RunControlPanel
 *     +0x0c CreateDevice     +0x1c Initialize
 *
 * 所有方法都是 x86 stdcall（COM 约定）：返回时弹返回地址 + 清参数。
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ---- 低 4GB 内存 ---- */
#define DI_BASE   0x3A400000u
#define DI_SIZE   0x2000u
#define DI_OBJ    (DI_BASE + 0x0000u)      /* 对象 */
#define DI_VT     (DI_BASE + 0x0100u)      /* vtable */
#define DI_DEV_OBJ (DI_BASE + 0x0400u)     /* 键盘设备对象 */
#define DI_DEV_VT  (DI_BASE + 0x0500u)     /* 键盘设备 vtable */

/* ---- token 段（和 GL 的 0x7D / kernel32 的 0x7E 都不冲突） ---- */
#define DI_TOKEN_BASE 0x7F000000u
/* 设备方法单独一段，避免和 IDirectInput8 的混在一起 */
#define DI_DEV_TOKEN_BASE 0x7F010000u
#define DI_DEV_T_COUNT 32u

enum {
    DI_T_QI = 0, DI_T_ADDREF, DI_T_RELEASE, DI_T_CREATEDEVICE,
    DI_T_ENUMDEVICES, DI_T_GETDEVSTATUS, DI_T_RUNCP, DI_T_INIT,
    DI_T_COUNT
};

static int g_di_state;      /* 0 未初始化 1 就绪 -1 失败 */
static uint32_t g_di_obj;

/* HRESULT / DI 返回码 */
#define S_OK                    0x00000000u
#define S_FALSE                 0x00000001u
#define E_NOINTERFACE           0x80004002u
#define DIERR_DEVICENOTREG      0x80070032u   /* ERROR_NOT_REGISTERED */
#define DI_NOTATTACHED          S_FALSE

/* IID_IUnknown {00000000-0000-0000-C000-000000000046}
 * IID_IDirectInput8A {BF798030-483A-4DA2-99BD-576B4C0BAD}}
 * 游戏只检查 S_OK/失败，所以只认 IUnknown 就够；其余给 E_NOINTERFACE。 */
static int iid_is_unknown(uint32_t p)
{
    static const uint8_t unk[16] = {
        0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
        0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    if (!p) return 0;
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = ld8(p + (uint32_t)i);
    return memcmp(buf, unk, 16) == 0;
}

/* ---- 方法实现 ---- */

/* QueryInterface(this, riid, ppvObject)  12 字节 */
static void di_QueryInterface(CPU *c)
{
    (void)A(c, 0);
    uint32_t riid = A(c, 1), ppv = A(c, 2);
    if (ppv) {
        if (iid_is_unknown(riid)) {
            st32(ppv, g_di_obj);
            c->eax = S_OK;
        } else {
            st32(ppv, 0);
            c->eax = E_NOINTERFACE;
        }
    } else {
        c->eax = E_NOINTERFACE;
    }
    R(c, 12);
}

/* AddRef(this)  4 字节 */
static void di_AddRef(CPU *c)
{
    uint32_t self = A(c, 0);
    uint32_t n = (self ? ld32(self + 4u) : 0u) + 1u;
    if (self) st32(self + 4u, n);
    c->eax = n;
    R(c, 4);
}

/* Release(this)  4 字节 */
static void di_Release(CPU *c)
{
    uint32_t self = A(c, 0);
    uint32_t n = self ? ld32(self + 4u) : 0u;
    if (n) n--;
    if (self) st32(self + 4u, n);
    c->eax = n;
    R(c, 4);
}

/* ==========================================================================
 * IDirectInputDevice8 假键盘设备（2026-11-05）
 *
 * 为什么必须做
 * ------------
 * 真机上手柄/键盘完全无反应。查日志发现：
 *   DirectInput8Create  调用 1 次（成功，拿到假 IDirectInput8）
 *   GetAsyncKeyState    调用 0 次
 *   PeekMessage         调用 0 次
 * 游戏既不读 GetAsyncKeyState，也不跑窗口消息循环 —— 它只认
 * DirectInput 的设备对象。而这个桩原本恒返回 DIERR_DEVICENOTREG，
 * 于是游戏拿不到任何输入设备，整条输入链是死的。
 *
 * 所以：CreateDevice 必须成功，并返回一个能用的
 * IDirectInputDevice8，其 GetDeviceState 把 host 侧的按键表整块拷出去。
 *
 * vtable 顺序严格按 dinput.h 的 IDirectInputDevice8A（32 个方法）。
 * 顺序错一个就会调到别的方法上，症状会很离谱。
 * ========================================================================== */

extern const unsigned char *host_key_table(void);

#define DI_OK        0x00000000u
#define DIERR_INPUTLOST 0x8007001Eu

/* 常用方法 */
/* QueryInterface(this, riid, ppvObject)  12 字节
 *
 * ★ 2026-11-05 修了一个致命参数错误：原来写的是 A(c,1)，
 * 那是 **riid**（输入的 GUID 指针，指向 guest 的只读常量区），
 * 往那里写数据直接段错误。输出参数是第 3 个，即 A(c,2)。
 * 崩溃日志：host_dinput_dynamic+0xa0，dev-method #1 之后立刻挂。 */
static void dev_QueryInterface(CPU *c)
{
    uint32_t ppv = A(c, 2);
    if (ppv) st32(ppv, DI_DEV_OBJ);
    c->eax = S_OK;
    R(c, 12);
}
static void dev_AddRef(CPU *c)         { c->eax = 2; R(c, 4); }
static void dev_Release(CPU *c)        { c->eax = 1; R(c, 4); }

/* GetCapabilities(this, lpDIDevCaps)  8 字节 */
static void dev_GetCapabilities(CPU *c)
{
    uint32_t p = A(c, 1);
    if (p) {
        /* DIDEVICEOBJECTINSTANCE / DIDEVCAPS 结构，只需把
         * dwDevType 写成键盘（DIDEVTYPE_KEYBOARD = 3）并给一个合理的
         * dwAxes/dwButtons/dwPOVs，游戏据此判断这是键盘。 */
        for (int i = 0; i < 44; i++) st8(p + (uint32_t)i, 0);
        st32(p + 0x00, 44);          /* dwSize */
        st32(p + 0x04, 3);           /* dwDevType = DI8DEVTYPE_KEYBOARD */
    }
    c->eax = S_OK; R(c, 8);
}

/* EnumObjects(this, cb, ref, flags)  16 字节 —— 不枚举，返回成功空集 */
static void dev_EnumObjects(CPU *c) { c->eax = S_OK; R(c, 16); }

/* GetProperty / SetProperty 各 16 / 16 字节 —— 一律成功 */
/* GetProperty(this, rguidProp, pdiph) — this+2 = 12 字节。原写 16 会多清 4 字节，
 * 直接把栈掀翻（guest stack pointer outside bounds → SIGABRT）。 */
static void dev_GetProperty(CPU *c) { c->eax = S_OK; R(c, 12); }
/* SetProperty(this, rguidProp, pdiph) — this+2 = 12 字节。同上，已修。 */
static void dev_SetProperty(CPU *c) { c->eax = S_OK; R(c, 12); }

/* Acquire / Unacquire */
static void dev_Acquire(CPU *c)   { c->eax = S_OK; R(c, 4); }
static void dev_Unacquire(CPU *c) { c->eax = S_OK; R(c, 4); }

/* ★ GetDeviceState(this, cbData, lpvData)  12 字节
 *
 * 这是整条链的关键：把 host 侧 256 字节按键表拷到 guest 缓冲。
 * DirectInput 键盘的约定是每个字节一个键，0x80 = 按下，0 = 释放。
 * 我们的 u_key_state 存的是 0/1，这里乘 0x80 转成 DInput 语义。 */
static void dev_GetDeviceState(CPU *c)
{
    uint32_t cb  = A(c, 1);
    uint32_t buf = A(c, 2);
    if (buf && cb) {
        const unsigned char *keys = host_key_table();
        uint32_t n = cb < 256u ? cb : 256u;
        for (uint32_t i = 0; i < n; i++)
            st8(buf + i, keys[i] ? 0x80u : 0x00u);
        /* 超出 256 的部分清零，避免游戏读到未初始化内存 */
        for (uint32_t i = n; i < cb && i < 1024u; i++) st8(buf + i, 0u);
    }
    c->eax = S_OK; R(c, 12);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags)  20 字节 */
/* ★ GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags)  20 字节
 *
 * 2026-11-05：游戏用**缓冲事件模式**读输入（索引 10），并且是标准的
 * 两步探测：
 *     GetDeviceData(cb=20, buf=NULL,     cap=0xFFFFFFFF)  ← 问"有多少事件"
 *     GetDeviceData(cb=20, buf=<有效>,   cap=<上一步的返回值>)
 *
 * 所以必须有真正的**队列**：先 diff 当前键状态入队，再按 cap 出队。
 * 原来直接拿"上一帧快照"对比、不回队列，导致第一步永远返回 0，
 * 游戏于是认为没有输入。
 *
 * DIDEVICEOBJECTDATA（x86，20 字节）：
 *   +0x00 dwOfs   键码   +0x04 dwData 0x80按下/0释放
 *   +0x08 dwTimeStamp     +0x0C dwSequence     +0x10 dwAppData
 *
 * 队列满时**丢弃最新的**：宁可丢事件也不能写越界。
 * 键盘事件量很小（一次最多几个键），64 深度绰绰有余。
 */
#define EVQ_MAX 64

typedef struct {
    unsigned char ofs;
    unsigned char data;
    unsigned long seq;
} din_ev;

static din_ev        g_evq[EVQ_MAX];
static unsigned      g_evq_head, g_evq_tail;
static unsigned char dev_prev_keys[256];
static unsigned long dev_seq;

/* 把当前键状态与上次快照的差异入队 */
/* ★★★ 2026-11-06：VK → DIK 扫描码映射（手柄无响应的真正原因）★★★
 *
 * 日志显示事件已经**成功交付**给游戏（队列 32 个、交付日志都有），
 * 但游戏毫无反应。原因是 dwOfs 的语义错了：
 *
 *   · 我们传的是 **VK 码**（空格 = 0x20 = 32）
 *   · DirectInput 的数据格式用 **DIK 扫描码**（空格 = 0x39 = 57）
 *
 * 游戏在 SetDataFormat 时给的是标准 DIK 布局，它按 dwOfs 当 DIK 索引
 * 去查自己的按键表，所以收到 "0x20" 会当成 DIK 0x20 = 'D' 键，
 * 而不是空格。按 A 没反应、所有键都错位，正是这个。
 *
 * 这是一张精简表，覆盖游戏全部默认键位（WASD/方向键/空格/E/Q/Tab/ESC）。
 * 未列出的键原样返回（至少不会是负数，不会破坏内存）。
 */
static unsigned char vk_to_dik(uint32_t vk)
{
    switch (vk) {
        case 0x1B: return 0x01;   /* ESC      */
        case 0x09: return 0x0F;   /* Tab      */
        case 0x0D: return 0x1C;   /* Enter    */
        case 0x20: return 0x39;   /* Space    */
        case 0x10: return 0x2A;   /* Shift    */
        case 0x11: return 0x1D;   /* Ctrl     */
        case 0x12: return 0x38;   /* Alt      */

        /* 字母：DIK 是打乱的，必须逐个别映射 */
        case 'Q': return 0x10;  case 'W': return 0x11;
        case 'E': return 0x12;  case 'R': return 0x13;
        case 'T': return 0x14;  case 'Y': return 0x15;
        case 'U': return 0x16;  case 'I': return 0x17;
        case 'O': return 0x18;  case 'P': return 0x19;
        case 'A': return 0x1E;  case 'S': return 0x1F;
        case 'D': return 0x20;  case 'F': return 0x21;
        case 'G': return 0x22;  case 'H': return 0x23;
        case 'J': return 0x24;  case 'K': return 0x25;
        case 'L': return 0x26;
        case 'Z': return 0x2C;  case 'X': return 0x2D;
        case 'C': return 0x2E;  case 'V': return 0x2F;
        case 'B': return 0x30;  case 'N': return 0x31;
        case 'M': return 0x32;

        /* 数字 1-9,0 */
        case '1': return 0x02;  case '2': return 0x03;
        case '3': return 0x04;  case '4': return 0x05;
        case '5': return 0x06;  case '6': return 0x07;
        case '7': return 0x08;  case '8': return 0x09;
        case '9': return 0x0A;  case '0': return 0x0B;

        /* 方向键（扩展键，DIK 在 0xC8..0xCD） */
        case 0x26: return 0xC8;   /* Up    */
        case 0x28: return 0xD0;   /* Down  */
        case 0x25: return 0xCB;   /* Left  */
        case 0x27: return 0xCD;   /* Right */

        default:   return (unsigned char)(vk & 0xFF);
    }
}

static void evq_fill(void)
{
    const unsigned char *cur = host_key_table();
    for (uint32_t i = 0; i < 256u; i++) {
        unsigned char now = cur[i] ? 0x80u : 0x00u;
        if (now == dev_prev_keys[i]) continue;
        dev_prev_keys[i] = now;

        unsigned nt = (g_evq_tail + 1u) % EVQ_MAX;
        if (nt == g_evq_head) return;      /* 满：丢弃，防止覆盖未读事件 */
        g_evq[g_evq_tail].ofs  = (unsigned char)i;
        g_evq[g_evq_tail].data = now;
        g_evq[g_evq_tail].seq  = ++dev_seq;
        g_evq_tail = nt;
    }
}

static unsigned evq_count(void)
{
    return (g_evq_tail + EVQ_MAX - g_evq_head) % EVQ_MAX;
}

static void dev_GetDeviceData(CPU *c)
{
    uint32_t cbObjectData = A(c, 1);
    uint32_t rgdod       = A(c, 2);
    uint32_t pInOut      = A(c, 3);

    evq_fill();                 /* 每次调用先把新变化收进队列 */

    /* 2026-11-05 诊断：只在“空格键有值”时打印。
     * 前面按次数限制的写法看不到注入后的状态（注入发生在 45 秒，
     * 那时调用次数早超了）。改成看内容触发。 */
    {
        const unsigned char *tb = host_key_table();
        if (tb[32]) {
            static unsigned d2;
            if (d2++ < 10u) {
                fprintf(stderr, "[dinput] injection effective: u_key[32]=%u prev[32]=%u queue=%u\n",
                        tb[32], dev_prev_keys[32], evq_count());
                fflush(NULL);
            }
        }
    }

    uint32_t cap = 0;
    if (pInOut) cap = ld32(pInOut);
    unsigned avail = evq_count();

    {
        static unsigned dbg;
        if (dbg++ < 4u) {
            fprintf(stderr, "[dinput] GetDeviceData(cb=%u, buf=%08x, cap=%u) queue=%u\n",
                    cbObjectData, rgdod, cap, avail);
            fflush(NULL);
        }
    }

    /* 只问数量：buf 为空，或 cap==INFINITE(0xFFFFFFFF) */
    if (!rgdod || cap == 0xFFFFFFFFu) {
        if (pInOut) st32(pInOut, avail);
        c->eax = S_OK; R(c, 20); return;
    }

    /* 正常取事件 */
    uint32_t n = 0;
    if (cbObjectData >= 16u) {
        while (n < cap && g_evq_head != g_evq_tail) {
            uint32_t off = rgdod + n * cbObjectData;
            st32(off + 0x00u, (uint32_t)vk_to_dik(g_evq[g_evq_head].ofs));
            st32(off + 0x04u, (uint32_t)g_evq[g_evq_head].data);
            st32(off + 0x08u, 0u);
            st32(off + 0x0Cu, (uint32_t)g_evq[g_evq_head].seq);
            st32(off + 0x10u, 0u);
            g_evq_head = (g_evq_head + 1u) % EVQ_MAX;
            n++;
        }
    }

    if (pInOut) st32(pInOut, n);

    static unsigned shown;
    if (n && shown++ < 12u) {
        fprintf(stderr, "[dinput] -> delivered %u key events\n", n);
        fflush(NULL);
    }

    c->eax = S_OK;
    R(c, 20);
}

/* SetDataFormat / SetEventNotification / SetCooperativeLevel */
static void dev_SetDataFormat(CPU *c)        { c->eax = S_OK; R(c, 8); }
static void dev_SetEventNotification(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_SetCooperativeLevel(CPU *c)  { c->eax = S_OK; R(c, 12); }

static void dev_GetObjectInfo(CPU *c) { c->eax = S_OK; R(c, 16); }
static void dev_GetDeviceInfo(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_RunControlPanel(CPU *c) { c->eax = S_OK; R(c, 12); }
static void dev_Initialize(CPU *c) { c->eax = S_OK; R(c, 16); }

/* 力反馈/效果相关 —— 键盘不支持，如实返回失败 */
/* CreateEffect(this, rguid, lpeff, ppeff, punkOuter) — this+4 = 20 字节。 */
static void dev_CreateEffect(CPU *c) { c->eax = 0x80004005u; R(c, 20); }
static void dev_EnumEffects(CPU *c) { c->eax = S_OK; R(c, 16); }
static void dev_GetEffectInfo(CPU *c) { c->eax = 0x80004005u; R(c, 12); }
static void dev_GetForceFeedbackState(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_SendForceFeedbackCommand(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_EnumCreatedEffectObjects(CPU *c) { c->eax = S_OK; R(c, 16); }
/* Escape(this, pdie) — this+1 = 8 字节。 */
static void dev_Escape(CPU *c) { c->eax = S_OK; R(c, 8); }

/* ★ Poll(this) —— 每次读输入前游戏会先 Poll。键盘是即时状态，直接成功。 */
static void dev_Poll(CPU *c) { c->eax = S_OK; R(c, 4); }

static void dev_SendDeviceData(CPU *c) { c->eax = S_OK; R(c, 20); }
static void dev_EnumEffectsInFile(CPU *c) { c->eax = S_OK; R(c, 20); }
/* WriteEffectToFile(this, szFileName, dwEntries, rgdife, dwFlags) — this+4 = 20 字节。 */
static void dev_WriteEffectToFile(CPU *c) { c->eax = S_OK; R(c, 20); }
static void dev_BuildActionMap(CPU *c) { c->eax = S_OK; R(c, 16); }
/* SetActionMap(this, pdiam, szUserName, dwFlags) — this+3 = 16 字节。 */
static void dev_SetActionMap(CPU *c) { c->eax = S_OK; R(c, 16); }
/* GetImageInfo(this, pdii) — this+1 = 8 字节。 */
static void dev_GetImageInfo(CPU *c) { c->eax = S_OK; R(c, 8); }

/* vtable：严格按 IDirectInputDevice8A 的声明顺序 */
static void (*const s_dev_fns[DI_DEV_T_COUNT])(CPU *) = {
    dev_QueryInterface,        /*  0 */
    dev_AddRef,                /*  1 */
    dev_Release,               /*  2 */
    dev_GetCapabilities,       /*  3 */
    dev_EnumObjects,           /*  4 */
    dev_GetProperty,           /*  5 */
    dev_SetProperty,           /*  6 */
    dev_Acquire,               /*  7 */
    dev_Unacquire,             /*  8 */
    dev_GetDeviceState,        /*  9 ★ */
    dev_GetDeviceData,         /* 10 */
    dev_SetDataFormat,         /* 11 */
    dev_SetEventNotification,  /* 12 */
    dev_SetCooperativeLevel,   /* 13 */
    dev_GetObjectInfo,         /* 14 */
    dev_GetDeviceInfo,         /* 15 */
    dev_RunControlPanel,       /* 16 */
    dev_Initialize,            /* 17 */
    dev_CreateEffect,          /* 18 */
    dev_EnumEffects,           /* 19 */
    dev_GetEffectInfo,         /* 20 */
    dev_GetForceFeedbackState, /* 21 */
    dev_SendForceFeedbackCommand, /* 22 */
    dev_EnumCreatedEffectObjects, /* 23 */
    dev_Escape,                /* 24 */
    dev_Poll,                  /* 25 ★ */
    dev_SendDeviceData,        /* 26 */
    dev_EnumEffectsInFile,     /* 27 */
    dev_WriteEffectToFile,     /* 28 */
    dev_BuildActionMap,        /* 29 */
    dev_SetActionMap,          /* 30 */
    dev_GetImageInfo           /* 31 */
};

/* CreateDevice(this, rguid, lplpDevice, pUnkOuter)  16 字节
 *
 * ★ 2026-11-05：从“恒失败”改成返回可用的假键盘设备。
 *   这是手柄/键盘能工作的前提。 */
static void di_CreateDevice(CPU *c)
{
    uint32_t ppDev = A(c, 2);

    /* 建立设备对象与 vtable（幂等） */
    for (unsigned i = 0; i < DI_DEV_T_COUNT; i++)
        st32(DI_DEV_VT + i * 4u, DI_DEV_TOKEN_BASE + i * 4u);
    st32(DI_DEV_OBJ + 0u, DI_DEV_VT);   /* lpVtbl */
    st32(DI_DEV_OBJ + 4u, 0x12345678u); /* 占用标记，便于排查 */

    if (ppDev) st32(ppDev, DI_DEV_OBJ);

    fprintf(stderr, "[dinput] ✓ CreateDevice -> fake keyboard device @ %08x\n", DI_DEV_OBJ);
    fflush(NULL);
    c->eax = S_OK;
    R(c, 16);
}

/* EnumDevices(this, dwDevType, callback, pvRef, dwFlags)  20 字节
 * ★ 这里返回 S_OK 且**不调用回调** —— 等于「枚举成功，设备数为 0」。
 *   这正是真机上没有手柄时的结果。 */
/* 枚举回调用的 DIDEVICEINSTANCEW（x86 32 位，共 1100 字节）
 *   +0x00 DWORD dwSize
 *   +0x04 GUID  guidInstance (16)
 *   +0x14 GUID  guidProduct  (16)
 *   +0x24 DWORD dwDevType
 *   +0x28 WCHAR tszInstanceName[260]
 *   +0x230 WCHAR tszProductName[260]
 *   +0x438 GUID  guidFFDriver
 *   +0x448 WORD  wUsagePage
 *   +0x44A WORD  wUsage */
#define DI_INST_BUF (DI_BASE + 0x0800u)

/* GUID_DEVCLASS_KEYBOARD = {4D36E96B-E325-11CE-BFC1-08002BE10318}
 * 内存布局是小端：Data1(4) Data2(2) Data3(2) Data4(8) */
static const unsigned char kbd_guid[16] = {
    0x6B, 0xE9, 0x36, 0x4D,   /* Data1 */
    0x25, 0xE3,               /* Data2 */
    0xCE, 0x11,               /* Data3 */
    0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18  /* Data4 */
};

/* EnumDevices(this, dwDevType, callback, pvRef, dwFlags)  20 字节
 *
 * ★ 2026-11-05：从这里开始报告**一个键盘设备**。
 *
 * 实测发现游戏只调了 EnumDevices(wType=4 = DIDEVTYPE_KEYBOARD)，
 * 得到“0 个设备”后就再也不调 CreateDevice 了 —— 整个输入层是死的。
 * 这就是“手柄/键盘完全无反应”的根因。
 *
 * 现在构造一个 DIDEVICEINSTANCEW 并调 guest 回调，游戏就会认为
 * 存在键盘，进而 CreateDevice，再通过 GetDeviceState 拿到按键。
 *
 * 回调签名：BOOL CALLBACK cb(LPCDIDEVICEINSTANCE, LPVOID) —— cdecl。
 * 返回 TRUE(1) 继续枚举，FALSE(0) 停止。 */
static void di_EnumDevices(CPU *c)
{
    uint32_t wType  = A(c, 1);
    uint32_t cbAddr = A(c, 2);
    uint32_t pvRef  = A(c, 3);

    static int shown;
    if (shown++ < 3) {
        fprintf(stderr, "[dinput] EnumDevices(wType=%08x) -> reporting 1 keyboard device\n", wType);
        fflush(NULL);
    }

    if (!cbAddr) { c->eax = S_OK; R(c, 20); return; }

    /* --- 构造 DIDEVICEINSTANCEW --- */
    uint32_t inst = DI_INST_BUF;
    for (int i = 0; i < 0x44C; i++) st8(inst + (uint32_t)i, 0u);
    st32(inst + 0x00, 0x44Cu);              /* dwSize */
    for (int i = 0; i < 16; i++) st8(inst + 0x04u + (uint32_t)i, kbd_guid[i]);
    for (int i = 0; i < 16; i++) st8(inst + 0x14u + (uint32_t)i, kbd_guid[i]);
    st32(inst + 0x24, 3u);                  /* DI8DEVTYPE_KEYBOARD */
    /* 名字写 UTF-16LE */
    const char *nm = "Keyboard";
    for (int i = 0; nm[i]; i++) {
        st16(inst + 0x28u + (uint32_t)i * 2u, (uint16_t)(unsigned char)nm[i]);
        st16(inst + 0x230u + (uint32_t)i * 2u, (uint16_t)(unsigned char)nm[i]);
    }

    /* --- 调 guest 回调：cdecl，先压 pvRef 再压 inst --- */
    {
        uint32_t saved = c->esp;
        gpush(c, pvRef);
        gpush(c, inst);
        gpush(c, 0xabc0d1u);        /* 返回地址占位，guest_call 不靠它 */
        guest_call(c, cbAddr);
        c->esp = saved;
    }

    c->eax = S_OK;
    R(c, 20);
}

/* GetDeviceStatus(this, rguidInstance)  8 字节 */
static void di_GetDeviceStatus(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    c->eax = DI_NOTATTACHED;
    R(c, 8);
}

/* RunControlPanel(this, hwndOwner, dwFlags)  12 字节 */
static void di_RunControlPanel(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = S_OK;
    R(c, 12);
}

/* Initialize(this, hinst, dwVersion)  12 字节 */
static void di_Initialize(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = S_OK;
    R(c, 12);
}

static void (*const s_di_fns[DI_T_COUNT])(CPU *) = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice,
    di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel, di_Initialize
};

/* 建立对象与 vtable（幂等） */
uint32_t host_dinput_obj(void)
{
    if (g_di_state == 1) return g_di_obj;
    if (g_di_state == -1) return 0;

    void *p = mmap((void *)(uintptr_t)DI_BASE, DI_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)DI_BASE) {
        fprintf(stderr, "[dinput] ✗ cannot construct object at 0x%08x\n", DI_BASE);
        g_di_state = -1;
        return 0;
    }

    /* vtable：每槽放一个 guest 可 call 的 token */
    for (unsigned i = 0; i < DI_T_COUNT; i++)
        st32(DI_VT + i * 4u, DI_TOKEN_BASE + i * 4u);

    /* 对象：第一个字段是 vtable 指针，第二个是引用计数 */
    st32(DI_OBJ + 0u, DI_VT);
    st32(DI_OBJ + 4u, 1u);

    g_di_obj = DI_OBJ;
    g_di_state = 1;
    fprintf(stderr, "[dinput] ✓ IDirectInput8 fake object @ %08x  vtable @ %08x\n",
            DI_OBJ, DI_VT);
    return g_di_obj;
}

/* guest_call 分发入口。1 = 已处理 */
int host_dinput_dynamic(CPU *__restrict c, uint32_t addr)
{
    /* ---- 设备的 vtable（32 个方法）---- */
    if (addr >= DI_DEV_TOKEN_BASE && addr < DI_DEV_TOKEN_BASE + DI_DEV_T_COUNT * 4u) {
        uint32_t off = addr - DI_DEV_TOKEN_BASE;
        if (off % 4u) return 0;
        uint32_t i = off / 4u;
        if (i >= DI_DEV_T_COUNT) return 0;
        /* 前 24 次设备方法调用，打出方法号，看游戏到底用了哪些 */
        static unsigned ndev;
        if (ndev++ < 24u) {
            fprintf(stderr, "[dinput] dev-method #%u (index %u)\n", ndev, i);
            fflush(NULL);
        }
        s_dev_fns[i](c);
        return 1;
    }

    /* ---- IDirectInput8 的 vtable ---- */
    if (addr < DI_TOKEN_BASE || addr >= DI_TOKEN_BASE + DI_T_COUNT * 4u) return 0;
    uint32_t off = addr - DI_TOKEN_BASE;
    if (off % 4u) return 0;
    uint32_t i = off / 4u;
    if (i >= DI_T_COUNT) return 0;
    {
        static unsigned nroot;
        if (nroot++ < 12u) {
            fprintf(stderr, "[dinput] root-method #%u (index %u)\n", nroot, i);
            fflush(NULL);
        }
    }
    s_di_fns[i](c);
    return 1;
}

int host_dinput_count(void) { return (int)DI_T_COUNT; }
