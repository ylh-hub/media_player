/*
 * image.c —— 图片解码与上屏实现
 * ============================================================
 * 为什么自己写缩放：fb 是"一格一像素"的显存，图片尺寸和屏幕不一致时
 * 必须采样。缩略图用盒滤波（一个目标像素 = 源图一块的平均，不闪纹），
 * 全屏大图用最近邻（快，静态图看不出锯齿）。
 */
#include "image.h"
#include "fb_ui.h"

#include <stdlib.h>
#include <math.h>

#include "stb_image.h"

Img *img_load(const char *path)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 3);
    if (data == NULL || w <= 0 || h <= 0) {
        if (data)
            stbi_image_free(data);
        return NULL;
    }
    Img *im = malloc(sizeof(*im));
    if (im == NULL) {
        stbi_image_free(data);
        return NULL;
    }
    im->rgb = data;
    im->w = w;
    im->h = h;
    return im;
}

void img_free(Img *im)
{
    if (im == NULL)
        return;
    if (im->rgb)
        stbi_image_free(im->rgb);
    free(im);
}

Img *img_thumb(const Img *src, int tw, int th)
{
    if (src == NULL || src->rgb == NULL || tw <= 0 || th <= 0)
        return NULL;
    Img *d = malloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    d->w = tw;
    d->h = th;
    d->rgb = malloc((size_t)tw * th * 3);
    if (d->rgb == NULL) {
        free(d);
        return NULL;
    }
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((long)y * src->h / th);
        int sy1 = (int)((long)(y + 1) * src->h / th);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > src->h)
            sy1 = src->h;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((long)x * src->w / tw);
            int sx1 = (int)((long)(x + 1) * src->w / tw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (sx1 > src->w)
                sx1 = src->w;
            unsigned long sr = 0, sg = 0, sb = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *row = src->rgb + (size_t)sy * src->w * 3;
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p = row + (size_t)sx * 3;
                    sr += p[0];
                    sg += p[1];
                    sb += p[2];
                    n++;
                }
            }
            unsigned char *o = d->rgb + ((size_t)y * tw + x) * 3;
            o[0] = n ? (unsigned char)(sr / n) : 0;
            o[1] = n ? (unsigned char)(sg / n) : 0;
            o[2] = n ? (unsigned char)(sb / n) : 0;
        }
    }
    return d;
}

void img_draw_fit_fb(const Img *im, int rx, int ry, int rw, int rh)
{
    if (im == NULL || im->rgb == NULL)
        return;
    fb_fill_rect(rx, ry, rw, rh, 0x000000);      /* 黑底 = 信箱边 */
    float s = fminf((float)rw / im->w, (float)rh / im->h);
    if (s <= 0.0f)
        return;
    int dw = (int)(im->w * s);
    int dh = (int)(im->h * s);
    if (dw > rw) dw = rw;
    if (dh > rh) dh = rh;
    int ox = rx + (rw - dw) / 2;
    int oy = ry + (rh - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((y + 0.5f) / s);
        if (sy >= im->h)
            sy = im->h - 1;
        const unsigned char *row = im->rgb + (size_t)sy * im->w * 3;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((x + 0.5f) / s);
            if (sx >= im->w)
                sx = im->w - 1;
            const unsigned char *p = row + (size_t)sx * 3;
            fb_draw_point(ox + x, oy + y,
                          (p[0] << 16) | (p[1] << 8) | p[2]);
        }
    }
}

void img_draw_fill_fb(const Img *im, int rx, int ry, int rw, int rh)
{
    if (im == NULL || im->rgb == NULL)
        return;
    float s = fmaxf((float)rw / im->w, (float)rh / im->h);
    if (s <= 0.0f)
        return;
    int sw = (int)(rw / s);
    int sh = (int)(rh / s);
    if (sw > im->w) sw = im->w;
    if (sh > im->h) sh = im->h;
    int sx0 = (im->w - sw) / 2;                  /* 裁中心 */
    int sy0 = (im->h - sh) / 2;
    for (int y = 0; y < rh; y++) {
        int sy = sy0 + (int)((y + 0.5f) / s);
        if (sy >= im->h)
            sy = im->h - 1;
        if (sy < 0)
            sy = 0;
        const unsigned char *row = im->rgb + (size_t)sy * im->w * 3;
        for (int x = 0; x < rw; x++) {
            int sx = sx0 + (int)((x + 0.5f) / s);
            if (sx >= im->w)
                sx = im->w - 1;
            if (sx < 0)
                sx = 0;
            const unsigned char *p = row + (size_t)sx * 3;
            fb_draw_point(rx + x, ry + y,
                          (p[0] << 16) | (p[1] << 8) | p[2]);
        }
    }
}
