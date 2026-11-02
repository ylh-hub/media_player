/*
 * fb_ui.c —— framebuffer 绘制层实现
 * ============================================================
 * framebuffer 大白话：/dev/fb0 就是屏幕的"显存文件"，
 * mmap 之后它就是一块 int 数组，1024*600 个格子，
 * 每个格子写一个 0xRRGGBB 颜色，屏幕对应像素就变色。
 * 为什么画点前要判边界：越界写会把 mmap 外的内存写坏（段错误）。
 * 为什么有锁：进度线程和触摸主循环都会画屏，不加锁会写花。
 * ============================================================
 */
#include "fb_ui.h"
#include "font16.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_fb_fd = -1;
static int *g_fb = NULL;
static size_t g_len = 0;                 /* 实际 mmap 的长度（deinit 用） */
static int g_yoff = 0;                   /* 可见缓冲区起始行（双缓冲板子上可能是 600） */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 保存/还原 fb 显示参数（视频播放前后用） */
static struct fb_var_screeninfo g_saved_var;
static int g_has_saved = 0;

int fb_init(void)
{
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd == -1) {
        perror("open /dev/fb0");
        return -1;
    }
    /* 自动去黑屏（FBIOBLANK=UNBLANK）：否则重启后开机 logo/黑屏状态
     * 会一直占着显示通道，我们画了也看不见（2026-11-02 实测踩坑） */
    ioctl(g_fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
    struct fb_var_screeninfo vinfo;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("ioctl FBIOGET_VSCREENINFO");
        return -1;
    }
    if (vinfo.xres != SCREEN_W || vinfo.yres != SCREEN_H ||
        vinfo.bits_per_pixel != 32)
        fprintf(stderr,
                "fb_ui 警告: 实际 %ux%u %ubpp，代码按 1024x600x32 绘制\n",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    /* 关键补丁（2026-11-02 "画了但屏幕没变"的对策）：
     * RK 系 fbdev 常配双缓冲（yres_virtual=2*yres），控制台可能正 pan 在
     * 第 2 块缓冲上，我们画第 1 块就看不见。
     * 对策：强制 yres_virtual=yres 且 yoffset=0，把可见区翻回开头；
     * 驱动拒绝就退化为"直接画在当前可见区"（g_yoff 偏移）。 */
    struct fb_var_screeninfo want = vinfo;
    want.yoffset = 0;
    if (want.yres_virtual > SCREEN_H)
        want.yres_virtual = SCREEN_H;
    if (ioctl(g_fb_fd, FBIOPUT_VSCREENINFO, &want) == 0) {
        g_yoff = 0;
        vinfo = want;
    } else {
        g_yoff = (int)vinfo.yoffset;     /* 可见区在第几行，就画在第几行 */
    }

    /* 按驱动报告的真实显存大小 mmap，别按 2.4MB 硬算（双缓冲会更大） */
    struct fb_fix_screeninfo finfo;
    g_len = (size_t)SCREEN_W * SCREEN_H * 4;
    if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0)
        g_len = (size_t)finfo.smem_len;

    g_fb = mmap(NULL, g_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb == MAP_FAILED) {
        perror("mmap fb");
        g_fb = NULL;
        return -1;
    }
    return 0;
}

void fb_deinit(void)
{
    if (g_fb) {
        munmap(g_fb, g_len);
        g_fb = NULL;
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
}

void fb_save_state(void)
{
    if (g_fb_fd >= 0 &&
        ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_saved_var) == 0)
        g_has_saved = 1;
}

void fb_restore_state(void)
{
    if (g_has_saved && g_fb_fd >= 0)
        ioctl(g_fb_fd, FBIOPUT_VSCREENINFO, &g_saved_var);
}

void fb_lock(void)   { pthread_mutex_lock(&g_lock); }
void fb_unlock(void) { pthread_mutex_unlock(&g_lock); }

void fb_draw_point(int x, int y, int color)
{
    if (g_fb == NULL || x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
        return;
    g_fb[(size_t)(y + g_yoff) * SCREEN_W + x] = color;
}

void fb_fill_rect(int x, int y, int w, int h, int color)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            fb_draw_point(i, j, color);
}

