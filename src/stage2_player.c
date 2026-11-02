/*
 * stage2_player.c —— 阶段 2：分层版控制台播放器
 * ============================================================
 * 相比 stage1 的升级（都是面试可讲的点）：
 *   1) mplayer 管理收进"控制层" mplayer_ctl.c —— 主程序不再碰 FIFO/fork
 *   2) select() 做 I/O 多路复用：键盘 0.5s 没输入也醒一次，
 *      用来看"这歌是不是自然播完了"→ 自动连播、刷新进度
 *   3) 进度 = 读线程解析 ANS_TIME_POSITION（在 mplayer_ctl.c 里）
 * VM 里调试: gcc -DMPLAYER_BIN='"mplayer"' （build.sh 已配好）
 */
#include <mplayer_ctl.h>

#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* printf 立即出，\r 刷新才看得到 */

    mp_subsys_init();                   /* 建 FIFO、装 SIGCHLD、起定时线程 */
    mp_start(g_songs[g_song_cur], 0);

    printf("=== stage2 控制台播放器 ===\n"
           "n=下一首  p=上一首  c=暂停/继续  +=音量  -=音量  q=退出\n");

    while (1) {
        /* select：0.5s 内键盘有输入就处理，没输入也醒来做巡检 */
        fd_set rfds;
        struct timeval tv = {0, 500 * 1000};
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

        if (r > 0) {                    /* 键盘有事 */
            char c;
            if (scanf(" %c", &c) != 1)
                continue;
            switch (c) {
            case 'n': mp_song_next();     break;
            case 'p': mp_song_prev();     break;
            case 'c': mp_pause_toggle();  break;
            case '+': mp_volume(+10);     break;
            case '-': mp_volume(-10);     break;
            case 'q': mp_subsys_deinit(); return 0;
            default:                      break;
            }
            printf("\n当前第 %d/%d 首\n> ", g_song_cur + 1, g_song_num);
        }

        /* 巡检：自然播完 → 自动连播（SIGCHLD 置的标志） */
        if (mp_song_finished()) {
            printf("\n播放完毕，自动下一首\n");
            mp_song_next();
        }

        /* 巡检：刷新进度（\r 回行首覆盖打印） */
        if (mp_state() == ST_PLAYING || mp_state() == ST_PAUSED)
            printf("\r[%3d/%3ds] %s   ",
                   mp_cur_sec(), mp_total_sec(),
                   mp_state() == ST_PAUSED ? "暂停" : "播放中");
    }
}
