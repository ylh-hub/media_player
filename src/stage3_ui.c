/*
 * stage3_ui.c —— 阶段 3：触摸屏 + framebuffer 界面（纯音乐版）
 * ============================================================
 * 新学点（都写进简历）：
 *   - mmap framebuffer 绘图（fb_ui.c）
 *   - input 子系统读触摸（touch.c）
 *   - 双线程并发画屏：主线程画交互反馈，进度线程每 0.5s 刷进度条，
 *     共享显存 → 用互斥锁保护（fb_lock/fb_unlock）
 * 界面布局（1024x600，无中文字库所以用图标+高亮，不显示歌名文字）：
 *   ┌──────────────────────────────────┐
 *   │ [退出]              顶部栏        │ 0~70
 *   │ 歌曲列表（当前行高亮+▶标记）      │ 90~466
 *   │ ██████████░░░░░░ 进度条           │ 468
 *   │  [◀上一首][⏯][下一首▶][vol-][vol+] │ 500~590
 *   └──────────────────────────────────┘
 * 运行: ./stage3_ui /dev/input/eventX   （X 用 touch_test 探测的结果）
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

/* ---- 布局常量（改界面只动这里） ---- */
#define LIST_X   32
#define LIST_Y   90
#define ROW_W    960
#define ROW_H    64
#define MAX_ROW  6                          /* 一屏最多显示 6 首 */

#define PB_X     32                         /* 进度条 */
#define PB_Y     468
#define PB_W     960
#define PB_H     14

#define BTN_Y    500                        /* 按钮行：5 个按钮，宽110 间隔30 */
#define BTN_H    90
#define BX_PREV  90                         /* 各按钮左边界 */
#define BX_PLAY  230
#define BX_NEXT  370
#define BX_VDEC  510
#define BX_VINC  650

#define EXIT_W   70                         /* 左上角退出热区 */
#define EXIT_H   56

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- 绘制：列表 + 进度条（内部自加锁，主/子线程都能调） ---- */
static void draw_list(void)
{
    int n = g_song_num < MAX_ROW ? g_song_num : MAX_ROW;
    for (int i = 0; i < n; i++) {
        fb_lock();
        fb_fill_rect(LIST_X, LIST_Y + i * ROW_H, ROW_W, ROW_H - 8,
                     i == g_song_cur ? C_HL : C_CARD);
        const char *p = strrchr(g_songs[i], '/');       /* 只取文件名部分 */
        fb_draw_string(LIST_X + 16, LIST_Y + i * ROW_H + 20,
                       p ? p + 1 : g_songs[i], C_TXT);
        if (i == g_song_cur)                /* 当前行右侧画 ▶ 标记 */
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
    if (total > 0) {                        /* 防除零：identify 失败就不画比例 */
        int w = (long)PB_W * mp_cur_sec() / total;
        if (w > PB_W) w = PB_W;
        if (w > 0)
            fb_fill_rect(PB_X, PB_Y, w, PB_H, C_ACCENT);
    }
    fb_unlock();
}

/* 音量条：顶部栏右侧，音量 ± 一按就能看到变化（2026-11-02 用户要求） */
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

/* 播放键图标随状态变：播放中=▶ 三角，暂停中=⏸ 双竖杠（2026-11-02 用户要求） */
static void draw_playbtn(void)
{
    static int last = -1;                   /* 状态没变就不重画 */
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

static void draw_static(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);          /* 顶部栏 */
    fb_fill_rect(0, EXIT_H / 2 - 14, 40, 4, C_TXT);    /* 退出热区: 两横杠 */
    fb_fill_rect(0, EXIT_H / 2 + 10, 40, 4, C_TXT);
    fb_fill_rect(PB_X, PB_Y, PB_W, PB_H, C_CARD);      /* 进度条底 */
    /* 五个按钮底板 */
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
    draw_list();
    draw_progress();
    draw_playbtn();
}

/* ---- 后台线程：每 0.5s 刷进度 + 自动连播 ---- */
static void *ui_ticker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (g_vol_ticks > 0 && --g_vol_ticks == 0)
            erase_volume();                 /* 2 秒到，音量条消失 */
        if (mp_song_finished())             /* 自然播完 → 下一首 */
            mp_song_next();
        if (mp_state() == ST_PLAYING || mp_state() == ST_PAUSED)
            draw_progress();
        draw_playbtn();                     /* 状态变了就换图标（内部有去重） */
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("stage3_ui build %s %s\n", __DATE__, __TIME__);   /* 确认跑的是新二进制 */

    if (mp_subsys_init() != 0) return 1;
    if (fb_init() != 0)        return 1;
    if (touch_init(argc > 1 ? argv[1] : NULL) != 0) return 1;

    draw_static();
    mp_start(g_songs[g_song_cur], 0);       /* 开机就播，效果直观 */

    pthread_t tid;
    pthread_create(&tid, NULL, ui_ticker, NULL);
    pthread_detach(tid);

    int x, y;
    while (touch_read(&x, &y) == 0) {
        printf("touch (%d,%d)\n", x, y);

        if (hit(x, y, 0, 0, EXIT_W, EXIT_H))            /* 退出 */
            break;
        if (hit(x, y, BX_PREV, BTN_Y, 110, BTN_H))      /* 上一首 */
            mp_song_prev();
        else if (hit(x, y, BX_PLAY, BTN_Y, 110, BTN_H)) /* 播放/暂停同键 */
            mp_pause_toggle();
        else if (hit(x, y, BX_NEXT, BTN_Y, 110, BTN_H)) /* 下一首 */
            mp_song_next();
        else if (hit(x, y, BX_VDEC, BTN_Y, 110, BTN_H)) { /* 音量- */
            mp_volume(-10);
            g_vol_ticks = 4;                /* 音量条显示 2 秒 */
            draw_volume();
        } else if (hit(x, y, BX_VINC, BTN_Y, 110, BTN_H)) { /* 音量+ */
            mp_volume(+10);
            g_vol_ticks = 4;
            draw_volume();
        }
        else if (y >= LIST_Y && y < LIST_Y + MAX_ROW * ROW_H &&
                 x >= LIST_X && x < LIST_X + ROW_W) {   /* 点列表切歌 */
            int row = (y - LIST_Y) / ROW_H;
            if (row < g_song_num && row != g_song_cur) {
                g_song_cur = row;
                mp_start(g_songs[row], 0);
            }
        }
        draw_playbtn();                     /* 播放键图标跟状态走 */
        draw_list();                        /* 歌曲下标可能变了，重画列表 */
    }

    mp_subsys_deinit();
    fb_deinit();
    touch_deinit();
    printf("exit\n");
    return 0;
}
