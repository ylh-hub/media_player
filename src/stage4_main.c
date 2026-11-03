/*
 * stage4_main.c —— 阶段 4：最终版（主界面 + 音乐 + 视频 + 相册）
 * ============================================================
 * 界面树：
 *   HOME（图标启动器：音乐/视频/相册/退出）
 *     ├─ MUSIC（网易云风：黑胶+歌词+控制条；☰ 歌单侧栏）
 *     ├─ VIDEO（视频列表 → 全屏 fbdev2 播放）
 *     └─ ALBUM（缩略图网格 → VIEW 大图：左右滑动切换、点按返回）
 *
 * 关键设计：
 *   1) 音乐页所有绘制都在 fb_lock() 内，ticker 只做"局部刷新"
 *   2) 视频期间 UI 线程停笔（fbdev2 独占 /dev/fb0）
 *   3) 相册图片用 stb_image 解码（jpg/png/bmp），缩略图懒加载+缓存
 *   4) 滑动/点按用手势接口 touch_read_gesture（位移 >60px 判滑动）
 *
 * 运行: ./media_player /dev/input/eventX
 */
#include <mplayer_ctl.h>
#include <fb_ui.h>
#include <touch.h>
#include <lrc.h>
#include <image.h>
#include <album.h>

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
#define CLR_CARD    0x242A3A
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
enum screen {
    SCR_HOME = 0,
    SCR_MUSIC,
    SCR_LIST,
    SCR_VIDEO,
    SCR_ALBUM,
    SCR_VIEW
};
static volatile int g_scr = SCR_HOME;
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

/* 视频列表页布局 */
#define LIST_X   32
#define LIST_Y   90
#define ROW_W    960
#define ROW_H    64
#define MAX_ROW  6
#define EXIT_W   70
#define EXIT_H   56

/* 主界面磁贴 */
#define TILE_Y   170
#define TILE_W   200
#define TILE_H   260
#define TILE_X(i) (64 + (i) * 240)

/* 相册网格 */
#define GRID_X(c) (32 + (c) * 248)
#define GRID_Y(r) (90 + (r) * 250)
#define GRID_W   228
#define GRID_H   220

static int g_vol_ticks = 0;
static int g_lyric_idx = -2;
static float g_angle = 0.0f;
static Img *g_view_img = NULL;      /* 当前大图（切换时释放重解） */
static int  g_view_idx = 0;
static Img *g_icon_music;           /* 主界面磁贴图标（/ylh/icons，缺失回退代码绘制） */
static Img *g_icon_album;

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

/* 超宽截断：按像素宽度画，避免文字冲出面板 */
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

/* Bresenham 粗线：唱针/照片图标用（fb 层只有点/矩形） */
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

/* 歌名映射：演示固定 4 首直接映射中文；不做 ID3 解析（两周内没必要） */
static const struct {
    const char *key;
    const char *cn;
} g_title_map[] = {
    {"huluwa",        "葫芦娃"},
    {"lanjingling",   "蓝精灵"},
    {"liangzhilaohu", "两只老虎"},
    {"qifengle",      "起风了"},
};

static const char *song_title(int idx)
{
    static char buf[128];
    if (idx < 0 || idx >= g_song_num)
        return "NO MUSIC";
    snprintf(buf, sizeof(buf), "%s", base_name(g_songs[idx]));
    char *dot = strrchr(buf, '.');
    if (dot)
        *dot = 0;
    for (size_t i = 0; i < sizeof(g_title_map) / sizeof(g_title_map[0]); i++)
        if (strcmp(buf, g_title_map[i].key) == 0)
            return g_title_map[i].cn;
    return buf;
}

/* ================= 主界面 HOME ================= */

/* 圆角矩形：中间两矩形 + 四角圆（fb 层没有圆角原语） */
static void rounded_rect(int x, int y, int w, int h, int r, int color)
{
    fb_fill_rect(x + r, y, w - 2 * r, h, color);
    fb_fill_rect(x, y + r, w, h - 2 * r, color);
    fb_fill_circle(x + r, y + r, r, color);
    fb_fill_circle(x + w - r, y + r, r, color);
    fb_fill_circle(x + r, y + h - r, r, color);
    fb_fill_circle(x + w - r, y + h - r, r, color);
}

