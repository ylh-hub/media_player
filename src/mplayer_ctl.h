#ifndef MPLAYER_CTL_H
#define MPLAYER_CTL_H
/*
 * mplayer_ctl.h —— "控制层"对外接口
 * 设计思想：上层（界面/主程序）不直接碰 FIFO、不直接 fork，
 * 只调这里的函数。以后想把 mplayer 换成别的播放器，上层一行不用改。
 */

#include <common.h>

/* 启动 mplayer 播一个文件
 * @path    文件完整路径，如 /ylh/media/huluwa.mp3
 * @is_video 1=视频(fbdev2 全屏输出)  0=音频(-vo null)
 * 返回 0 成功；内部自动完成"先停旧的再启新的"
 */
int mp_start(const char *path, int is_video);

/* 停止当前 mplayer（优雅退出：先 quit，不退再 SIGKILL 兜底） */
void mp_stop(void);

/* 给 mplayer 发一条 slave 指令，如 "pause\n"、"volume 50 1\n" */
void mp_send(const char *cmd);

/* 播放 / 暂停 二合一开关（同一个键）：
 * PLAYING -> 发 pause -> PAUSED
 * PAUSED  -> 发 pause -> PLAYING
 * （mplayer 的 pause 指令本身就是"切换"语义） */
void mp_pause_toggle(void);

/* 切歌：dir=+1 下一首，-1 上一首（内部处理越界回绕） */
void mp_song_next(void);
void mp_song_prev(void);

/* 音量 0~100（增量式：当前值 ± step）。C 不支持重载，查询用另一个名字 */
void mp_volume(int delta);
void mp_volume_set(int v); /* 直接设到某个值（点音量条用） */
int  mp_volume_get(void);  /* 查询当前音量（画音量条用） */

/* 进度查询（本地计时方案，见 .c 文件注释） */
int  mp_cur_sec(void);     /* 当前已播秒数 */
int  mp_total_sec(void);   /* 总秒数（启动时 identify 获得） */

/* 按百分比跳转（0~100）：mplayer slave 的 seek <pct> 2 = 绝对百分比。
 * 进度条点击用这个，比相对秒数稳（多次点击不会漂移） */
void mp_seek_percent(int pct);

/* 播放器当前状态（ST_STOPPED / ST_PLAYING / ST_PAUSED） */
int  mp_state(void);

/* 程序总入口前调用一次：建 FIFO、装 SIGCHLD、启动进度定时线程。
 * 返回 0 成功；失败返回 -1（调用方自行决定退出） */
int  mp_subsys_init(void);

/* 扫描 MEDIA_DIR 自动生成歌单/视频列表（mp_subsys_init 里已自动调用；
 * 以后加"刷新列表"按钮可直接再调它）。扩展名不区分大小写 */
void mp_scan_media(void);

/* 单实例保护：杀掉残留的旧播放器实例和孤儿 mplayer（跳过自己） */
void mp_kill_stale(void);

/* 退出前调用：停 mplayer、关 FIFO */
void mp_subsys_deinit(void);

/* 供主循环查询：当前这首是不是自然播完了（SIGCHLD 置的标志），
 * 查完自动清零。返回 1 表示播完该切下一首 */
int  mp_song_finished(void);

#endif
