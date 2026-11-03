#ifndef IMAGE_H
#define IMAGE_H
/*
 * image.h —— 图片解码与上屏（相册/图标用）
 * 解码用 stb_image（public domain 单头文件，支持 jpg/png/bmp）；
 * 板子没有任何图像库，这是"零依赖"路线：一个 .h 进仓库就能解码。
 */

typedef struct {
    unsigned char *rgb;   /* RGB888，顶行在前 */
    int w, h;
} Img;

/* 解码文件；失败返回 NULL（不支持的格式/坏文件都走这里） */
Img *img_load(const char *path);
void img_free(Img *im);

/* 盒滤波缩略图（一次性生成后缓存，避免每次重画都解码） */
Img *img_thumb(const Img *src, int tw, int th);

/* 等比缩放画进矩形（不足处留黑边）；调用方持 fb_lock */
void img_draw_fit_fb(const Img *im, int rx, int ry, int rw, int rh);

/* 裁中心铺满矩形（图标/缩略图用）；调用方持 fb_lock */
void img_draw_fill_fb(const Img *im, int rx, int ry, int rw, int rh);

#endif
