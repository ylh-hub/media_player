/*
 * mplayer_ctl.c —— 控制层实现
 * ============================================================
 * 职责：把"启动播放器 / 发指令 / 回收子进程 / 进度获取"全部收在这里。
 * 上层只调 mplayer_ctl.h 里的函数，不知道 mplayer 的存在细节。
 *
 * 设计决策（面试高频）：
 * 1) fork + execl 而不是 system()
 *    system() = fork + sh -c + 内部还套一层 fork，多两层进程；
 *    而且 shell 拼接文件名有注入/转义问题，信号处理时 system 也有坑。
 * 2) 切歌不发 killall -9
 *    killall 会把系统里所有 mplayer 全杀掉（误伤），也不给 mplayer
 *    保存状态的机会。标准做法：先发 quit 让它自己退，等 300ms，
 *    没退才 SIGKILL 兜底，最后 waitpid 回收防僵尸。
 * 3) SIGCHLD 处理函数里只做两件事：waitpid 回收 + 置标志。
 *    处理函数里能安全调用的函数很少（async-signal-safe），
 *    复杂逻辑（比如自动切歌）交给主循环查标志位。
 * 4) 进度获取：mplayer 对 get_time_pos 的应答（ANS_TIME_POSITION=x）
 *    打印在它的 stdout 上。我们把子进程 stdout 接到 pipe，
 *    用一个读线程逐字节 read 自己拼行（绕开 stdio 缓冲），
 *    解析后加锁存入全局变量。
 * 5) 总时长不依赖应答：启动前单独跑一次
 *      mplayer64 -identify -frames 0 -ao null -vo null 文件
 *    这是一次性进程，退出时缓冲必然刷新，从输出里抓 ID_LENGTH。
 * ============================================================
 */
#include "mplayer_ctl.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ---------------- 内部状态 ---------------- */
static volatile sig_atomic_t g_pid = -1;      /* mplayer 子进程 pid，-1=没有 */
static int    g_fifo_fd  = -1;                /* FIFO 的 fd（O_RDWR 常开） */
static int    g_state    = ST_STOPPED;        /* 播放器状态机 */
static volatile sig_atomic_t g_song_done = 0; /* 自然播完标志（SIGCHLD 置位） */
/* 代际号：每次成功启动一轮播放就 +1。
 * SIGCHLD 只记"哪一代"播完了；上层查询时若代际对不上，说明是上一轮
 * 停旧歌时留下的回调，直接丢弃——这是"视频刚进就退"竞态的根因修复。 */
static volatile sig_atomic_t g_gen = 0;
static volatile sig_atomic_t g_done_gen = 0;
static volatile sig_atomic_t g_ready = 0;     /* mplayer 就绪标志（看到 Starting playback 才置 1） */

static int    g_volume   = 70;                /* 当前音量 0~100 */

/* 状态量互斥锁：g_state / g_volume / g_song_cur / g_song_num 会被
 * UI 线程写、协议线程读，用这把锁保护，避免形式上的 data race。
 * 注意：g_song_num 只在 mp_scan_media()（init 阶段、协议线程启动前）写，
 * 以后若加"运行时刷新列表"，也要在这把锁里改。 */
static pthread_mutex_t g_ctl_lock = PTHREAD_MUTEX_INITIALIZER;

/* 进度共享数据：读线程写，主线程读，用互斥锁保护 */
static pthread_mutex_t g_pos_lock = PTHREAD_MUTEX_INITIALIZER;
static float  g_cur_sec   = 0.0f;
static float  g_total_sec = 0.0f;
static volatile int g_pipe_fd = -1;           /* mplayer stdout 的读端 */

/* ---------------- 播放列表 ----------------
 * 文件路径的实际存储区；g_songs/g_videos 只是指针数组，
 * 由 mp_scan_media() 在启动时扫描 MEDIA_DIR 自动填充。
 * （这就是指导书里的 readdir 加分项——文件名随便起，不用改代码）
 */
static char g_song_path[SONG_MAX][320];
static char g_video_path[VIDEO_MAX][320];

char *g_songs[SONG_MAX];
int g_song_num = 0;
int g_song_cur = 0;

char *g_videos[VIDEO_MAX];
int g_video_num = 0;
int g_video_cur = 0;

