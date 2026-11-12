#ifndef CTL_CORE_H
#define CTL_CORE_H

/*
 * ctl_core.h —— 控制面核心：命令队列 / 完成队列 / 状态文本
 *
 * 线程模型：
 *   网络线程 / Modbus 线程  -> ctl_push_cmd()
 *   GUI 线程                -> ctl_take_cmd() 执行 -> ctl_push_done()
 *   IO 线程                 -> ctl_take_done() 写回客户端
 *
 * 本文件不依赖 LVGL / mplayer，可单独 x86 单测。
 */

#include "ctl_proto.h"

typedef struct {
    ctl_cmd_id_t id;
    int arg;            /* vol / seek / 曲目序号 */
    unsigned seq;       /* 响应关联序号 */
    int conn_id;        /* 回给哪个连接；-1 = 本地/Modbus */
    int transport;      /* ctl_transport_t，决定完成事件回哪个队列 */
} ctl_cmd_t;

typedef struct {
    unsigned seq;
    int conn_id;
    int code;                    /* CTL_OK / CTL_E_* */
    char payload[CTL_LINE_MAX];  /* 完整响应行（含 \n 由发送方补） */
} ctl_done_t;

/* 命令队列：满返回 -1 */
int ctl_push_cmd(const ctl_cmd_t *c);
/* 取命令：返回实际取出条数（最多 max） */
int ctl_take_cmd(ctl_cmd_t *out, int max);

/* 完成队列（按 transport 分开）：满返回 -1 */
int ctl_push_done(int transport, const ctl_done_t *d);
/* 取完成事件：1 = 取到，0 = 空 */
int ctl_take_done(int transport, ctl_done_t *out);

/* 状态文本：GUI 线程 set，IO 线程 get 副本 */
void ctl_set_status(const char *text);
int  ctl_status_text(char *buf, int cap);

/*
 * 解析一行命令并入队。
 * transport: ctl_transport_t；conn_id 是该通道内的连接槽位。
 * 成功返回 CTL_OK；失败返回错误码（调用方直接回 ERR）。
 * quit 不在这里处理（由接入层关连接）。
 */
int ctl_parse_line(const char *line, int transport, int conn_id, unsigned seq,
                   char *err, int cap);

#endif
