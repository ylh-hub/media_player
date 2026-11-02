#ifndef COMMON_H
#define COMMON_H
/*
 * common.h —— 全项目公共常量
 * 为什么单独放一个头文件：路径、屏幕参数这些"配置"，改一处全项目生效，
 * 面试可以讲这是"配置与逻辑分离"的最小实践。
 */

/* ===== 一、媒体目录（按你板子实际情况改这里） =====
 * 假设你把 mp3/mp4 都拷到板子的 /ylh/media/ 目录
 * 建目录命令（串口里执行）：mkdir -p /ylh/media
 */
#define MEDIA_DIR   "/ylh/media"

/* ===== 二、播放列表 =====
 * 阶段 1~4 先用固定数组（和课程例程一致）；
 * 加分项：用 readdir() 扫描 MEDIA_DIR 自动生成列表（指导书 第 7 节）
 */
#define SONG_MAX    16
extern char *g_songs[SONG_MAX];   /* 歌曲文件完整路径 */
extern int   g_song_num;          /* 实际歌曲数 */
extern int   g_song_cur;          /* 当前播放下标 */

/* 视频列表，同理 */
#define VIDEO_MAX   8
extern char *g_videos[VIDEO_MAX];
extern int   g_video_num;
extern int   g_video_cur;

/* ===== 三、屏幕与 FIFO ===== */
#define SCREEN_W    1024
#define SCREEN_H    600
#define FIFO_PATH   "/tmp/media.fifo"   /* 板子上一定可写的目录，比 /home/xx 通用 */

/* 播放器二进制名：板子上是 mplayer64；
 * 在 VM 里调试逻辑层时用 gcc -DMPLAYER_BIN='"mplayer"' 覆盖 */
#ifndef MPLAYER_BIN
#define MPLAYER_BIN "mplayer64"
#endif

/* ===== 四、播放器状态机 ===== */
/* 为什么用状态机：界面按键"播放/暂停同一键"本质是状态切换，
 * 把状态显式定义出来，代码可读、面试好讲 */
enum player_state {
    ST_STOPPED = 0,   /* 没在播 */
    ST_PLAYING,       /* 播放中 */
    ST_PAUSED,        /* 已发 pause，mplayer 暂停中 */
};

#endif
