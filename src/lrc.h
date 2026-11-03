#ifndef LRC_H
#define LRC_H
/*
 * lrc.h —— 歌词解析（.lrc 文件，[mm:ss.xx]一行词）
 * 歌词文件放在媒体同目录、同名不同扩展名：huluwa.mp3 -> huluwa.lrc
 * 没有歌词文件 = 无歌词模式，界面显示提示。
 */

/* 载入与歌曲同名的 .lrc；返回解析出的行数（0=没有歌词） */
int  lrc_load(const char *song_path);

int  lrc_count(void);                /* 歌词总行数 */
const char *lrc_text(int i);         /* 第 i 行文本（UTF-8，可直接给 fb_text） */

/* 当前播放秒数对应的高亮行：无词/还没唱到第一行返回 -1 */
int  lrc_index(int cur_sec);

#endif
