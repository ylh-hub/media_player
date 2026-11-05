#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "modbus_rtu.h"

/* ================= 模拟寄存器表 + 命令槽（测试桩） ================= */
static uint16_t g_regs[MBRTU_REG_COUNT];
static int      g_cmd_pending = 0;
static unsigned g_accept_cnt  = 0;

static int t_read(int off, uint16_t *out)
{
    if (off < 0 || off >= MBRTU_REG_COUNT) return 2;
    *out = g_regs[off];
    return 0;
}

static int t_write(int off, uint16_t val)
{
    if (off < 0 || off >= MBRTU_REG_COUNT) return 2;

    if (off == 1) return 2;            /* 播放状态：只读 */

    if (off == 6) {                    /* 音量 */
        if (val > 100) return 3;
        g_regs[6] = val;
        return 0;
    }

    if (off == 7) {                    /* 命令 */
        if (val == 0 || val > 4) return 3;
        if (g_cmd_pending) return 6;   /* 槽满 -> busy */
        g_cmd_pending = 1;
        g_accept_cnt++;
        g_regs[11] = (uint16_t)(g_accept_cnt & 0xFFFF);
        return 0;
    }

    return 2;                          /* 其他都只读 */
}

static const mbrtu_hooks t_hooks = { t_read, t_write };

/* ================= 测试小工具 ================= */
static int failures = 0;

