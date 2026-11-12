#!/bin/sh
set -e
export PATH=/usr/local/arm-linux/bin:$PATH
cd /mnt/hgfs/share-2/fq/三阶段/1_20261026_并发/media_player
aarch64-linux-gnu-gcc -O2 -static -I src -o build/ctl_client tests/ctl_client.c
ls -l build/ctl_client
