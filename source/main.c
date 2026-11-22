/*
 * PS3 M3U Player - a simple IPTV / M3U playlist player for PS3 homebrew (PSL1GHT)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>
#include <lv2/sysfs.h>

#include "util.h"
#include "gfx.h"
#include "font.h"
#include "net.h"
#include "m3u.h"
#include "player.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define APP_TITLE   "M3U Player"
#define APP_VERSION "1.0"
#define DEFAULT_APPDIR "/dev_hdd0/game/M3UP00001/USRDIR"

/* colors */
#define C_BG_TOP     0xff0e1726
#define C_BG_BOTTOM  0xff070b12
#define C_HEADER     0xff111d30
#define C_TEXT       0xffffffff
#define C_DIM        0xff8f9bb3
#define C_ACCENT     0xff3d8bfd
#define C_SEL        0xff1f5fbf
#define C_STAR       0xffffc940
#define C_ERROR      0xffff6b6b

/* ------------------------------------------------------------------ */
/* app state                                                            */

enum { SCR_SOURCES, SCR_LOADING, SCR_GROUPS, SCR_CHANNELS, SCR_PLAYER };

typedef struct {
    char *name;
    char *url;     /* http(s) URL or local path; NULL for special rows */
    int kind;      /* 0 playlist, 1 favorites, 2 setting:quality, 3 setting:aspect, 4 rescan */
} source_t;

typedef struct {
    int sel, top;
} listpos;

static char g_appdir[256];
static int g_quit;
static int g_ok_is_circle;

static int g_maxh = 576;
static int g_aspect = ASPECT_FIT;

static source_t *g_sources;
static int g_nsources;

static m3u_list g_pl;          /* currently opened playlist */
static int g_pl_loaded;
static m3u_list g_fav;         /* favorites */
static char g_pl_title[128];

static const m3u_entry **g_view;   /* channels shown in SCR_CHANNELS */
static int g_nview;
static int g_view_is_fav;
static char g_view_title[160];

static int g_screen = SCR_SOURCES;
static listpos g_pos_sources, g_pos_groups, g_pos_channels;
static int g_dirty = 1;

/* playback */
static int g_play_index;
static uint64_t g_overlay_until;
static int g_show_info;

/* background playlist loader */
typedef struct {
    char *url;
    char *text;
    char err[160];
    volatile int done;
} loader_t;
static loader_t *g_loader;
static sys_ppu_thread_t g_loader_tid;
static int g_loader_running;
static int g_loader_cancelled;
static char g_message[200];     /* error toast on list screens */
static uint64_t g_message_until;

/* ------------------------------------------------------------------ */
/* input                                                                */

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_OK, B_BACK, B_TRIANGLE, B_SQUARE,
       B_L1, B_R1, B_L2, B_R2, B_START, B_SELECT, B_COUNT };

static int g_held[B_COUNT], g_pressed[B_COUNT];
static uint64_t g_repeat_at[B_COUNT];
static padData g_pad_last[7];

