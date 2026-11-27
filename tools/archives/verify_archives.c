/* verify_archives.c — 用游戏自己的 fold 校验和验证归档解码
 *
 * fold 的确切算法（在真实归档上试出来的，不是猜的）：
 *   s = rotr32(s,1) + word,  初值 0xababeb98
 *   0x200 的暂存区只清零一次；每块只覆盖有数据的部分，
 *   所以不满的最后一块，尾部字节保留的是上一块残留内容。
 *   补零版本只有 55/200 通过，残留版本 200/200 —— 差别就在这里。
 *
 * 索引条目 +0x00 = h1 (djb2, 基值 0x1505)，+0x04 = h2 (FNV-1a, 基值 0x5BB2220E)。
 * mode 0 的解密密钥由 h2 派生。
 *
 * 只验 mode 0（Bogocrypt1），因为它不需要 zlib，是纯算术。
 * mode 1/2 的验证据 Python 版本。
 *
 * 用法: verify_archives <packed目录>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <zlib.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static uint32_t fold(const uint8_t *d, size_t n) {
    static uint8_t buf[0x200];
    uint32_t s = 0xababeb98u;
    int inited = 0;
    for (size_t off = 0; off < n; off += 0x200) {
        size_t len = n - off < 0x200 ? n - off : 0x200;
        if (!inited) { memset(buf, 0, sizeof buf); inited = 1; }
        memcpy(buf, d + off, len);
        size_t n4 = (len + 3) & ~(size_t)3;
        for (size_t i = 0; i < n4; i += 4)
            s = rotr32(s, 1) + rd32(buf + i);
    }
    return s;
}

/* mode 0：Bogocrypt1，密钥由 h2 派生 */
static void decrypt0(uint8_t *buf, size_t n, uint32_t h2) {
    uint32_t key = (h2 ^ 0xF9524287u) | 1u;
    static uint8_t blk[1024];
    for (size_t s0 = 0; s0 < n; s0 += 1024) {
        size_t len = n - s0 < 1024 ? n - s0 : 1024;
        memcpy(blk, buf + s0, len);
        for (int w = 0; w < 256; w++) {
            uint8_t *q = blk + w * 4;
            q[0] ^= (uint8_t)(key);
            q[1] ^= (uint8_t)(key >> 8);
            q[2] ^= (uint8_t)(key >> 16);
            q[3] ^= (uint8_t)(key >> 24);
            switch (key & 15) {
            case 2:  { uint8_t t = q[0]; q[0] = q[3]; q[3] = t;
                       t = q[1]; q[1] = q[2]; q[2] = t; } break;
            case 9:  { uint8_t t = q[0]; q[0] = q[1]; q[1] = t;
                       t = q[2]; q[2] = q[3]; q[3] = t; } break;
            case 13: { uint8_t t = q[0]; q[0] = q[2]; q[2] = t;
                       t = q[1]; q[1] = q[3]; q[3] = t; } break;
            }
            uint32_t t = key ^ (key << 8);
            key = key ^ (t >> 9) ^ (key << 8)
                  ^ (((t >> 9) ^ key ^ (key << 8)) << 23);
        }
        memcpy(buf + s0, blk, len);
    }
}

/* mode 2：4 字节长度前缀 + 裸 DEFLATE；长度恰为 0x400 时直存 */
static int dec_mode2(const uint8_t *pay, uint32_t span, uint8_t *out, uint32_t want) {
    uint32_t at = 0, produced = 0;
    z_stream zs;
    while (produced < want && at + 4 <= span) {
        uint32_t raw = rd32(pay + at);
        at += 4;
        uint32_t ln = raw & 0x7FFFFFFFu;
        if (ln > 0x7FF || at + ln > span) return -1;
        if (ln == 0x400) {
            memcpy(out + produced, pay + at, ln);
            produced += ln;
            at += ln;
            continue;
        }
        memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, -15) != Z_OK) return -1;
        zs.next_in = (Bytef *)(pay + at);
        zs.avail_in = ln;
        zs.next_out = out + produced;
        zs.avail_out = want - produced;
        int rc = inflate(&zs, Z_FINISH);
        uint32_t got = (want - produced) - zs.avail_out;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) return -1;
        produced += got;
        at += ln;
    }
    return (int)produced;
}

