/*
 * stage4_main.c —— 阶段 4：最终版（音乐 + 视频整合）
 * ============================================================
 * 界面：网易云风格音乐页（深蓝渐变 + 黑胶 + 歌词 + 底部控制条 + 歌单侧栏）
 * 视频：mplayer64 -vo fbdev2 全屏接管，退出后 save/restore fb 参数
 *
 * 关键设计：
 *   1) 音乐页所有绘制都在 fb_lock() 内，ticker 只做"局部刷新"
 *   2) 歌词用 lrc.c 解析同名 .lrc，当前行高亮，按播放秒数滚动
 *   3) 视频期间 UI 线程停笔（fbdev2 独占 /dev/fb0）
 *   4) 视频退出回音乐页时重新拉起当前歌（否则界面假播）
 *
 * 运行: ./media_player /dev/input/eventX
 */
#include <mplayer_ctl.h>
#include <fb_ui.h>
#include <touch.h>
#include <lrc.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------------- 配色（深蓝网易云风） ---------------- */
#define CLR_TOP     0x2B3245
#define CLR_BOT     0x1C2130
#define CLR_TXT     0xE8EAF0
#define CLR_DIM     0x7A8194
#define CLR_RED     0xD33A31
#define CLR_DISC    0x101216
#define CLR_GROOVE  0x24262B
#define CLR_LABEL   0x3A4A6B
#define CLR_TRACK   0x3A4152
#define CLR_PANEL   0xF4F4F4
#define CLR_PTXT    0x333333
#define CLR_PDIM    0x999999

/* ---------------- 界面状态 ---------------- */
enum screen { SCR_MUSIC = 0, SCR_LIST, SCR_VIDEO, SCR_MENU };
static volatile int g_scr = SCR_MUSIC;
static volatile int g_video_done = 0;
static volatile int g_video_playing = 0;

/* ---------------- 音乐页布局 ---------------- */
#define VCX       250
#define VCY       280
#define VR        165
#define LYRIC_X   500
#define LYRIC_Y   200
#define LYRIC_W   492
#define LYRIC_H   250
#define LYRIC_STEP 34
#define PB_X      32
#define PB_Y      470
#define PB_W      960
#define PB_H      4
#define BTN_Y     545
#define BTN_PREV  150
#define BTN_PLAY  250
#define BTN_NEXT  350
#define BTN_LIST  450
#define TOP_H     56

/* 视频/菜单页沿用旧布局 */
#define LIST_X   32
#define LIST_Y   90
#define ROW_W    960
#define ROW_H    64
#define MAX_ROW  6
#define EXIT_W   70
#define EXIT_H   56

static int g_vol_ticks = 0;      /* 音量条剩余 tick（1 tick=0.5s） */
static int g_lyric_idx = -2;     /* 上次高亮的歌词行，-2=强制重画 */
static float g_angle = 0.0f;     /* 黑胶高光点角度（弧度） */

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void mmss(int sec, char *buf, size_t n)
{
    if (sec < 0) sec = 0;
    snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
}

static int u8len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c >> 5) == 6) return 2;
    if ((c >> 4) == 14) return 3;
    return 4;
}

/* 超宽截断：按像素宽度画，避免歌词/标题冲出面板 */
static void text_clip(int x, int y, const char *s, int color, int scale, int maxw)
{
    char one[5];
    int cx = x;
    while (s && *s) {
        int n = u8len((unsigned char)*s);
        if (n > 4) n = 1;
        memcpy(one, s, n);
        one[n] = 0;
        int w = fb_text_width(one, scale);
        if (cx + w > x + maxw)
            break;
        fb_text(cx, y, one, color, scale);
        cx += w;
        s += n;
    }
}

