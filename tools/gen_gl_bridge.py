#!/usr/bin/env python3
"""Generate the guest<->GL bridge signature table and wrappers from the gl4es headers.

Why generate instead of hand-writing: a hand-listed signature table always misses
entries, and a missing entry returns 0 at runtime -- the guest stores that 0 and
does `call 0`, which surfaces as "indirect call to address 0", very far from the
real cause. Generating guarantees coverage of every entry point gl4es actually
exports.

Input:
  include/GL/gl.h       GL 1.1-2.1
  include/GL/glext.h    extensions
Output:
  gl4es_bridge_gen.c    one static wrapper per unique parameter signature, plus
                        the name table

Type mapping (x86-32 calling convention):
  GLenum/GLint/GLuint/GLsizei/GLboolean/GLbitfield/GLubyte/... and all pointers
      -> 'i'  32-bit integer slot
  GLfloat/GLclampf                                                -> 'f'
  GLdouble/GLclampd                                               -> 'd' (takes 2 slots)
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
    """Classify one x86 stack argument.

    ** 2026-11-10 fix (a severe old bug):
    The old implementation compared the **full parameter declaration**
    (including the variable name) against type names:
        base = "GLclampf red"  !=  "GLclampf"
    so it never matched and **every argument fell through to the default 'i'**.
    The result was 16 signatures that were all plain 'i'; every float/double
    argument was pushed into x0-x7 as an integer, while gl4es read its float
    parameters from s0-s3 -- picking up random leftover values.

    Affected: every GL entry point with a float parameter, including
       glClearDepth(GLclampd)   -> the depth clear value was garbage
       glClearColor(4xGLclampf) -> the clear colour was garbage
       glDepthRange(2xGLclampd) -> the depth range was garbage
       glUniform1f/4f, glBlendColor, glTexParameterf ...
    This is exactly why entire classes of sprites/monsters/HUD went missing:
    with an unpredictable depth clear value and glDepthFunc(GL_GREATER), every
    fragment drawn later was rejected.

    Fix: strip the variable name first (the last token of the declaration) and
    match only the type.
    """
    t = t.strip()
    if not t or t == "void":
        return None
    if "*" in t:
        return "i"                     # pointer: also a 4-byte slot on x86
    if "[" in t:
        # ** 2026-11-10: array parameters **decay to pointers** in C and must be
        # passed as a 32-bit slot (e.g. glLoadTransposeMatrixf(const GLfloat m[16])).
        # Mistaking them for 'f' passes a float value as a pointer to gl4es -> crash.
        return "i"
    body = t.split("[")[0].strip()
    parts = body.split()
    base = " ".join(parts[:-1]) if len(parts) > 1 else body
    base = base.replace("const", "").strip()
    if base in ("GLfloat", "GLclampf"):
        return "f"
    if base in ("GLdouble", "GLclampd"):
        return "d"
    # every other integer type is 'i'
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
            rk = "s"                    # string: must be copied into the low 4GB
                                       # (glGetString returns const GLubyte *)
        elif ret.startswith("void") and "*" not in ret:
            rk = "v"
        elif "*" in ret:
            rk = "p"                    # other pointers (glMapBuffer etc.); return as i for now
        else:
            rk = "i"
        out[name] = (sig, rk)
    return out


def ctype(k):
    # 'i' must be 64-bit: an aarch64 stack argument occupies an 8-byte slot. If
    # declared 32-bit, clang writes only the low 4 bytes and the high 4 are
    # uninitialised stack garbage. gl4es reading it as a 64-bit pointer then sees
    # dirty high bits -- measured turning 0x48080010 into 0x7f48080010 and
    # crashing inside bionic memcpy.
    # Register arguments (x0-x7) written via w registers zero the high 32 bits
    # automatically and are safe either way; 64-bit everywhere keeps the 9th and
    # later stack arguments correct too.
    return {"i": "uint64_t", "f": "float", "d": "double"}[k]


def emit_wrapper(sig, idx, u32ret=False):
    """Emit one wrapper calling via sig. double occupies 2 four-byte slots.
    With u32ret=True the scalar return value goes into c->eax (the void version
    would drop it)."""
    lines = []
    # compute the x86 stack slot where each argument starts
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
        print("parsed no declarations at all; check GL4ES_SRC", file=sys.stderr)
        return 1

    # keep only what gl4es really exports (exported-but-undeclared cannot be
    # reached, and vice versa)
    sigs = sorted({v[0] for v in decls.values()})
    sig_idx = {s: i for i, s in enumerate(sigs)}

    # ** 2026-11-10: generator self-check gate.
    #
    # Incident: kind_of() once compared full parameter declarations (including
    # the variable name) against type names, so all 16 signatures were plain 'i'
    # and every float/double argument was passed as an integer into x0-x7 while
    # gl4es read from s0-s3 -- picking up random leftovers. The symptom was an
    # unpredictable clear colour, alpha test and depth clear value, and a wildly
    # corrupted image.
    #
    # That bug reached a device because it "looked like it ran". So make it a hard
    # gate here: the GL headers necessarily contain many GLfloat/GLclampf
    # parameters, so if not a single 'f' was generated the type classification is
    # broken and the build must fail.
    n_f = sum(1 for sg in sigs if "f" in sg)
    n_d = sum(1 for sg in sigs if "d" in sg)
    if n_f == 0 or n_d == 0:
        print("** signature self-check failed: generated no %s parameter signatures."
              % ("float" if n_f == 0 else "double"), file=sys.stderr)
        print("   That means kind_of() type classification is broken -- every float"
              " parameter would be passed as an integer and the image would be"
              " completely corrupted.", file=sys.stderr)
        return 1

    L = []
    L.append("/* GENERATED by gen_gl_bridge.py -- do not edit.")
    L.append(" * Source: gl4es/include/GL/{gl.h,glext.h}")
    L.append(" * %d entry points, %d parameter signatures */" % (len(decls), len(sigs)))
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
    L.append("/* name -> (signature index, return kind) */")
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
    print("%d entry points, %d signatures -> %s" % (len(decls), len(sigs), OUT))
    from collections import Counter
    top = Counter(v[0] for v in decls.values()).most_common(8)
    print("most common signatures:", ", ".join("%r:%d" % (s, n) for s, n in top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
