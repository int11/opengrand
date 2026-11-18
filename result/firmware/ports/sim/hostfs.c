/* Simulator USB memory: a host directory. Only plain files directly inside
 * the directory are visible; names with '/' or starting with '.' are rejected. */
#define _POSIX_C_SOURCE 200809L
#include "hostfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ca79ng/kproto.h"

static bool valid_name(const char *n)
{
    return n && n[0] && n[0] != '.' && !strchr(n, '/') && strlen(n) < FS_NAME_MAX;
}

static void path_of(const hostfs_t *h, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", h->dir, name);
}

static bool h_mounted(void *ctx)
{
    hostfs_t *h = ctx;
    struct stat st;
    return h->attached && stat(h->dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool h_list(void *ctx, uint32_t index, char *name, size_t cap, uint32_t *size)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h))
        return false;
    DIR *d = opendir(h->dir);
    if (!d)
        return false;
    char *names[1024];
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024) {
        if (!valid_name(e->d_name))
            continue;
        char p[1024];
        struct stat st;
        path_of(h, e->d_name, p, sizeof p);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp);
    bool ok = index < n;
    if (ok) {
        snprintf(name, cap, "%s", names[index]);
        char p[1024];
        struct stat st;
        path_of(h, names[index], p, sizeof p);
        *size = stat(p, &st) == 0 ? (uint32_t)st.st_size : 0;
    }
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    return ok;
}

static int32_t h_read(void *ctx, const char *name, uint8_t *buf, uint32_t cap)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h)) return -(int32_t)KP_RES_NO_MEDIA;
    if (!valid_name(name)) return -(int32_t)KP_RES_PATH;
    char p[1024];
    path_of(h, name, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -(int32_t)KP_RES_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (uint32_t)sz > cap) { fclose(f); return (int32_t)cap + 1; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return (int32_t)r;
}

static uint16_t h_write(void *ctx, const char *name, const uint8_t *data, uint32_t len, bool overwrite)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h)) return KP_RES_NO_MEDIA;
    if (!valid_name(name)) return KP_RES_PATH;
    char p[1024];
    path_of(h, name, p, sizeof p);
    if (!overwrite && access(p, F_OK) == 0) return KP_RES_EXISTS;
    FILE *f = fopen(p, "wb");
    if (!f) return KP_RES_ACCESS;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? KP_RES_OK : KP_RES_FULL;
}

static uint16_t h_remove(void *ctx, const char *name)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h)) return KP_RES_NO_MEDIA;
    if (!valid_name(name)) return KP_RES_PATH;
    char p[1024];
    path_of(h, name, p, sizeof p);
    return unlink(p) == 0 ? KP_RES_OK : (errno == ENOENT ? KP_RES_NOT_FOUND : KP_RES_ACCESS);
}

static uint16_t h_rename(void *ctx, const char *from, const char *to)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h)) return KP_RES_NO_MEDIA;
    if (!valid_name(from) || !valid_name(to)) return KP_RES_PATH;
    char a[1024], b[1024];
    path_of(h, from, a, sizeof a);
    path_of(h, to, b, sizeof b);
    if (access(a, F_OK) != 0) return KP_RES_NOT_FOUND;
    if (access(b, F_OK) == 0) return KP_RES_EXISTS;
    return rename(a, b) == 0 ? KP_RES_OK : KP_RES_ACCESS;
}

static uint16_t h_format(void *ctx)
{
    hostfs_t *h = ctx;
    if (!h_mounted(h)) return KP_RES_NO_MEDIA;
    char name[FS_NAME_MAX];
    uint32_t size;
    while (h_list(h, 0, name, sizeof name, &size))
        if (h_remove(h, name) != KP_RES_OK)
            return KP_RES_ACCESS;
    return KP_RES_OK;
}

void hostfs_bind(hostfs_t *h, const char *dir, hal_fs_t *out)
{
    snprintf(h->dir, sizeof h->dir, "%s", dir);
    h->attached = true;
    *out = (hal_fs_t){h, h_mounted, h_list, h_read, h_write, h_remove, h_rename, h_format};
}