/* Bresenham 粗线：画唱针用（fb 层只有点/矩形，没有线） */
static void draw_line(int x0, int y0, int x1, int y1, int th, int color)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fb_fill_rect(x0 - th / 2, y0 - th / 2, th, th, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ================= 音乐页绘制（调用方持锁） ================= */

static void draw_topbar_locked(void)
{
    /* 左上角：三条横杠（菜单） */
    for (int i = 0; i < 3; i++)
        fb_fill_rect(24, 20 + i * 9, 26, 3, CLR_TXT);
    /* 右上角：胶片图标 + VIDEO */
    fb_icon_film(905, 28, 12, CLR_TXT);
    fb_text(924, 20, "VIDEO", CLR_DIM, 1);
}

static void draw_vinyl_locked(void)
{
    fb_fill_circle(VCX, VCY, VR, CLR_DISC);
    fb_draw_circle(VCX, VCY, 150, CLR_GROOVE);
    fb_draw_circle(VCX, VCY, 132, CLR_GROOVE);
    fb_draw_circle(VCX, VCY, 114, CLR_GROOVE);
    fb_fill_circle(VCX, VCY, 78, CLR_LABEL);
    fb_draw_circle(VCX, VCY, 78, 0x5A6A8A);
    fb_fill_circle(VCX, VCY, 6, 0x0B0C0E);

    char idx[8];
    snprintf(idx, sizeof(idx), "%02d", g_song_cur + 1);
    fb_text(VCX - 16, VCY - 8, idx, CLR_TXT, 2);

    /* 唱针：播放时落针，暂停时抬起（只画两个静态位） */
    int paused = (mp_state() == ST_PAUSED);
    int ex = 320, ey = paused ? 185 : 205;
    draw_line(430, 100, ex, ey, 5, 0xC9CEDB);
    fb_fill_circle(430, 100, 10, 0x8E97A8);
    fb_fill_circle(442, 88, 8, 0x6F7888);

    /* 圆周高光点：播放中旋转（10 秒一圈） */
    int dx = (int)(cos(g_angle) * 142);
    int dy = (int)(sin(g_angle) * 142);
    fb_fill_circle(VCX + dx, VCY + dy, 4, 0xE8EAF0);
}

static void draw_info_locked(void)
{
    char title[128] = "NO MUSIC";
    char meta[128];
    if (g_song_num > 0) {
        snprintf(title, sizeof(title), "%s", base_name(g_songs[g_song_cur]));
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
    }
    char dur[16];
    mmss(mp_total_sec(), dur, sizeof(dur));
    snprintf(meta, sizeof(meta), "MP3   %s   LOCAL", dur);

    text_clip(LYRIC_X, 72, title, CLR_TXT, 2, 490);
    fb_text(LYRIC_X, 116, meta, CLR_DIM, 1);

    /* tab：LYRICS 激活 / LIST 未激活 */
    fb_text(LYRIC_X, 150, "LYRICS", CLR_TXT, 1);
    fb_fill_rect(LYRIC_X, 172, 52, 3, CLR_RED);
    fb_text(LYRIC_X + 90, 150, "LIST", CLR_DIM, 1);
}

static void draw_lyrics_locked(int idx)
{
    fb_gradient_rect(LYRIC_X, LYRIC_Y, LYRIC_W, LYRIC_H);
    if (lrc_count() == 0) {
        fb_text(LYRIC_X, LYRIC_Y + 6, "NO LYRICS", CLR_DIM, 1);
        if (g_song_num > 0)
            text_clip(LYRIC_X, LYRIC_Y + 40, base_name(g_songs[g_song_cur]),
                      CLR_DIM, 1, LYRIC_W);
        return;
    }
    /* 当前行放第 3 行，向上留 2 行，随播放滚动 */
    for (int k = 0; k < 7; k++) {
        int i = idx - 2 + k;
        if (i < 0 || i >= lrc_count())
            continue;
        int y = LYRIC_Y + 6 + k * LYRIC_STEP;
        if (i == idx) {
            text_clip(LYRIC_X, y, lrc_text(i), CLR_TXT, 1, LYRIC_W);
            /* 重复画一次右移 1px，制造"加粗"效果 */
            text_clip(LYRIC_X + 1, y, lrc_text(i), CLR_TXT, 1, LYRIC_W - 1);
        } else {
            text_clip(LYRIC_X, y, lrc_text(i), CLR_DIM, 1, LYRIC_W);
        }
    }
}

static void draw_progress_locked(void)
{
    int total = mp_total_sec();
    int cur = mp_cur_sec();
    /* 先盖掉旧进度条和滑块（滑块半径 6，比条高，不盖会留残影），
     * 再画新的；这块背景是渐变，用 gradient_rect 保持底色一致 */
    fb_gradient_rect(PB_X - 8, PB_Y - 12, PB_W + 16, 30);
    fb_fill_rect(PB_X, PB_Y, PB_W, PB_H, CLR_TRACK);
    if (total > 0) {
        int w = (int)((long)PB_W * cur / total);
        if (w > PB_W) w = PB_W;
        if (w > 0)
            fb_fill_rect(PB_X, PB_Y, w, PB_H, CLR_RED);
        fb_fill_circle(PB_X + w, PB_Y + PB_H / 2, 6, CLR_TXT);
    }
    char buf[16];
    mmss(cur, buf, sizeof(buf));
    fb_text(PB_X, PB_Y + 12, buf, CLR_DIM, 1);
    mmss(total, buf, sizeof(buf));
    fb_text(PB_X + PB_W - 40, PB_Y + 12, buf, CLR_DIM, 1);
}

static void draw_play_button_locked(void)
{
    fb_fill_circle(BTN_PLAY, BTN_Y, 32, CLR_RED);
    if (mp_state() == ST_PAUSED)
        fb_icon_play(BTN_PLAY, BTN_Y, 14, CLR_TXT);
    else
        fb_icon_pause(BTN_PLAY, BTN_Y, 14, CLR_TXT);
}

static void draw_volume_locked(void)
{
    /* 底部音量入口 + 顶部音量条（按音量键后显示 2 秒） */
    fb_text(860, BTN_Y - 8, "VOL", CLR_DIM, 1);
    if (g_vol_ticks <= 0)
        return;
    int x = 700, y = 20, w = 180, h = 14;
    fb_fill_rect(x, y, w, h, CLR_TRACK);
    int vw = (w - 4) * mp_volume_get() / 100;
    if (vw > 0)
        fb_fill_rect(x + 2, y + 2, vw, h - 4, CLR_RED);
    fb_draw_rect(x, y, w, h, CLR_DIM);
}

static void draw_controls_locked(void)
{
    fb_icon_prev(BTN_PREV, BTN_Y, 18, CLR_TXT);
    draw_play_button_locked();
    fb_icon_next(BTN_NEXT, BTN_Y, 18, CLR_TXT);
    for (int i = 0; i < 3; i++)
        fb_fill_rect(BTN_LIST - 16, BTN_Y - 12 + i * 10, 32, 3, CLR_TXT);
    draw_volume_locked();
}

static void draw_music(void)
{
    fb_lock();
    fb_gradient(CLR_TOP, CLR_BOT);
    draw_topbar_locked();
    draw_vinyl_locked();
    draw_info_locked();
    int idx = lrc_index(mp_cur_sec());
    g_lyric_idx = idx;
    draw_lyrics_locked(idx);
    draw_progress_locked();
    draw_controls_locked();
    fb_unlock();
}

/* 局部刷新：歌词行变化才重画 */
static void update_lyrics(void)
{
    int idx = lrc_index(mp_cur_sec());
    if (idx == g_lyric_idx)
        return;
    g_lyric_idx = idx;
    fb_lock();
    draw_lyrics_locked(idx);
    fb_unlock();
}

/* 局部刷新：黑胶高光点转一格（不重画整盘） */
static void update_vinyl(void)
{
    if (mp_state() != ST_PLAYING)
        return;
    int ox = VCX + (int)(cos(g_angle) * 142);
    int oy = VCY + (int)(sin(g_angle) * 142);
    g_angle += 0.314f;                       /* 每 0.5s 转 18°，10 秒一圈 */
    if (g_angle > 6.2831853f)
        g_angle -= 6.2831853f;
    int nx = VCX + (int)(cos(g_angle) * 142);
    int ny = VCY + (int)(sin(g_angle) * 142);
    fb_lock();
    fb_fill_circle(ox, oy, 6, CLR_DISC);
    fb_fill_circle(nx, ny, 4, 0xE8EAF0);
    fb_unlock();
}

/* ================= 歌单侧栏 ================= */

static void draw_list_panel(void)
{
    fb_lock();
    fb_fill_rect(644, 0, 380, SCREEN_H, CLR_PANEL);
    fb_text(668, 22, "PLAYLIST", CLR_PTXT, 1);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d", g_song_num);
    fb_text(744, 22, cnt, CLR_PDIM, 1);
    fb_text(988, 16, "X", CLR_PDIM, 1);

    for (int i = 0; i < g_song_num && i < 9; i++) {
        int y = 76 + i * 52;
        if (i == g_song_cur) {
            fb_fill_rect(644, y, 4, 44, CLR_RED);
            fb_fill_rect(644, y, 380, 44, 0xFBE9E8);
        }
        char idx[8];
        snprintf(idx, sizeof(idx), "%d", i + 1);
        fb_text(664, y + 14, idx, i == g_song_cur ? CLR_RED : CLR_PDIM, 1);

        char title[128];
        snprintf(title, sizeof(title), "%s", base_name(g_songs[i]));
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
        text_clip(700, y + 14, title,
                  i == g_song_cur ? CLR_RED : CLR_PTXT, 1, 250);
    }
    fb_unlock();
}

/* ================= 视频/菜单页（沿用旧版） ================= */

static void draw_video(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);
    fb_icon_back(50, 35, 14, C_TXT);
    fb_icon_film(SCREEN_W / 2, 35, 14, C_TXT);
    fb_unlock();
    int n = g_video_num < MAX_ROW ? g_video_num : MAX_ROW;
    for (int i = 0; i < n; i++) {
        fb_lock();
        fb_fill_rect(LIST_X, LIST_Y + i * ROW_H, ROW_W, ROW_H - 8, C_CARD);
        fb_icon_play(LIST_X + 60, LIST_Y + i * ROW_H + (ROW_H - 8) / 2,
                     12, C_TXT);
        const char *p = base_name(g_videos[i]);
        fb_draw_string(LIST_X + 100, LIST_Y + i * ROW_H + 20, p, C_TXT);
        fb_unlock();
    }
}