/* 忽略大小写的后缀匹配：视频 .mp4/.flv/.avi/.mkv，音频 .mp3/.wav/.flac */
static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lu = strlen(suffix);
    if (ls < lu)
        return 0;
    s += ls - lu;
    while (*suffix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*suffix))
            return 0;
        s++;
        suffix++;
    }
    return 1;
}

static int is_video_name(const char *n)
{
    return ends_with_ci(n, ".mp4") || ends_with_ci(n, ".flv") ||
           ends_with_ci(n, ".avi") || ends_with_ci(n, ".mkv") ||
           ends_with_ci(n, ".mov") || ends_with_ci(n, ".rmvb");
}

static int is_audio_name(const char *n)
{
    return ends_with_ci(n, ".mp3") || ends_with_ci(n, ".wav") ||
           ends_with_ci(n, ".flac") || ends_with_ci(n, ".wma");
}

/* qsort 比较器：按完整路径字典序，保证每次开机列表顺序一致 */
static int cmp_pstr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void mp_scan_media(void)
{
    g_song_num = 0;
    g_video_num = 0;
    g_song_cur = 0;
    g_video_cur = 0;

    DIR *d = opendir(MEDIA_DIR);
    if (d == NULL) {
        perror("mp_scan_media: opendir " MEDIA_DIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;                       /* 跳过 . .. 和隐藏文件 */
        if (is_video_name(e->d_name) && g_video_num < VIDEO_MAX) {
            /* 先取下标再自增：同一表达式里"自增"和"取值"未定序（UB），
             * 之前会导致列表错位、末项空串 */
            int i = g_video_num;
            snprintf(g_video_path[i], sizeof(g_video_path[0]),
                     MEDIA_DIR "/%s", e->d_name);
            g_videos[i] = g_video_path[i];
            g_video_num = i + 1;
        } else if (is_audio_name(e->d_name) && g_song_num < SONG_MAX) {
            int i = g_song_num;
            snprintf(g_song_path[i], sizeof(g_song_path[0]),
                     MEDIA_DIR "/%s", e->d_name);
            g_songs[i] = g_song_path[i];
            g_song_num = i + 1;
        }
    }
    closedir(d);
    qsort(g_songs,  g_song_num,  sizeof(char *), cmp_pstr);
    qsort(g_videos, g_video_num, sizeof(char *), cmp_pstr);
    printf("scan " MEDIA_DIR ": %d songs, %d videos\n", g_song_num, g_video_num);
}

/* ---------------- SIGCHLD：最小安全处理 ---------------- */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved = errno;              /* 信号可能打断系统调用，先存 errno */
    pid_t p;
    /* 循环回收：可能有多个子进程同时死；被信号打断(EINTR)就再来一次 */
    do {
        p = waitpid(-1, NULL, WNOHANG);
        if (p > 0 && p == (pid_t)g_pid) {   /* 只有 mplayer 死了才算"歌播完了" */
            g_pid = -1;
            g_done_gen = g_gen;             /* 记下这是哪一代的结束事件 */
            g_song_done = 1;
        }
    } while (p > 0 || (p == -1 && errno == EINTR));
    errno = saved;
}

/* ---------------- 进度读线程 ----------------
 * 批量 read（512B 一批）而不是逐字节：
 * 系统调用次数少一个数量级；行内解析放到用户态做，效果一样。
 * 顺带做"启动握手"：看到 "Starting playback" 才认为 mplayer
 * 真正开始干活，之前的指令它可能还没监听，不发。
 */
static void *reader_thread(void *arg)
{
    int fd = (int)(long)arg;        /* 管道读端由参数传入 */
    char buf[512];
    char line[512];
    int  len = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r')
                continue;
            if (ch != '\n' && len < (int)sizeof(line) - 1) {
                line[len++] = ch;
                continue;
            }
            /* 攒够了一行（超长行走到这里会被截断丢弃，不会撑爆） */
            line[len] = '\0';
            len = 0;
            if (strstr(line, "Starting playback") != NULL) {
                g_ready = 1;        /* 握手成功 */
            } else if (strncmp(line, "ANS_LENGTH=", 11) == 0) {
                pthread_mutex_lock(&g_pos_lock);
                g_total_sec = atof(line + 11);
                pthread_mutex_unlock(&g_pos_lock);
            } else if (strncmp(line, "ANS_TIME_POSITION=", 18) == 0) {
                pthread_mutex_lock(&g_pos_lock);
                g_cur_sec = atof(line + 18);
                pthread_mutex_unlock(&g_pos_lock);
            }
        }
    }
    close(fd);                      /* 读线程自己关自己的 fd，避免复用冲突 */
    if (g_pipe_fd == fd)
        g_pipe_fd = -1;
    return NULL;
}