static void ok(const char *name, int cond)
{
    printf("  %-36s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

/* 组一帧 8 字节请求：addr, fc, 两个 16 位参数 */
static int mk(uint8_t *f, uint8_t addr, uint8_t fc, uint16_t a, uint16_t b)
{
    uint16_t c;
    f[0] = addr; f[1] = fc;
    f[2] = (uint8_t)(a >> 8); f[3] = (uint8_t)(a & 0xFF);
    f[4] = (uint8_t)(b >> 8); f[5] = (uint8_t)(b & 0xFF);
    c = mbrtu_crc16(f, 6);
    f[6] = (uint8_t)(c & 0xFF);
    f[7] = (uint8_t)(c >> 8);
    return 8;
}

static int crc_ok(const uint8_t *r, int n)
{
    uint16_t c, got;
    if (n < 4) return 0;
    c   = mbrtu_crc16(r, n - 2);
    got = (uint16_t)(r[n - 2] | ((uint16_t)r[n - 1] << 8));
    return c == got;
}

/* ================= 12 个用例 ================= */
int main(void)
{
    uint8_t req[300], resp[300];
    int n, r;

    mbrtu_set_hooks(&t_hooks);

    memset(g_regs, 0, sizeof(g_regs));
    g_regs[0]  = 0x0100;   /* 版本 v1.0 */
    g_regs[1]  = 1;        /* 播放中 */
    g_regs[2]  = 1;        /* 当前曲目 1（0-based） */
    g_regs[3]  = 3;        /* 共 3 首 */
    g_regs[4]  = 10;       /* 当前 10 秒 */
    g_regs[5]  = 100;      /* 总 100 秒 */
    g_regs[6]  = 30;       /* 音量 30 */
    g_regs[8]  = 0;        /* 错误码 0 */
    g_regs[9]  = 0x2345;   /* uptime 低 16 */
    g_regs[10] = 0x0001;   /* uptime 高 16 -> 0x00012345 */

    printf("Modbus 解析器自测：\n");

    /* U1 读 12 个寄存器 */
    n = mk(req, 0x01, 0x03, 0, 12);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U1 read 12 -> 29 bytes", r == 29);
    if (r == 29) {
        ok("U1 addr/fc/bytecount", resp[0] == 0x01 && resp[1] == 0x03 && resp[2] == 0x18);
        ok("U1 reg0 = 0x0100", resp[3] == 0x01 && resp[4] == 0x00);
        ok("U1 crc ok", crc_ok(resp, r));
    }

    /* U2 写音量 50 */
    n = mk(req, 0x01, 0x06, 6, 50);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U2 write vol 50 -> echo 8", r == 8 && memcmp(req, resp, 8) == 0);
    ok("U2 reg6 = 50", g_regs[6] == 50);

    /* U3 写命令 2（下一首） */
    g_cmd_pending = 0; g_accept_cnt = 0; g_regs[11] = 0;
    n = mk(req, 0x01, 0x06, 7, 2);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U3 write cmd 2 -> echo 8", r == 8);
    ok("U3 accept cnt = 1", g_accept_cnt == 1 && g_regs[11] == 1);

    /* U4 非法功能码 04 -> 01 84 01 */
    n = mk(req, 0x01, 0x04, 0, 1);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U4 fc04 -> exception 01", r == 5 && resp[0] == 0x01 && resp[1] == 0x84 && resp[2] == 0x01);
    ok("U4 exception crc", crc_ok(resp, r));

    /* U5 非法地址 999 -> 02 */
    n = mk(req, 0x01, 0x03, 999, 1);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U5 addr 999 -> exception 02", r == 5 && resp[1] == 0x83 && resp[2] == 0x02);

    /* U6 CRC 错 -> 静默 */
    n = mk(req, 0x01, 0x03, 0, 12);
    req[7] ^= 0xFF;
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U6 bad crc -> silent", r == 0);

    /* U7 短帧 3 字节 -> -1 */
    req[0] = 0x01; req[1] = 0x03; req[2] = 0x00;
    r = mbrtu_parse(req, 3, resp, sizeof(resp));
    ok("U7 short frame -> -1", r == -1);

    /* U8 错站号 02 -> 静默 */
    n = mk(req, 0x02, 0x03, 0, 12);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U8 wrong slave -> silent", r == 0);

    /* U9 广播写音量 -> 执行、无响应 */
    g_regs[6] = 30;
    n = mk(req, 0x00, 0x06, 6, 77);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U9 broadcast write -> 0 resp", r == 0 && g_regs[6] == 77);

    /* U9b 广播读 -> 静默 */
    n = mk(req, 0x00, 0x03, 0, 12);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U9b broadcast read -> silent", r == 0);

    /* U10 边界：音量 101 / count 0 / count 33 / count 12 */
    n = mk(req, 0x01, 0x06, 6, 101);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U10 vol 101 -> 03", r == 5 && resp[2] == 0x03);

    n = mk(req, 0x01, 0x03, 0, 0);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U10 count 0 -> 03", r == 5 && resp[2] == 0x03);

    n = mk(req, 0x01, 0x03, 0, 33);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U10 count 33 from 0 -> 02", r == 5 && resp[2] == 0x02);

    n = mk(req, 0x01, 0x03, 0, 12);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U10 count 12 -> normal", r == 29);

    /* U11 超长帧 257 字节 -> -1 */
    memset(req, 0, sizeof(req));
    req[0] = 0x01; req[1] = 0x03;
    r = mbrtu_parse(req, 257, resp, sizeof(resp));
    ok("U11 257-byte -> -1", r == -1);

    /* U12 uptime 映射 / 写只读 / 命令值越界 / 计数回绕 */
    {
        uint16_t v = 0;
        t_read(9, &v);  ok("U12 uptime low = 0x2345", v == 0x2345);
        t_read(10, &v); ok("U12 uptime high = 0x0001", v == 0x0001);
    }

    n = mk(req, 0x01, 0x06, 1, 0);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U12 write RO reg1 -> 02", r == 5 && resp[2] == 0x02);

    n = mk(req, 0x01, 0x06, 7, 0);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U12 cmd 0 -> 03", r == 5 && resp[2] == 0x03);

    n = mk(req, 0x01, 0x06, 7, 5);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U12 cmd 5 -> 03", r == 5 && resp[2] == 0x03);

    g_cmd_pending = 0; g_accept_cnt = 65535; g_regs[11] = 0xFFFF;
    n = mk(req, 0x01, 0x06, 7, 2);
    r = mbrtu_parse(req, n, resp, sizeof(resp));
    ok("U12 count wrap -> 0", r == 8 && g_regs[11] == 0);

    if (failures)
        printf("FAILED (%d)\n", failures);
    else
        printf("ALL PASS\n");
    return failures ? 1 : 0;
}
