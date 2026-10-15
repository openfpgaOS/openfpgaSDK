//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* decode_dump words.bin base: print qsim's decode of every instruction */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

int main(int argc, char **argv)
{
    if (argc == 2 && argv[1][0] == '-') {       /* list mode: "addr hexword" lines on stdin */
        char line[256];
        while (fgets(line, sizeof line, stdin)) {
            unsigned long a, w;
            if (sscanf(line, "%lx %lx", &a, &w) != 2) continue;
            const char *nm;
            int rd, rs1, rs2;
            int32_t imm;
            int ok = qsim_decode_fields((uint32_t)w, &nm, &rd, &rs1, &rs2, &imm);
            printf("%08lx %08lx %s %d %d %d %d %d\n", a, w, nm, rd, rs1, rs2, imm, ok);
        }
        return 0;
    }
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 0);
    static unsigned char buf[1 << 24];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t o = 0; o + 2 <= n;) {
        uint32_t w = buf[o] | (uint32_t)buf[o + 1] << 8;
        unsigned len = (w & 3) == 3 ? 4 : 2;
        if (len == 4) {
            if (o + 4 > n) break;
            w |= (uint32_t)buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24;
        }
        const char *nm;
        int rd, rs1, rs2;
        int32_t imm;
        int ok = qsim_decode_fields(w, &nm, &rd, &rs1, &rs2, &imm);
        printf("%08" PRIx32 " %08" PRIx32 " %s %d %d %d %d %d\n", base + (uint32_t)o, w, nm, rd, rs1, rs2, imm, ok);
        o += len;
    }
    return 0;
}