/* ---------------- 定时线程：每 0.5s 问一次进度 ---------------- */
static void *ticker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (mp_state() == ST_PLAYING && g_pid > 0)
            mp_send("get_time_pos\n");
    }
    return NULL;                    /* 不会走到这里 */
}

/* ---------------- 预取总时长 ---------------- */
static float probe_total_sec(const char *path)
{
    int fds[2];
    if (pipe(fds) == -1)
        return 0.0f;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        /* -frames 0：不解码就退出，只打印文件信息，很快 */
        execlp(MPLAYER_BIN, MPLAYER_BIN, "-identify", "-frames", "0",
               "-ao", "null", "-vo", "null", path, (char *)NULL);
        _exit(127);                 /* execlp 失败才会到这里 */
    }
    close(fds[1]);

    /* 把输出全读下来再找 ID_LENGTH=，避免"关键词跨缓冲区被截断" */
    static char acc[65536];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof(acc) - 1 &&
           (n = read(fds[0], acc + got, sizeof(acc) - 1 - got)) > 0)
        got += (size_t)n;
    acc[got] = '\0';
    close(fds[0]);
    waitpid(pid, NULL, 0);

    char *p = strstr(acc, "ID_LENGTH=");
    return p ? atof(p + 10) : 0.0f;
}

/* ---------------- 对外接口 ---------------- */

void mp_send(const char *cmd)
{
    if (g_fifo_fd < 0 || cmd == NULL)
        return;
    /* 未就绪（还没看到 Starting playback）时只放行 quit：
     * 过早发 pause/volume 可能被 mplayer 丢掉，不如不发 */
    if (!g_ready && strncmp(cmd, "quit", 4) != 0)
        return;
    if (write(g_fifo_fd, cmd, strlen(cmd)) == -1)
        perror("mp_send: write fifo");
}

int mp_start(const char *path, int is_video)
{
    if (path == NULL || access(path, R_OK) == -1) {
        fprintf(stderr, "mp_start: 文件不存在或不可读: %s\n", path);
        return -1;
    }
    if (g_pid > 0)
        mp_stop();                  /* 先停旧的 */

    /* 等上一轮的读线程退出（它退出时会把自己 fd 关掉并置 -1），
     * 防止旧线程读到新管道造成解析错乱 */
    for (int i = 0; i < 30 && g_pipe_fd >= 0; i++)
        usleep(10 * 1000);

    int fds[2];
    if (pipe(fds) == -1) {
        perror("mp_start: pipe");
        return -1;
    }

    /* fork 前屏蔽 SIGCHLD：防止子进程秒死时 handler 抢在 g_pid 赋值前跑 */
    sigset_t mask, old;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, &old);

    pid_t pid = fork();
    if (pid == 0) {                 /* ---- 子进程 ---- */
        dup2(fds[1], STDOUT_FILENO);/* stdout -> pipe，应答从这读 */
        close(fds[0]);
        close(fds[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {              /* stderr 丢弃，减少解析噪音 */
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        if (is_video)
            execlp(MPLAYER_BIN, MPLAYER_BIN, "-vo", "fbdev2",
                   "-slave", "-quiet", "-input", "file=" FIFO_PATH,
                   "-geometry", "0:0", "-zoom",
                   "-x", "1024", "-y", "600", path, (char *)NULL);
        else
            execlp(MPLAYER_BIN, MPLAYER_BIN, "-vo", "null",
                   "-slave", "-quiet", "-input", "file=" FIFO_PATH,
                   path, (char *)NULL);
        _exit(127);
    }

    /* ---- 父进程 ---- */
    close(fds[1]);                  /* 父进程只用读端 */
    g_pipe_fd   = fds[0];
    g_pid       = (sig_atomic_t)pid;
    g_gen++;                        /* 新一代开始：上一轮的结束事件自动作废 */
    g_done_gen  = 0;
    g_song_done = 0;
    g_ready     = 0;                /* 等读线程看到 Starting playback 再置 1 */
    pthread_mutex_lock(&g_ctl_lock);
    g_state     = ST_PLAYING;
    pthread_mutex_unlock(&g_ctl_lock);
    pthread_sigmask(SIG_SETMASK, &old, NULL);   /* 恢复信号屏蔽 */

    pthread_t tid;
    pthread_create(&tid, NULL, reader_thread, (void *)(long)fds[0]);
    pthread_detach(tid);            /* 读线程收尾自清理，不需要 join */

    /* 总时长：identify 预取（ANS_LENGTH 作为运行中的补充） */
    float t = probe_total_sec(path);
    pthread_mutex_lock(&g_pos_lock);
    g_total_sec = t;
    g_cur_sec   = 0.0f;
    pthread_mutex_unlock(&g_pos_lock);

    return 0;
}

void mp_stop(void)
{
    if (g_pid <= 0)
        return;
    pid_t pid = (pid_t)g_pid;

    /* 三步退场（DeepSeek 评审结论：直接 SIGKILL 不给 mplayer 释放
     * 音频设备/fb 状态的机会，下一首可能 device busy 或花屏）：
     * 1) quit   —— 最体面，mplayer 自己收尾
     * 2) SIGTERM —— 可被捕获，mplayer 走清理路径
     * 3) SIGKILL —— 最后兜底 */
    mp_send("quit\n");
    for (int i = 0; i < 30 && g_pid > 0; i++)   /* 最多等 300ms */
        usleep(10 * 1000);

    if (g_pid > 0) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30 && g_pid > 0; i++)
            usleep(10 * 1000);
    }
    if (g_pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);      /* 兜底路径自己回收（handler 可能已回收，无害） */
        g_pid = -1;
    }
    pthread_mutex_lock(&g_ctl_lock);
    g_state = ST_STOPPED;
    pthread_mutex_unlock(&g_ctl_lock);
}

