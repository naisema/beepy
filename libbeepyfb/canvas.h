/* libbeepyfb/canvas.h -- 1bpp backbuffer and drawing primitives. */
#ifndef BEEPYFB_CANVAS_H
#define BEEPYFB_CANVAS_H

/* The Beepy's Sharp memory LCD. */
#define SCR_W 400
#define SCR_H 240

#define INK 1
#define PAPER 0

typedef struct {
    int w, h, stride;
    unsigned char *bits; /* 1bpp, MSB leftmost */
} canvas_t;

canvas_t *canvas_new(int w, int h);
void canvas_clear(canvas_t *c, int ink);
void px(canvas_t *c, int x, int y, int ink);
void hline(canvas_t *c, int x, int y, int w, int ink);
void vline(canvas_t *c, int x, int y, int h, int ink);
void fillrect(canvas_t *c, int x, int y, int w, int h, int ink);
void rect(canvas_t *c, int x, int y, int w, int h, int ink);
void checker(canvas_t *c, int x, int y, int w, int h);
void circle(canvas_t *c, int cx, int cy, int r, int ink);
void disc(canvas_t *c, int cx, int cy, int r, int ink);
void dots_circle(canvas_t *c, int cx, int cy, int r, int step);

#endif /* BEEPYFB_CANVAS_H */
