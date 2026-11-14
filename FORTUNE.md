# ForTune Hiby

Lean Compás-based player (`FORTUNE_SHELL=1`). v1: **NetEase Cloud Music** + **local library** only.

## Build / 安装（与 MusicDownloader Compás 会话同一套路）

本机 macOS **不**交叉编译。仓库：[`Dlinuigh/compas-player`](https://github.com/Dlinuigh/compas-player)。

1. 分支 `fortune-hiby`（含 `.fortune-shell` 标记，CI 的 `make target` 自动 `FORTUNE_SHELL=1`）
2. Actions → **Daily executable build** 或 **ForTune Hiby build**（`fortune-hiby` **只编 r3ii_2025**）
3. 下 artifact → 设备开 ADB：
   ```sh
   adb push compas_player_target_r3ii_2025 /data/mnt/sd_0/.compas/compas_player
   ```
4. 海贝首页 → **更新播放器** → **Install from SD card**

本地仅语法检查：`make host FORTUNE_SHELL=1`（需 SDL2）；真机包以 CI 为准。
## Login

Write `MUSIC_U=…` to `SD/ForTune/session.txt` (or `SD/NetEase/session.txt`). Imported once, then stored under `SD/.compas/fortune_netease.txt`.

## ADB（必留）

开发与后续迭代**必须**能用 ADB：拉日志、推二进制、改文件。首页提供 **ADB** / **调试日志** 开关；切 USB DAC 会占用口，调试前请保持 ADB。

```sh
# 设备已开 ADB 后
adb devices
adb push compas_player_target_* /data/mnt/sd_0/.compas/compas_player
adb shell
# 日志（开「调试日志」后）
adb pull /data/mnt/sd_0/.logs/ ./logs/
# 跟进程 stderr（路径以机上实际 loader 为准）
adb shell 'logread -f'   # 或 cat /proc/... 视设备
```

重启后若曾保存 ADB 模式，Compás 会尝试恢复；ForTune 不主动改掉已开启的 ADB。

## 机上更新播放器

首页 → **更新播放器**（复用 Compás `player_update`）：

1. **Install from SD card**：把新二进制放到 `SD/.compas/compas_player`，再选此项；安装到 `/usr/data/compas_player` 后重启  
2. **Check for online update**：需 Wi‑Fi（上游发布源；ForTune 定制包通常用 SD）

ADB 推送后也走同一路径：`adb push … /.compas/compas_player` → 机上点更新，不必拆卡。

## Layout

| Path | Role |
| --- | --- |
| `src/ui/fortune/` | Three-tab shell |
| `src/netease/` | eapi + session + play |
| `docs/fortune/` | Local plan/CHANGELOG/INSTALL (gitignored upstream `/docs/`) |

More notes also under MusicCast `docs/fortune-hiby/`.
