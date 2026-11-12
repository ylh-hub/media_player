#!/bin/sh
set -e
P=/mnt/hgfs/share-2/fq/三阶段/1_20261026_并发/media_player
cd "$P"
gcc -Wall -Wextra -Werror -I src -o build/ctl_test tests/ctl_test.c src/ctl_core.c -lpthread
./build/ctl_test