/* mode 1：LZW，字典跨 chunk 保留 */
#define LZW_SLOTS 0x1000
typedef struct {
    int32_t prefix[LZW_SLOTS], chain[LZW_SLOTS], nxt[LZW_SLOTS];
    uint8_t first[LZW_SLOTS];
    int next_code, width;
} lzw_t;

static void lzw_reset(lzw_t *s) {
    /* prefix 也必须初始化为 -1：calloc 给的是 0，而 emit() 靠它判断
     * 链的末端（-1）。用 0 会让链错误地指向槽 0，输出全错。 */
    for (int i = 0; i < LZW_SLOTS; i++) { s->chain[i] = -1; s->prefix[i] = -1; }
    s->next_code = 0x100;
    s->width = 8;
}
static int lzw_emit(lzw_t *s, int code, uint8_t *out, uint32_t *o, uint32_t cap) {
    static uint8_t scratch[LZW_SLOTS];
    int n = 0;
    while (code != -1 && n < LZW_SLOTS) {
        scratch[n++] = s->first[code];
        code = s->prefix[code];
    }
    for (int i = n - 1; i >= 0 && *o < cap; i--) out[(*o)++] = scratch[i];
    return n ? scratch[n - 1] : 0;
}
static void lzw_insert(lzw_t *s, int prefix, int fc) {
    if (prefix < 0 || prefix >= LZW_SLOTS || s->next_code > 0xFFF) return;
    int slot = s->next_code;
    s->prefix[slot] = prefix;
    s->chain[slot] = -1;
    s->first[slot] = (uint8_t)fc;
    s->nxt[slot] = s->chain[prefix];
    s->chain[prefix] = slot;
    s->next_code++;
}
static void lzw_chunk(lzw_t *s, const uint8_t *d, uint32_t n, uint8_t *out, uint32_t *o, uint32_t cap) {
    uint64_t acc = 0;
    int bits = 0;
    uint32_t pos = 0;
    /* 位读取：返回 -1 表示数据用尽 */
    #define RD(cnt, dst) do {                                        \
        while (bits < (cnt)) {                                       \
            if (pos >= n) { (dst) = -1; goto rd_done; }              \
            acc = (acc << 8) | d[pos++]; bits += 8;                  \
        }                                                            \
        bits -= (cnt);                                               \
        (dst) = (int)((acc >> bits) & ((1u << (cnt)) - 1));          \
    } while (0)

    int code, first_char;
    RD(s->width, code);
    if (code < 0 || code >= LZW_SLOTS) return;
    first_char = lzw_emit(s, code, out, o, cap);
    int prev = code;
    for (;;) {
        if (s->next_code + 1 >= LZW_SLOTS) {
            lzw_reset(s);
            RD(8, code);
            if (code < 0) break;
            first_char = lzw_emit(s, code, out, o, cap);
            prev = code;
        }
        while ((1 << s->width) <= s->next_code) s->width++;
        RD(s->width, code);
        if (code < 0 || code > s->next_code) break;
        if (code == s->next_code) {
            lzw_insert(s, prev, first_char);
            first_char = lzw_emit(s, code, out, o, cap);
        } else {
            first_char = lzw_emit(s, code, out, o, cap);
            lzw_insert(s, prev, first_char);
        }
        prev = code;
    }
rd_done:
    #undef RD
}

