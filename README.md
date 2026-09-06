# vshbgm-mod
plays a looping tune on the vsh

## Music files

Place an optional startup cue, idle playlist, and game-launch cue in the plugin
directory:

```text
ms0:/seplugins/vshbgm/startup.mp3
ms0:/seplugins/vshbgm/bgm.m3u
ms0:/seplugins/vshbgm/gameboot.mp3
```

`startup.mp3` plays once when the VSH plugin starts. The playlist then plays
while the XMB is idle. `gameboot.mp3` plays immediately before a detected game
or homebrew launch. The launch wait is capped at three seconds so a damaged or
overlong cue cannot hold the boot process indefinitely.

Playlist tracks are played in order and the playlist loops after the last
track. Blank lines and M3U comments (including `#EXTM3U` and `#EXTINF`) are
ignored.

Playlist entries may be absolute PSP paths or paths relative to the playlist:

```m3u
#EXTM3U
music/first.mp3
music/second.mp3
ms0:/MUSIC/third.mp3
```

Up to 64 MP3 tracks are loaded from a playlist. If no usable playlist or track
is found, `ms0:/seplugins/vshbgm/bgm.mp3` and then `ms0:/bgm.mp3` remain as
single-file fallbacks.

## Configuration

Settings are loaded from `ms0:/seplugins/vshbgm/vshbgm.ini`. A default file is
created automatically if it does not exist. The repository also includes
`vshbgm.ini.example`:

```ini
[vshbgm]
volume = 50
shuffle = 0
startup_sound = startup.mp3
idle_playlist = bgm.m3u
gameboot_sound = gameboot.mp3
```

`volume` accepts values from 0 to 100. Set `shuffle` to `1`, `true`, `yes`, or
`on` to randomize the playlist order when it is loaded. Use `0`, `false`, `no`,
or `off` to play tracks in their listed order. Audio paths may be absolute PSP
paths or relative to `ms0:/seplugins/vshbgm/`. Leave `startup_sound` or
`gameboot_sound` empty to disable that cue. The configuration is reloaded when
the plugin starts and after the PSP resumes from suspend.

## Playback hotkeys

- Hold `L + R` and press `Circle` to stop playback.
- Hold `L + R` and press `Cross (X)` to start playback.

Hotkeys are edge-triggered, so holding a combination does not repeatedly change
the playback state.

```c
/*
 * vshbgm - system menu music, anyon?
 *
 * This program is licensed under the GPL-v2 license.
 * https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html
 *
 * heavily inspired by mp3play_lite - code from ARK4 and CXMB:
 * https://github.com/PSP-Archive/MP3PlayerPlugin/
 * https://github.com/PSP-Archive/ARK-4/
 * https://github.com/PSP-Archive/CXMB
 */
 ```
 
 [Download](https://the-sauna.icu/vshbgm/)
