#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t *px;     /* 0xAARRGGBB, main RAM */
    int w, h;
} canvas_t;

typedef struct {
    uint32_t y_off, u_off, v_off;   /* RSX offsets */
    int w, h;
    int y_stride, uv_stride;
    int sar_num, sar_den;
} video_tex;

enum { ASPECT_FIT = 0, ASPECT_STRETCH = 1, ASPECT_ZOOM = 2, ASPECT_COUNT };

enum { ICON_CROSS, ICON_CIRCLE, ICON_TRIANGLE, ICON_SQUARE };

int gfx_init(void);
canvas_t *gfx_canvas(void);
int gfx_width(void);
int gfx_height(void);
float gfx_scale(void);      /* screen height / 720 */

/* allocate RSX-visible texture memory from the pool */
void *gfx_tex_alloc(size_t size, size_t align, uint32_t *offset);

void gfx_commit_overlay(void);  /* upload canvas to the overlay texture */

void gfx_frame_begin(void);
void gfx_draw_video(const video_tex *v, int aspect_mode);
void gfx_draw_overlay(void);
void gfx_frame_end(void);

/* canvas drawing (all coords in real screen pixels) */
void cv_clear(canvas_t *c, uint32_t argb);
void cv_fill(canvas_t *c, int x, int y, int w, int h, uint32_t argb);
void cv_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void cv_icon(canvas_t *c, int type, int cx, int cy, int r);

#endif