/* 右指实心三角（左缘垂直）：播放键/摄像机内三角 */
static void tri_right(int x0, int cy, int s, int color)
{
    for (int dy = -s; dy <= s; dy++) {
        int wdt = s - abs(dy);
        if (wdt > 0)
            fb_fill_rect(x0, cy + dy, wdt, 1, color);
    }
}

/* 左指实心三角（右缘垂直）：摄像机镜头 */
static void tri_left_apex(int x1, int cy, int s, int color)
{
    for (int dy = -s; dy <= s; dy++) {
        int wdt = s - abs(dy);
        if (wdt > 0)
            fb_fill_rect(x1 - wdt, cy + dy, wdt, 1, color);
    }
}

/* 音乐磁贴：红圆角方块 + 白色双音符（参照 assets/icon_music_ref.jpg） */
static void draw_icon_music(int cx, int cy)
{
    rounded_rect(cx - 60, cy - 60, 120, 120, 28, 0xE8384F);
    fb_fill_circle(cx - 20, cy + 26, 14, CLR_TXT);
    fb_fill_circle(cx + 22, cy + 16, 14, CLR_TXT);
    fb_fill_rect(cx - 10, cy - 34, 9, 60, CLR_TXT);
    fb_fill_rect(cx + 32, cy - 44, 9, 60, CLR_TXT);
    draw_line(cx - 6, cy - 36, cx + 36, cy - 46, 12, CLR_TXT);
}

/* 视频磁贴：深色圆 + 白色摄像机（参照 assets/icon_video_ref.jpg） */
static void draw_icon_video(int cx, int cy)
{
    fb_fill_circle(cx, cy, 60, 0x262B36);
    rounded_rect(cx - 42, cy - 26, 62, 52, 10, CLR_TXT);
    tri_right(cx - 20, cy, 18, 0x262B36);
    tri_left_apex(cx + 46, cy, 20, CLR_TXT);
}

/* 相册磁贴：八瓣彩色花 + 中心=相册第一张图（参照 assets/icon_album_ref.jpg） */
static void draw_icon_album(int cx, int cy)
{
    static const int petal[8] = {0xFFB300, 0xD4E157, 0x66BB6A, 0x26C6DA,
                                 0x42A5F5, 0x7E57C2, 0xEC407A, 0xEF5350};
    for (int k = 0; k < 8; k++) {
        double a = k * 3.14159265 / 4.0;
        int px = cx + (int)(cos(a) * 32);
        int py = cy + (int)(sin(a) * 32);
        fb_fill_circle(px, py, 26, petal[k]);
    }
    Img *t = album_thumb(0);
    if (t != NULL) {
        img_draw_fill_fb(t, cx - 22, cy - 22, 44, 44);
        fb_draw_circle(cx, cy, 24, CLR_TXT);
    } else {
        fb_fill_circle(cx, cy, 8, CLR_TXT);
    }
}

static void draw_power_icon(int cx, int cy, int color)
{
    fb_draw_circle(cx, cy + 6, 26, color);
    fb_fill_rect(cx - 3, cy - 28, 6, 26, color);
}

static void draw_home(void)
{
    static const char *labels[4] = {"音乐", "视频", "相册", "退出"};
    fb_lock();
    fb_gradient(CLR_TOP, CLR_BOT);
    fb_text(472, 24, "媒体播放器", CLR_TXT, 1);
    for (int i = 0; i < 4; i++) {
        int x = TILE_X(i);
        fb_fill_rect(x, TILE_Y, TILE_W, TILE_H, CLR_CARD);
        int cx = x + TILE_W / 2, cy = TILE_Y + 100;
        if (i == 0) {
            if (g_icon_music != NULL)
                img_draw_fit_fb(g_icon_music, x + 20, TILE_Y + 20, 160, 160);
            else
                draw_icon_music(cx, cy);
        } else if (i == 1) {
            /* 视频磁贴用代码绘制：参考图是 JPEG，抠透明底总有压缩伪影，
             * 代码画的圆+摄像机更干净，也和参考图同风格 */
            draw_icon_video(cx, cy);
        } else if (i == 2) {
            if (g_icon_album != NULL)
                img_draw_fit_fb(g_icon_album, x + 20, TILE_Y + 20, 160, 160);
            else
                draw_icon_album(cx, cy);
        } else {
            draw_power_icon(cx, cy, CLR_TXT);
        }
        int w = fb_text_width(labels[i], 1);
        fb_text(cx - w / 2, TILE_Y + 200, labels[i], CLR_TXT, 1);
    }
    fb_unlock();
}

