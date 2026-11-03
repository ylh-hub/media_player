#!/bin/sh
# 在板子上执行（U 盘挂载后）：
#   sh /media/sda1/mplayer/scripts/install.sh
# 如果挂载点不是 /media/sda1，先 ls /media 看实际目录。
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
echo "package: $SRC"

[ -f "$SRC/bin/media_player" ] || { echo "ERROR: 缺少 bin/media_player"; exit 1; }

mkdir -p /ylh/media

# 1) 主程序
cp "$SRC/bin/media_player" /ylh/media_player
chmod +x /ylh/media_player

# 2) mplayer64（板子没有就装；已有则覆盖保证版本一致）
if [ -f "$SRC/bin/mplayer64" ]; then
    cp "$SRC/bin/mplayer64" /ylh/mplayer64
    cp "$SRC/bin/mplayer64" /usr/bin/mplayer64
    chmod +x /ylh/mplayer64 /usr/bin/mplayer64
fi

# 3) 媒体 + 歌词（同名 .lrc 放媒体目录）
cp "$SRC"/media/* /ylh/media/ 2>/dev/null || true
cp "$SRC"/lrc/*.lrc /ylh/media/ 2>/dev/null || true

# 4) 开机自启脚本
cp "$SRC/scripts/S99mp" /etc/init.d/S99mp
chmod +x /etc/init.d/S99mp

echo "---- verify ----"
ls -l /ylh/media_player /usr/bin/mplayer64 /etc/init.d/S99mp
ls /ylh/media
echo "install done."
echo "手动启动: /etc/init.d/S99mp start"
echo "重启验证: reboot   （约 15 秒后应自动出界面）"
