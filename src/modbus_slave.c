#include "modbus_slave.h"
#include "modbus_rtu.h"
#include <mplayer_ctl.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * modbus_slave.c —— 接入层
 *
 * 线程模型（面试可讲）：
 *   协议线程：select 等 FIFO 数据 → 5ms 静默判帧 → 每帧刷新快照
 *             → mbrtu_parse → 把响应写回 FIFO
 *   主线程  ：ui_ticker 每 500ms 取一次命令邮箱，执行播放/暂停/切歌
 *
 * 为什么命令不在协议线程直接执行：
 *   mp_* 虽然能调，但切歌会动 g_song_cur、界面状态，集中到主线程更安全。
 *   协议线程只做"读快照 + 入队"，临界区只有几微秒。
 */

#define IN_FIFO       "/tmp/modbus_in"
#define OUT_FIFO      "/tmp/modbus_out"
#define FIFO_GAP_MS   5.0     /* FIFO 注入用 5ms；真实串口按波特率算 */
#define TICK_US       5000    /* select 唤醒周期 5ms */

/* ---------- 协议线程 <-> 主线程 的共享状态 ---------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int            g_cmd_pending = 0;
static unsigned short g_cmd         = 0;
static unsigned short g_accept_cnt  = 0;   /* 11 号：接受计数，模 65536 */
static unsigned short g_last_error  = 0;   /* 8 号：错误码 */

/* ---------- 每帧快照（只由协议线程访问，不需要锁） ---------- */
static uint16_t g_snap[MBRTU_REG_COUNT];

/* ---------- 读回调：全部从快照取，保证一帧内数据一致 ---------- */
static int h_read(int off, uint16_t *out)
{
    if (off < 0 || off >= MBRTU_REG_COUNT)
        return 2;
    *out = g_snap[off];
    return 0;
}

/* ---------- 写回调：只允许音量和命令 ---------- */
static int h_write(int off, uint16_t val)
{
    switch (off) {
    case 6:                         /* 音量：绝对值，0~100 */
        if (val > 100)
            return 3;
        mp_volume_set((int)val);
        return 0;

    case 7:                         /* 命令：单槽邮箱，满则 06 busy */
        if (val == 0 || val > 4)
            return 3;
        pthread_mutex_lock(&g_lock);
        if (g_cmd_pending) {
            pthread_mutex_unlock(&g_lock);
            return 6;
        }
        g_cmd_pending = 1;
        g_cmd = val;
        g_accept_cnt++;             /* unsigned short 自然回绕 */
        pthread_mutex_unlock(&g_lock);
        return 0;

    default:                        /* 其余寄存器只读 */
        return 2;
    }
}

static const mbrtu_hooks g_hooks = { h_read, h_write };

/* ---------- 每帧开始刷新快照 ---------- */
static void snap_refresh(void)
{
    struct timespec ts;
    uint32_t up;
    int v;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    up = (uint32_t)ts.tv_sec;

    g_snap[0] = 0x0100;                                   /* 协议版本 v1.0 */
    g_snap[1] = (uint16_t)mp_state();                     /* 0 停 1 播 2 暂停 */
    g_snap[2] = (g_song_cur < 0) ? 0xFFFF : (uint16_t)g_song_cur;
    g_snap[3] = (g_song_num < 0) ? 0      : (uint16_t)g_song_num;

    v = mp_cur_sec();   g_snap[4] = (v < 0) ? 0xFFFF : (uint16_t)v;
    v = mp_total_sec(); g_snap[5] = (v < 0) ? 0xFFFF : (uint16_t)v;

    g_snap[6] = (uint16_t)mp_volume_get();
    g_snap[7] = 0;                                        /* 命令寄存器读恒 0 */

    pthread_mutex_lock(&g_lock);
    g_snap[8]  = g_last_error;
    g_snap[9]  = (uint16_t)(up & 0xFFFF);                 /* uptime 低 16 位 */
    g_snap[10] = (uint16_t)((up >> 16) & 0xFFFF);         /* uptime 高 16 位 */
    g_snap[11] = g_accept_cnt;
    pthread_mutex_unlock(&g_lock);
}

/* ---------- FIFO 线程 ---------- */
static void *slave_thread(void *arg)
{
    uint8_t  buf[512];
    uint8_t  resp[512];
    int      blen = 0;
    int      in_fd = -1, out_fd = -1;
    struct timespec last;

    (void)arg;

    signal(SIGPIPE, SIG_IGN);        /* 对端断开时别把进程带走 */

    mkfifo(IN_FIFO, 0666);           /* 已存在（EEXIST）直接忽略 */
    mkfifo(OUT_FIFO, 0666);

    /* O_RDWR 打开 FIFO：自己既是读者也是写者。
     * 这样对端断开不会 read()==0 忙等，也不会写端 ENXIO。 */
    in_fd  = open(IN_FIFO, O_RDWR | O_NONBLOCK);
    out_fd = open(OUT_FIFO, O_RDWR | O_NONBLOCK);
    if (in_fd < 0) {
        fprintf(stderr, "[modbus] open %s failed: %s\n", IN_FIFO, strerror(errno));
        return NULL;
    }

    mbrtu_set_hooks(&g_hooks);
    clock_gettime(CLOCK_MONOTONIC, &last);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int r;

        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = TICK_US;

        r = select(in_fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(in_fd, &rfds)) {
            int n = (int)read(in_fd, buf + blen, sizeof(buf) - (size_t)blen);
            if (n > 0) {
                blen += n;
                clock_gettime(CLOCK_MONOTONIC, &last);

                /* 积压超过一帧，先按 8 字节切，避免粘包丢帧 */
                while (blen >= 8) {
                    int rl;
                    snap_refresh();
                    rl = mbrtu_parse(buf, 8, resp, sizeof(resp));
                    if (rl > 0 && out_fd >= 0)
                        (void)write(out_fd, resp, (size_t)rl);
                    if (blen > 8)
                        memmove(buf, buf + 8, (size_t)(blen - 8));
                    blen -= 8;
                }
            }
        }

        /* 静默判帧：剩余不足 8 字节的，等 5ms 没新字节就按畸形处理 */
        if (blen > 0) {
            struct timespec now;
            double dt;

            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (now.tv_sec - last.tv_sec) * 1000.0
               + (now.tv_nsec - last.tv_nsec) / 1000000.0;

            if (dt >= FIFO_GAP_MS) {
                int rl;
                snap_refresh();
                rl = mbrtu_parse(buf, blen, resp, sizeof(resp));
                if (rl > 0 && out_fd >= 0)
                    (void)write(out_fd, resp, (size_t)rl);
                blen = 0;
            }
        }
    }
    return NULL;
}

int mbrtu_slave_start(void)
{
    pthread_t tid;

    if (pthread_create(&tid, NULL, slave_thread, NULL) != 0)
        return -1;
    pthread_detach(tid);
    return 0;
}

int mbrtu_take_cmd(unsigned short *cmd)
{
    int got = 0;

    pthread_mutex_lock(&g_lock);
    if (g_cmd_pending) {
        *cmd = g_cmd;
        g_cmd_pending = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

void mbrtu_set_last_error(unsigned short err)
{
    pthread_mutex_lock(&g_lock);
    g_last_error = err;
    pthread_mutex_unlock(&g_lock);
}