void fb_draw_rect(int x, int y, int w, int h, int color)
{
    fb_fill_rect(x, y, w, 3, color);
    fb_fill_rect(x, y + h - 3, w, 3, color);
    fb_fill_rect(x, y, 3, h, color);
    fb_fill_rect(x + w - 3, y, 3, h, color);
}

void fb_clear(int color)
{
    fb_fill_rect(0, 0, SCREEN_W, SCREEN_H, color);
}

/* ---------------- 图标（纯代码绘制，零资源文件） ---------------- */

/* 右指三角（播放/下一首共用） */
static void tri_right(int cx, int cy, int r, int color)
{
    for (int dy = -r; dy <= r; dy++) {
        int w = 2 * (r - (dy < 0 ? -dy : dy));   /* 中间最宽，两端收尖 */
        for (int dx = 0; dx <= w; dx++)
            fb_draw_point(cx - r + dx, cy + dy, color);
    }
}

/* 左指三角（上一首/返回共用） */
static void tri_left(int cx, int cy, int r, int color)
{
    for (int dy = -r; dy <= r; dy++) {
        int w = 2 * (r - (dy < 0 ? -dy : dy));
        for (int dx = 0; dx <= w; dx++)
            fb_draw_point(cx + r - dx, cy + dy, color);
    }
}

void fb_icon_play(int cx, int cy, int r, int color)
{
    tri_right(cx - r / 4, cy, r, color);
}

void fb_icon_pause(int cx, int cy, int r, int color)
{
    int w = r * 2 / 3;
    fb_fill_rect(cx - r, cy - r, w, 2 * r, color);
    fb_fill_rect(cx + r - w + 1, cy - r, w, 2 * r, color);
}

void fb_icon_next(int cx, int cy, int r, int color)
{
    tri_right(cx - 4, cy, r, color);
    fb_fill_rect(cx + r, cy - r, 5, 2 * r, color);
}

void fb_icon_prev(int cx, int cy, int r, int color)
{
    tri_left(cx + 4, cy, r, color);
    fb_fill_rect(cx - r - 4, cy - r, 5, 2 * r, color);
}

void fb_icon_plus(int cx, int cy, int r, int color)
{
    fb_fill_rect(cx - r, cy - 3, 2 * r, 6, color);
    fb_fill_rect(cx - 3, cy - r, 6, 2 * r, color);
}

void fb_icon_minus(int cx, int cy, int r, int color)
{
    fb_fill_rect(cx - r, cy - 3, 2 * r, 6, color);
}

void fb_icon_music(int cx, int cy, int r, int color)
{
    /* 简化音符：实心圆 + 右侧竖杆 */
    int rr = r / 2;
    for (int dy = -rr; dy <= rr; dy++)
        for (int dx = -rr; dx <= rr; dx++)
            if (dx * dx + dy * dy <= rr * rr)
                fb_draw_point(cx - rr + dx, cy + rr + dy, color);
    fb_fill_rect(cx + rr - 2, cy - r + 2, 4, r + rr, color);
}

void fb_icon_film(int cx, int cy, int r, int color)
{
    int w = 2 * r, h = r + r / 2;
    int x0 = cx - r, y0 = cy - h / 2;
    fb_draw_rect(x0, y0, w, h, color);
    for (int j = y0 + 6; j < y0 + h - 8; j += 10) {   /* 两侧胶片孔 */
        fb_fill_rect(x0 + 5, j, 5, 5, color);
        fb_fill_rect(x0 + w - 10, j, 5, 5, color);
    }
}

void fb_icon_back(int cx, int cy, int r, int color)
{
    tri_left(cx + r / 3, cy, r, color);
    fb_fill_rect(cx - r, cy - 3, r + r / 3, 6, color);
}

/* ---------------- 文字（ASCII 8x16，font16.c 点阵） ---------------- */

void fb_draw_char(int x, int y, char ch, int color)
{
    if (ch < 32 || ch > 126)
        ch = '?';
    const unsigned char *g = font16[(int)ch - 32];
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++)
            if (g[r] & (0x80 >> c))
                fb_draw_point(x + c, y + r, color);
}

void fb_draw_string(int x, int y, const char *s, int color)
{
    if (s == NULL)
        return;
    while (*s) {
        fb_draw_char(x, y, *s++, color);
        x += 8;
    }
}
