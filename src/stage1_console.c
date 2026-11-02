/*
 * stage1_console.c —— 阶段 1：控制台音乐播放器（教学版）
 * ============================================================
 * 这是你 mp3/mplayer2.c 的"修正版"，结构保持一样，方便对照。
 *
 * 对照原版的 3 个 bug（面试讲"我如何迭代自己代码"的素材）：
 *   bug1: scanf("%c", &c) 把按完命令的回车 '\n' 也当成一条命令
 *         → 修法：scanf(" %c", &c)，%c 前的空格让 scanf 跳过空白符
 *   bug2: system("killall -9 mplayer") 会杀掉系统里所有 mplayer，
 *         且 -9 连"保存状态收尾"的机会都不给
 *         → 修法：向 FIFO 发 quit 让 mplayer 自己退，wait 回收
 *   bug3: 'q' 退出时直接 exit(1)，子进程没人收 → 僵尸进程
 *         → 修法：退出前 wait(NULL)
 *
 * 板子上编译产物运行；VM 里调试把 MPLAYER_BIN 编译成 "mplayer"。
 */
#include <common.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *mp3[] = {
    "huluwa.mp3", "lanjingling.mp3", "liangzhilaohu.mp3", "qifengle.mp3",
};
#define N (int)(sizeof(mp3) / sizeof(mp3[0]))

int main(void)
{
    /* 建 FIFO（已存在不算错）+ O_RDWR 常开（写永不阻塞） */
    if (mkfifo(FIFO_PATH, 0664) == -1 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }
    int fd = open(FIFO_PATH, O_RDWR);
    if (fd == -1) {
        perror("open fifo");
        exit(1);
    }

    int i = 0;
    while (1) {
        pid_t pid = fork();
        if (pid == 0) {                     /* 子进程：变成 mplayer */
            char path[160];
            snprintf(path, sizeof(path), MEDIA_DIR "/%s", mp3[i]);
            execlp(MPLAYER_BIN, MPLAYER_BIN, "-slave", "-quiet",
                   "-input", "file=" FIFO_PATH, path, (char *)NULL);
            perror("execlp");               /* exec 失败才会到这 */
            _exit(127);
        }

        /* 父进程：收键盘命令 */
        printf("第 %d/%d 首: %s\n", i + 1, N, mp3[i]);
        printf("命令: n=下一首  p=上一首  c=暂停/继续  q=退出\n> ");

        char c;
        int switch_song = 0;
        while (scanf(" %c", &c) == 1) {     /* bug1 修复: " %c" 跳过回车 */
            if (c == 'n') { i = (i + 1) % N; switch_song = 1; break; }
            if (c == 'p') { i = (i - 1 + N) % N; switch_song = 1; break; }
            if (c == 'c') {                 /* 暂停/继续：同一指令，天然同一键 */
                if (write(fd, "pause\n", 6) == -1)
                    perror("write fifo");
            }
            if (c == 'q') {                 /* bug3 修复: 退出前回收 */
                if (write(fd, "quit\n", 5) == -1)
                    perror("write fifo");
                wait(NULL);
                close(fd);
                printf("bye\n");
                return 0;
            }
        }

        if (switch_song) {                  /* bug2 修复: 不再 killall -9 */
            if (write(fd, "quit\n", 5) == -1)
                perror("write fifo");
            wait(NULL);                     /* 等 mplayer 退干净再开下一首 */
        }
    }
}
