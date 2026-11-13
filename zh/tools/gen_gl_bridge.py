#!/usr/bin/env python3
"""从 gl4es 的头文件生成 guest↔GL 桥的签名表与 wrapper。

为什么生成而不是手写：手工列签名必然漏项，而漏项的后果是运行时返回 0，
guest 存下这个 0 然后 `call 0` —— 报出来的是「indirect call to address 0」，
离真正原因很远。生成能保证覆盖 gl4es 真正导出的每一个入口。

输入：
  include/GL/gl.h       GL 1.1-2.1
  include/GL/glext.h    扩展
输出：
  gl4es_bridge_gen.c    每个唯一参数签名一个 static wrapper + 名字表

类型映射（x86-32 调用约定）：
  GLenum/GLint/GLuint/GLsizei/GLboolean/GLbitfield/GLubyte/... 以及一切指针
      -> 'i'  32 位整数槽
  GLfloat/GLclampf                                                -> 'f'
  GLdouble/GLclampd                                               -> 'd'（占 2 槽）
"""
import os
import re
import sys

GL4ES = os.environ.get("GL4ES_SRC", "/data/data/com.termux/files/home/tmp/gl4es")
OUT = os.environ.get("GL_BRIDGE_OUT",
                     "/data/data/com.termux/files/home/tmp/rgxm_build/gl4es_bridge_gen.c")

DECL = re.compile(
    r"GLAPI\s+(?P<ret>[A-Za-z_][A-Za-z0-9_ \*]*?)\s+(?:GLAPIENTRY|APIENTRY)\s+"
    r"(?P<name>gl[A-Za-z0-9_]+)\s*\((?P<args>[^;]*?)\)\s*;",
    re.S)


def kind_of(t):
    """判定一个 x86 栈参数的种类。

    ★★ 2026-11-10 修复（严重的旧 bug）：
    旧实现把**完整参数声明**（含变量名）拿去和类型名比较：
        base = "GLclampf red"  !=  "GLclampf"
    于是永远匹配不上，**全部落到默认的 'i'**。结果是 16 种签名全是
    清一色 'i'，所有 float/double 参数都被当整数塞进 x0-x7，
    而 gl4es 的 float 形参从 s0-s3 读 → 拿到随机残留值。

    受影响：每一个带浮点参数的 GL 入口，包括
       glClearDepth(GLclampd)   → 深度清除值是垃圾
       glClearColor(4×GLclampf) → 清屏颜色是垃圾
       glDepthRange(2×GLclampd) → 深度范围是垃圾
       glUniform1f/4f、glBlendColor、glTexParameterf …
    这正好解释「精灵/怪物/HUD 整类缺失」：深度清除值不可预测，
    配合 glDepthFunc(GL_GREATER) 会把后画的片段全挡掉。

    修法：先去掉变量名（声明里最后一个 token），只匹配类型部分。
    """
    t = t.strip()
    if not t or t == "void":
        return None
    if "*" in t:
        return "i"                     # 指针：x86 上也是 4 字节槽
    if "[" in t:
        # ★★ 2026-11-10：数组形参在 C 里**退化为指针**，
        # 必须按 32 位槽传（例如 glLoadTransposeMatrixf(const GLfloat m[16])）。
        # 误判成 'f' 会把 float 值当指针传给 gl4es → 崩。
        return "i"
    body = t.split("[")[0].strip()
    parts = body.split()
    base = " ".join(parts[:-1]) if len(parts) > 1 else body
    base = base.replace("const", "").strip()
    if base in ("GLfloat", "GLclampf"):
        return "f"
    if base in ("GLdouble", "GLclampd"):
        return "d"
    # 其余整数类一律 'i'
    return "i"