void mp_pause_toggle(void)
{
    /* mplayer 的 pause 指令本身就是"播着就暂停、暂停着就继续"，
     * 程序侧只需要同步维护状态机 */
    int st;

    pthread_mutex_lock(&g_ctl_lock);
    st = g_state;
    if (st == ST_PLAYING)
        g_state = ST_PAUSED;
    else if (st == ST_PAUSED)
        g_state = ST_PLAYING;
    pthread_mutex_unlock(&g_ctl_lock);

    if (st == ST_PLAYING || st == ST_PAUSED)
        mp_send("pause\n");
}

void mp_song_next(void)
{
    int n;

    pthread_mutex_lock(&g_ctl_lock);
    if (g_song_num == 0) {
        pthread_mutex_unlock(&g_ctl_lock);
        return;
    }
    g_song_cur = (g_song_cur + 1) % g_song_num;
    n = g_song_cur;
    pthread_mutex_unlock(&g_ctl_lock);

    mp_start(g_songs[n], 0);
}

void mp_song_prev(void)
{
    int n;

    pthread_mutex_lock(&g_ctl_lock);
    if (g_song_num == 0) {
        pthread_mutex_unlock(&g_ctl_lock);
        return;
    }
    g_song_cur = (g_song_cur - 1 + g_song_num) % g_song_num;
    n = g_song_cur;
    pthread_mutex_unlock(&g_ctl_lock);

    mp_start(g_songs[n], 0);
}

void mp_volume(int delta)
{
    int v;
    char cmd[32];

    pthread_mutex_lock(&g_ctl_lock);
    g_volume += delta;
    if (g_volume < 0)   g_volume = 0;
    if (g_volume > 100) g_volume = 100;
    v = g_volume;
    pthread_mutex_unlock(&g_ctl_lock);

    snprintf(cmd, sizeof(cmd), "volume %d 1\n", v);
    mp_send(cmd);
}

int mp_volume_get(void)             /* 当前音量值（画音量条用） */
{
    int v;

    pthread_mutex_lock(&g_ctl_lock);
    v = g_volume;
    pthread_mutex_unlock(&g_ctl_lock);
    return v;
}

void mp_volume_set(int v)
{
    char cmd[32];

    if (v < 0)   v = 0;
    if (v > 100) v = 100;

    pthread_mutex_lock(&g_ctl_lock);
    g_volume = v;
    pthread_mutex_unlock(&g_ctl_lock);

    snprintf(cmd, sizeof(cmd), "volume %d 1\n", v);
    mp_send(cmd);
}

