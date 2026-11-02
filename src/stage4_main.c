/*
 * stage4_main.c —— 阶段 4：最终版（音乐 + 视频整合）
 * ============================================================
 * 在 stage3 基础上加：
 *   1) 右上角"视频"按钮 → 视频列表界面
 *   2) 点视频 → fb_save_state() → mplayer fbdev2 全屏接管屏幕
 *   3) 视频中：点屏幕左边缘(0~200) 退出回界面；点其余位置 暂停/继续
 *   4) 视频结束（自然播完或退出）→ fb_restore_state() 还原显示参数
 *      （三方评审定的关键动作：不还原会花屏）
 * 运行: ./media_player /dev/input/eventX
 */
#include <mplayer_ctl.h>
#include <fb_ui.h>
#include <touch.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- 界面状态机 ---- */
enum screen { SCR_MUSIC = 0, SCR_VIDEO, SCR_MENU };
static volatile int g_scr = SCR_MUSIC;
static volatile int g_video_done = 0;       /* 视频自然播完标志（线程置位） */

/* ---- 布局常量（音乐页与 stage3 相同，此处略注释） ---- */
#define LIST_X   32
#define LIST_Y   90
#define ROW_W    960
#define ROW_H    64
#define MAX_ROW  6
#define PB_X     32
#define PB_Y     468
#define PB_W     960
#define PB_H     14
#define BTN_Y    500
#define BTN_H    90
#define BX_PREV  90
#define BX_PLAY  230
#define BX_NEXT  370
#define BX_VDEC  510
#define BX_VINC  650
#define EXIT_W   70
#define EXIT_H   56
#define VID_BTN_X  900                      /* 音乐页右上角"视频"按钮 */
#define VID_BTN_Y  8
#define VID_BTN_W  110
#define VID_BTN_H  56

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void draw_list_mucis(void)
{
    int n = g_song_num < MAX_ROW ? g_song_num : MAX_ROW;
    for (int i = 0; i < n; i++) {
        fb_lock();
        fb_fill_rect(LIST_X, LIST_Y + i * ROW_H, ROW_W, ROW_H - 8,
                     i == g_song_cur ? C_HL : C_CARD);
        const char *p = strrchr(g_songs[i], '/');       /* 只取文件名部分 */
        fb_draw_string(LIST_X + 16, LIST_Y + i * ROW_H + 20,
                       p ? p + 1 : g_songs[i], C_TXT);
        if (i == g_song_cur)
            fb_icon_play(LIST_X + ROW_W - 50,
                         LIST_Y + i * ROW_H + (ROW_H - 8) / 2, 14, C_TXT);
        fb_unlock();
    }
}

static void draw_progress(void)
{
    fb_lock();
    fb_fill_rect(PB_X, PB_Y, PB_W, PB_H, C_CARD);
    int total = mp_total_sec();
    if (total > 0) {
        int w = (long)PB_W * mp_cur_sec() / total;
        if (w > PB_W) w = PB_W;
        if (w > 0)
            fb_fill_rect(PB_X, PB_Y, w, PB_H, C_ACCENT);
    }
    fb_unlock();
}

/* 播放键图标随状态变：播放中=▶，暂停中=⏸（与 stage3 相同）
 * 注意：必须定义在 draw_music 之前——C 语言先声明后使用 */
static void draw_playbtn(void)
{
    static int last = -1;
    int st = mp_state();
    if (st == last)
        return;
    fb_lock();
    fb_fill_rect(BX_PLAY, BTN_Y, 110, BTN_H, C_ACCENT);
    int cy = BTN_Y + BTN_H / 2;
    if (st == ST_PAUSED)
        fb_icon_pause(BX_PLAY + 55, cy, 18, C_TXT);
    else
        fb_icon_play(BX_PLAY + 55, cy, 18, C_TXT);
    fb_unlock();
    last = st;
}

/* 音量条：顶部栏右侧（与 stage3 相同） */
static void draw_volume(void)
{
    fb_lock();
    fb_fill_rect(756, 26, 128, 18, C_BG);
    fb_draw_rect(756, 26, 128, 18, C_TXT);
    int w = (long)(128 - 6) * mp_volume_get() / 100;
    if (w > 0)
        fb_fill_rect(759, 29, w, 12, C_ACCENT);
    fb_unlock();
}

static void erase_volume(void)      /* 到时用顶部栏同色盖掉 */
{
    fb_lock();
    fb_fill_rect(756, 26, 128, 18, C_CARD);
    fb_unlock();
}

