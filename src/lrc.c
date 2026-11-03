/*
 * lrc.c —— .lrc 歌词解析
 * ============================================================
 * .lrc 大白话：一个文本文件，每行 = "[分钟:秒]歌词内容"，
 * 例如 [00:08.00]一根藤上七朵花 —— 表示第 8 秒唱这句。
 * 播放时拿"当前秒数"和每行时间比一比，就知道该高亮哪句。
 * 面试点：文本协议解析 + 按时间轴线性查找（行数少，顺序找足够快）。
 * ============================================================
 */
#include "lrc.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int  sec;           /* 该行起始秒 */
    char text[128];     /* 歌词内容（UTF-8） */
} LrcLine;

static LrcLine g_lines[64];
static int     g_num = 0;

int lrc_load(const char *song_path)
{
    g_num = 0;
    if (song_path == NULL)
        return 0;

    /* huluwa.mp3 -> huluwa.lrc（把最后一个 '.' 后面换掉） */
    const char *dot = strrchr(song_path, '.');
    if (dot == NULL)
        return 0;
    int base = (int)(dot - song_path);
    if (base > 180)
        base = 180;

    char path[200];
    snprintf(path, sizeof(path), "%.*s.lrc", base, song_path);

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return 0;                       /* 没有歌词文件，静默降级 */

    char line[256];
    while (g_num < 64 && fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';         /* 去掉行尾换行 */
        char *p = strchr(line, '[');
        if (p == NULL)
            continue;
        int mm = 0, ss = 0;
        if (sscanf(p, "[%d:%d]", &mm, &ss) != 2)
            continue;
        char *rb = strchr(p, ']');
        if (rb == NULL)
            continue;
        char *text = rb + 1;
        while (*text == ' ')
            text++;
        if (*text == '\0')
            continue;
        g_lines[g_num].sec = mm * 60 + ss;
        snprintf(g_lines[g_num].text, sizeof(g_lines[0].text), "%s", text);
        g_num++;
    }
    fclose(fp);
    return g_num;
}

int lrc_count(void)
{
    return g_num;
}

const char *lrc_text(int i)
{
    if (i < 0 || i >= g_num)
        return "";
    return g_lines[i].text;
}

int lrc_index(int cur_sec)
{
    int idx = -1;
    for (int i = 0; i < g_num; i++) {
        if (g_lines[i].sec <= cur_sec)
            idx = i;
        else
            break;                      /* 行按时间升序，后面不可能匹配 */
    }
    return idx;
}
