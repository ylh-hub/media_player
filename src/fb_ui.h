#ifndef FB_UI_H
#define FB_UI_H
/*
 * fb_ui.h —— framebuffer 绘制层
 * framebuffer 是什么：/dev/fb0 是屏幕对应的"显存文件"，
 * 把它 mmap 进内存后，往里写一个 int 就是点亮一个像素。
 */
#include <common.h>

/* 主题色（深蓝配色，和你之前界面风格一致） */
#define C_BG      0x0A2C5E   /* 背景 */
#define C_CARD    0x0F3873   /* 卡片 */
#define C_HL      0x1B5AA6   /* 高亮 */
#define C_TXT     0xD6E6FB   /* 浅色文字 */
#define C_ACCENT  0x2E7CF6   /* 强调蓝 */
#define C_GREY    0x5A7BA6   /* 次要 */

int  fb_init(void);        /* 打开 /dev/fb0 + mmap，成功返回 0 */
void fb_deinit(void);

/* fb 状态保护（三方评审结论）：
 * mplayer 的 fbdev2 会改显示参数，被 SIGKILL 时不还原 → 花屏。
 * 播视频前 fb_save_state()，退出视频后 fb_restore_state() */
void fb_save_state(void);
void fb_restore_state(void);

/* 绘制前加锁（进度线程和主线程都会画，防写花） */
void fb_lock(void);
void fb_unlock(void);

void fb_draw_point(int x, int y, int color);
void fb_fill_rect(int x, int y, int w, int h, int color);
void fb_draw_rect(int x, int y, int w, int h, int color);  /* 空心框 */
void fb_clear(int color);

/* 图标按钮（纯代码绘制，不依赖图片资源和字库） */
void fb_icon_play (int cx, int cy, int r, int color); /* 播放：三角 */
void fb_icon_pause(int cx, int cy, int r, int color); /* 暂停：双竖条 */
void fb_icon_next (int cx, int cy, int r, int color); /* 下一首：三角+竖线 */
void fb_icon_prev (int cx, int cy, int r, int color); /* 上一首 */
void fb_icon_plus (int cx, int cy, int r, int color); /* 音量+ */
void fb_icon_minus(int cx, int cy, int r, int color); /* 音量- */
void fb_icon_music(int cx, int cy, int r, int color); /* 音乐：音符 */
void fb_icon_film (int cx, int cy, int r, int color); /* 视频 */
void fb_icon_back (int cx, int cy, int r, int color); /* 返回：左箭头 */

/* 文字：ASCII 8x16 点阵（数据在 font16.c，gen_font16.py 生成）。
 * 注意：中文显示不了（无中文字库），文件名用拼音。
 * 这俩是"画笔"不自带锁，调用方先 fb_lock() */
void fb_draw_char(int x, int y, char ch, int color);
void fb_draw_string(int x, int y, const char *s, int color);

/* ---- UI v2（网易云风格）----
 * 1) 垂直渐变背景（调用后记住两端色，fb_gradient_rect 可局部重画）
 * 2) 圆（实心/空心）
 * 3) 中英混排文字：ASCII 走 8x16 font16，中文走 16x16 font_cn */
void fb_gradient(int top, int bottom);
void fb_gradient_rect(int x, int y, int w, int h);
void fb_fill_circle(int cx, int cy, int r, int color);
void fb_draw_circle(int cx, int cy, int r, int color);
void fb_text(int x, int y, const char *utf8, int color, int scale);
int  fb_text_width(const char *utf8, int scale);

#endif