static int g_vol_ticks = 0;         /* 音量条剩余显示 tick（1 tick=0.5s，到 0 隐藏） */

static void draw_music(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);
    fb_fill_rect(0, EXIT_H / 2 - 14, 40, 4, C_TXT);
    fb_fill_rect(0, EXIT_H / 2 + 10, 40, 4, C_TXT);
    fb_fill_rect(VID_BTN_X, VID_BTN_Y, VID_BTN_W, VID_BTN_H, C_CARD);
    fb_icon_film(VID_BTN_X + VID_BTN_W / 2, VID_BTN_Y + VID_BTN_H / 2,
                 14, C_TXT);
    fb_fill_rect(PB_X, PB_Y, PB_W, PB_H, C_CARD);
    fb_fill_rect(BX_PREV, BTN_Y, 110, BTN_H, C_CARD);
    fb_fill_rect(BX_PLAY, BTN_Y, 110, BTN_H, C_ACCENT);
    fb_fill_rect(BX_NEXT, BTN_Y, 110, BTN_H, C_CARD);
    fb_fill_rect(BX_VDEC, BTN_Y, 110, BTN_H, C_CARD);
    fb_fill_rect(BX_VINC, BTN_Y, 110, BTN_H, C_CARD);
    int cy = BTN_Y + BTN_H / 2;
    fb_icon_prev(BX_PREV + 55, cy, 18, C_TXT);
    fb_icon_next(BX_NEXT + 55, cy, 18, C_TXT);
    fb_icon_minus(BX_VDEC + 55, cy, 18, C_TXT);
    fb_icon_plus(BX_VINC + 55, cy, 18, C_TXT);
    fb_unlock();
    draw_list_mucis();
    draw_progress();
    draw_playbtn();
}

static void draw_video(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);
    fb_icon_back(50, 35, 14, C_TXT);        /* 左上角返回 */
    fb_icon_film(SCREEN_W / 2, 35, 14, C_TXT);
    fb_unlock();
    int n = g_video_num < MAX_ROW ? g_video_num : MAX_ROW;
    for (int i = 0; i < n; i++) {
        fb_lock();
        fb_fill_rect(LIST_X, LIST_Y + i * ROW_H, ROW_W, ROW_H - 8, C_CARD);
        fb_icon_play(LIST_X + 60, LIST_Y + i * ROW_H + (ROW_H - 8) / 2,
                     12, C_TXT);
        const char *p = strrchr(g_videos[i], '/');      /* 视频文件名 */
        fb_draw_string(LIST_X + 100, LIST_Y + i * ROW_H + 20,
                       p ? p + 1 : g_videos[i], C_TXT);
        fb_unlock();
    }
}

/* 菜单页：左上角进来。VIDEO=去视频列表，EXIT=退出程序 */
static void draw_menu(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);
    fb_icon_back(50, 35, 14, C_TXT);            /* 左上角返回音乐页 */
    fb_draw_string(464, 28, "MENU", C_TXT);
    fb_fill_rect(LIST_X, 180, ROW_W, 120, C_CARD);
    fb_icon_film(LIST_X + 80, 240, 22, C_TXT);
    fb_draw_string(LIST_X + 160, 232, "VIDEO", C_TXT);
    fb_fill_rect(LIST_X, 330, ROW_W, 120, C_CARD);
    fb_icon_minus(LIST_X + 80, 390, 18, C_TXT);
    fb_draw_string(LIST_X + 160, 382, "EXIT", C_TXT);
    fb_unlock();
}

/* ---- 视频全屏播放（阻塞直到播完/用户退出） ---- */
static void play_video(int idx)
{
    fb_save_state();                        /* 关键：先保存 fb 参数 */
    g_video_done = 0;
    if (mp_start(g_videos[idx], 1) != 0) {  /* 1 = fbdev2 全屏 */
        fb_restore_state();
        return;
    }
    for (;;) {
        fd_set rfds;
        struct timeval tv = {0, 300 * 1000};
        FD_ZERO(&rfds);
        FD_SET(touch_fd(), &rfds);
        int r = select(touch_fd() + 1, &rfds, NULL, NULL, &tv);

        if (g_video_done)                   /* 自然播完 */
            break;
        if (r > 0) {
            int x, y;
            if (touch_read(&x, &y) == 0) {
                if (x < 200) {              /* 左边缘 = 退出视频 */
                    mp_stop();
                    break;
                }
                mp_pause_toggle();          /* 其余区域 = 暂停/继续 */
            }
        }
    }
    mp_stop();                              /* 没退干净就兜底（已退则直接返回） */
    fb_restore_state();                     /* 关键：还原 fb 参数，防花屏 */
    draw_video();                           /* 回到视频列表页 */
}

