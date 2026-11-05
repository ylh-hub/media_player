#!/bin/sh
# 在 VM 里编译并运行全部 x86 自测（CRC + 解析器）
set -e
P=/mnt/hgfs/share-2/fq/三阶段/1_20261026_并发/media_player
mkdir -p "$P/build"

echo "== compile crc =="
gcc -Wall -Wextra -Werror -I "$P/src" \
    -o "$P/build/test_crc" \
    "$P/tests/test_crc.c" "$P/src/modbus_rtu.c"

echo "== compile parse =="
gcc -Wall -Wextra -Werror -I "$P/src" \
    -o "$P/build/test_parse" \
    "$P/tests/test_parse.c" "$P/src/modbus_rtu.c"

echo "== run crc =="
"$P/build/test_crc"

echo "== run parse =="
"$P/build/test_parse"

echo "== compile fifo =="
gcc -Wall -Wextra -Werror -I "$P/src" \
    -o "$P/build/test_fifo" \
    "$P/tests/test_fifo.c" "$P/src/modbus_slave.c" "$P/src/modbus_rtu.c" "$P/tests/stub_player.c" \
    -lpthread

echo "== run fifo =="
"$P/build/test_fifo"
