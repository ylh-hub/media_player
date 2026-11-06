#!/bin/sh
# 用 ASan + UBSan 跑一遍单测和 FIFO 集成测试（x86 证据）
set -e
P=/mnt/hgfs/share-2/fq/三阶段/1_20261026_并发/media_player
mkdir -p "$P/build"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

echo "== build parse (san) =="
gcc -Wall -Wextra -Werror $SAN -I "$P/src" \
    -o "$P/build/test_parse_san" \
    "$P/tests/test_parse.c" "$P/src/modbus_rtu.c"

echo "== build fifo (san) =="
gcc -Wall -Wextra -Werror $SAN -I "$P/src" \
    -o "$P/build/test_fifo_san" \
    "$P/tests/test_fifo.c" "$P/src/modbus_slave.c" "$P/src/modbus_rtu.c" "$P/tests/stub_player.c" \
    -lpthread

echo "== run parse (san) =="
ASAN_OPTIONS=detect_leaks=1 "$P/build/test_parse_san"

echo "== run fifo (san) =="
ASAN_OPTIONS=detect_leaks=0 "$P/build/test_fifo_san"
