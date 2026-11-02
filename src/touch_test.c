/*
 * touch_test.c —— 触摸探测工具（阶段 3 的第一个任务）
 * ============================================================
 * 用途：搞清楚两件事，后面 stage3/4 全靠它：
 *   1) 触摸屏到底是 /dev/input/event 几？
 *   2) 触摸原始坐标范围是多少？（有的屏 0~1024，有的 0~4096）
 * 用法（板子串口里）：
 *   cat /proc/bus/input/devices   ← 找 Name 带 "touch" 的那项，看 Handlers= eventX
 *   ./touch_test /dev/input/eventX
 * 然后手指在屏上点几下，观察打印：
 *   - ABS_X / ABS_Y 的 value 范围 → 决定要不要做坐标缩放
 *   - 每次点击结束会打印 == CLICK (x,y) ==
 */
#include <linux/input.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/input/event3";
    int fd = open(dev, O_RDONLY);
    if (fd == -1) {
        perror(dev);
        return 1;
    }
    printf("探测 %s ... 手指按下并抬起试试 (Ctrl+C 退出)\n", dev);

    struct input_event ev;
    int cur_x = -1, cur_y = -1, pressed = 0;

    for (;;) {
        if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
            break;
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                cur_x = ev.value;
                printf("  X = %d\n", ev.value);
            } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                cur_y = ev.value;
                printf("  Y = %d\n", ev.value);
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1) {
                pressed = 1;
                printf("  按下\n");
            } else if (ev.value == 0 && pressed) {
                pressed = 0;
                printf("== CLICK (%d, %d) ==\n", cur_x, cur_y);
            }
        } else {
            printf("  type=%d code=%d value=%d\n", ev.type, ev.code, ev.value);
        }
    }
    close(fd);
    return 0;
}
