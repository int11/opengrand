#ifndef CA79NG_SIM_HOSTFS_H
#define CA79NG_SIM_HOSTFS_H

#include <stdbool.h>

#include "ca79ng/fs.h"

/* USB memory backed by a host directory (flat: regular files in `dir`). */
typedef struct {
    char dir[512];
    bool attached;
} hostfs_t;

void hostfs_bind(hostfs_t *h, const char *dir, hal_fs_t *out);

#endif