/* ---- 后台线程：进度刷新 + 自动连播 ----
 * 注意：视频全屏期间绝不能往 fb 画任何东西（会盖到视频上），
 * 所以画屏动作全部用 g_scr == SCR_MUSIC 把门 */
static void *ui_ticker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (g_vol_ticks > 0 && --g_vol_ticks == 0)
            erase_volume();                 /* 2 秒到，音量条消失 */
        if (mp_song_finished()) {
            if (g_scr == SCR_MUSIC)
                mp_song_next();             /* 音乐自动连播 */
            else
                g_video_done = 1;           /* 通知 play_video 收尾 */
        }
        if (g_scr == SCR_MUSIC) {
            if (mp_state() == ST_PLAYING || mp_state() == ST_PAUSED)
                draw_progress();
            draw_playbtn();                 /* 图标跟状态走 */
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("media_player build %s %s\n", __DATE__, __TIME__); /* 确认跑的是新二进制 */

    if (mp_subsys_init() != 0) return 1;
    if (fb_init() != 0)        return 1;
    if (touch_init(argc > 1 ? argv[1] : NULL) != 0) return 1;

    draw_music();
    mp_start(g_songs[g_song_cur], 0);

    pthread_t tid;
    pthread_create(&tid, NULL, ui_ticker, NULL);
    pthread_detach(tid);

    int x, y;
    while (touch_read(&x, &y) == 0) {
        if (g_scr == SCR_MUSIC) {
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {  /* 左上角 → 菜单页 */
                g_scr = SCR_MENU;
                draw_menu();
                continue;
            }
            if (hit(x, y, VID_BTN_X, VID_BTN_Y, VID_BTN_W, VID_BTN_H)) {
                g_scr = SCR_VIDEO;
                draw_video();
                continue;
            }
            if (hit(x, y, BX_PREV, BTN_Y, 110, BTN_H))
                mp_song_prev();
            else if (hit(x, y, BX_PLAY, BTN_Y, 110, BTN_H))
                mp_pause_toggle();
            else if (hit(x, y, BX_NEXT, BTN_Y, 110, BTN_H))
                mp_song_next();
            else if (hit(x, y, BX_VDEC, BTN_Y, 110, BTN_H)) {
                mp_volume(-10);
                g_vol_ticks = 4;            /* 音量条显示 2 秒 */
                draw_volume();
            } else if (hit(x, y, BX_VINC, BTN_Y, 110, BTN_H)) {
                mp_volume(+10);
                g_vol_ticks = 4;
                draw_volume();
            }
            else if (y >= LIST_Y && y < LIST_Y + MAX_ROW * ROW_H &&
                     x >= LIST_X && x < LIST_X + ROW_W) {
                int row = (y - LIST_Y) / ROW_H;
                if (row < g_song_num && row != g_song_cur) {
                    g_song_cur = row;
                    mp_start(g_songs[row], 0);
                }
            }
            draw_playbtn();                 /* 图标跟状态走 */
            draw_list_mucis();
        } else if (g_scr == SCR_VIDEO) {    /* 视频列表页 */
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {   /* 返回音乐页 */
                g_scr = SCR_MUSIC;
                draw_music();
                continue;
            }
            if (y >= LIST_Y && y < LIST_Y + MAX_ROW * ROW_H &&
                x >= LIST_X && x < LIST_X + ROW_W) {
                int row = (y - LIST_Y) / ROW_H;
                if (row < g_video_num) {
                    g_video_cur = row;
                    play_video(row);        /* 阻塞到视频结束 */
                }
            }
        } else {                            /* SCR_MENU 菜单页 */
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {   /* 返回音乐页 */
                g_scr = SCR_MUSIC;
                draw_music();
                continue;
            }
            if (hit(x, y, LIST_X, 180, ROW_W, 120)) {/* VIDEO → 视频列表 */
                g_scr = SCR_VIDEO;
                draw_video();
                continue;
            }
            if (hit(x, y, LIST_X, 330, ROW_W, 120))
                break;                      /* EXIT → 退出程序 */
        }
    }

    mp_subsys_deinit();
    fb_deinit();
    touch_deinit();
    printf("exit\n");
    return 0;
}
