/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "visual_check.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 8) return 2;
    uint8_t bytes[1857]; uint64_t pc[4];
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    int error = ferror(f);
    if (fclose(f) || error) return 2;
    for (unsigned i = 0; i < 4; i++) pc[i] = strtoull(argv[4+i], NULL, 0);
    return ew_visual_trace_ok(bytes, n, strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0), pc) ? 0 : 1;
}
