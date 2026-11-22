#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include <tiny3d.h>

#include "gfx.h"

#define TEX_POOL_SIZE (64 * 1024 * 1024)

static canvas_t g_canvas;
static uint8_t *g_pool;
static size_t g_pool_used;
static void *g_ov_ptr[2];
static uint32_t g_ov_off[2];
static int g_ov_front = -1;
static int g_w, g_h;

int gfx_init(void)
{
    tiny3d_Init(1024 * 1024);

    g_w = Video_Resolution.width;
    g_h = Video_Resolution.height;
    if (g_w <= 0 || g_h <= 0) {
        g_w = 1280;
        g_h = 720;
    }

    g_pool = tiny3d_AllocTexture(TEX_POOL_SIZE);
    if (!g_pool)
        return -1;
    g_pool_used = 0;

    g_canvas.w = g_w;
    g_canvas.h = g_h;
    g_canvas.px = memalign(128, (size_t)g_w * g_h * 4);
    if (!g_canvas.px)
        return -1;
    memset(g_canvas.px, 0, (size_t)g_w * g_h * 4);

    g_ov_ptr[0] = gfx_tex_alloc((size_t)g_w * g_h * 4, 128, &g_ov_off[0]);
    g_ov_ptr[1] = gfx_tex_alloc((size_t)g_w * g_h * 4, 128, &g_ov_off[1]);
    if (!g_ov_ptr[0] || !g_ov_ptr[1])
        return -1;
    return 0;
}

canvas_t *gfx_canvas(void) { return &g_canvas; }
int gfx_width(void) { return g_w; }
int gfx_height(void) { return g_h; }
float gfx_scale(void) { return (float)g_h / 720.0f; }

void *gfx_tex_alloc(size_t size, size_t align, uint32_t *offset)
{
    size_t start = (g_pool_used + align - 1) & ~(align - 1);
    void *p;
    if (start + size > TEX_POOL_SIZE)
        return NULL;
    p = g_pool + start;
    g_pool_used = start + size;
    if (offset)
        *offset = tiny3d_TextureOffset(p);
    return p;
}

void gfx_commit_overlay(void)
{
    int back = (g_ov_front == 0) ? 1 : 0;
    memcpy(g_ov_ptr[back], g_canvas.px, (size_t)g_w * g_h * 4);
    g_ov_front = back;
}

void gfx_frame_begin(void)
{
    tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
    tiny3d_AlphaTest(1, 0x01, TINY3D_ALPHA_FUNC_GEQUAL);
    tiny3d_BlendFunc(1, TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
                     TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ZERO,
                     TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    tiny3d_Project2D();
}

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 int yuv)
{
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, 65535);
    if (yuv)
        tiny3d_VertexFcolor(1.0f, 1.0f, 1.0f, 0.0f);
    else
        tiny3d_VertexColor(0xffffffff);
    tiny3d_VertexTexture(u0, v0);
    tiny3d_VertexPos(x + w, y, 65535);
    tiny3d_VertexTexture(u1, v0);
    tiny3d_VertexPos(x + w, y + h, 65535);
    tiny3d_VertexTexture(u1, v1);
    tiny3d_VertexPos(x, y + h, 65535);
    tiny3d_VertexTexture(u0, v1);
    tiny3d_End();
}

