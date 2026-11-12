#ifndef CTL_UNIX_H
#define CTL_UNIX_H

/* 启动 UNIX 域控制 socket（/run/media_player.sock，失败回退 /tmp）。
 * 成功返回 0；已有实例或创建失败返回 -1（不阻塞播放器主功能）。 */
int ctl_unix_start(void);
void ctl_unix_stop(void);

#endif
