/*
 * touch.c —— 触摸屏读取实现
 * ============================================================
 * input_event 大白话：触摸屏驱动不停往外吐"事件"，
 * 每个事件三件套 {type, code, value}：
 *   type=EV_ABS(3)  code=ABS_X(0)/ABS_Y(1)  value=坐标
 *   type=EV_KEY(1)  code=BTN_TOUCH(330)     value=1按下/0抬起
 * 一次点击 = 一串坐标事件 + 一个"抬起"事件，
 * 所以我们攒坐标，等"抬起"了才返回 —— 这就是一次完整的 touch_read。
 * 兼容多点触摸协议：ABS_MT_POSITION_X/Y(53/54) 也一并接收。
 * ============================================================
 */
#include "touch.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <unistd.h>

static int g_fd = -1;

int touch_init(const char *dev)
{
    if (dev == NULL)
        dev = "/dev/input/event6";   /* 2026-11-02 实测 RK356X: Goodix 触摸 = event6 */
    g_fd = open(dev, O_RDONLY);
    if (g_fd == -1) {
        perror("open touch");
        return -1;
    }
    return 0;
}

int touch_read(int *x, int *y)
{
    struct input_event ev;
    int cur_x = -1, cur_y = -1;
    int pressed = 0;

    for (;;) {
        if (read(g_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
            return -1;               /* 设备出错/被关闭 */

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)
                cur_x = ev.value;
            else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)
                cur_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1) {
                pressed = 1;
            } else if (ev.value == 0 && pressed && cur_x >= 0 && cur_y >= 0) {
                *x = cur_x;          /* 抬起瞬间锁定坐标 = 用户点击位置 */
                *y = cur_y;
                return 0;
            }
        }
        /* 其他事件（EV_SYN 等）直接忽略 */
    }
}

int touch_fd(void)
{
    return g_fd;
}

void touch_deinit(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
