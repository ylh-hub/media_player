#ifndef CTL_TCP_H
#define CTL_TCP_H

/* 启动 TCP 控制服务（0.0.0.0:19000，epoll 单线程，多客户端）。
 * 成功返回 0；绑定失败返回 -1（不阻塞播放器主功能）。 */
int ctl_tcp_start(void);
void ctl_tcp_stop(void);

#endif
