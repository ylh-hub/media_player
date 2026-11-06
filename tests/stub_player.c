/*
 * stub_player.c —— 给 tests/test_fifo.c 用的播放器桩
 * 只实现 modbus_slave.c 真正引用到的几个函数和全局量，
 * 目的是让 FIFO 接入层能在 x86 上单独跑起来（不依赖 fb/触摸/mplayer）。
 */
#include <mplayer_ctl.h>

char *g_songs[SONG_MAX];
int   g_song_num = 0;
int   g_song_cur = 0;

char *g_videos[VIDEO_MAX];
int   g_video_num = 0;
int   g_video_cur = 0;

static int g_vol   = 30;
static int g_state = ST_STOPPED;

int  mp_state(void)        { return g_state; }
void mp_pause_toggle(void) { g_state = (g_state == ST_PLAYING) ? ST_PAUSED : ST_PLAYING; }
void mp_volume_set(int v)  { if (v < 0) v = 0; if (v > 100) v = 100; g_vol = v; }
int  mp_volume_get(void)   { return g_vol; }
int  mp_cur_sec(void)      { return 10; }
int  mp_total_sec(void)    { return 100; }

/* modbus_slave.c 现在只调这个快照接口 */
void mp_get_snapshot(mp_snapshot_t *s)
{
    if (!s) return;
    s->state     = g_state;
    s->volume    = g_vol;
    s->song_cur  = g_song_cur;
    s->song_num  = g_song_num;
    s->cur_sec   = 10;
    s->total_sec = 100;
}

void mp_song_set_cur(int idx) { g_song_cur = idx; }
