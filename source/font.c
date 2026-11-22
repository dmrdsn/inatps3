#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"
#include "util.h"

#define MAX_FACES   4
#define CACHE_SIZE  4096   /* power of two */

typedef struct {
    uint32_t cp;
    int16_t px;          /* 0 = empty slot */
    int16_t left, top, adv;
    uint16_t w, h;
    uint8_t *bmp;
} glyph_t;

static FT_Library g_ft;
static FT_Face g_faces[MAX_FACES];
static int g_nfaces;
static int g_face_px[MAX_FACES];
static glyph_t g_cache[CACHE_SIZE];
static int g_cache_used;

static const char *k_font_paths[] = {
    "/dev_flash/data/font/SCE-PS3-SR-R-LATIN2.TTF",
    "/dev_flash/data/font/SCE-PS3-VR-R-LATIN2.TTF",
    "/dev_flash/data/font/SCE-PS3-SR-R-LATIN.TTF",
    "/dev_flash/data/font/SCE-PS3-VR-R-LATIN.TTF",
    "/dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF",
    "/dev_flash/data/font/SCE-PS3-SR-R-JPN.TTF",
    NULL
};

int font_init(void)
{
    int i;
    if (FT_Init_FreeType(&g_ft) != 0)
        return 0;
    for (i = 0; k_font_paths[i] && g_nfaces < MAX_FACES; i++) {
        FT_Face f;
        if (FT_New_Face(g_ft, k_font_paths[i], 0, &f) == 0) {
            g_faces[g_nfaces] = f;
            g_face_px[g_nfaces] = 0;
            g_nfaces++;
        }
    }
    return g_nfaces;
}

static void cache_flush(void)
{
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        free(g_cache[i].bmp);
        g_cache[i].bmp = NULL;
        g_cache[i].px = 0;
    }
    g_cache_used = 0;
}

static const glyph_t *get_glyph(uint32_t cp, int px)
{
    unsigned h = ((cp * 2654435761u) ^ (unsigned)(px * 97)) & (CACHE_SIZE - 1);
    glyph_t *g;
    int i;

    for (;;) {
        g = &g_cache[h];
        if (g->px == 0)
            break;
        if (g->cp == cp && g->px == px)
            return g;
        h = (h + 1) & (CACHE_SIZE - 1);
    }

    if (g_cache_used > CACHE_SIZE * 3 / 4) {
        cache_flush();
        return get_glyph(cp, px);
    }

    g->cp = cp;
    g->px = (int16_t)px;
    g->left = g->top = 0;
    g->adv = (int16_t)(px / 3);
    g->w = g->h = 0;
    g->bmp = NULL;
    g_cache_used++;

    for (i = 0; i < g_nfaces; i++) {
        FT_Face f = g_faces[i];
        FT_UInt idx = FT_Get_Char_Index(f, cp);
        FT_GlyphSlot slot;
        if (idx == 0)
            continue;
        if (g_face_px[i] != px) {
            FT_Set_Pixel_Sizes(f, 0, px);
            g_face_px[i] = px;
        }
        if (FT_Load_Glyph(f, idx, FT_LOAD_RENDER) != 0)
            continue;
        slot = f->glyph;
        g->left = (int16_t)slot->bitmap_left;
        g->top = (int16_t)slot->bitmap_top;
        g->adv = (int16_t)(slot->advance.x >> 6);
        g->w = (uint16_t)slot->bitmap.width;
        g->h = (uint16_t)slot->bitmap.rows;
        if (g->w && g->h) {
            int r;
            g->bmp = malloc((size_t)g->w * g->h);
            if (g->bmp)
                for (r = 0; r < g->h; r++)
                    memcpy(g->bmp + (size_t)r * g->w,
                           slot->bitmap.buffer + (size_t)r * slot->bitmap.pitch, g->w);
        }
        break;
    }
    return g;
}

int font_text_width(const char *s, int px)
{
    int w = 0;
    uint32_t cp;
    while ((cp = util_utf8_next(&s)) != 0)
        w += get_glyph(cp, px)->adv;
    return w;
}

static void blit_glyph(canvas_t *c, const glyph_t *g, int pen_x, int base_y, uint32_t argb)
{
    int gx = pen_x + g->left, gy = base_y - g->top, r, k;
    uint32_t ca = argb >> 24, cr = (argb >> 16) & 255, cg = (argb >> 8) & 255, cb = argb & 255;
    if (!g->bmp)
        return;
    for (r = 0; r < g->h; r++) {
        int y = gy + r;
        uint32_t *row;
        const uint8_t *src;
        if (y < 0 || y >= c->h)
            continue;
        row = c->px + (size_t)y * c->w;
        src = g->bmp + (size_t)r * g->w;
        for (k = 0; k < g->w; k++) {
            int x = gx + k;
            uint32_t a, d, ia, da, oa;
            if (x < 0 || x >= c->w || src[k] == 0)
                continue;
            a = (src[k] * ca) / 255;
            d = row[x];
            ia = 255 - a;
            da = d >> 24;
            oa = a + (da * ia) / 255;
            row[x] = (oa << 24) |
                     (((cr * a + ((d >> 16) & 255) * ia) / 255) << 16) |
                     (((cg * a + ((d >> 8) & 255) * ia) / 255) << 8) |
                     ((cb * a + (d & 255) * ia) / 255);
        }
    }
}

int font_draw(canvas_t *c, int x, int y, const char *s, int px, uint32_t argb, int max_w)
{
    int pen = x, base = y + (px * 4) / 5;
    int dots_w = 0;
    const char *p = s;
    uint32_t cp;

    if (max_w > 0 && font_text_width(s, px) > max_w)
        dots_w = font_text_width("...", px);
    else
        max_w = 0;

    while ((cp = util_utf8_next(&p)) != 0) {
        const glyph_t *g = get_glyph(cp, px);
        if (max_w > 0 && (pen - x) + g->adv > max_w - dots_w) {
            const char *d = "...";
            uint32_t dc;
            while ((dc = util_utf8_next(&d)) != 0) {
                const glyph_t *dg = get_glyph(dc, px);
                blit_glyph(c, dg, pen, base, argb);
                pen += dg->adv;
            }
            break;
        }
        blit_glyph(c, g, pen, base, argb);
        pen += g->adv;
    }
    return pen - x;
}