/* ================= 音乐页绘制（调用方持锁） ================= */

static void draw_topbar_locked(void)
{
    for (int i = 0; i < 3; i++)
        fb_fill_rect(24, 20 + i * 9, 26, 3, CLR_TXT);
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

    int paused = (mp_state() == ST_PAUSED);
    int ex = 320, ey = paused ? 185 : 205;
    draw_line(430, 100, ex, ey, 5, 0xC9CEDB);
    fb_fill_circle(430, 100, 10, 0x8E97A8);
    fb_fill_circle(442, 88, 8, 0x6F7888);

    int dx = (int)(cos(g_angle) * 142);
    int dy = (int)(sin(g_angle) * 142);
    fb_fill_circle(VCX + dx, VCY + dy, 4, 0xE8EAF0);
}

static void draw_info_locked(void)
{
    char meta[128];
    const char *title = song_title(g_song_cur);
    char dur[16];
    mmss(mp_total_sec(), dur, sizeof(dur));
    snprintf(meta, sizeof(meta), "MP3   %s   LOCAL", dur);

    text_clip(LYRIC_X, 72, title, CLR_TXT, 2, 490);
    fb_text(LYRIC_X, 116, meta, CLR_DIM, 1);

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
    for (int k = 0; k < 7; k++) {
        int i = idx - 2 + k;
        if (i < 0 || i >= lrc_count())
            continue;
        int y = LYRIC_Y + 6 + k * LYRIC_STEP;
        if (i == idx) {
            text_clip(LYRIC_X, y, lrc_text(i), CLR_TXT, 1, LYRIC_W);
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

static void update_vinyl(void)
{
    if (mp_state() != ST_PLAYING)
        return;
    int ox = VCX + (int)(cos(g_angle) * 142);
    int oy = VCY + (int)(sin(g_angle) * 142);
    g_angle += 0.314f;
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
        text_clip(700, y + 14, song_title(i),
                  i == g_song_cur ? CLR_RED : CLR_PTXT, 1, 250);
    }
    fb_unlock();
}

/* ================= 视频列表 ================= */

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
        fb_draw_string(LIST_X + 100, LIST_Y + i * ROW_H + 20,
                       base_name(g_videos[i]), C_TXT);
        fb_unlock();
    }
}

/* ================= 相册 ================= */

static void draw_album(void)
{
    fb_lock();
    fb_gradient(CLR_TOP, CLR_BOT);
    fb_icon_back(50, 35, 14, CLR_TXT);
    fb_text(84, 26, "相册", CLR_TXT, 1);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d", album_count());
    fb_text(130, 26, cnt, CLR_DIM, 1);
    if (album_count() == 0)
        fb_text(380, 300, "无图片: 拷到 /ylh/photos", CLR_DIM, 1);
    fb_unlock();

    for (int i = 0; i < album_count() && i < 8; i++) {
        int x = GRID_X(i % 4), y = GRID_Y(i / 4);
        Img *th = album_thumb(i);
        fb_lock();
        if (th != NULL)
            img_draw_fill_fb(th, x, y, GRID_W, GRID_H);
        else
            fb_fill_rect(x, y, GRID_W, GRID_H, CLR_CARD);
        fb_draw_rect(x - 1, y - 1, GRID_W + 2, GRID_H + 2, CLR_DIM);
        fb_unlock();
    }
    if (album_count() > 8) {
        char more[16];
        snprintf(more, sizeof(more), "+%d", album_count() - 8);
        fb_lock();
        fb_text(960, 570, more, CLR_DIM, 1);
        fb_unlock();
    }
}

