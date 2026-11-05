#include <stdio.h>
#include <stdint.h>
#include "modbus_rtu.h"

/* 三个已知帧，CRC 已经用独立脚本算过，写死在这里当"标准答案" */
static int check(const char *name, const uint8_t *f, int n, uint16_t expect)
{
    uint16_t got = mbrtu_crc16(f, n);
    int pass = (got == expect);

    printf("  %-22s expect=%04X got=%04X  %s\n",
           name, expect, got, pass ? "PASS" : "FAIL");
    return pass;
}

int main(void)
{
    /* 读 12 个寄存器：01 03 00 00 00 0C -> 线上 45 CF */
    uint8_t read12[]  = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0C};
    /* 写音量 50：01 06 00 06 00 32 -> 线上 E8 1E */
    uint8_t writevol[] = {0x01, 0x06, 0x00, 0x06, 0x00, 0x32};
    /* 读 10 个寄存器：01 03 00 00 00 0A -> 线上 C5 CD */
    uint8_t read10[]  = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};

    int ok = 1;
    printf("CRC16/Modbus 自测：\n");
    ok &= check("read 12 regs",  read12,   6, 0xCF45);
    ok &= check("write vol 50",  writevol, 6, 0x1EE8);
    ok &= check("read 10 regs",  read10,   6, 0xCDC5);

    printf(ok ? "3/3 PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
