#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

/*
 * modbus_slave.h —— Modbus-RTU 接入层（FIFO 线程 + 命令邮箱）
 *
 * 分工：
 *   协议线程：读 FIFO → 判帧 → mbrtu_parse → 写 FIFO；只读快照、只入队命令
 *   主线程  ：ui_ticker 里取命令并执行（避免协议线程直接操作 UI 状态）
 */

/* 启动从站线程（detached）。返回 0 成功，-1 失败 */
int mbrtu_slave_start(void);

/* 主线程调用：取一条待执行命令。
 * 返回 1 = 取到（*cmd 有效）；0 = 当前没有 */
int mbrtu_take_cmd(unsigned short *cmd);

/* 主线程执行完命令后回写错误码寄存器：
 * 0 正常 / 1 mplayer 未运行 / 2 播放失败 */
void mbrtu_set_last_error(unsigned short err);

#endif