def parse(path):
    try:
        src = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return {}
    out = {}
    for m in DECL.finditer(src):
        name = m.group("name")
        args = m.group("args").strip()
        if args in ("void", ""):
            sig = ""
        else:
            kinds = []
            for a in args.split(","):
                k = kind_of(a)
                if k:
                    kinds.append(k)
            sig = "".join(kinds)
        ret = " ".join(m.group("ret").split())
        if "*" in ret and ("char" in ret or "GLubyte" in ret or "GLchar" in ret):
            rk = "s"                    # 字符串，要拷进低 4GB
                                       # （glGetString 返回 const GLubyte *）
        elif ret.startswith("void") and "*" not in ret:
            rk = "v"
        elif "*" in ret:
            rk = "p"                    # 其他指针（glMapBuffer 等），先按 i 返回
        else:
            rk = "i"
        out[name] = (sig, rk)
    return out


def ctype(k):
    # 'i' 必须用 64 位：aarch64 的栈参数占 8 字节槽，若按 32 位声明，
    # clang 只写低 4 字节，高 4 字节是未初始化的栈垃圾。gl4es 按 64 位
    # 指针接收时会读到脏高位 —— 实测把 0x48080010 变成 0x7f48080010，
    # 崩在 bionic memcpy 里。
    # 寄存器参数（x0-x7）用 w 寄存器写会自动清零高 32 位，本来安全；
    # 统一按 64 位是为了让第 9 个及以后的栈参数也正确。
    return {"i": "uint64_t", "f": "float", "d": "double"}[k]


def emit_wrapper(sig, idx, u32ret=False):
    """生成一个按 sig 调用的 wrapper。double 占 2 个 4 字节槽。
    u32ret=True 时把标量返回值写进 c->eax（void 版会丢掉返回值）。"""
    lines = []
    # 计算每个参数在 x86 栈上的起始槽位
    slots = []
    pos = 0
    for k in sig:
        slots.append(pos)
        pos += 2 if k == "d" else 1
    nbytes = pos * 4

    decl_args = ", ".join(ctype(k) for k in sig) if sig else "void"
    call_args = ", ".join(
        ("gsd(c,%d)" % s) if k == "d" else
        ("gsf(c,%d)" % s) if k == "f" else
        ("gs32(c,%d)" % s)
        for k, s in zip(sig, slots))

    tag = ("r%d" % idx) if u32ret else ("w%d" % idx)
    lines.append("static void %s(CPU *c, void *f)" % tag)
    lines.append("{")
    if u32ret:
        lines.append("    c->eax = ((uint32_t (*)(%s))f)(%s);" % (decl_args, call_args))
    else:
        lines.append("    ((void (*)(%s))f)(%s);" % (decl_args, call_args))
    lines.append("    gret(c, %du);" % nbytes)
    lines.append("}")
    return "\n".join(lines)


