// orbitals-recomp --oodle-batch IN OUT: Oodle streams (Kraken, Mermaid, Selkie, Leviathan) decompressed in a batch with
// ooz (third_party/ooz). Why: Unreal's IoStore compresses most of Orbitals' containers with Oodle, shader code
// included, and the setup's Python tools (tools/shaders/iostore.py) cannot decode it; this is the same executable the
// player already has, so the setup needs no extra program and no proprietary library. One process for thousands of
// blocks: a process per block would cost minutes on Windows.
//
// IN is a sequence of records, little endian: u32 raw_size, u32 comp_size, comp_size bytes of one Oodle stream.
// OUT, one record per input record, in order: i32 status (raw_size, or -1 when the stream is malformed), then, only
// with status >= 0, raw_size bytes. Prints "oodle: N streams, B bytes, F failed" at the end; exit 0 unless a file
// cannot be read or written (a malformed stream is reported in OUT, not by the exit code).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ooz.h"

#define MAX_RAW (64u << 20)    // an IoStore block is 64 KiB to 256 KiB, a shader group up to a few MiB

static int rd32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static int wr32(FILE *f, int32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return fwrite(b, 1, 4, f) == 4;
}

int oodle_batch_main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: orbitals-recomp --oodle-batch IN OUT\n");
        return 2;
    }
    FILE *in = fopen(argv[0], "rb"), *out = fopen(argv[1], "wb");
    if (!in || !out) {
        fprintf(stderr, "oodle: cannot open %s\n", in ? argv[1] : argv[0]);
        return 1;
    }
    uint8_t *src = NULL, *dst = NULL;
    size_t src_cap = 0, dst_cap = 0;
    uint64_t n = 0, bytes = 0, failed = 0;
    uint32_t raw, comp;
    int rc = 0;
    while (rd32(in, &raw)) {
        if (!rd32(in, &comp) || raw > MAX_RAW || comp > MAX_RAW) {
            fprintf(stderr, "oodle: bad record %llu (raw %u, compressed %u)\n", (unsigned long long)n, raw, comp);
            rc = 1;
            break;
        }
        if (comp + OOZ_SAFE_SPACE > src_cap) {   // zero padding: the decoder may read a little past the stream
            src_cap = comp + OOZ_SAFE_SPACE;
            src = realloc(src, src_cap);
        }
        if (raw + OOZ_SAFE_SPACE > dst_cap) {
            dst_cap = raw + OOZ_SAFE_SPACE;
            dst = realloc(dst, dst_cap);
        }
        if (!src || !dst || fread(src, 1, comp, in) != comp) {
            fprintf(stderr, "oodle: record %llu is truncated\n", (unsigned long long)n);
            rc = 1;
            break;
        }
        memset(src + comp, 0, OOZ_SAFE_SPACE);
        int got = raw ? ooz_decompress(src, comp, dst, raw) : 0;
        int ok = got == (int)raw;
        if (!wr32(out, ok ? (int32_t)raw : -1) || (ok && fwrite(dst, 1, raw, out) != raw)) {
            fprintf(stderr, "oodle: cannot write %s\n", argv[1]);
            rc = 1;
            break;
        }
        n++;
        bytes += ok ? raw : 0;
        failed += !ok;
    }
    free(src);
    free(dst);
    if (fclose(out)) rc = 1;
    fclose(in);
    printf("oodle: %llu streams, %llu bytes, %llu failed\n", (unsigned long long)n, (unsigned long long)bytes,
           (unsigned long long)failed);
    return rc;
}