static void draw_menu(void)
{
    fb_lock();
    fb_clear(C_BG);
    fb_fill_rect(0, 0, SCREEN_W, 70, C_CARD);
    fb_icon_back(50, 35, 14, C_TXT);
    fb_draw_string(464, 28, "MENU", C_TXT);
    fb_fill_rect(LIST_X, 180, ROW_W, 120, C_CARD);
    fb_icon_film(LIST_X + 80, 240, 22, C_TXT);
    fb_draw_string(LIST_X + 160, 232, "VIDEO", C_TXT);
    fb_fill_rect(LIST_X, 330, ROW_W, 120, C_CARD);
    fb_icon_minus(LIST_X + 80, 390, 18, C_TXT);
    fb_draw_string(LIST_X + 160, 382, "EXIT", C_TXT);
    fb_unlock();
}

/* ================= 播放控制 ================= */

static void start_song(int idx)
{
    if (idx < 0 || idx >= g_song_num)
        return;
    g_song_cur = idx;
    mp_start(g_songs[idx], 0);
    lrc_load(g_songs[idx]);
    g_lyric_idx = -2;
    g_angle = 0.0f;
}

static void song_step(int d)
{
    if (g_song_num <= 0)
        return;
    int n = (g_song_cur + d + g_song_num) % g_song_num;
    start_song(n);
}

