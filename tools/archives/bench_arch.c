/* bench_arch.c — 实测 ARCH000 归档解压的原生耗时
 *
 * 只做 mode 0x02 的 chunk 循环：4 字节长度 + 直存(0x400) 或 raw DEFLATE。
 * 目的：给「预解压能省多少」一个原生下限，用来判断 17.5 秒启动里
 * 解压占多大比重。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <zlib.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <archive.a>\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)sz);
    if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 1; }
    fclose(f);

    if (memcmp(blob, "ARCH000", 7) != 0) { fprintf(stderr, "bad magic\n"); return 1; }
    uint8_t mode = blob[7];
    uint32_t idx = rd32(blob + 8);
    uint16_t cnt = (uint16_t)(blob[12] | blob[13] << 8);
    printf("%s  mode=0x%02x  entries=%u  size=%.1f MB\n",
           argv[1], mode, cnt, (double)sz / 1048576.0);

    /* offsets 升序 */
    uint32_t *off = malloc(sizeof(uint32_t) * cnt);
    uint32_t *len = malloc(sizeof(uint32_t) * cnt);
    for (uint16_t i = 0; i < cnt; i++) {
        const uint8_t *e = blob + idx + (size_t)i * 20;
        off[i] = rd32(e + 8);
        len[i] = rd32(e + 12);
    }
    /* 简单插入排序 —— cnt 最多 ~5500，够用 */
    for (uint16_t i = 1; i < cnt; i++) {
        uint32_t o = off[i], l = len[i];
        int j = i - 1;
        while (j >= 0 && off[j] > o) { off[j+1] = off[j]; len[j+1] = len[j]; j--; }
        off[j+1] = o; len[j+1] = l;
    }

    if (mode != 0x02) {
        printf("  非 mode 0x02，跳过（本基准只测 chunked）\n");
        return 0;
    }

    uint8_t *outbuf = malloc(16u << 20);
    unsigned long n_stored = 0, n_deflate = 0, n_fail = 0;
    unsigned long long bytes_stored = 0, bytes_deflate = 0;
    z_stream zs;

    double t0 = now();
    for (uint16_t i = 0; i < cnt; i++) {
        uint32_t start = off[i];
        uint32_t span = (i + 1 < cnt ? off[i+1] : idx) - start;
        uint32_t want = len[i];
        uint32_t at = 0, done = 0;

        while (done < want && at + 4 <= span) {
            uint32_t raw = rd32(blob + start + at);
            at += 4;
            uint32_t cl = raw & 0x7FFFFFFFu;
            if (cl > 0x7FF || at + cl > span) break;

            if (cl == 0x400) {
                memcpy(outbuf, blob + start + at, cl);
                done += cl; bytes_stored += cl; n_stored++;
                at += cl;
                continue;
            }

            memset(&zs, 0, sizeof zs);
            if (inflateInit2(&zs, -15) != Z_OK) { n_fail++; at += cl; continue; }
            zs.next_in = (Bytef *)(blob + start + at);
            zs.avail_in = cl;
            zs.next_out = outbuf;
            zs.avail_out = 16u << 20;
            int rc = inflate(&zs, Z_FINISH);
            if (rc == Z_STREAM_END || rc == Z_OK || rc == Z_BUF_ERROR) {
                uint32_t got = (16u << 20) - zs.avail_out;
                done += got; bytes_deflate += got; n_deflate++;
            } else {
                n_fail++;
            }
            inflateEnd(&zs);
            at += cl;
        }
    }
    double dt = now() - t0;

    printf("  stored chunk : %8lu  -> %7.1f MB\n", n_stored, bytes_stored / 1048576.0);
    printf("  deflate chunk: %8lu  -> %7.1f MB   (fail %lu)\n", n_deflate, bytes_deflate / 1048576.0, n_fail);
    printf("  原生耗时     : %.2f 秒  (%.1f MB/s 输出)\n",
           dt, (bytes_stored + bytes_deflate) / 1048576.0 / dt);
    return 0;
}