void gfx_draw_video(const video_tex *v, int aspect_mode)
{
    const float VW = 848.0f, VH = 512.0f;
    float screen_dar = (Video_aspect == 1) ? (4.0f / 3.0f) : (16.0f / 9.0f);
    float sar = (v->sar_num > 0 && v->sar_den > 0) ? (float)v->sar_num / (float)v->sar_den : 1.0f;
    float video_dar = ((float)v->w * sar) / (float)v->h;
    float dw = VW, dh = VH, u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

    if (v->w <= 0 || v->h <= 0)
        return;

    if (aspect_mode == ASPECT_FIT) {
        if (video_dar > screen_dar)
            dh = VH * screen_dar / video_dar;
        else
            dw = VW * video_dar / screen_dar;
    } else if (aspect_mode == ASPECT_ZOOM) {
        /* fill the screen, crop the overflow */
        if (video_dar > screen_dar) {
            float keep = screen_dar / video_dar;
            u0 = (1.0f - keep) * 0.5f;
            u1 = 1.0f - u0;
        } else {
            float keep = video_dar / screen_dar;
            v0 = (1.0f - keep) * 0.5f;
            v1 = 1.0f - v0;
        }
    }

    tiny3d_Enable_YUV(USE_YUV_8BIT_TEXTURES);
    tiny3d_SetTextureWrap(0, v->y_off, v->w, v->h, v->y_stride,
                          TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    tiny3d_SetTextureWrap(1, v->u_off, (v->w + 1) / 2, (v->h + 1) / 2, v->uv_stride,
                          TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    tiny3d_SetTextureWrap(2, v->v_off, (v->w + 1) / 2, (v->h + 1) / 2, v->uv_stride,
                          TINY3D_TEX_FORMAT_L8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    quad((VW - dw) * 0.5f, (VH - dh) * 0.5f, dw, dh, u0, v0, u1, v1, 1);
    tiny3d_Disable_YUV();
}

void gfx_draw_overlay(void)
{
    if (g_ov_front < 0)
        return;
    tiny3d_SetTextureWrap(0, g_ov_off[g_ov_front], g_w, g_h, g_w * 4,
                          TINY3D_TEX_FORMAT_A8R8G8B8, TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, TEXTURE_LINEAR);
    quad(0.0f, 0.0f, 848.0f, 512.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0);
}

void gfx_frame_end(void)
{
    tiny3d_Flip();
}

/* ------------------------------------------------------------------ */

static inline uint32_t blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24, ia, da, oa, r, g, b;
    if (a == 255)
        return src;
    if (a == 0)
        return dst;
    ia = 255 - a;
    da = dst >> 24;
    oa = a + (da * ia) / 255;
    r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia) / 255;
    g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia) / 255;
    b = ((src & 255) * a + (dst & 255) * ia) / 255;
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

void cv_clear(canvas_t *c, uint32_t argb)
{
    size_t i, n = (size_t)c->w * c->h;
    if (argb == 0) {
        memset(c->px, 0, n * 4);
        return;
    }
    for (i = 0; i < n; i++)
        c->px[i] = argb;
}

void cv_fill(canvas_t *c, int x, int y, int w, int h, uint32_t argb)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++) {
        uint32_t *row = c->px + (size_t)(y + j) * c->w + x;
        if ((argb >> 24) == 255) {
            for (i = 0; i < w; i++)
                row[i] = argb;
        } else {
            for (i = 0; i < w; i++)
                row[i] = blend(row[i], argb);
        }
    }
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int t, int max)
{
    uint32_t out = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        out |= (uint32_t)((ca + (cb - ca) * t / max) & 255) << sh;
    }
    return out;
}

void cv_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    int j;
    for (j = 0; j < h; j++)
        cv_fill(c, x, y + j, w, 1, lerp_color(top, bottom, j, h > 1 ? h - 1 : 1));
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    float dx, dy;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    dx = ax + t * vx - px;
    dy = ay + t * vy - py;
    return sqrtf(dx * dx + dy * dy);
}

/* PlayStation-style face button glyphs drawn procedurally */
void cv_icon(canvas_t *c, int type, int cx, int cy, int r)
{
    static const uint32_t colors[4] = { 0xff7cb2e8, 0xffff6666, 0xff40e2a0, 0xffff69f8 };
    float th = r * 0.22f;
    int x, y;
    for (y = cy - r - 2; y <= cy + r + 2; y++) {
        if (y < 0 || y >= c->h)
            continue;
        for (x = cx - r - 2; x <= cx + r + 2; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy, d = 1e9f, cov;
            if (x < 0 || x >= c->w)
                continue;
            switch (type) {
            case ICON_CROSS: {
                float k = r * 0.62f;
                float d1 = seg_dist(px, py, -k, -k, k, k);
                float d2 = seg_dist(px, py, -k, k, k, -k);
                d = (d1 < d2 ? d1 : d2) - th * 0.5f;
                break;
            }
            case ICON_CIRCLE: {
                float rr = sqrtf(px * px + py * py);
                d = fabsf(rr - r * 0.62f) - th * 0.5f;
                break;
            }
            case ICON_TRIANGLE: {
                float k = r * 0.72f;
                float ax = 0, ay = -k, bx = k * 0.87f, by = k * 0.5f, qx = -k * 0.87f, qy = k * 0.5f;
                float d1 = seg_dist(px, py, ax, ay, bx, by);
                float d2 = seg_dist(px, py, bx, by, qx, qy);
                float d3 = seg_dist(px, py, qx, qy, ax, ay);
                d = d1 < d2 ? d1 : d2;
                d = (d < d3 ? d : d3) - th * 0.5f;
                break;
            }
            default: {
                float k = r * 0.55f;
                float d1 = seg_dist(px, py, -k, -k, k, -k);
                float d2 = seg_dist(px, py, k, -k, k, k);
                float d3 = seg_dist(px, py, k, k, -k, k);
                float d4 = seg_dist(px, py, -k, k, -k, -k);
                d = d1 < d2 ? d1 : d2;
                d = d < d3 ? d : d3;
                d = (d < d4 ? d : d4) - th * 0.5f;
                break;
            }
            }
            cov = 0.5f - d;
            if (cov <= 0)
                continue;
            if (cov > 1)
                cov = 1;
            c->px[(size_t)y * c->w + x] =
                blend(c->px[(size_t)y * c->w + x],
                      ((uint32_t)(cov * 255) << 24) | (colors[type] & 0x00ffffff));
        }
    }
}