/* 视频播放：全屏 fbdev2，UI 停笔 */
static void play_video(int idx)
{
    if (idx < 0 || idx >= g_video_num)
        return;
    fb_save_state();
    g_video_done = 0;
    g_video_playing = 0;
    if (mp_start(g_videos[idx], 1) != 0) {
        fb_restore_state();
        return;
    }
    g_video_done = 0;
    g_video_playing = 1;
    for (;;) {
        fd_set rfds;
        struct timeval tv = {0, 300 * 1000};
        FD_ZERO(&rfds);
        FD_SET(touch_fd(), &rfds);
        int r = select(touch_fd() + 1, &rfds, NULL, NULL, &tv);
        if (g_video_done)
            break;
        if (r > 0) {
            int x, y;
            if (touch_read(&x, &y) == 0) {
                if (x < 200) {              /* 左边缘退出 */
                    mp_stop();
                    break;
                }
                mp_pause_toggle();          /* 其余区域暂停/继续 */
            }
        }
    }
    g_video_playing = 0;
    g_video_done = 0;
    mp_stop();
    fb_restore_state();
    draw_video();
}

/* 回音乐页：视频/菜单返回都走这里；停着就重新拉起，避免"假播" */
static void enter_music(void)
{
    g_scr = SCR_MUSIC;
    if (g_song_num > 0 && mp_state() == ST_STOPPED) {
        if (g_song_cur < 0 || g_song_cur >= g_song_num)
            g_song_cur = 0;
        start_song(g_song_cur);
    } else if (g_song_num > 0) {
        lrc_load(g_songs[g_song_cur]);
    }
    draw_music();
}

/* ================= 后台线程 ================= */

static void *ui_ticker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (g_vol_ticks > 0 && --g_vol_ticks == 0) {
            fb_lock();
            fb_gradient_rect(700, 16, 184, 24);   /* 盖掉音量条 */
            fb_unlock();
        }
        if (mp_song_finished()) {
            if (g_scr == SCR_MUSIC) {
                song_step(+1);                    /* 自动连播 */
                draw_music();
            } else if (g_video_playing) {
                g_video_done = 1;
            }
        }
        if (g_scr == SCR_MUSIC) {
            fb_lock();
            draw_progress_locked();
            fb_unlock();
            update_lyrics();
            update_vinyl();
            static int last_state = -1;
            int st = mp_state();
            if (st != last_state) {
                fb_lock();
                draw_play_button_locked();
                fb_unlock();
                last_state = st;
            }
        }
    }
    return NULL;
}

/* ================= 触摸处理 ================= */