static int dec_mode1(const uint8_t *pay, uint32_t span, uint8_t *out, uint32_t want) {
    lzw_t *s = calloc(1, sizeof *s);
    if (!s) return -1;
    for (int i = 0; i < LZW_SLOTS; i++) s->first[i] = (uint8_t)i;
    lzw_reset(s);
    uint32_t at = 0, produced = 0;
    while (at + 4 <= span) {
        int32_t ln;
        memcpy(&ln, pay + at, 4);
        at += 4;
        if (ln <= 0 || at + (uint32_t)ln > span) break;
        lzw_chunk(s, pay + at, (uint32_t)ln, out, &produced, want);
        at += (uint32_t)ln;
    }
    free(s);
    return (int)produced;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <packed dir>\n", argv[0]); return 2; }
    DIR *d = opendir(argv[1]);
    if (!d) { perror("opendir"); return 1; }

    long tot_ok = 0, tot_bad = 0;
    struct dirent *e;
    printf("%-24s %5s %8s %8s %5s\n", "archive", "mode", "entries", "fold ok", "bad");
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (nl < 3 || strcmp(e->d_name + nl - 2, ".a")) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *blob = malloc((size_t)sz);
        if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(blob); continue; }
        fclose(f);
        if (memcmp(blob, "ARCH000", 7)) { free(blob); continue; }

        uint8_t mode = blob[7];
        uint32_t idx = rd32(blob + 8);
        uint16_t cnt = (uint16_t)(blob[12] | blob[13] << 8);
        if (mode != 0x00 && mode != 0x01 && mode != 0x02) {
            printf("%-24s 0x%02x %8u %8s %5s\n", e->d_name, mode, cnt, "?", "?");
            free(blob);
            continue;
        }

        /* 按 offset 排序，span = 下一条 src offset - 本条 offset */
        typedef struct { uint32_t off, size, h1, h2, ck; } ent;
        ent *v = malloc(sizeof(ent) * cnt);
        for (uint16_t i = 0; i < cnt; i++) {
            const uint8_t *p = blob + idx + (size_t)i * 20;
            v[i].h1 = rd32(p); v[i].h2 = rd32(p + 4);
            v[i].off = rd32(p + 8); v[i].size = rd32(p + 12); v[i].ck = rd32(p + 16);
        }
        /* cnt 最多 2300，插入排序够 */
        for (uint16_t i = 1; i < cnt; i++) {
            ent t = v[i]; int j = i - 1;
            while (j >= 0 && v[j].off > t.off) { v[j + 1] = v[j]; j--; }
            v[j + 1] = t;
        }

        long ok = 0, bad = 0;
        for (uint16_t i = 0; i < cnt; i++) {
            uint32_t nxt = (i + 1 < cnt) ? v[i + 1].off : idx;
            uint32_t span = nxt - v[i].off;
            if (v[i].off + span > (uint32_t)sz) { bad++; continue; }
            uint8_t *buf = malloc(v[i].size ? v[i].size : 1);
            uint32_t got = 0;
            int produced;
            if (mode == 0x00) {
                uint8_t *tmp = malloc(span);
                memcpy(tmp, blob + v[i].off, span);
                decrypt0(tmp, span, v[i].h2);
                produced = (int)v[i].size;
                memcpy(buf, tmp, v[i].size);
                free(tmp);
            } else if (mode == 0x01) {
                produced = dec_mode1(blob + v[i].off, span, buf, v[i].size);
            } else {
                produced = dec_mode2(blob + v[i].off, span, buf, v[i].size);
            }

            if (produced < 0 || (uint32_t)produced != v[i].size) {
                bad++;
                if (bad == 1)
                    printf("    first bad: #%u size want=%u got=%d (decode)\n",
                           i, v[i].size, produced);
            } else {
                got = fold(buf, v[i].size);
                if (got == v[i].ck) ok++;
                else {
                    bad++;
                    if (bad == 1)
                        printf("    first bad: #%u size=%u got=%08x want=%08x\n",
                               i, v[i].size, got, v[i].ck);
                }
            }
            free(buf);
        }
        free(v);
        free(blob);
        printf("%-24s 0x%02x %8u %8ld %5ld\n", e->d_name, mode, cnt, ok, bad);
        tot_ok += ok; tot_bad += bad;
    }
    closedir(d);
    printf("\ntotal: fold ok %ld, bad %ld\n", tot_ok, tot_bad);
    return tot_bad ? 1 : 0;
}
