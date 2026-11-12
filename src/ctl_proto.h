#ifndef CTL_PROTO_H
#define CTL_PROTO_H

/*
 * ctl_proto.h —— 控制面协议定义（UNIX / TCP 共用）
 *
 * 行协议：一行一条命令，\n 结尾，最长 CTL_LINE_MAX 字节。
 * OK 只表示"已受理"；需要执行结果用 status 二次确认。
 */

#define CTL_LINE_MAX     256
#define CTL_CMD_QUEUE    32
#define CTL_DONE_QUEUE   64
#define CTL_TICK_BATCH   16

typedef enum {
    CTL_PLAY = 0,
    CTL_PAUSE,
    CTL_TOGGLE,
    CTL_STOP,
    CTL_NEXT,
    CTL_PREV,
    CTL_SEEK,
    CTL_PLAY_INDEX,
    CTL_VOL,
    CTL_STATUS,
    CTL_LIST
} ctl_cmd_id_t;

/* 完成事件按传输通道分队列，避免 UNIX/TCP 两个 IO 线程互相吃掉响应 */
typedef enum {
    CTL_TRANSPORT_UNIX = 0,
    CTL_TRANSPORT_TCP  = 1,
    CTL_TRANSPORT_MAX
} ctl_transport_t;

/* 错误码：不套 Modbus（文本协议没有"非法地址"概念） */
enum {
    CTL_OK        = 0,
    CTL_E_SYNTAX  = 1,   /* 语法错误 / 行长超限 */
    CTL_E_ARG     = 2,   /* 参数错误 */
    CTL_E_RANGE   = 3,   /* 越界（音量/序号/seek） */
    CTL_E_UNKNOWN = 4,   /* 未知命令 */
    CTL_E_EXEC    = 5,   /* 执行失败（mplayer 未运行等） */
    CTL_E_BUSY    = 6    /* 队列满 */
};

#endif