static void on_music_touch(int x, int y)
{
    if (hit(x, y, 0, 0, 80, TOP_H)) {              /* 菜单 */
        g_scr = SCR_MENU;
        draw_menu();
        return;
    }
    if (hit(x, y, 880, 0, 144, TOP_H)) {           /* 视频页 */
        g_scr = SCR_VIDEO;
        draw_video();
        return;
    }
    int dx = x - VCX, dy = y - VCY;
    if (dx * dx + dy * dy <= (VR + 6) * (VR + 6)) { /* 黑胶：暂停/继续 */
        mp_pause_toggle();
        fb_lock();
        draw_play_button_locked();
        fb_unlock();
        return;
    }
    if (hit(x, y, PB_X, PB_Y - 12, PB_W, 30)) {     /* 进度条跳转 */
        int pct = (x - PB_X) * 100 / PB_W;
        mp_seek_percent(pct);
        fb_lock();
        draw_progress_locked();
        fb_unlock();
        return;
    }
    if (hit(x, y, BTN_PREV - 40, BTN_Y - 32, 80, 64)) {
        song_step(-1);
        draw_music();
        return;
    }
    if (hit(x, y, BTN_PLAY - 40, BTN_Y - 32, 80, 64)) {
        mp_pause_toggle();
        fb_lock();
        draw_play_button_locked();
        fb_unlock();
        return;
    }
    if (hit(x, y, BTN_NEXT - 40, BTN_Y - 32, 80, 64)) {
        song_step(+1);
        draw_music();
        return;
    }
    if (hit(x, y, BTN_LIST - 30, BTN_Y - 26, 60, 52)) { /* 歌单侧栏 */
        g_scr = SCR_LIST;
        draw_list_panel();
        return;
    }
    if (hit(x, y, 850, BTN_Y - 26, 100, 52)) {     /* 音量入口 */
        g_vol_ticks = 4;
        fb_lock();
        fb_gradient_rect(700, 16, 184, 24);
        draw_volume_locked();
        fb_unlock();
        return;
    }
    if (g_vol_ticks > 0 && hit(x, y, 700, 16, 180, 24)) { /* 点音量条设音量 */
        mp_volume_set((x - 700) * 100 / 180);
        g_vol_ticks = 4;
        fb_lock();
        fb_gradient_rect(700, 16, 184, 24);
        draw_volume_locked();
        fb_unlock();
        return;
    }
}

static void on_list_touch(int x, int y)
{
    if (x < 644 || hit(x, y, 976, 0, 48, 56)) {     /* 点面板外/关闭 */
        g_scr = SCR_MUSIC;
        draw_music();
        return;
    }
    int row = (y - 76) / 52;
    if (y >= 76 && row < g_song_num) {
        start_song(row);
        draw_list_panel();
    }
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("media_player build %s %s\n", __DATE__, __TIME__);

    mp_kill_stale();                        /* 单实例保护（扫 /proc，跳过自己） */

    if (mp_subsys_init() != 0) return 1;
    if (fb_init() != 0)        return 1;
    if (touch_init(argc > 1 ? argv[1] : NULL) != 0) return 1;

    if (g_song_num > 0) {
        lrc_load(g_songs[g_song_cur]);
        mp_start(g_songs[g_song_cur], 0);
    }
    draw_music();

    /* 调试入口：./media_player /dev/input/event6 list
     * 直接打开歌单侧栏，方便截图/验收（正常使用不传这个参数） */
    if (argc > 2 && strcmp(argv[2], "list") == 0) {
        g_scr = SCR_LIST;
        draw_list_panel();
    }

    pthread_t tid;
    pthread_create(&tid, NULL, ui_ticker, NULL);
    pthread_detach(tid);

    int x, y;
    while (touch_read(&x, &y) == 0) {
        if (g_scr == SCR_MUSIC) {
            on_music_touch(x, y);
        } else if (g_scr == SCR_LIST) {
            on_list_touch(x, y);
        } else if (g_scr == SCR_VIDEO) {
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {
                enter_music();
                continue;
            }
            if (y >= LIST_Y && y < LIST_Y + MAX_ROW * ROW_H &&
                x >= LIST_X && x < LIST_X + ROW_W) {
                int row = (y - LIST_Y) / ROW_H;
                if (row < g_video_num) {
                    g_video_cur = row;
                    play_video(row);
                }
            }
        } else {                            /* SCR_MENU */
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {
                enter_music();
                continue;
            }
            if (hit(x, y, LIST_X, 180, ROW_W, 120)) {
                g_scr = SCR_VIDEO;
                draw_video();
                continue;
            }
            if (hit(x, y, LIST_X, 330, ROW_W, 120))
                break;                      /* EXIT */
        }
    }

    mp_subsys_deinit();
    fb_deinit();
    touch_deinit();
    printf("exit\n");
    return 0;
}
