#ifndef ALBUM_H
#define ALBUM_H
/*
 * album.h —— 相册：扫描 /ylh/photos 里的图片，提供缩略图缓存与大图解码
 */
#include "image.h"

#define ALBUM_DIR  "/ylh/photos"
#define ALBUM_MAX  32
#define THUMB_W    228
#define THUMB_H    220

/* 扫描目录（启动时调一次）；返回图片数。支持 jpg/jpeg/png/bmp，不分大小写 */
int  album_scan(void);
int  album_count(void);
const char *album_path(int i);
const char *album_name(int i);

/* 缩略图（首次调用解码+缩放并缓存，之后零成本）；失败返回 NULL */
Img *album_thumb(int i);

/* 大图（每次新解码，调用方 img_free）；失败返回 NULL */
Img *album_full(int i);

#endif