def main():
    decls = {}
    for h in ("gl.h", "glext.h"):
        decls.update(parse(os.path.join(GL4ES, "include", "GL", h)))
    if not decls:
        print("没解析到任何声明，检查 GL4ES_SRC", file=sys.stderr)
        return 1

    # 只保留 gl4es 真正导出的（导出但头文件没声明的会走不到，反之亦然）
    sigs = sorted({v[0] for v in decls.values()})
    sig_idx = {s: i for i, s in enumerate(sigs)}

    # ★★ 2026-11-10：生成器自检闸门。
    #
    # 事故：kind_of() 曾把完整参数声明（含变量名）和类型名比较，
    # 于是 16 种签名全是清一色 'i'，所有 float/double 参数都被当整数
    # 传进 x0-x7，而 gl4es 从 s0-s3 读 —— 拿到随机残留值。
    # 表现是清屏色、alpha test、深度清除值全部不可预测，画面整片错乱。
    #
    # 这个 bug 能一路走到真机，是因为「看起来能跑」。所以在这里
    # 做成硬闸门：GL 头文件里必然有大量 GLfloat/GLclampf 参数，
    # 如果一种 'f' 都没生成出来，就说明类型判定坏了，直接构建失败。
    n_f = sum(1 for sg in sigs if "f" in sg)
    n_d = sum(1 for sg in sigs if "d" in sg)
    if n_f == 0 or n_d == 0:
        print("★ 签名表自检失败：没有生成任何 %s 参数签名。"
              % ("float" if n_f == 0 else "double"), file=sys.stderr)
        print("  这说明 kind_of() 的类型判定坏了 —— 所有浮点参数都会被"
              "当整数传，画面会整片错乱。", file=sys.stderr)
        return 1

    L = []
    L.append("/* GENERATED by gen_gl_bridge.py -- do not edit.")
    L.append(" * 来源: gl4es/include/GL/{gl.h,glext.h}")
    L.append(" * %d 个入口, %d 种参数签名 */" % (len(decls), len(sigs)))
    L.append('#include "guest.h"')
    L.append("")
    L.append("static inline uint32_t gs32(CPU *c, int i)")
    L.append("{")
    L.append("    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));")
    L.append("}")
    L.append("static inline float gsf(CPU *c, int i)")
    L.append("{")
    L.append("    uint32_t v = gs32(c, i); float f; __builtin_memcpy(&f, &v, 4); return f;")
    L.append("}")
    L.append("static inline double gsd(CPU *c, int i)")
    L.append("{")
    L.append("    uint64_t lo = gs32(c, i), hi = gs32(c, i + 1);")
    L.append("    uint64_t raw = lo | (hi << 32); double d; __builtin_memcpy(&d, &raw, 8); return d;")
    L.append("}")
    L.append("static inline void gret(CPU *c, uint32_t n)")
    L.append("{")
    L.append("    (void)gpop(c); (void)guest_stack_adjust(c, n, 0u);")
    L.append("}")
    L.append("")
    for i, s in enumerate(sigs):
        L.append(emit_wrapper(s, i, False))
        L.append("")
        L.append(emit_wrapper(s, i, True))
        L.append("")
    L.append("typedef void (*glbridge_fn)(CPU *, void *);")
    L.append("glbridge_fn gl_bridge_wrapper(int idx, int want_u32ret);")
    L.append("glbridge_fn gl_bridge_wrapper(int idx, int want_u32ret)")
    L.append("{")
    L.append("    if (want_u32ret) switch (idx) {")
    for i, s in enumerate(sigs):
        L.append("    case %d: return r%d;" % (i, i))
    L.append("    default: return 0;")
    L.append("    }")
    L.append("    switch (idx) {")
    for i, s in enumerate(sigs):
        L.append("    case %d: return w%d;" % (i, i))
    L.append("    default: return 0;")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("/* name -> (签名索引, 返回类型) */")
    L.append("typedef struct { const char *name; short sig; char ret; } gl_bridge_decl;")
    L.append("static const gl_bridge_decl GL_DECLS[] = {")
    for name in sorted(decls):
        sig, rk = decls[name]
        L.append('    {"%s", %d, \'%s\'},' % (name, sig_idx[sig], rk))
    L.append("};")
    L.append("const gl_bridge_decl *gl_bridge_lookup(const char *name);")
    L.append("int gl_bridge_count(void);")
    L.append("")
    L.append("const gl_bridge_decl *gl_bridge_lookup(const char *name)")
    L.append("{")
    L.append("    int lo = 0, hi = (int)(sizeof GL_DECLS / sizeof GL_DECLS[0]) - 1;")
    L.append("    while (lo <= hi) {")
    L.append("        int mid = (lo + hi) / 2;")
    L.append("        int r = __builtin_strcmp(name, GL_DECLS[mid].name);")
    L.append("        if (r == 0) return &GL_DECLS[mid];")
    L.append("        if (r < 0) hi = mid - 1; else lo = mid + 1;")
    L.append("    }")
    L.append("    return 0;")
    L.append("}")
    L.append("int gl_bridge_count(void) { return (int)(sizeof GL_DECLS / sizeof GL_DECLS[0]); }")

    open(OUT, "w").write("\n".join(L) + "\n")
    print("入口 %d 个，签名 %d 种 -> %s" % (len(decls), len(sigs), OUT))
    from collections import Counter
    top = Counter(v[0] for v in decls.values()).most_common(8)
    print("最常见签名:", ", ".join("%r:%d" % (s, n) for s, n in top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
