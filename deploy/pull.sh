#!/bin/sh
# pull.sh —— 从 PC 的 HTTP 服务拉最新 media_player 并重启（A 方案日常开发）
# 前提：PC 上已在 build/ 目录起服务：
#   python -m http.server 8000 --bind 0.0.0.0
# 用法（板子上）：sh /ylh/pull.sh
set -e
PC=http://172.20.0.1:8000

wget -q -O /tmp/mp.new     $PC/media_player
wget -q -O /tmp/mp.new.md5 $PC/media_player.md5

a=$(md5sum < /tmp/mp.new | awk '{print $1}')
b=$(awk '{print $1}' /tmp/mp.new.md5)
if [ "$a" != "$b" ]; then
    echo "md5 不一致，放弃替换: 下载=$a 服务器=$b"
    rm -f /tmp/mp.new /tmp/mp.new.md5
    exit 1
fi

mv /tmp/mp.new /ylh/media_player
chmod +x /ylh/media_player
rm -f /tmp/mp.new.md5
/etc/init.d/S99mp restart
echo "pull ok: $(md5sum /ylh/media_player | awk '{print $1}')"
