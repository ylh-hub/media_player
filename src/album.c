/*
 * album.c —— 相册实现
 * ============================================================
 * 和歌单一个套路：启动时 readdir 扫目录，名字排序保证顺序稳定；
 * 缩略图"懒加载 + 缓存"：进相册页才解码第一屏，之后翻页不重复解码。
 */
#include "album.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

static char g_paths[ALBUM_MAX][300];
static char g_names[ALBUM_MAX][260];
static int  g_num = 0;
static Img *g_thumbs[ALBUM_MAX];

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lu = strlen(suffix);
    if (ls < lu)
        return 0;
    s += ls - lu;
    while (*suffix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*suffix))
            return 0;
        s++;
        suffix++;
    }
    return 1;
}

static int is_image_name(const char *n)
{
    return ends_with_ci(n, ".jpg") || ends_with_ci(n, ".jpeg") ||
           ends_with_ci(n, ".png") || ends_with_ci(n, ".bmp");
}

static int cmp_idx(const void *a, const void *b)
{
    return strcmp(g_names[*(const int *)a], g_names[*(const int *)b]);
}

int album_scan(void)
{
    for (int i = 0; i < g_num; i++) {
        img_free(g_thumbs[i]);
        g_thumbs[i] = NULL;
    }
    g_num = 0;

    DIR *d = opendir(ALBUM_DIR);
    if (d == NULL) {
        mkdir(ALBUM_DIR, 0755);          /* 没有就建，下次拷图进去就能用 */
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_num < ALBUM_MAX) {
        if (e->d_name[0] == '.' || !is_image_name(e->d_name))
            continue;
        snprintf(g_paths[g_num], sizeof(g_paths[0]),
                 ALBUM_DIR "/%s", e->d_name);
        snprintf(g_names[g_num], sizeof(g_names[0]), "%s", e->d_name);
        g_num++;
    }
    closedir(d);

    int order[ALBUM_MAX];
    for (int i = 0; i < g_num; i++)
        order[i] = i;
    qsort(order, g_num, sizeof(int), cmp_idx);
    /* 按 order 重排（小数组，直接搬） */
    char tp[ALBUM_MAX][300], tn[ALBUM_MAX][260];
    for (int i = 0; i < g_num; i++) {
        memcpy(tp[i], g_paths[order[i]], sizeof(tp[0]));
        memcpy(tn[i], g_names[order[i]], sizeof(tn[0]));
    }
    memcpy(g_paths, tp, sizeof(g_paths));
    memcpy(g_names, tn, sizeof(g_names));

    printf("scan " ALBUM_DIR ": %d photos\n", g_num);
    return g_num;
}

int album_count(void)          { return g_num; }
const char *album_path(int i)  { return (i >= 0 && i < g_num) ? g_paths[i] : NULL; }
const char *album_name(int i)  { return (i >= 0 && i < g_num) ? g_names[i] : NULL; }

Img *album_thumb(int i)
{
    if (i < 0 || i >= g_num)
        return NULL;
    if (g_thumbs[i] == NULL) {
        Img *full = img_load(g_paths[i]);
        if (full != NULL) {
            g_thumbs[i] = img_thumb(full, THUMB_W, THUMB_H);
            img_free(full);
        }
    }
    return g_thumbs[i];
}

Img *album_full(int i)
{
    if (i < 0 || i >= g_num)
        return NULL;
    return img_load(g_paths[i]);
}
