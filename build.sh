#!/bin/sh
# ============================================================
# build.sh —— 一键编译（在 Ubuntu 虚拟机里执行）
# 用法:
#   bash build.sh
# 前提: 交叉工具链 aarch64-linux-gnu-gcc 在 PATH
#       （没有就 export PATH=/path/to/linaro/bin:$PATH）
# 产物: build/ 下 4 个 ARM 可执行文件 + 1 个 VM x86 调试文件
# ============================================================
set -e
cd "$(dirname "$0")"
CC_ARM=${CC_ARM:-aarch64-linux-gnu-gcc}
CC_X86=${CC_X86:-gcc}
CFLAGS="-Isrc -O1 -Wall"
mkdir -p build

# ---- 板子用（aarch64 静态链接：不依赖板子上的 so，少一类坑） ----
# 第三方 stb_image 单独编译、不加 -Wall（别人的代码，警告不归我们清）
$CC_ARM -Isrc -O1 -c src/stb_image.c -o build/stb_image.o
$CC_ARM $CFLAGS -static -o build/stage1_console src/stage1_console.c
$CC_ARM $CFLAGS -static -o build/touch_test     src/touch_test.c
$CC_ARM $CFLAGS -static -o build/stage2_player  \
        src/stage2_player.c src/mplayer_ctl.c -lpthread
$CC_ARM $CFLAGS -static -o build/stage3_ui      \
        src/stage3_ui.c src/mplayer_ctl.c src/fb_ui.c src/font16.c src/font_cn.c src/touch.c src/lrc.c -lpthread -lm
$CC_ARM $CFLAGS -static -o build/media_player   \
        src/stage4_main.c src/mplayer_ctl.c src/fb_ui.c src/font16.c src/font_cn.c src/touch.c src/lrc.c \
        src/image.c src/album.c src/modbus_slave.c src/modbus_rtu.c build/stb_image.o -lpthread -lm

# ---- VM 调试用（x86 + 系统 mplayer）：验证控制层逻辑，fb/触摸测不了 ----
$CC_X86 $CFLAGS -DMPLAYER_BIN='"mplayer"' -o build/stage2_player_x86 \
        src/stage2_player.c src/mplayer_ctl.c -lpthread

echo "编译完成，产物在 build/:"
ls -l build/
# 给 HTTP 部署用：板子 sh /ylh/pull.sh 会拿这个 md5 校验完整性
md5sum build/media_player > build/media_player.md5
echo "md5: $(cat build/media_player.md5)"

# 板侧联调脚本：随 HTTP 服务一起提供（wget 到板子上跑）
cp tests/board_modbus_test.sh build/board_modbus_test.sh
