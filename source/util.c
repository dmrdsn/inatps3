#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>
#include <lv2/sysfs.h>

#include "util.h"

uint64_t util_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void util_sleep_ms(int ms)
{
    if (ms <= 0) {
        sysThreadYield();
        return;
    }
    usleep(ms * 1000);
}

uint32_t util_utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (c == 0)
        return 0;
    if (c < 0x80) {
        *pp += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *pp += 2;
        return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp += 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *pp += 4;
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    /* invalid byte: treat as Latin-1 so legacy playlists still show something */
    *pp += 1;
    return c;
}

char *util_strdup(const char *s)
{
    size_t n;
    char *d;
    if (!s)
        return NULL;
    n = strlen(s);
    d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

void util_trim(char *s)
{
    size_t n;
    char *p = s;
    if (!s)
        return;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
}

int util_istarts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

int util_iends(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix), i;
    if (lx > ls)
        return 0;
    s += ls - lx;
    for (i = 0; i < lx; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

void util_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n;
    if (size == 0)
        return;
    n = src ? strlen(src) : 0;
    if (n >= size)
        n = size - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

char *util_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, n = 0;
    if (!f)
        return NULL;
    for (;;) {
        size_t r;
        if (cap - n < 65536 + 1) {
            size_t ncap = cap ? cap * 2 : 262144;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        r = fread(buf + n, 1, cap - n - 1, f);
        if (r == 0)
            break;
        n += r;
    }
    fclose(f);
    if (!buf)
        return NULL;
    buf[n] = 0;
    if (len)
        *len = n;
    return buf;
}

int util_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t w;
    if (!f)
        return -1;
    w = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return w == len ? 0 : -1;
}

int util_file_exists(const char *path)
{
    sysFSStat st;
    return sysFsStat(path, &st) == 0;
}

int util_dir_exists(const char *path)
{
    s32 fd;
    if (sysFsOpendir(path, &fd) != 0)
        return 0;
    sysFsClosedir(fd);
    return 1;
}

void util_mkdirs(const char *path)
{
    char tmp[512];
    char *p;
    util_strlcpy(tmp, path, sizeof(tmp));
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (strchr(tmp + 1, '/'))     /* skip the device root (/dev_hdd0) */
                sysFsMkdir(tmp, 0777);
            *p = '/';
        }
    }
    sysFsMkdir(tmp, 0777);
}

int util_mutex_init(util_mutex *m)
{
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
    m->ok = (sysMutexCreate(&m->m, &attr) == 0);
    return m->ok ? 0 : -1;
}

void util_mutex_lock(util_mutex *m)
{
    if (m->ok)
        sysMutexLock(m->m, 0);
}

void util_mutex_unlock(util_mutex *m)
{
    if (m->ok)
        sysMutexUnlock(m->m);
}

void util_mutex_destroy(util_mutex *m)
{
    if (m->ok)
        sysMutexDestroy(m->m);
    m->ok = 0;
}

typedef struct {
    util_thread_fn fn;
    void *arg;
} thread_tramp;

static void thread_entry(void *p)
{
    thread_tramp t = *(thread_tramp *)p;
    free(p);
    t.fn(t.arg);
    sysThreadExit(0);
}

int util_thread_start(sys_ppu_thread_t *id, util_thread_fn fn, void *arg,
                      const char *name, int prio, int stack_kb)
{
    thread_tramp *t = malloc(sizeof(*t));
    char tname[28];
    s32 ret;
    if (!t)
        return -1;
    t->fn = fn;
    t->arg = arg;
    util_strlcpy(tname, name, sizeof(tname));
    ret = sysThreadCreate(id, thread_entry, t, prio, (u64)stack_kb * 1024,
                          THREAD_JOINABLE, tname);
    if (ret != 0) {
        free(t);
        return -1;
    }
    return 0;
}

void util_thread_join(sys_ppu_thread_t id)
{
    u64 rv;
    sysThreadJoin(id, &rv);
}

int util_buf_append(util_buf *b, const void *data, size_t len)
{
    if (b->len + len + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 65536;
        char *nd;
        while (ncap < b->len + len + 1)
            ncap *= 2;
        nd = realloc(b->data, ncap);
        if (!nd)
            return -1;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 0;
}

void util_buf_free(util_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
