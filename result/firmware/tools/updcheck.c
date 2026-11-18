/* Verifies an update container with the firmware's own parser (cross-check for tools/mkupd.py). */
#include <stdio.h>
#include <stdlib.h>

#include "ca79ng/update.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.upd\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);
    upd_policy_t pol = {.group = 0x04, .machine = 0x1E, .sub = 0x0002, .current_version = 0};
    upd_info_t info;
    upd_result_t r = upd_verify(buf, (size_t)n, &pol, &info);
    printf("%s: %s", argv[1], upd_strerror(r));
    if (r == UPD_OK)
        printf(" (%u section(s), version %u.%u.%u)", info.count, info.version >> 16, (info.version >> 8) & 255, info.version & 255);
    putchar('\n');
    free(buf);
    return r == UPD_OK ? 0 : 1;
}
