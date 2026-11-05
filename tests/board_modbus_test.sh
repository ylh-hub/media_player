#!/bin/sh
# board_modbus_test.sh —— 在板子上验证 Modbus 从站（FIFO 链路）
# 前提：/ylh/media_player 已在运行（sh /etc/init.d/S99mp restart）
# 用法： sh /ylh/board_modbus_test.sh
# 说明：用八进制转义直接 printf 原始字节，不依赖 busybox 的 \x 支持

send() {
    printf "$2" > /tmp/modbus_in
    sleep 0.2
    echo "--- $1 ---"
    timeout 1 cat /tmp/modbus_out | od -An -tx1
}

echo "=== Modbus FIFO test ==="
ls -l /tmp/modbus_in /tmp/modbus_out 2>/dev/null

# 1) 读 12 个寄存器，期望 29 字节：01 03 18 ...
send "read12  expect: 01 03 18 ..(24B).. CRC" '\001\003\000\000\000\014\105\317'

# 2) 写音量 60，期望回显：01 06 00 06 00 3C 69 DA
send "vol60   expect: 01 06 00 06 00 3C 69 DA" '\001\006\000\006\000\074\151\332'

# 3) 下一首，期望回显：01 06 00 07 00 02 B9 CA
send "next    expect: 01 06 00 07 00 02 B9 CA" '\001\006\000\007\000\002\271\312'

# 4) CRC 错帧，期望无任何输出
send "badcrc  expect: (nothing)" '\001\003\000\000\000\014\105\316'

echo "=== done ==="
