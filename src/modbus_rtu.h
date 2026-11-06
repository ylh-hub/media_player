#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>

/*
 * modbus_rtu.h —— Modbus-RTU 协议栈（纯逻辑，不碰串口/FIFO）
 *
 * 设计原则：这一层只认"字节数组"，不认文件描述符。
 * 好处：x86 上能单元测试；以后换 FIFO / 真串口，这一层一行不用改。
 */

/* ===== 一、本设备的固定参数 ===== */
#define MBRTU_SLAVE_ADDR     0x01   /* 本机站号 */
#define MBRTU_FC_READ        0x03   /* 功能码：读保持寄存器 */
#define MBRTU_FC_WRITE       0x06   /* 功能码：写单寄存器 */
#define MBRTU_MAX_ADU        256    /* RTU 最大帧长（规范） */
#define MBRTU_REG_COUNT      12     /* 本设备寄存器个数（0~11） */
#define MBRTU_REG_CMD        7      /* 命令寄存器 offset（广播不允许写） */
#define MBRTU_MAX_READ_REGS  125    /* 规范允许一次最多读 125 个 */

/* ===== 二、业务回调（把协议和播放器解耦） =====
 * 返回码约定（和异常码对齐）：
 *   0 成功
 *   2 非法地址（比如写只读寄存器）
 *   3 非法数据值（比如音量 >100、命令值越界）
 *   6 从站忙（命令槽还没被主线程消费）
 */
typedef struct {
    int (*read)(int offset, uint16_t *out);   /* 读一个寄存器，写进 *out */
    int (*write)(int offset, uint16_t val);   /* 写一个寄存器 */
} mbrtu_hooks;

void mbrtu_set_hooks(const mbrtu_hooks *h);

/* ===== 三、CRC16/Modbus =====
 * 多项式 0xA001（0x8005 反转），初值 0xFFFF
 * 返回 16 位值；组帧上线时先发低字节、再发高字节
 * 例：01 03 00 00 00 0A 的 CRC 值是 0xCDC5，线上是 C5 CD
 */
uint16_t mbrtu_crc16(const uint8_t *buf, int len);

/* ===== 四、解析一帧请求，直接产出响应 =====
 * 返回值：
 *   > 0  响应帧字节数（已含 CRC，可直接写到 FIFO/串口）
 *     0  静默丢弃（不是给我的 / CRC 错 / 广播 / 长度不对）
 *    -1  畸形帧或超长帧（<4 字节 或 >256 字节）
 *
 * 处理顺序（面试要能背）：
 *   长度 → 站号 → CRC → 广播 → 功能码 → 精确帧长 → 数量 → 地址 → 值
 */
int mbrtu_parse(const uint8_t *frame, int len, uint8_t *resp, int resp_max);

/* ===== 五、判帧间隔（3.5 字符时间，单位 ms） =====
 * 8N1 每字符 10 位：t3.5 = 3.5 × 10 / baud 秒
 *   9600  → 约 3.65
 *   19200 → 约 1.82
 *   >19200 → 规范固定 1.75
 * 注意：FIFO 注入只是模拟，真实 t3.5 要靠 UART idle 或硬件定时器
 */
double mbrtu_frame_gap_ms(int baud);

#endif
