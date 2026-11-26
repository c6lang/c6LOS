# GUI/ABI 验收截图

这些图由本分支的验收命令实际生成，PPM 原件在 `/tmp`（mktemp 的脚本会在
退出时删除；下面是从当时留下的 PPM 转成的 PNG）：

| 文件 | 来源 | 说明 |
|---|---|---|
| `g2-fb0-800x600.png` | `make check-linuxabi-g2` | QEMU VBE 800×600 真实 screendump；`g2dev` 先铺红/绿/蓝/黄四色带，再落 0x112233/0x445566/0x778899 三个断言像素；内核走 `C6FBIOFLUSH` 缩放到 VBE 面板。之前只有三个孤立像素，所以截图看起来全黑；已补可见测试图。 |
| `g3-cdp-chrome-host-1280x661.png` | `make check-linuxabi-g3` | 宿主 Chromium 139 的 CDP `Page.screencastFrame`（PNG）经 `c6chrome-display` 解码；页面是 data URL 的 `Hello`。客户机内同一二进制写 `/dev/fb0`。 |
| `g4-c6wl-demo-1280x800.png` | `make check-linuxabi-g4` | 自包含 `c6wl` 合成器 + `wl_demo` 的 wl_shm buffer：红底 + 第三帧绿色方块（x=500..620,y=250..370）。 |

还没生成：**Chrome 页面显示在 fb0 上**的截图（G5 尚未完成；客户机里目前只验证到 `Chromium 131.0.6778.108 --version`）。G3 宿主截图的 PPM 原件是 `/tmp/frame.ppm`，G4 是 `/tmp/wlout.ppm`，G2 是 `/tmp/g2-visible.ppm`（生成命令 `G2_KEEP_PPM=/tmp/g2-visible.ppm make check-linuxabi-g2`）。
