/* libbeepyfb/font.h -- embedded 5x7 font and text drawing. */
#ifndef BEEPYFB_FONT_H
#define BEEPYFB_FONT_H

#include "canvas.h"

#define GLYPH_W 5
#define GLYPH_H 7
#define CELL_W 6 /* glyph + 1px spacing */
#define CELL_H 8

const unsigned char *glyph(int c);
int text_w(const char *s, int scale);
void draw_text(canvas_t *c, int x, int y, const char *s, int scale, int ink);
void draw_ctext(canvas_t *c, int cx, int y, const char *s, int scale, int ink);

#endif /* BEEPYFB_FONT_H */