static void draw_view(void)
{
    img_free(g_view_img);
    g_view_img = album_full(g_view_idx);
    fb_lock();
    fb_clear(0x000000);
    if (g_view_img != NULL)
        img_draw_fit_fb(g_view_img, 0, 0, SCREEN_W, SCREEN_H);
    char badge[16];
    snprintf(badge, sizeof(badge), "%d/%d", g_view_idx + 1, album_count());
    fb_fill_rect(944, 12, 68, 26, 0x000000);
    fb_text(952, 16, badge, CLR_TXT, 1);
    fb_text(24, 570, "滑动切换 点按返回", CLR_DIM, 1);
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

static void go_home(void)
{
    if (g_scr == SCR_MUSIC)
        mp_stop();                     /* 回主界面停音乐，行为可预期 */
    img_free(g_view_img);
    g_view_img = NULL;
    g_scr = SCR_HOME;
    draw_home();
}

/* 视频播放：全屏 fbdev2，UI 停笔 */
static void play_video(int idx)
{
    if (idx < 0 || idx >= g_video_num)
        return;
    int prev_scr = g_scr;
    g_scr = SCR_VIDEO;
    fb_save_state();
    g_video_done = 0;
    g_video_playing = 0;
    if (mp_start(g_videos[idx], 1) != 0) {
        g_scr = prev_scr;
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
                if (x < 200) {
                    mp_stop();
                    break;
                }
                mp_pause_toggle();
            }
        }
    }
    g_video_playing = 0;
    g_video_done = 0;
    mp_stop();
    fb_restore_state();
    draw_video();
}

/* ================= 后台线程 ================= */

