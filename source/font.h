#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include "gfx.h"

int font_init(void);                       /* returns number of faces loaded */
int font_text_width(const char *utf8, int px);
/* Draws text with its top-left at (x, y). Truncates with "..." beyond max_w (0 = no limit).
   Returns the drawn width. */
int font_draw(canvas_t *c, int x, int y, const char *utf8, int px, uint32_t argb, int max_w);

#endif
