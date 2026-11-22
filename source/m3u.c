#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "m3u.h"
#include "util.h"

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static int ghash_rebuild(m3u_list *l, int size)
{
    int i;
    int *nh = malloc(sizeof(int) * size);
    if (!nh)
        return -1;
    for (i = 0; i < size; i++)
        nh[i] = -1;
    for (i = 0; i < l->ngroups; i++) {
        unsigned h = hash_str(l->groups[i].name) & (size - 1);
        while (nh[h] >= 0)
            h = (h + 1) & (size - 1);
        nh[h] = i;
    }
    free(l->ghash);
    l->ghash = nh;
    l->ghash_size = size;
    return 0;
}

static int group_index(m3u_list *l, const char *name)
{
    unsigned h;
    if (!name || !*name)
        return -1;
    if (!l->ghash && ghash_rebuild(l, 256) != 0)
        return -1;
    h = hash_str(name) & (l->ghash_size - 1);
    while (l->ghash[h] >= 0) {
        if (strcmp(l->groups[l->ghash[h]].name, name) == 0)
            return l->ghash[h];
        h = (h + 1) & (l->ghash_size - 1);
    }
    /* new group */
    if (l->ngroups == l->gcap) {
        int ncap = l->gcap ? l->gcap * 2 : 64;
        m3u_group *ng = realloc(l->groups, sizeof(m3u_group) * ncap);
        if (!ng)
            return -1;
        l->groups = ng;
        l->gcap = ncap;
    }
    l->groups[l->ngroups].name = name;
    l->groups[l->ngroups].count = 0;
    l->ngroups++;
    if (l->ngroups * 2 > l->ghash_size)
        ghash_rebuild(l, l->ghash_size * 2);
    else
        l->ghash[h] = l->ngroups - 1;
    return l->ngroups - 1;
}

static int add_entry(m3u_list *l, const m3u_entry *e)
{
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 1024;
        m3u_entry *ni = realloc(l->items, sizeof(m3u_entry) * ncap);
        if (!ni)
            return -1;
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->count++] = *e;
    return 0;
}

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Parses `#EXTINF:<dur> key="val" key2=val2,Display Name` in place. */
static void parse_extinf(char *p, const char **name, const char **group)
{
    *name = NULL;
    *group = NULL;
    /* skip duration */
    p = skip_ws(p);
    while (*p && *p != ' ' && *p != '\t' && *p != ',')
        p++;
    for (;;) {
        char *key, *val;
        p = skip_ws(p);
        if (*p == 0)
            return;
        if (*p == ',') {
            char *n = p + 1;
            util_trim(n);
            *name = n;
            return;
        }
        key = p;
        while (*p && *p != '=' && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '=') {
            /* bare word, ignore */
            if (*p == ' ' || *p == '\t')
                *p++ = 0;
            continue;
        }
        *p++ = 0;   /* terminate key */
        if (*p == '"') {
            val = ++p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
                *p++ = 0;
        } else {
            val = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',')
                p++;
            if (*p == ' ' || *p == '\t')
                *p++ = 0;
            else if (*p == ',') {
                /* value immediately followed by the name separator */
                char *n = p + 1;
                *p = 0;
                if (!strcasecmp(key, "group-title"))
                    *group = val;
                util_trim(n);
                *name = n;
                return;
            }
        }
        if (!strcasecmp(key, "group-title"))
            *group = val;
    }
}

int m3u_parse(m3u_list *l, char *text)
{
    char *p = text;
    const char *pend_name = NULL, *pend_group = NULL, *pend_ua = NULL, *pend_ref = NULL;
    const char *cur_grp = NULL;   /* #EXTGRP */

    memset(l, 0, sizeof(*l));
    l->text = text;

    /* skip UTF-8 BOM */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        util_trim(line);
        if (!*line)
            continue;

        if (line[0] == '#') {
            if (util_istarts(line, "#EXTINF:")) {
                parse_extinf(line + 8, &pend_name, &pend_group);
            } else if (util_istarts(line, "#EXTGRP:")) {
                char *g = line + 8;
                util_trim(g);
                cur_grp = g;
            } else if (util_istarts(line, "#EXTVLCOPT:")) {
                char *o = line + 11;
                if (util_istarts(o, "http-user-agent="))
                    pend_ua = o + 16;
                else if (util_istarts(o, "http-referrer="))
                    pend_ref = o + 14;
                else if (util_istarts(o, "http-referer="))
                    pend_ref = o + 13;
            }
            continue;
        }

        /* URL line */
        {
            m3u_entry e;
            const char *g = pend_group ? pend_group : (cur_grp ? cur_grp : "");
            e.url = line;
            e.name = (pend_name && *pend_name) ? pend_name : line;
            e.group = g;
            e.ua = pend_ua;
            e.referer = pend_ref;
            e.group_idx = group_index(l, g);
            if (e.group_idx >= 0)
                l->groups[e.group_idx].count++;
            if (add_entry(l, &e) != 0)
                return -1;
        }
        pend_name = pend_group = pend_ua = pend_ref = NULL;
    }
    return 0;
}

void m3u_free(m3u_list *l)
{
    free(l->items);
    free(l->groups);
    free(l->ghash);
    free(l->text);
    memset(l, 0, sizeof(*l));
}
