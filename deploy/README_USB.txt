RK3568 触摸屏多媒体播放器 · U 盘答辩包
========================================

一、这个 U 盘里有什么
--------------------
mplayer/
├── bin/
│   ├── media_player   播放器主程序（ARM，已带网易云风界面）
│   └── mplayer64      音视频后端（板子上没有就装上）
├── media/             4 首歌 + 2 个视频（1.mp4、04.flv）
├── lrc/               4 首歌词（和 mp3 同名，播放时自动加载）
├── assets/icons/      主界面磁贴图标（music.jpg/album.jpg，装到 /ylh/icons）
├── scripts/
│   ├── install.sh     一键安装到板子（复制程序/媒体/图标/自启脚本）
│   ├── S99mp          开机自启脚本（装到 /etc/init.d/）
│   └── run.sh         手动重启播放器
└── README.txt         本文件

相册图片不放 U 盘：直接拷到板子 /ylh/photos（jpg/png/bmp），
或 scp：scp 照片.jpg rkboard:/ylh/photos/ 然后 /etc/init.d/S99mp restart。

二、第一次使用（约 2 分钟）
--------------------------
1. U 盘插到板子上，挂载后进入目录（挂载点可能是 /media/sda1 或 /mnt/udisk）：
     ls /media
     cd /media/sda1/mplayer

2. 跑安装脚本：
     sh scripts/install.sh

3. 手动启动一次，确认能出界面：
     /etc/init.d/S99mp start
     看屏幕：应出现深蓝音乐页（黑胶 + 歌词 + 底部控制条）

4. 重启验证开机自启：
     reboot
   约 15 秒后应自动进入音乐页并开始播放（不用敲任何命令）。

三、答辩当天（3 步）
--------------------
1. U 盘插板子（如果已经装过，可只插着装媒体；程序已在 /ylh）。
2. 上电，等 15 秒。
3. 手指点屏验收：
   - 点黑胶：暂停/继续
   - 点进度条：跳转
   - 点左右箭头：切歌（歌词跟着换）
   - 点底部歌单按钮：右侧歌单，点第 2 行切歌
   - 右上 VIDEO → 1.mp4：全屏；触左边缘退出，回音乐页音乐自动恢复
   - 连续进出视频 3~5 次：不花屏、不闪进度条

四、日志与排错
--------------
- 启动日志：/ylh/mp.log
     tail -n 30 /ylh/mp.log
  正常应看到：
     media_player build ...
     scan /ylh/media: 4 songs, 2 videos
- 停止播放器：
     /etc/init.d/S99mp stop
- 重启播放器：
     /etc/init.d/S99mp restart
- 视频文件名必须和 /ylh/media 下一致（1.mp4、04.flv）；扩展名大小写都认。
- 中文文件名建议改拼音：界面字体只嵌了歌词用字，中文文件名可能显示 "?"。

五、注意
--------
- U 盘必须是 FAT32（板子不认 NTFS）。
- 安装脚本会覆盖 /ylh/media_player、/usr/bin/mplayer64 和 /etc/init.d/S99mp；
  想回退就删掉 /etc/init.d/S99mp 再重启（或 /etc/init.d/S99mp stop）。
- 演示前建议先插 U 盘试一次，别到现场才发现挂载点不同。

六、_旧文件/ 是什么
------------------
里面是 11-02 旧版程序和媒体副本（stage3_ui、touch_test、旧 mplayer64 等），
本包不依赖它；确认不需要可以整个删掉，不影响安装和演示。

七、SSH 通道（2026-11-03 开通，日常开发首选）
--------------------------------------------
- 连接：ssh rkboard   （~/.ssh/config 已配：root@172.20.0.50 + ~/.ssh/rk_board_ec）
- 传文件：scp 本地文件 rkboard:/ylh/
- 密钥必须是 ECDSA：板载 dropbear 是裁剪版，只认 ecdsa 公钥
  （ed25519 报 unknown algo、RSA 静默拒绝，都是这个原因）。
- 板端两处公钥：/root/.ssh/authorized_keys 和 /etc/dropbear/authorized_keys（同一份）。
- 2026-11-03 还修了 /etc/init.d/S50dropbear 里的 `-R`（禁止 root 登录），已去掉并重启服务。
- 串口（COM8 1500000）退居兜底：SSH 不通时才用；注意 WorkBuddy 也会抢 COM8，用前先确认。