static void read_input(void)
{
    padInfo info;
    int raw[B_COUNT];
    int i, b;
    uint64_t now = util_time_us();

    memset(raw, 0, sizeof(raw));
    ioPadGetInfo(&info);
    for (i = 0; i < 7; i++) {
        padData d;
        if (!info.status[i])
            continue;
        if (ioPadGetData(i, &d) == 0 && d.len > 0)
            g_pad_last[i] = d;
        d = g_pad_last[i];
        raw[B_UP] |= d.BTN_UP;
        raw[B_DOWN] |= d.BTN_DOWN;
        raw[B_LEFT] |= d.BTN_LEFT;
        raw[B_RIGHT] |= d.BTN_RIGHT;
        raw[B_OK] |= g_ok_is_circle ? d.BTN_CIRCLE : d.BTN_CROSS;
        raw[B_BACK] |= g_ok_is_circle ? d.BTN_CROSS : d.BTN_CIRCLE;
        raw[B_TRIANGLE] |= d.BTN_TRIANGLE;
        raw[B_SQUARE] |= d.BTN_SQUARE;
        raw[B_L1] |= d.BTN_L1;
        raw[B_R1] |= d.BTN_R1;
        raw[B_L2] |= d.BTN_L2;
        raw[B_R2] |= d.BTN_R2;
        raw[B_START] |= d.BTN_START;
        raw[B_SELECT] |= d.BTN_SELECT;
    }

    for (b = 0; b < B_COUNT; b++) {
        int repeatable = (b == B_UP || b == B_DOWN || b == B_LEFT || b == B_RIGHT ||
                          b == B_L1 || b == B_R1);
        g_pressed[b] = 0;
        if (raw[b] && !g_held[b]) {
            g_pressed[b] = 1;
            g_repeat_at[b] = now + 400000ULL;
        } else if (raw[b] && repeatable && now >= g_repeat_at[b]) {
            g_pressed[b] = 1;
            g_repeat_at[b] = now + 70000ULL;
        }
        g_held[b] = raw[b];
        if (g_pressed[b])
            g_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* settings / favorites                                                 */

static void path_join(char *out, size_t n, const char *a, const char *b)
{
    snprintf(out, n, "%s/%s", a, b);
}

static void settings_load(void)
{
    char path[320];
    char *t;
    path_join(path, sizeof(path), g_appdir, "settings.txt");
    t = util_read_file(path, NULL);
    if (!t)
        return;
    {
        char *p = strstr(t, "maxh=");
        if (p)
            g_maxh = atoi(p + 5);
        p = strstr(t, "aspect=");
        if (p)
            g_aspect = atoi(p + 7);
    }
    if (g_maxh != 360 && g_maxh != 576 && g_maxh != 720 && g_maxh != 1080)
        g_maxh = 576;
    if (g_aspect < 0 || g_aspect >= ASPECT_COUNT)
        g_aspect = ASPECT_FIT;
    free(t);
}

static void settings_save(void)
{
    char path[320], buf[64];
    path_join(path, sizeof(path), g_appdir, "settings.txt");
    snprintf(buf, sizeof(buf), "maxh=%d\naspect=%d\n", g_maxh, g_aspect);
    util_write_file(path, buf, strlen(buf));
}

static void fav_load(void)
{
    char path[320];
    char *t;
    m3u_free(&g_fav);
    path_join(path, sizeof(path), g_appdir, "favorites.m3u");
    t = util_read_file(path, NULL);
    if (!t)
        t = util_strdup("#EXTM3U\n");
    if (t)
        m3u_parse(&g_fav, t);
}

static int fav_find(const char *url)
{
    int i;
    for (i = 0; i < g_fav.count; i++)
        if (strcmp(g_fav.items[i].url, url) == 0)
            return i;
    return -1;
}

static void buf_entry(util_buf *b, const m3u_entry *e)
{
    util_buf_append(b, "#EXTINF:-1", 10);
    if (e->group && *e->group) {
        util_buf_append(b, " group-title=\"", 14);
        util_buf_append(b, e->group, strlen(e->group));
        util_buf_append(b, "\"", 1);
    }
    util_buf_append(b, ",", 1);
    util_buf_append(b, e->name, strlen(e->name));
    util_buf_append(b, "\n", 1);
    if (e->ua && *e->ua) {
        util_buf_append(b, "#EXTVLCOPT:http-user-agent=", 27);
        util_buf_append(b, e->ua, strlen(e->ua));
        util_buf_append(b, "\n", 1);
    }
    if (e->referer && *e->referer) {
        util_buf_append(b, "#EXTVLCOPT:http-referrer=", 25);
        util_buf_append(b, e->referer, strlen(e->referer));
        util_buf_append(b, "\n", 1);
    }
    util_buf_append(b, e->url, strlen(e->url));
    util_buf_append(b, "\n", 1);
}

/* Toggles a favorite. Returns 1 if it is now a favorite. Invalidates g_fav pointers. */
static int fav_toggle(const m3u_entry *e)
{
    util_buf b;
    char path[320];
    int i, idx = fav_find(e->url), now_fav;
    char *text;

    memset(&b, 0, sizeof(b));
    util_buf_append(&b, "#EXTM3U\n", 8);
    for (i = 0; i < g_fav.count; i++)
        if (i != idx)
            buf_entry(&b, &g_fav.items[i]);
    if (idx < 0)
        buf_entry(&b, e);
    now_fav = idx < 0;

    path_join(path, sizeof(path), g_appdir, "favorites.m3u");
    util_write_file(path, b.data, b.len);

    text = b.data;   /* hand the buffer to the parser */
    m3u_free(&g_fav);
    m3u_parse(&g_fav, text);
    return now_fav;
}

/* ------------------------------------------------------------------ */
/* sources                                                              */

static void source_add(const char *name, const char *url, int kind)
{
    source_t *ns = realloc(g_sources, sizeof(source_t) * (g_nsources + 1));
    if (!ns)
        return;
    g_sources = ns;
    g_sources[g_nsources].name = util_strdup(name);
    g_sources[g_nsources].url = url ? util_strdup(url) : NULL;
    g_sources[g_nsources].kind = kind;
    g_nsources++;
}

static void sources_free(void)
{
    int i;
    for (i = 0; i < g_nsources; i++) {
        free(g_sources[i].name);
        free(g_sources[i].url);
    }
    free(g_sources);
    g_sources = NULL;
    g_nsources = 0;
}

static void scan_playlists_txt(const char *dir)
{
    char path[320];
    char *t, *p, *line;
    path_join(path, sizeof(path), dir, "playlists.txt");
    t = util_read_file(path, NULL);
    if (!t)
        return;
    p = t;
    while (*p) {
        char *bar;
        line = p;
        p = strchr(p, '\n');
        if (p)
            *p++ = 0;
        else
            p = line + strlen(line);
        util_trim(line);
        if (!*line || line[0] == '#')
            continue;
        bar = strchr(line, '|');
        if (bar) {
            *bar = 0;
            util_trim(line);
            util_trim(bar + 1);
            if (*(bar + 1))
                source_add(*line ? line : bar + 1, bar + 1, 0);
        } else {
            source_add(line, line, 0);
        }
    }
    free(t);
}

static void scan_dir_m3u(const char *dir)
{
    s32 fd;
    sysFSDirent ent;
    u64 nread;
    if (sysFsOpendir(dir, &fd) != 0)
        return;
    while (sysFsReaddir(fd, &ent, &nread) == 0 && nread > 0) {
        char full[512];
        if (ent.d_name[0] == '.')
            continue;
        if (!util_iends(ent.d_name, ".m3u") && !util_iends(ent.d_name, ".m3u8"))
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent.d_name);
        source_add(ent.d_name, full, 0);
    }
    sysFsClosedir(fd);
}

static void sources_scan(void)
{
    int i;
    sources_free();
    source_add("Favoriler", NULL, 1);

    scan_playlists_txt(g_appdir);
    scan_dir_m3u(g_appdir);
    for (i = 0; i < 8; i++) {
        char dir[64];
        snprintf(dir, sizeof(dir), "/dev_usb%03d", i);
        if (!util_dir_exists(dir))
            continue;
        scan_playlists_txt(dir);
        scan_dir_m3u(dir);
        snprintf(dir, sizeof(dir), "/dev_usb%03d/m3u", i);
        scan_playlists_txt(dir);
        scan_dir_m3u(dir);
    }
    source_add("", NULL, 2);   /* quality */
    source_add("", NULL, 3);   /* aspect */
    source_add("Kaynakları yeniden tara", NULL, 4);
}

/* ------------------------------------------------------------------ */
/* playlist loading                                                     */

static void loader_thread(void *arg)
{
    loader_t *l = (loader_t *)arg;
    l->text = net_get(l->url, NULL, NULL, l->err, sizeof(l->err));
    l->done = 1;
}

static void loader_reap(void)
{
    if (g_loader_running) {
        util_thread_join(g_loader_tid);
        g_loader_running = 0;
    }
    if (g_loader) {
        free(g_loader->url);
        free(g_loader->text);
        free(g_loader);
        g_loader = NULL;
    }
}

static void toast(const char *msg)
{
    util_strlcpy(g_message, msg, sizeof(g_message));
    g_message_until = util_time_us() + 5000000ULL;
    g_dirty = 1;
}

static void open_playlist(const source_t *src)
{
    loader_reap();
    g_loader = calloc(1, sizeof(loader_t));
    if (!g_loader)
        return;
    g_loader->url = util_strdup(src->url);
    util_strlcpy(g_pl_title, src->name, sizeof(g_pl_title));
    g_loader_cancelled = 0;
    if (util_thread_start(&g_loader_tid, loader_thread, g_loader, "loader", 1400, 256) != 0) {
        loader_reap();
        toast("Yükleme başlatılamadı");
        return;
    }
    g_loader_running = 1;
    g_screen = SCR_LOADING;
}

static void build_view_group(int group)   /* group -1 = all */
{
    int i, n = 0;
    free(g_view);
    g_view = malloc(sizeof(*g_view) * (g_pl.count ? g_pl.count : 1));
    if (!g_view) {
        g_nview = 0;
        return;
    }
    for (i = 0; i < g_pl.count; i++)
        if (group < 0 || g_pl.items[i].group_idx == group)
            g_view[n++] = &g_pl.items[i];
    g_nview = n;
    g_view_is_fav = 0;
    if (group < 0)
        snprintf(g_view_title, sizeof(g_view_title), "%s", "Tüm kanallar");
    else
        snprintf(g_view_title, sizeof(g_view_title), "%s", g_pl.groups[group].name);
}

static void build_view_fav(void)
{
    int i;
    free(g_view);
    g_view = malloc(sizeof(*g_view) * (g_fav.count ? g_fav.count : 1));
    g_nview = 0;
    if (!g_view)
        return;
    for (i = 0; i < g_fav.count; i++)
        g_view[i] = &g_fav.items[i];
    g_nview = g_fav.count;
    g_view_is_fav = 1;
    util_strlcpy(g_view_title, "Favoriler", sizeof(g_view_title));
}

static void poll_loader(void)
{
    if (!g_loader || !g_loader->done)
        return;
    util_thread_join(g_loader_tid);
    g_loader_running = 0;

    if (g_loader_cancelled) {
        loader_reap();
        return;
    }
    if (!g_loader->text) {
        toast(g_loader->err[0] ? g_loader->err : "Liste yüklenemedi");
        g_screen = SCR_SOURCES;
        loader_reap();
        return;
    }

    if (g_pl_loaded)
        m3u_free(&g_pl);
    g_pl_loaded = 0;
    if (m3u_parse(&g_pl, g_loader->text) != 0 || g_pl.count == 0) {
        if (g_pl.text == g_loader->text)
            g_loader->text = NULL;      /* owned by g_pl now */
        m3u_free(&g_pl);
        toast("Listede oynatılabilir kanal bulunamadı");
        g_screen = SCR_SOURCES;
        loader_reap();
        return;
    }
    g_loader->text = NULL;   /* owned by g_pl */
    g_pl_loaded = 1;
    loader_reap();

    if (g_pl.ngroups > 1) {
        memset(&g_pos_groups, 0, sizeof(g_pos_groups));
        g_screen = SCR_GROUPS;
    } else {
        build_view_group(-1);
        util_strlcpy(g_view_title, g_pl_title, sizeof(g_view_title));
        memset(&g_pos_channels, 0, sizeof(g_pos_channels));
        g_screen = SCR_CHANNELS;
    }
    g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                      */

static float g_s = 1.0f;
#define S(v) ((int)((v) * g_s))

static void draw_background(canvas_t *c, const char *title, const char *crumb)
{
    cv_vgradient(c, 0, 0, c->w, c->h, C_BG_TOP, C_BG_BOTTOM);
    cv_fill(c, 0, 0, c->w, S(88), C_HEADER);
    cv_fill(c, 0, S(88), c->w, S(2), C_ACCENT);
    font_draw(c, S(48), S(24), title, S(36), C_TEXT, S(640));
    if (crumb && *crumb) {
        int w = font_text_width(crumb, S(24));
        int maxw = S(520);
        if (w > maxw)
            w = maxw;
        font_draw(c, c->w - S(48) - w, S(34), crumb, S(24), C_DIM, maxw);
    }
}

typedef struct {
    int icon;
    const char *label;
} hint_t;

static void draw_hints(canvas_t *c, const hint_t *h, int n, const char *extra)
{
    int x = S(48), y = S(668), i;
    for (i = 0; i < n; i++) {
        if (h[i].icon >= 0) {
            cv_icon(c, h[i].icon, x + S(13), y + S(14), S(13));
            x += S(32);
        }
        x += font_draw(c, x, y, h[i].label, S(24), C_DIM, 0) + S(30);
    }
    if (extra && *extra) {
        int w = font_text_width(extra, S(22));
        font_draw(c, c->w - S(48) - w, y + S(2), extra, S(22), C_DIM, 0);
    }
}

#define LIST_TOP   112
#define ROW_H      50
#define LIST_ROWS  10

typedef const char *(*label_fn)(int i, char *tmp, size_t tmplen);

static void list_clamp(listpos *p, int count)
{
    if (p->sel >= count)
        p->sel = count - 1;
    if (p->sel < 0)
        p->sel = 0;
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + LIST_ROWS)
        p->top = p->sel - LIST_ROWS + 1;
    if (p->top > count - LIST_ROWS)
        p->top = count - LIST_ROWS;
    if (p->top < 0)
        p->top = 0;
}

static void list_input(listpos *p, int count)
{
    if (count <= 0)
        return;
    if (g_pressed[B_UP])
        p->sel = (p->sel > 0) ? p->sel - 1 : count - 1;
    if (g_pressed[B_DOWN])
        p->sel = (p->sel < count - 1) ? p->sel + 1 : 0;
    if (g_pressed[B_L1] || g_pressed[B_LEFT])
        p->sel -= LIST_ROWS;
    if (g_pressed[B_R1] || g_pressed[B_RIGHT])
        p->sel += LIST_ROWS;
    if (g_pressed[B_L2])
        p->sel = 0;
    if (g_pressed[B_R2])
        p->sel = count - 1;
    list_clamp(p, count);
}

static void draw_list(canvas_t *c, listpos *p, int count, label_fn label, label_fn right,
                      int numbered, int star_col)
{
    int r;
    char tmp[256], tmp2[64];
    int x0 = S(48), w = c->w - S(96);
    list_clamp(p, count);
    for (r = 0; r < LIST_ROWS; r++) {
        int i = p->top + r;
        int y = S(LIST_TOP + r * ROW_H);
        int tx = x0 + S(20);
        const char *rt;
        int rw = 0;
        if (i >= count)
            break;
        if (i == p->sel) {
            cv_fill(c, x0, y, w, S(ROW_H - 4), C_SEL);
            cv_fill(c, x0, y, S(6), S(ROW_H - 4), C_ACCENT);
        } else if (r & 1) {
            cv_fill(c, x0, y, w, S(ROW_H - 4), 0x18ffffff);
        }
        if (numbered) {
            char num[16];
            int nw;
            snprintf(num, sizeof(num), "%d", i + 1);
            nw = font_text_width(num, S(22));
            font_draw(c, tx + S(56) - nw, y + S(11), num, S(22), C_DIM, 0);
            tx += S(76);
        }
        rt = right ? right(i, tmp2, sizeof(tmp2)) : NULL;
        if (rt && *rt) {
            rw = font_text_width(rt, S(24));
            font_draw(c, x0 + w - S(20) - rw, y + S(10), rt, S(24),
                      star_col && i >= 0 ? C_STAR : C_DIM, 0);
            rw += S(24);
        }
        font_draw(c, tx, y + S(8), label(i, tmp, sizeof(tmp)), S(28), C_TEXT,
                  x0 + w - S(20) - rw - tx);
    }
    /* scrollbar */
    if (count > LIST_ROWS) {
        int track_y = S(LIST_TOP), track_h = S(LIST_ROWS * ROW_H - 4);
        int th = track_h * LIST_ROWS / count, ty;
        if (th < S(24))
            th = S(24);
        ty = track_y + (track_h - th) * p->top / (count - LIST_ROWS);
        cv_fill(c, c->w - S(36), track_y, S(5), track_h, 0x30ffffff);
        cv_fill(c, c->w - S(36), ty, S(5), th, C_ACCENT);
    }
}

static void draw_toast(canvas_t *c)
{
    int w, x, y;
    if (!g_message[0] || util_time_us() > g_message_until)
        return;
    w = font_text_width(g_message, S(26)) + S(48);
    if (w > c->w - S(96))
        w = c->w - S(96);
    x = (c->w - w) / 2;
    y = S(590);
    cv_fill(c, x, y, w, S(52), 0xe0301a1a);
    cv_fill(c, x, y, S(6), S(52), C_ERROR);
    font_draw(c, x + S(24), y + S(12), g_message, S(26), C_TEXT, w - S(36));
}

static void draw_center_message(canvas_t *c, const char *line1, const char *line2)
{
    int w1 = font_text_width(line1, S(30));
    font_draw(c, (c->w - w1) / 2, S(300), line1, S(30), C_TEXT, 0);
    if (line2 && *line2) {
        int w2 = font_text_width(line2, S(24));
        if (w2 > c->w - S(96))
            w2 = c->w - S(96);
        font_draw(c, (c->w - w2) / 2, S(350), line2, S(24), C_DIM, c->w - S(96));
    }
}

/* ------------------------------------------------------------------ */
/* screens                                                              */

static const char *quality_label(void)
{
    switch (g_maxh) {
    case 360: return "360p (en hızlı)";
    case 720: return "720p";
    case 1080: return "En yüksek";
    default: return "576p (önerilen)";
    }
}

static const char *aspect_label(int a)
{
    switch (a) {
    case ASPECT_STRETCH: return "Ekrana yay";
    case ASPECT_ZOOM: return "Yakınlaştır";
    default: return "Sığdır";
    }
}

static const char *src_label(int i, char *tmp, size_t n)
{
    switch (g_sources[i].kind) {
    case 2: snprintf(tmp, n, "Ayar: HLS kalite sınırı"); return tmp;
    case 3: snprintf(tmp, n, "Ayar: Görüntü oranı"); return tmp;
    default: return g_sources[i].name;
    }
}

static const char *src_right(int i, char *tmp, size_t n)
{
    switch (g_sources[i].kind) {
    case 1: snprintf(tmp, n, "%d kanal", g_fav.count); return tmp;
    case 2: return quality_label();
    case 3: return aspect_label(g_aspect);
    case 0:
        return util_istarts(g_sources[i].url, "http") ? "URL" : "Dosya";
    default: return NULL;
    }
}

static void screen_sources(canvas_t *c, int draw)
{
    if (!draw) {
        list_input(&g_pos_sources, g_nsources);
        if (g_pressed[B_OK] && g_nsources > 0) {
            source_t *s = &g_sources[g_pos_sources.sel];
            switch (s->kind) {
            case 0:
                open_playlist(s);
                break;
            case 1:
                build_view_fav();
                memset(&g_pos_channels, 0, sizeof(g_pos_channels));
                g_screen = SCR_CHANNELS;
                break;
            case 2:
                g_maxh = g_maxh == 360 ? 576 : g_maxh == 576 ? 720 : g_maxh == 720 ? 1080 : 360;
                settings_save();
                break;
            case 3:
                g_aspect = (g_aspect + 1) % ASPECT_COUNT;
                settings_save();
                break;
            case 4:
                sources_scan();
                toast(g_nsources > 4 ? "Kaynaklar güncellendi" : "Hiç liste bulunamadı");
                break;
            }
        }
        if (g_pressed[B_START])
            g_quit = 1;
        return;
    }
    {
        static const hint_t h[] = { { ICON_CROSS, "Seç" }, { -1, "START: Çıkış" } };
        hint_t hh[2];
        memcpy(hh, h, sizeof(hh));
        if (g_ok_is_circle)
            hh[0].icon = ICON_CIRCLE;
        draw_background(c, APP_TITLE, "Kaynaklar");
        draw_list(c, &g_pos_sources, g_nsources, src_label, src_right, 0, 0);
        if (g_nsources <= 4)
            draw_center_message(c, "Liste bulunamadı",
                                "USB belleğin köküne .m3u dosyası veya playlists.txt koyup \"yeniden tara\"yı seçin");
        draw_hints(c, hh, 2, "v" APP_VERSION);
    }
}

static void screen_loading(canvas_t *c, int draw)
{
    if (!draw) {
        poll_loader();
        if (g_screen == SCR_LOADING && g_pressed[B_BACK]) {
            g_loader_cancelled = 1;
            g_screen = SCR_SOURCES;
        }
        return;
    }
    {
        static int spin;
        char dots[8] = "";
        int i;
        hint_t h[1] = { { ICON_CIRCLE, "İptal" } };
        if (g_ok_is_circle)
            h[0].icon = ICON_CROSS;
        spin = (spin + 1) % 40;
        for (i = 0; i < spin / 10; i++)
            strcat(dots, ".");
        draw_background(c, APP_TITLE, g_pl_title);
        {
            char line[64];
            snprintf(line, sizeof(line), "Liste yükleniyor%s", dots);
            draw_center_message(c, line, g_pl_title);
        }
        draw_hints(c, h, 1, NULL);
    }
}

static const char *grp_label(int i, char *tmp, size_t n)
{
    if (i == 0)
        return "Tüm kanallar";
    snprintf(tmp, n, "%s", g_pl.groups[i - 1].name);
    return tmp;
}

static const char *grp_right(int i, char *tmp, size_t n)
{
    snprintf(tmp, n, "%d", i == 0 ? g_pl.count : g_pl.groups[i - 1].count);
    return tmp;
}

static void screen_groups(canvas_t *c, int draw)
{
    int count = g_pl.ngroups + 1;
    if (!draw) {
        list_input(&g_pos_groups, count);
        if (g_pressed[B_OK]) {
            build_view_group(g_pos_groups.sel - 1);
            memset(&g_pos_channels, 0, sizeof(g_pos_channels));
            g_screen = SCR_CHANNELS;
        } else if (g_pressed[B_BACK]) {
            g_screen = SCR_SOURCES;
        }
        return;
    }
    {
        hint_t h[2] = { { ICON_CROSS, "Aç" }, { ICON_CIRCLE, "Geri" } };
        if (g_ok_is_circle) {
            h[0].icon = ICON_CIRCLE;
            h[1].icon = ICON_CROSS;
        }
        draw_background(c, g_pl_title, "Gruplar");
        draw_list(c, &g_pos_groups, count, grp_label, grp_right, 0, 0);
        draw_hints(c, h, 2, NULL);
    }
}

static const char *ch_label(int i, char *tmp, size_t n)
{
    (void)tmp; (void)n;
    return g_view[i]->name;
}

static const char *ch_right(int i, char *tmp, size_t n)
{
    (void)tmp; (void)n;
    if (g_view_is_fav || fav_find(g_view[i]->url) >= 0)
        return "\xe2\x98\x85";   /* ★ */
    return NULL;
}

static void start_playback(int index)
{
    const m3u_entry *e;
    if (index < 0 || index >= g_nview)
        return;
    g_play_index = index;
    e = g_view[index];
    player_play(e->url, e->ua, e->referer, g_maxh);
    g_overlay_until = util_time_us() + 6000000ULL;
    g_screen = SCR_PLAYER;
    g_dirty = 1;
}

static void toggle_fav_current(const m3u_entry *e)
{
    int now;
    m3u_entry copy;
    char *name = util_strdup(e->name), *url = util_strdup(e->url);
    char *group = util_strdup(e->group ? e->group : "");
    char *ua = e->ua ? util_strdup(e->ua) : NULL, *ref = e->referer ? util_strdup(e->referer) : NULL;
    copy.name = name;
    copy.url = url;
    copy.group = group;
    copy.ua = ua;
    copy.referer = ref;
    copy.group_idx = -1;
    now = fav_toggle(&copy);   /* may invalidate e if it points into g_fav */
    if (g_view_is_fav) {
        int sel = g_pos_channels.sel;
        build_view_fav();
        g_pos_channels.sel = sel;
        list_clamp(&g_pos_channels, g_nview);
    }
    toast(now ? "Favorilere eklendi" : "Favorilerden çıkarıldı");
    free(name);
    free(url);
    free(group);
    free(ua);
    free(ref);
}

static void screen_channels(canvas_t *c, int draw)
{
    if (!draw) {
        list_input(&g_pos_channels, g_nview);
        if (g_pressed[B_OK] && g_nview > 0) {
            start_playback(g_pos_channels.sel);
        } else if (g_pressed[B_TRIANGLE] && g_nview > 0) {
            toggle_fav_current(g_view[g_pos_channels.sel]);
        } else if (g_pressed[B_BACK]) {
            if (g_view_is_fav || !g_pl_loaded || g_pl.ngroups <= 1)
                g_screen = SCR_SOURCES;
            else
                g_screen = SCR_GROUPS;
        }
        return;
    }
    {
        hint_t h[4] = { { ICON_CROSS, "Oynat" }, { ICON_CIRCLE, "Geri" },
                        { ICON_TRIANGLE, "Favori" }, { -1, "L1/R1: Sayfa" } };
        char crumb[64];
        if (g_ok_is_circle) {
            h[0].icon = ICON_CIRCLE;
            h[1].icon = ICON_CROSS;
        }
        snprintf(crumb, sizeof(crumb), "%d kanal", g_nview);
        draw_background(c, g_view_title, crumb);
        draw_list(c, &g_pos_channels, g_nview, ch_label, ch_right, 1, 1);
        if (g_nview == 0)
            draw_center_message(c, g_view_is_fav ? "Henüz favori yok" : "Bu grupta kanal yok",
                                g_view_is_fav ? "Kanal listesinde Üçgen tuşuyla favori ekleyin" : "");
        draw_hints(c, h, 4, NULL);
    }
}

static void screen_player(canvas_t *c, int draw)
{
    char msg[160];
    player_state st = player_get_state(msg, sizeof(msg));
    uint64_t now = util_time_us();

    if (!draw) {
        if (g_pressed[B_BACK]) {
            player_stop();
            g_screen = SCR_CHANNELS;
            g_pos_channels.sel = g_play_index;
            list_clamp(&g_pos_channels, g_nview);
            return;
        }
        if ((g_pressed[B_UP] || g_pressed[B_R1]) && g_nview > 1)
            start_playback((g_play_index + g_nview - 1) % g_nview);
        else if ((g_pressed[B_DOWN] || g_pressed[B_L1]) && g_nview > 1)
            start_playback((g_play_index + 1) % g_nview);
        if (g_pressed[B_OK]) {
            if (now < g_overlay_until && !g_show_info)
                g_show_info = 1;
            else if (now < g_overlay_until)
                g_overlay_until = 0, g_show_info = 0;
            else
                g_overlay_until = now + 6000000ULL;
        }
        if (g_pressed[B_SQUARE]) {
            g_aspect = (g_aspect + 1) % ASPECT_COUNT;
            settings_save();
            g_overlay_until = now + 3000000ULL;
        }
        if (g_pressed[B_TRIANGLE] && g_play_index < g_nview) {
            toggle_fav_current(g_view[g_play_index]);
            if (g_play_index >= g_nview)
                g_play_index = g_nview - 1;
            g_overlay_until = now + 3000000ULL;
        }
        if (g_pressed[B_LEFT] || g_pressed[B_RIGHT] || g_pressed[B_SELECT])
            g_overlay_until = now + 6000000ULL;
        return;
    }

    cv_clear(c, 0);
    if (st == PS_PLAYING && now > g_overlay_until)
        return;   /* clean picture */

    {
        const m3u_entry *e = (g_play_index < g_nview) ? g_view[g_play_index] : NULL;
        char title[200], info[160];
        int is_fav = e && fav_find(e->url) >= 0;
        hint_t h[5] = { { ICON_CIRCLE, "Geri" }, { -1, "Yukarı/Aşağı: Kanal" },
                        { ICON_CROSS, "Bilgi" }, { ICON_SQUARE, "" }, { ICON_TRIANGLE, "Favori" } };
        char aspect_hint[48];
        if (g_ok_is_circle) {
            h[0].icon = ICON_CROSS;
            h[2].icon = ICON_CIRCLE;
        }
        snprintf(aspect_hint, sizeof(aspect_hint), "%s", aspect_label(g_aspect));
        h[3].label = aspect_hint;

        /* top bar */
        cv_vgradient(c, 0, 0, c->w, S(120), 0xd0000000, 0x00000000);
        snprintf(title, sizeof(title), "%d  %s%s", g_play_index + 1, e ? e->name : "",
                 is_fav ? "  \xe2\x98\x85" : "");
        font_draw(c, S(48), S(28), title, S(34), C_TEXT, c->w - S(96));

        /* bottom bar */
        cv_vgradient(c, 0, S(560), c->w, S(160), 0x00000000, 0xd8000000);
        if (st == PS_ERROR) {
            font_draw(c, S(48), S(600), msg, S(28), C_ERROR, c->w - S(96));
        } else if (st == PS_ENDED || st == PS_CONNECTING || st == PS_BUFFERING) {
            font_draw(c, S(48), S(600), msg[0] ? msg : "Bekleniyor...", S(28), C_TEXT,
                      c->w - S(96));
        } else if (g_show_info) {
            player_info(info, sizeof(info));
            font_draw(c, S(48), S(600), info, S(24), C_DIM, c->w - S(96));
        }
        if (g_show_info && st != PS_PLAYING) {
            player_info(info, sizeof(info));
            if (info[0])
                font_draw(c, S(48), S(560), info, S(22), C_DIM, c->w - S(96));
        }
        draw_hints(c, h, 5, NULL);
    }
}

/* ------------------------------------------------------------------ */

static void sys_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == SYSUTIL_EXIT_GAME)
        g_quit = 1;
}

