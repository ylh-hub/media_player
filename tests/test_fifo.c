#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "modbus_rtu.h"
#include "modbus_slave.h"

static void msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int build_req(uint8_t *f, uint8_t addr, uint8_t fc, uint16_t a, uint16_t b)
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

static int failures = 0;

static void ok(const char *name, int cond)
{
    printf("  %-40s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

int main(void)
{
    uint8_t req[8], resp[300];
    int in_fd = -1, out_fd = -1, n, i;

    unlink("/tmp/modbus_in");
    unlink("/tmp/modbus_out");

    if (mbrtu_slave_start() != 0) {
        printf("start failed\n");
        return 1;
    }
    msleep(300);

    for (i = 0; i < 20 && in_fd < 0; i++) {
        in_fd = open("/tmp/modbus_in", O_WRONLY | O_NONBLOCK);
        if (in_fd < 0) msleep(50);
    }
    out_fd = open("/tmp/modbus_out", O_RDONLY | O_NONBLOCK);
    if (in_fd < 0 || out_fd < 0) {
        printf("open fifo failed (in=%d out=%d)\n", in_fd, out_fd);
        return 1;
    }

    printf("FIFO 集成自测：\n");

    /* I1a 读 12 个寄存器 */
    build_req(req, 0x01, 0x03, 0, 12);
    (void)write(in_fd, req, 8);
    msleep(120);
    n = (int)read(out_fd, resp, sizeof(resp));
    ok("I1a read12 -> 29 bytes", n == 29);
    if (n == 29) {
        uint16_t c   = mbrtu_crc16(resp, 27);
        uint16_t got = (uint16_t)(resp[27] | ((uint16_t)resp[28] << 8));
        ok("I1a crc ok", c == got);
        ok("I1a bytecount 0x18", resp[2] == 0x18);
    }

    /* I1b 写音量 50 -> 回显 */
    build_req(req, 0x01, 0x06, 6, 50);
    (void)write(in_fd, req, 8);
    msleep(120);
    n = (int)read(out_fd, resp, sizeof(resp));
    ok("I1b write vol -> echo 8", n == 8 && memcmp(req, resp, 8) == 0);

    /* I1c CRC 错 -> 无响应 */
    build_req(req, 0x01, 0x03, 0, 12);
    req[7] ^= 0xFF;
    (void)write(in_fd, req, 8);
    msleep(120);
    n = (int)read(out_fd, resp, sizeof(resp));
    ok("I1c bad crc -> no response", n <= 0);

    /* I2 写端关掉重开，进程不死，下一条命令还能响应 */
    close(in_fd);
    msleep(50);
    in_fd = open("/tmp/modbus_in", O_WRONLY | O_NONBLOCK);
    ok("I2 reopen writer", in_fd >= 0);
    if (in_fd >= 0) {
        build_req(req, 0x01, 0x03, 0, 12);
        (void)write(in_fd, req, 8);
        msleep(120);
        n = (int)read(out_fd, resp, sizeof(resp));
        ok("I2 next cmd still works", n == 29);
    }

    /* I3 两帧粘连 -> 至少有响应，进程不死 */
    {
        uint8_t two[16];
        build_req(two,     0x01, 0x03, 0, 12);
        build_req(two + 8, 0x01, 0x06, 6, 60);
        (void)write(in_fd, two, 16);
        msleep(200);
        n = (int)read(out_fd, resp, sizeof(resp));
        ok("I3 back-to-back -> response", n > 0);
    }

    if (failures)
        printf("FAILED (%d)\n", failures);
    else
        printf("ALL PASS\n");
    return failures ? 1 : 0;
}
