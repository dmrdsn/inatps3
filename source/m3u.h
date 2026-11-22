#ifndef M3U_H
#define M3U_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *url;
    const char *group;     /* may be "" */
    const char *ua;        /* #EXTVLCOPT:http-user-agent, may be NULL */
    const char *referer;   /* #EXTVLCOPT:http-referrer, may be NULL */
    int group_idx;         /* index into m3u_list.groups, -1 if none */
} m3u_entry;

typedef struct {
    const char *name;
    int count;
} m3u_group;

typedef struct {
    char *text;            /* owned buffer, entries point into it */
    m3u_entry *items;
    int count, cap;
    m3u_group *groups;
    int ngroups, gcap;
    int *ghash;            /* open addressing hash -> group index */
    int ghash_size;
} m3u_list;

/* Takes ownership of `text` (malloc'd, NUL-terminated). Returns 0 on success. */
int m3u_parse(m3u_list *l, char *text);
void m3u_free(m3u_list *l);

#endif
