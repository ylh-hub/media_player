#ifndef TOUCH_H
#define TOUCH_H
/*
 * touch.h —— 触摸屏读取
 * Linux 触摸屏就是字符设备 /dev/input/eventX，
 * read 出来的是 struct input_event 事件流。
 * 具体是 event 几、坐标范围多少，用 touch_test 先探测（阶段 3 第一步）。
 */

/* 打开触摸设备；dev 传 NULL 用默认 "/dev/input/event3"。
 * 返回 0 成功。成功后可用 touch_fd() 拿 fd 给 select 用 */
int  touch_init(const char *dev);

/* 阻塞直到完成一次"点击"（按下到抬起），把抬起时的坐标写入 *x 和 *y。
 * 返回 0 成功；设备出错返回 -1。滑动会被忽略（只认点按） */
int  touch_read(int *x, int *y);

/* 手势版：一次"按下→移动→抬起"分类成点按或左右滑动。
 * 返回 TOUCH_TAP / TOUCH_SWIPE_LEFT / TOUCH_SWIPE_RIGHT；设备出错 -1。
 * *x,*y = 抬起坐标；*dx = 水平位移（像素，左滑为负）。
 * 判定阈值 60px：小于它算点按（手抖不算滑动） */
enum {
    TOUCH_TAP = 0,
    TOUCH_SWIPE_LEFT,
    TOUCH_SWIPE_RIGHT
};
int  touch_read_gesture(int *x, int *y, int *dx);

/* 返回触摸设备 fd（供 select/轮询），未初始化返回 -1 */
int  touch_fd(void);

void touch_deinit(void);

#endif