static void *ui_ticker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (g_vol_ticks > 0) {
            if (--g_vol_ticks == 0 && !g_video_playing) {
                fb_lock();
                fb_gradient_rect(700, 16, 184, 24);
                fb_unlock();
            }
        }
        if (mp_song_finished()) {
            if (g_scr == SCR_MUSIC) {
                song_step(+1);
                draw_music();
            } else if (g_video_playing) {
                g_video_done = 1;
            }
        }
        if (g_scr == SCR_MUSIC && !g_video_playing) {
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

static int on_home_touch(int x, int y)
{
    for (int i = 0; i < 4; i++) {
        if (!hit(x, y, TILE_X(i), TILE_Y, TILE_W, TILE_H))
            continue;
        if (i == 0)
            enter_music();
        else if (i == 1) {
            g_scr = SCR_VIDEO;
            draw_video();
        } else if (i == 2) {
            g_scr = SCR_ALBUM;
            draw_album();
        } else {
            return 1;                  /* 退出 */
        }
        return 0;
    }
    return 0;
}

static void on_music_touch(int x, int y)
{
    if (hit(x, y, 0, 0, 80, TOP_H)) {              /* 左上 = 回主界面 */
        go_home();
        return;
    }
    if (hit(x, y, 880, 0, 144, TOP_H)) {
        g_scr = SCR_VIDEO;
        draw_video();
        return;
    }
    int dx = x - VCX, dy = y - VCY;
    if (dx * dx + dy * dy <= (VR + 6) * (VR + 6)) {
        mp_pause_toggle();
        fb_lock();
        draw_play_button_locked();
        fb_unlock();
        return;
    }
    if (hit(x, y, PB_X, PB_Y - 12, PB_W, 30)) {
        mp_seek_percent((x - PB_X) * 100 / PB_W);
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
    if (hit(x, y, BTN_LIST - 30, BTN_Y - 26, 60, 52)) {
        g_scr = SCR_LIST;
        draw_list_panel();
        return;
    }
    if (hit(x, y, 850, BTN_Y - 26, 100, 52)) {
        g_vol_ticks = 4;
        fb_lock();
        fb_gradient_rect(700, 16, 184, 24);
        draw_volume_locked();
        fb_unlock();
        return;
    }
    if (g_vol_ticks > 0 && hit(x, y, 700, 16, 180, 24)) {
        mp_volume_set((x - 700) * 100 / 180);
        g_vol_ticks = 4;
        fb_lock();
        fb_gradient_rect(700, 16, 184, 24);
        draw_volume_locked();
        fb_unlock();
    }
}

static void on_list_touch(int x, int y)
{
    if (x < 644 || hit(x, y, 976, 0, 48, 56)) {
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

static void on_album_touch(int x, int y)
{
    if (hit(x, y, 0, 0, 100, 70)) {
        go_home();
        return;
    }
    for (int i = 0; i < album_count() && i < 8; i++) {
        int gx = GRID_X(i % 4), gy = GRID_Y(i / 4);
        if (hit(x, y, gx, gy, GRID_W, GRID_H)) {
            g_view_idx = i;
            g_scr = SCR_VIEW;
            draw_view();
            return;
        }
    }
}

static void on_view_gesture(int ges, int x, int y)
{
    (void)x;
    (void)y;
    int n = album_count();
    if (n <= 0) {
        g_scr = SCR_ALBUM;
        draw_album();
        return;
    }
    if (ges == TOUCH_TAP) {
        g_scr = SCR_ALBUM;
        draw_album();
        return;
    }
    int d = (ges == TOUCH_SWIPE_LEFT) ? 1 : -1;
    g_view_idx = (g_view_idx + d + n) % n;
    draw_view();
}

/* ================= 主入口 ================= */

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("media_player build %s %s\n", __DATE__, __TIME__);

    mp_kill_stale();

    if (mp_subsys_init() != 0) return 1;
    if (fb_init() != 0)        return 1;
    if (touch_init(argc > 1 ? argv[1] : NULL) != 0) return 1;
    album_scan();
    g_icon_music = img_load("/ylh/icons/music.jpg");
    g_icon_album = img_load("/ylh/icons/album.jpg");

    draw_home();

    /* 调试入口（验收/截图用，正常使用不传）：
     *   ./media_player <dev> list  —— 进音乐页并打开歌单侧栏
     *   ./media_player <dev> video —— 直接播第一个视频
     *   ./media_player <dev> album —— 直接进相册缩略图页
     *   ./media_player <dev> view  —— 直接进第一张大图 */
    if (argc > 2 && strcmp(argv[2], "list") == 0) {
        enter_music();
        g_scr = SCR_LIST;
        draw_list_panel();
    } else if (argc > 2 && strcmp(argv[2], "video") == 0) {
        play_video(0);
    } else if (argc > 2 && strcmp(argv[2], "album") == 0) {
        g_scr = SCR_ALBUM;
        draw_album();
    } else if (argc > 2 && strcmp(argv[2], "view") == 0) {
        g_view_idx = 0;
        g_scr = SCR_VIEW;
        draw_view();
    }

    pthread_t tid;
    pthread_create(&tid, NULL, ui_ticker, NULL);
    pthread_detach(tid);

    int exit_req = 0;
    int x, y, dx;
    while (!exit_req) {
        int ges = touch_read_gesture(&x, &y, &dx);
        if (ges < 0)
            break;
        if (g_scr == SCR_VIEW) {
            on_view_gesture(ges, x, y);
            continue;
        }
        if (ges != TOUCH_TAP)
            continue;                  /* 其他页忽略滑动 */

        if (g_scr == SCR_HOME) {
            exit_req = on_home_touch(x, y);
        } else if (g_scr == SCR_MUSIC) {
            on_music_touch(x, y);
        } else if (g_scr == SCR_LIST) {
            on_list_touch(x, y);
        } else if (g_scr == SCR_VIDEO) {
            if (hit(x, y, 0, 0, EXIT_W, EXIT_H)) {
                go_home();
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
        } else if (g_scr == SCR_ALBUM) {
            on_album_touch(x, y);
        }
    }

    img_free(g_view_img);
    g_view_img = NULL;
    img_free(g_icon_music);
    img_free(g_icon_album);
    mp_subsys_deinit();
    fb_deinit();
    touch_deinit();
    printf("exit\n");
    return 0;
}
