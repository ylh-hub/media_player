#include "modbus_rtu.h"
#include <string.h>

/*
 * modbus_rtu.c —— Modbus-RTU 从站协议栈
 *
 * 这个文件只做三件事：
 *   1) 算 CRC
 *   2) 判一帧是不是发给我的、有没有被传坏
 *   3) 按 03/06 组响应，数据从回调拿
 * 它不知道播放器、不知道 FIFO、不知道串口 —— 所以能单独测。
 */

static mbrtu_hooks g_hooks;   /* 默认全 0（没注册回调时按"全 0 寄存器"处理） */

void mbrtu_set_hooks(const mbrtu_hooks *h)
{
    if (h)
        g_hooks = *h;
    else
        memset(&g_hooks, 0, sizeof(g_hooks));
}

/* ============================ CRC ============================ */

uint16_t mbrtu_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x0001)
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* 在 buf[0..n-1] 后面补 CRC（低字节先发） */
static void append_crc(uint8_t *buf, int n)
{
    uint16_t crc = mbrtu_crc16(buf, n);
    buf[n]     = (uint8_t)(crc & 0xFF);
    buf[n + 1] = (uint8_t)((crc >> 8) & 0xFF);
}

/* ========================= 异常响应 ========================= */

/* 异常帧固定 5 字节：addr, fc|0x80, code, crc_lo, crc_hi */
static int build_exception(uint8_t *resp, int resp_max,
                           uint8_t addr, uint8_t fc, uint8_t code)
{
    if (resp_max < 5)
        return 0;
    resp[0] = addr;
    resp[1] = (uint8_t)(fc | 0x80);
    resp[2] = code;
    append_crc(resp, 3);
    return 5;
}

/* ========================= 03 读寄存器 ========================= */

static int handle_read(const uint8_t *frame, uint8_t *resp, int resp_max)
{
    uint16_t start = (uint16_t)((frame[2] << 8) | frame[3]);
    uint16_t count = (uint16_t)((frame[4] << 8) | frame[5]);
    uint16_t reg;
    int i, rc, n;

    /* 数量：0 或 >125 -> 03（125 是规范上限，126 才拒） */
    if (count == 0 || count > MBRTU_MAX_READ_REGS)
        return build_exception(resp, resp_max, frame[0], MBRTU_FC_READ, 0x03);

    /* 地址范围：start ~ start+count-1 必须落在 0~11 -> 否则 02 */
    if ((int)start + (int)count > MBRTU_REG_COUNT)
        return build_exception(resp, resp_max, frame[0], MBRTU_FC_READ, 0x02);

    /* 正常响应：addr + fc + byte_count + data(2*count) + crc(2) */
    n = 3 + (int)count * 2;
    if (n + 2 > resp_max)
        return 0;   /* 缓冲区不够，宁可不回也不越界 */

    resp[0] = frame[0];
    resp[1] = MBRTU_FC_READ;
    resp[2] = (uint8_t)(count * 2);

    for (i = 0; i < count; i++) {
        reg = 0xFFFF;
        rc = g_hooks.read ? g_hooks.read((int)start + i, &reg) : 0;
        if (rc != 0)
            return build_exception(resp, resp_max, frame[0], MBRTU_FC_READ,
                                   (uint8_t)rc);
        resp[3 + i * 2]     = (uint8_t)(reg >> 8);   /* 寄存器值大端 */
        resp[3 + i * 2 + 1] = (uint8_t)(reg & 0xFF);
    }

    append_crc(resp, n);
    return n + 2;
}

/* ========================= 06 写单寄存器 ========================= */

static int handle_write(const uint8_t *frame, uint8_t *resp, int resp_max)
{
    uint16_t off = (uint16_t)((frame[2] << 8) | frame[3]);
    uint16_t val = (uint16_t)((frame[4] << 8) | frame[5]);
    int rc;

    /* 地址越界 -> 02 */
    if (off >= MBRTU_REG_COUNT)
        return build_exception(resp, resp_max, frame[0], MBRTU_FC_WRITE, 0x02);

    /* 具体能不能写、值合不合法，交给业务回调判断 */
    rc = g_hooks.write ? g_hooks.write((int)off, val) : 0;
    if (rc != 0)
        return build_exception(resp, resp_max, frame[0], MBRTU_FC_WRITE,
                               (uint8_t)rc);

    /* 06 正常响应 = 原帧回显 */
    if (resp_max < 8)
        return 0;
    memcpy(resp, frame, 8);
    return 8;
}

/* ========================= 主解析入口 ========================= */

int mbrtu_parse(const uint8_t *frame, int len, uint8_t *resp, int resp_max)
{
    uint8_t  addr, fc;
    uint16_t crc_calc, crc_recv;

    /* 门卫 1：长度。最短 4 字节（addr+fc+crc2），最长 256 */
    if (len < 4)
        return -1;
    if (len > MBRTU_MAX_ADU)
        return -1;

    /* 门卫 2：站号。0x00 广播、0x01 本机；其他不是给我的，静默 */
    addr = frame[0];
    if (addr != 0x00 && addr != MBRTU_SLAVE_ADDR)
        return 0;

    /* 门卫 3：CRC。低字节在前 */
    crc_calc = mbrtu_crc16(frame, len - 2);
    crc_recv = (uint16_t)(frame[len - 2] | ((uint16_t)frame[len - 1] << 8));
    if (crc_calc != crc_recv)
        return 0;

    fc = frame[1];

    /* 广播：写=执行不回复；读/其他=静默；永不回异常 */
    if (addr == 0x00) {
        if (fc == MBRTU_FC_WRITE && len == 8) {
            uint16_t off = (uint16_t)((frame[2] << 8) | frame[3]);
            uint16_t val = (uint16_t)((frame[4] << 8) | frame[5]);
            /* 广播写命令（7 号）会让总线上所有从站一起切歌，现场风险大：
             * 只允许广播写音量这类设置寄存器，命令寄存器静默丢弃 */
            if (g_hooks.write && off < MBRTU_REG_COUNT && off != MBRTU_REG_CMD)
                (void)g_hooks.write((int)off, val);
        }
        return 0;
    }

    /* 门卫 4：功能码。不支持就回 01（这是正常异常响应，不是静默） */
    if (fc != MBRTU_FC_READ && fc != MBRTU_FC_WRITE)
        return build_exception(resp, resp_max, addr, fc, 0x01);

    /* 门卫 5：精确帧长。03/06 请求固定 8 字节，多一少一都丢 */
    if (len != 8)
        return 0;

    if (fc == MBRTU_FC_READ)
        return handle_read(frame, resp, resp_max);

    return handle_write(frame, resp, resp_max);
}

/* ========================= 判帧间隔 ========================= */

double mbrtu_frame_gap_ms(int baud)
{
    if (baud <= 0)
        return 4.0;
    if (baud > 19200)
        return 1.75;                      /* 规范：高波特率固定 1.75ms */
    return 35000.0 / (double)baud;        /* 3.5 字符 × 10 位 / 波特率 */
}