void mp_seek_percent(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "seek %d 2\n", pct);   /* 2 = 绝对百分比 */
    mp_send(cmd);
}

int mp_cur_sec(void)
{
    int v;
    pthread_mutex_lock(&g_pos_lock);
    v = (int)g_cur_sec;
    pthread_mutex_unlock(&g_pos_lock);
    return v;
}

int mp_total_sec(void)
{
    int v;
    pthread_mutex_lock(&g_pos_lock);
    v = (int)g_total_sec;
    pthread_mutex_unlock(&g_pos_lock);
    return v;
}

int mp_state(void)
{
    int st;

    pthread_mutex_lock(&g_ctl_lock);
    st = g_state;
    pthread_mutex_unlock(&g_ctl_lock);
    return st;
}

void mp_song_set_cur(int idx)
{
    pthread_mutex_lock(&g_ctl_lock);
    g_song_cur = idx;
    pthread_mutex_unlock(&g_ctl_lock);
}

void mp_get_snapshot(mp_snapshot_t *s)
{
    if (!s)
        return;

    /* 先取控制类状态，再取进度；两把锁不嵌套，避免死锁 */
    pthread_mutex_lock(&g_ctl_lock);
    s->state    = g_state;
    s->volume   = g_volume;
    s->song_cur = g_song_cur;
    s->song_num = g_song_num;
    pthread_mutex_unlock(&g_ctl_lock);

    pthread_mutex_lock(&g_pos_lock);
    s->cur_sec   = (int)g_cur_sec;
    s->total_sec = (int)g_total_sec;
    pthread_mutex_unlock(&g_pos_lock);
}
int mp_song_finished(void)
{
    if (!g_song_done)
        return 0;
    g_song_done = 0;            /* 查询即清零 */
    /* 代际校验：停旧歌时 handler 也会置位，但那一代的编号已经过期，
     * 不能再当成"当前这首播完"，否则 ticker 会误触发视频退出 */
    if (g_done_gen != g_gen)
        return 0;
    return 1;
}

/* 单实例保护：扫 /proc 杀掉残留的旧实例（media_player/stage3_ui/mplayer64），
 * 但绝不杀自己——busybox 的 `killall media_player` 会连自己一起杀（2026-11-03 实测自杀），
 * 所以必须按 pid 精确排除。 */
void mp_kill_stale(void)
{
    int mypid = getpid();
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;                       /* 只要数字目录（进程） */
        int pid = atoi(e->d_name);
        if (pid == mypid || pid <= 1)
            continue;                       /* 不杀自己、不杀 init */
        char path[320];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *fp = fopen(path, "r");
        if (fp == NULL)
            continue;
        char cmd[256];
        int n = (int)fread(cmd, 1, sizeof(cmd) - 1, fp);
        fclose(fp);
        if (n <= 0)
            continue;
        cmd[n] = '\0';
        if (strstr(cmd, "media_player") || strstr(cmd, "stage3_ui") ||
            strstr(cmd, "mplayer64"))
            kill(pid, SIGKILL);
    }
    closedir(d);
    usleep(300 * 1000);                     /* 给被杀进程一点收尸时间 */
}

int mp_subsys_init(void)
{
    /* 1. 建 FIFO（已存在不算错） */
    if (mkfifo(FIFO_PATH, 0664) == -1 && errno != EEXIST) {
        perror("mkfifo");
        return -1;
    }
    /* 2. O_RDWR 打开后常开：自己既当读端又当写端，
     *    write 永远不会因为没有读者而阻塞 */
    g_fifo_fd = open(FIFO_PATH, O_RDWR);
    if (g_fifo_fd == -1) {
        perror("open fifo");
        return -1;
    }
    /* 3. 装 SIGCHLD 处理函数 */
    signal(SIGCHLD, sigchld_handler);
    /* 4. 扫描媒体目录，自动生成歌单/视频列表 */
    mp_scan_media();
    /* 5. 启动进度定时线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, ticker_thread, NULL);
    pthread_detach(tid);
    return 0;
}

void mp_subsys_deinit(void)
{
    mp_stop();
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    unlink(FIFO_PATH);
}