static int g_cleaned;
static void app_cleanup(void)
{
    if (g_cleaned)
        return;
    g_cleaned = 1;
    player_shutdown();
    if (g_loader_running) {
        util_thread_join(g_loader_tid);
        g_loader_running = 0;
    }
    net_shutdown();
}

static void init_appdir(const char *argv0)
{
    util_strlcpy(g_appdir, DEFAULT_APPDIR, sizeof(g_appdir));
    if (argv0 && util_istarts(argv0, "/dev_")) {
        char tmp[256];
        char *slash;
        util_strlcpy(tmp, argv0, sizeof(tmp));
        slash = strrchr(tmp, '/');
        if (slash && slash != tmp) {
            *slash = 0;
            util_strlcpy(g_appdir, tmp, sizeof(g_appdir));
        }
    }
    if (!util_dir_exists(g_appdir))
        util_mkdirs(g_appdir);
}

int main(int argc, const char *argv[])
{
    canvas_t *c;
    int last_screen = -1;
    uint64_t last_draw = 0;
    player_state last_state = PS_IDLE;
    char last_msg[160] = "";

    init_appdir(argc > 0 ? argv[0] : NULL);

    if (gfx_init() != 0)
        return 0;
    g_s = gfx_scale();
    c = gfx_canvas();

    ioPadInit(7);
    {
        s32 v = 1;
        if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &v) == 0)
            g_ok_is_circle = (v == 0);
    }
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);

    font_init();
    net_init();
    player_init();
    atexit(app_cleanup);

    settings_load();
    fav_load();
    sources_scan();

    while (!g_quit) {
        uint64_t now;
        video_tex vt;
        int have_video = 0;

        sysUtilCheckCallback();
        read_input();

        switch (g_screen) {
        case SCR_SOURCES:  screen_sources(c, 0); break;
        case SCR_LOADING:  screen_loading(c, 0); break;
        case SCR_GROUPS:   screen_groups(c, 0); break;
        case SCR_CHANNELS: screen_channels(c, 0); break;
        case SCR_PLAYER:   screen_player(c, 0); break;
        }

        now = util_time_us();
        if (g_screen != last_screen)
            g_dirty = 1;
        if (g_screen == SCR_LOADING && now - last_draw > 100000ULL)
            g_dirty = 1;
        if (g_message[0] && now > g_message_until) {
            g_message[0] = 0;
            g_dirty = 1;
        }
        if (g_screen == SCR_PLAYER) {
            char msg[160];
            player_state st = player_get_state(msg, sizeof(msg));
            if (st != last_state || strcmp(msg, last_msg) != 0) {
                g_dirty = 1;
                last_state = st;
                util_strlcpy(last_msg, msg, sizeof(last_msg));
            }
            /* hide/show the overlay on its timer */
            if (g_overlay_until && now > g_overlay_until && last_draw < g_overlay_until)
                g_dirty = 1;
        }

        if (g_dirty) {
            switch (g_screen) {
            case SCR_SOURCES:  screen_sources(c, 1); break;
            case SCR_LOADING:  screen_loading(c, 1); break;
            case SCR_GROUPS:   screen_groups(c, 1); break;
            case SCR_CHANNELS: screen_channels(c, 1); break;
            case SCR_PLAYER:   screen_player(c, 1); break;
            }
            draw_toast(c);
            gfx_commit_overlay();
            g_dirty = 0;
            last_draw = now;
            last_screen = g_screen;
        }

        gfx_frame_begin();
        if (g_screen == SCR_PLAYER)
            have_video = player_video_frame(&vt);
        if (have_video)
            gfx_draw_video(&vt, g_aspect);
        gfx_draw_overlay();
        gfx_frame_end();
        player_frame_done();
    }

    app_cleanup();
    sources_free();
    if (g_pl_loaded)
        m3u_free(&g_pl);
    m3u_free(&g_fav);
    free(g_view);
    return 0;
}
