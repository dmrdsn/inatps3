#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/thread.h>
#include <sys/mutex.h>

/* time / sleep */
uint64_t util_time_us(void);
void util_sleep_ms(int ms);

/* strings */
uint32_t util_utf8_next(const char **p);     /* returns 0 at end of string */
char *util_strdup(const char *s);
void util_trim(char *s);
int util_istarts(const char *s, const char *prefix);   /* case-insensitive */
int util_iends(const char *s, const char *suffix);     /* case-insensitive */
void util_strlcpy(char *dst, const char *src, size_t size);

/* files */
char *util_read_file(const char *path, size_t *len);   /* NUL-terminated, malloc'd */
int util_write_file(const char *path, const void *data, size_t len);
int util_file_exists(const char *path);
int util_dir_exists(const char *path);
void util_mkdirs(const char *path);

/* mutex */
typedef struct { sys_mutex_t m; int ok; } util_mutex;
int util_mutex_init(util_mutex *m);
void util_mutex_lock(util_mutex *m);
void util_mutex_unlock(util_mutex *m);
void util_mutex_destroy(util_mutex *m);

/* threads (entry wrapper calls sysThreadExit for you) */
typedef void (*util_thread_fn)(void *arg);
int util_thread_start(sys_ppu_thread_t *id, util_thread_fn fn, void *arg,
                      const char *name, int prio, int stack_kb);
void util_thread_join(sys_ppu_thread_t id);

/* tiny growable string buffer */
typedef struct { char *data; size_t len, cap; } util_buf;
int util_buf_append(util_buf *b, const void *data, size_t len);
void util_buf_free(util_buf *b);

#endif
