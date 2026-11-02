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
 * 返回 0 成功；设备出错返回 -1 */
int  touch_read(int *x, int *y);

/* 返回触摸设备 fd（供 select/轮询），未初始化返回 -1 */
int  touch_fd(void);

void touch_deinit(void);

#endif
