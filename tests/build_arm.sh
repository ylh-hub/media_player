#!/bin/sh
# 在 VM 里跑完整的 ARM 交叉编译
set -e
export PATH=/usr/local/arm-linux/bin:$PATH
cd /mnt/hgfs/share-2/fq/三阶段/1_20261026_并发/media_player
sh build.sh
