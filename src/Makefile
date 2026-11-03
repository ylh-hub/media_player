# Makefile —— media_player 一键编译
# ============================================================
# 用法:
#   make            编译全部 5 个程序 (stage1~3 / media_player / touch_test)
#   make media_player   只编最终版
#   make clean      清理所有可执行文件
#
# VM 里调试播放逻辑时(用 mplayer 而不是板子的 mplayer64):
#   make DEFINES='-DMPLAYER_BIN="mplayer"'

CC     = gcc
CFLAGS = -I. -Wall -g $(DEFINES)
LIBS   = -lm -lpthread

# 公共模块: 所有阶段共用, 链接时全带上(gcc 只取用到的部分)
COMMON = fb_ui.c font_cn.c font16.c lrc.c mplayer_ctl.c touch.c

all: stage1 stage2 stage3 media_player touch_test

stage1: stage1_console.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

stage2: stage2_player.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

stage3: stage3_ui.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# 最终版: 输出名 media_player, 与源码注释里 ./media_player 运行方式一致
media_player: stage4_main.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

touch_test: touch_test.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f stage1 stage2 stage3 media_player touch_test

.PHONY: all clean
