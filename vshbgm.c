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
 * https://github.com/PSP-Archive/CXMB_Reloaded
 */


#include "systemctrl.h"
#include "utils.h"
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <pspsdk.h>
#include <pspsysevent.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("vshbgm-mod", 0x1000, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

typedef struct {
  SceUID hnd;
  int f_sz, d_st, d_st0, fr_sz, o_num;
  u8 *r_buf, *d_buf, *o_buf[2];
  unsigned long *c_buf;
} DecodeData;

#define PLAYLIST_MAX_TRACKS 64
#define PLAYLIST_PATH_MAX 256
#define BGM_DIRECTORY "ms0:/seplugins/vshbgm/"
#define CONFIG_PATH "ms0:/seplugins/vshbgm/vshbgm.ini"
#define GAMEBOOT_TIMEOUT_US 3000000

enum PlaybackMode {
  PLAYBACK_IDLE,
  PLAYBACK_STARTUP,
  PLAYBACK_GAMEBOOT
};

typedef struct {
  char tracks[PLAYLIST_MAX_TRACKS][PLAYLIST_PATH_MAX];
  int count;
  int next;
} Playlist;

typedef struct {
  int volume;
  int shuffle;
  char startup_sound[PLAYLIST_PATH_MAX];
  char idle_playlist[PLAYLIST_PATH_MAX];
  char gameboot_sound[PLAYLIST_PATH_MAX];
} Config;

static SceUID bgm_thid = -1;
static int r_flg = 0, chan = -1, stop = 0;
static volatile int actv = 0;
static volatile u32 l_tim = 0;
static Playlist playlist;
static Config config;
static u32 random_state = 0;
static int playback_enabled = 1;
static int stop_hotkey_down = 0;
static int start_hotkey_down = 0;
static int startup_played = 0;
static volatile int game_launch_requested = 0;
static volatile int game_launch_complete = 1;
static volatile int game_launch_hook_active = 0;

static int (*_glen)(int);
static int (*_acd)(unsigned long *, int);
static int (*_loadexec_ms2)(const char *, void *);
static int (*_loadexec_disc)(const char *, void *);
static int (*_vsh_loadexec_ms2)(const char *, void *);
static int (*_vsh_loadexec_disc)(const char *, void *);
static unsigned long *our_buf = NULL;

void *bgm_memory_alloc(u32 size) {
  u32 alloc_size = size + 64 + sizeof(SceUID);
  SceUID memid = sceKernelAllocPartitionMemory(2, "umem", 0, alloc_size, NULL);
  if (memid < 0)
    return NULL;

  u32 ptr_base = (u32)sceKernelGetBlockHeadAddr(memid);
  u32 ptr_aligned = (ptr_base + sizeof(SceUID) + 63) & ~63;
  memcpy((void *)(ptr_aligned - sizeof(SceUID)), &memid, sizeof(SceUID));

  return (void *)ptr_aligned;
}

int bgm_free_alloc(void *ptr) {
  if (!ptr)
    return -1;
  SceUID memid;
  memcpy(&memid, (void *)((u32)ptr - sizeof(SceUID)), sizeof(SceUID));
  return sceKernelFreePartitionMemory(memid);
}

u32 bgm_find_func(char *lib, char *name, u32 nid) {
  return sctrlHENFindFunction(lib, name, nid);
}

int bgm_check_audio_active(int (*glen)(int), int our_chan) {
  if (!glen)
    return 0;
  for (int i = 0; i < 8; i++) {
    if (i != our_chan) {
      int len = glen(i);
      if (len > 2000)
        return 1;
    }
  }
  return 0;
}

int bgm_Get_Framesize(u8 *buf) {
  u32 header = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];

  if (((header & 0x180000) >> 19) != 3)
    return 0;

  if (((header & 0x60000) >> 17) != 1)
    return 0;

  int bitrate = (header & 0xF000) >> 12;
  int padding = (header & 0x200) >> 9;

  if (bitrate == 9)
    return 417 + padding;
  if (bitrate == 11)
    return 626 + padding;

  return 0;
}

static int acPat(unsigned long *buf, int type) {
  if (buf != our_buf && (type == 0x00001000 || type == 0x00001001 ||
                         type == 0x00001002 || type == 0x00001003)) {
    actv = 1;
    l_tim = sceKernelGetSystemTimeLow();
  }
  return _acd(buf, type);
}

static void Hook(void) {
  if (_acd)
    return;

  u32 g = bgm_find_func("sceAudio_Driver", "sceAudio", 0xB011922F);
  if (!g)
    g = bgm_find_func("sceAudio", "sceAudio", 0xB011922F);

  if (g)
    _glen = (void *)g;

  u32 a = bgm_find_func("sceAudiocodec", "sceAudiocodec", 0x70A703F8);
  if (a) {
    _acd = (void *)a;
    sctrlHENPatchSyscall((void *)a, acPat);
  }

  sceKernelDcacheWritebackAll();
  sceKernelIcacheClearAll();
}

static int MP3_Init(const char *file, DecodeData *mp3) {
  if ((mp3->hnd = sceIoOpen(file, PSP_O_RDONLY, 0777)) < 0)
    return -1;
  mp3->f_sz = sceIoLseek32(mp3->hnd, 0, PSP_SEEK_END);
  sceIoLseek32(mp3->hnd, 0, PSP_SEEK_SET);
  u8 hdr[10];
  mp3->d_st =
      (sceIoRead(mp3->hnd, hdr, 10) == 10 && !strncmp((char *)hdr, "ID3", 3))
          ? ((hdr[6] << 21) | (hdr[7] << 14) | (hdr[8] << 7) | hdr[9]) + 10
          : 0;
  mp3->d_st0 = mp3->d_st;
  sceIoLseek32(mp3->hnd, mp3->d_st, PSP_SEEK_SET);

  if (!(mp3->c_buf = bgm_memory_alloc(sizeof(unsigned long) * 65)) ||
      !(mp3->d_buf = bgm_memory_alloc(1152 * 4)) ||
      !(mp3->o_buf[0] = bgm_memory_alloc(1152 * 4)) ||
      !(mp3->o_buf[1] = bgm_memory_alloc(1152 * 4)) ||
      !(mp3->r_buf = bgm_memory_alloc(4096)))
    return -1;

  our_buf = mp3->c_buf;
  memset(mp3->c_buf, 0, sizeof(unsigned long) * 65);
  memset(mp3->d_buf, 0, 1152 * 4);
  memset(mp3->o_buf[0], 0, 1152 * 4);
  memset(mp3->o_buf[1], 0, 1152 * 4);

  if (sceAudiocodecCheckNeedMem(mp3->c_buf, PSP_CODEC_MP3) < 0 ||
      sceAudiocodecGetEDRAM(mp3->c_buf, PSP_CODEC_MP3) < 0 ||
      sceAudiocodecInit(mp3->c_buf, PSP_CODEC_MP3) < 0)
    return -1;
  mp3->fr_sz = 0;
  mp3->o_num = 1;
  return 0;
}

static int MP3_End(DecodeData *mp3) {
  sceAudiocodecReleaseEDRAM(mp3->c_buf);
  bgm_free_alloc(mp3->r_buf);
  bgm_free_alloc(mp3->c_buf);
  bgm_free_alloc(mp3->d_buf);
  bgm_free_alloc(mp3->o_buf[0]);
  bgm_free_alloc(mp3->o_buf[1]);
  mp3->r_buf = mp3->d_buf = mp3->o_buf[0] = mp3->o_buf[1] = NULL;
  mp3->c_buf = NULL;
  sceIoClose(mp3->hnd);
  mp3->hnd = -1;
  return 0;
}

static int MP3_Decode(DecodeData *mp3) {
  sceIoLseek32(mp3->hnd, mp3->d_st, PSP_SEEK_SET);

  int read_len = sceIoRead(mp3->hnd, mp3->r_buf, 1024);
  if (read_len < 4)
    return -1;

  if ((mp3->fr_sz = bgm_Get_Framesize(mp3->r_buf)) <= 0)
    return -1;

  if (mp3->fr_sz > read_len)
    return -1;
  mp3->c_buf[6] = (unsigned long)mp3->r_buf;
  mp3->c_buf[8] = (unsigned long)mp3->d_buf;
  mp3->c_buf[7] = mp3->c_buf[10] = mp3->fr_sz;
  mp3->c_buf[9] = (1152 * 4);
  if (sceAudiocodecDecode(mp3->c_buf, PSP_CODEC_MP3) < 0)
    return -1;
  memcpy(mp3->o_buf[mp3->o_num ^= 1], mp3->d_buf, 1152 * 4);
  return ((mp3->d_st += mp3->fr_sz) >= mp3->f_sz) ? 1 : 0;
}

static int Suspend_Handler(int id, char *name, void *prm, int *res) {
  if (id == 0x100 || id == 0x400) {
    if (id == 0x400)
      sceKernelDelayThread(1000000);
    r_flg = 1;
  }
  return 0;
}

PspSysEventHandler bgm_events = {0x40, "Suspend_Event", 0x0000FF00,
                                 Suspend_Handler};

static SceUID get_thread_id(const char *name) {
  int ret, count, i;
  SceUID ids[128];

  ret = sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, sizeof(ids),
                                    &count);
  if (ret < 0)
    return -1;

  for (i = 0; i < count; ++i) {
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    ret = sceKernelReferThreadStatus(ids[i], &info);
    if (ret < 0)
      continue;
    if (strcmp(info.name, name) == 0)
      return ids[i];
  }
  return -2;
}

static int is_player_active(void) {
  if (get_thread_id("VshCacheIoPrefetchThread") >= 0)
    return 1;

  if (get_thread_id("VideoDecoder") >= 0 || get_thread_id("AudioDecoder") >= 0)
    return 1;

  if (sceKernelFindModuleByName("sceUSB_Stor_Driver"))
    return 1;

  if (sceKernelFindModuleByName("camera_plugin_module"))
    return 2;
  if (sceKernelFindModuleByName("tdb_plugin_module"))
    return 2;
  if (sceKernelFindModuleByName("radioshack_plugin_module"))
    return 1;
  if (sceKernelFindModuleByName("skype_main_plugin_module"))
    return 1;

  return 0;
}

static int simple_atoi(const char *s) {
  int res = 0;
  if (*s < '0' || *s > '9')
    return -1;
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    if (res > 100)
      return 101;
    s++;
  }
  return *s == '\0' ? res : -1;
}

static int ReadTextLine(SceUID fd, char *line, int size) {
  int len = 0;
  int read = 0;
  char c;

  while ((read = sceIoRead(fd, &c, 1)) == 1) {
    if (c == '\n')
      break;
    if (c != '\r' && len < size - 1)
      line[len++] = c;
  }

  line[len] = '\0';
  if (len == 0 && read != 1)
    return -1;
  return len;
}

static char *Trim(char *text) {
  int len;
  while (*text == ' ' || *text == '\t')
    text++;

  len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
    text[--len] = '\0';
  return text;
}

static int StringEqualsIgnoreCase(const char *left, const char *right) {
  while (*left && *right) {
    char a = *left++;
    char b = *right++;
    if (a >= 'A' && a <= 'Z')
      a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
      b += 'a' - 'A';
    if (a != b)
      return 0;
  }
  return *left == '\0' && *right == '\0';
}

static int ParseBoolean(const char *value, int *result) {
  if (StringEqualsIgnoreCase(value, "1") ||
      StringEqualsIgnoreCase(value, "true") ||
      StringEqualsIgnoreCase(value, "yes") ||
      StringEqualsIgnoreCase(value, "on")) {
    *result = 1;
    return 0;
  }
  if (StringEqualsIgnoreCase(value, "0") ||
      StringEqualsIgnoreCase(value, "false") ||
      StringEqualsIgnoreCase(value, "no") ||
      StringEqualsIgnoreCase(value, "off")) {
    *result = 0;
    return 0;
  }
  return -1;
}

static void SetConfigPath(char *destination, const char *value) {
  int written = 0;
  int i;

  destination[0] = '\0';
  if (!value[0])
    return;

  if (!strstr(value, ":/") && !strstr(value, ":\\")) {
    for (i = 0; BGM_DIRECTORY[i] && written < PLAYLIST_PATH_MAX - 1; i++)
      destination[written++] = BGM_DIRECTORY[i];
  }
  for (i = 0; value[i] && written < PLAYLIST_PATH_MAX - 1; i++)
    destination[written++] = value[i] == '\\' ? '/' : value[i];
  destination[written] = '\0';
}

static void WriteDefaultConfig(void) {
  static const char defaults[] =
      "[vshbgm]\n"
      "volume = 50\n"
      "shuffle = 0\n"
      "startup_sound = startup.mp3\n"
      "idle_playlist = bgm.m3u\n"
      "gameboot_sound = gameboot.mp3\n";
  SceUID fd = sceIoOpen(CONFIG_PATH,
                        PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, defaults, sizeof(defaults) - 1);
    sceIoClose(fd);
  }
}

static void LoadConfig(Config *settings) {
  char line[PLAYLIST_PATH_MAX + 32];
  settings->volume = 50;
  settings->shuffle = 0;
  SetConfigPath(settings->startup_sound, "startup.mp3");
  SetConfigPath(settings->idle_playlist, "bgm.m3u");
  SetConfigPath(settings->gameboot_sound, "gameboot.mp3");

  SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0777);
  if (fd < 0) {
    WriteDefaultConfig();
    return;
  }

  while (1) {
    int len = ReadTextLine(fd, line, sizeof(line));
    if (len < 0)
      break;

    char *key = Trim(line);
    if (!key[0] || key[0] == '#' || key[0] == ';' || key[0] == '[')
      continue;

    char *equals = strchr(key, '=');
    if (!equals)
      continue;
    *equals = '\0';
    key = Trim(key);
    char *value = Trim(equals + 1);

    if (StringEqualsIgnoreCase(key, "volume")) {
      int volume = simple_atoi(value);
      if (volume >= 0 && volume <= 100)
        settings->volume = volume;
    } else if (StringEqualsIgnoreCase(key, "shuffle")) {
      ParseBoolean(value, &settings->shuffle);
    } else if (StringEqualsIgnoreCase(key, "startup_sound")) {
      SetConfigPath(settings->startup_sound, value);
    } else if (StringEqualsIgnoreCase(key, "idle_playlist")) {
      SetConfigPath(settings->idle_playlist, value);
    } else if (StringEqualsIgnoreCase(key, "gameboot_sound")) {
      SetConfigPath(settings->gameboot_sound, value);
    }
  }

  sceIoClose(fd);
}

static void GetPlaylistDirectory(const char *path, char *directory, int size) {
  int i;
  int slash = -1;

  for (i = 0; path[i] != '\0' && i < size - 1; i++) {
    directory[i] = path[i];
    if (path[i] == '/' || path[i] == '\\')
      slash = i;
  }

  if (slash >= 0)
    directory[slash + 1] = '\0';
  else
    directory[0] = '\0';
}

static void ResolvePlaylistPath(const char *directory, const char *entry,
                                char *path, int size) {
  int i = 0;
  int j = 0;

  if (strstr(entry, ":/") || strstr(entry, ":\\")) {
    directory = "";
  } else if (entry[0] == '/' || entry[0] == '\\') {
    while (directory[i] && directory[i] != ':') {
      if (j < size - 1)
        path[j++] = directory[i];
      i++;
    }
    if (directory[i] == ':' && j < size - 1)
      path[j++] = ':';
    directory = "";
  }

  for (i = 0; directory[i] && j < size - 1; i++)
    path[j++] = directory[i];
  for (i = 0; entry[i] && j < size - 1; i++)
    path[j++] = entry[i] == '\\' ? '/' : entry[i];
  path[j] = '\0';
}

static int LoadPlaylist(const char *path, Playlist *playlist) {
  SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
  if (fd < 0)
    return -1;

  char directory[PLAYLIST_PATH_MAX];
  char line[PLAYLIST_PATH_MAX];
  GetPlaylistDirectory(path, directory, sizeof(directory));
  playlist->count = 0;
  playlist->next = 0;

  while (playlist->count < PLAYLIST_MAX_TRACKS) {
    int len = ReadTextLine(fd, line, sizeof(line));
    if (len < 0)
      break;
    if (len == 0)
      continue;

    char *entry = line;
    if (len >= 3 && playlist->count == 0 && (u8)entry[0] == 0xEF &&
        (u8)entry[1] == 0xBB && (u8)entry[2] == 0xBF)
      entry += 3;

    while (*entry == ' ' || *entry == '\t')
      entry++;
    len = strlen(entry);
    while (len > 0 && (entry[len - 1] == ' ' || entry[len - 1] == '\t'))
      entry[--len] = '\0';

    if (!entry[0] || entry[0] == '#')
      continue;

    ResolvePlaylistPath(directory, entry,
                        playlist->tracks[playlist->count],
                        PLAYLIST_PATH_MAX);
    playlist->count++;
  }

  sceIoClose(fd);
  return playlist->count > 0 ? 0 : -1;
}

static u32 NextRandom(void) {
  if (!random_state)
    random_state = sceKernelGetSystemTimeLow() ^ 0xA5A5A5A5;
  random_state = random_state * 1664525 + 1013904223;
  return random_state;
}

static void ShufflePlaylist(Playlist *playlist) {
  int i;
  char temporary[PLAYLIST_PATH_MAX];

  random_state ^= sceKernelGetSystemTimeLow();
  for (i = playlist->count - 1; i > 0; i--) {
    int other = NextRandom() % (i + 1);
    if (other == i)
      continue;
    memcpy(temporary, playlist->tracks[i], PLAYLIST_PATH_MAX);
    memcpy(playlist->tracks[i], playlist->tracks[other], PLAYLIST_PATH_MAX);
    memcpy(playlist->tracks[other], temporary, PLAYLIST_PATH_MAX);
  }
}

static void LoadPreferredPlaylist(Playlist *playlist) {
  playlist->count = 0;
  playlist->next = 0;

  if (config.idle_playlist[0] &&
      LoadPlaylist(config.idle_playlist, playlist) == 0) {
    if (config.shuffle)
      ShufflePlaylist(playlist);
    return;
  }
  if (LoadPlaylist("ms0:/bgm.m3u", playlist) == 0 && config.shuffle)
    ShufflePlaylist(playlist);
}

static int OpenNextTrack(Playlist *playlist, DecodeData *mp3) {
  int attempts = playlist->count;
  while (attempts-- > 0) {
    const char *path = playlist->tracks[playlist->next];
    playlist->next = (playlist->next + 1) % playlist->count;
    if (MP3_Init(path, mp3) == 0)
      return 0;
  }

  if (MP3_Init("ms0:/seplugins/vshbgm/bgm.mp3", mp3) == 0)
    return 0;
  if (MP3_Init("ms0:/bgm.mp3", mp3) == 0)
    return 0;
  return -1;
}

static int RequestGameLaunchCue(void) {
  u32 started;

  if (game_launch_hook_active || stop || !playback_enabled ||
      !config.gameboot_sound[0])
    return 0;

  game_launch_hook_active = 1;
  game_launch_complete = 0;
  game_launch_requested = 1;
  started = sceKernelGetSystemTimeLow();

  while (!game_launch_complete && !stop &&
         sceKernelGetSystemTimeLow() - started < GAMEBOOT_TIMEOUT_US)
    sceKernelDelayThread(10000);

  game_launch_requested = 0;
  game_launch_complete = 1;
  return 1;
}

static int LoadExecMs2Patched(const char *file, void *param) {
  int owns_hook = RequestGameLaunchCue();
  int result = _loadexec_ms2(file, param);
  if (owns_hook)
    game_launch_hook_active = 0;
  return result;
}

static int LoadExecDiscPatched(const char *file, void *param) {
  int owns_hook = RequestGameLaunchCue();
  int result = _loadexec_disc(file, param);
  if (owns_hook)
    game_launch_hook_active = 0;
  return result;
}

static int VshLoadExecMs2Patched(const char *file, void *param) {
  int owns_hook = RequestGameLaunchCue();
  int result = _vsh_loadexec_ms2(file, param);
  if (owns_hook)
    game_launch_hook_active = 0;
  return result;
}

static int VshLoadExecDiscPatched(const char *file, void *param) {
  int owns_hook = RequestGameLaunchCue();
  int result = _vsh_loadexec_disc(file, param);
  if (owns_hook)
    game_launch_hook_active = 0;
  return result;
}

static void HookGameLaunch(void) {
  u32 function;

  if (!_vsh_loadexec_ms2) {
    function =
        bgm_find_func("sceVshBridge_Driver", "sceVshBridge", 0x97FB006F);
    if (function) {
      _vsh_loadexec_ms2 = (void *)function;
      sctrlHENPatchSyscall((void *)function, VshLoadExecMs2Patched);
    }
  }

  if (!_vsh_loadexec_disc) {
    function =
        bgm_find_func("sceVshBridge_Driver", "sceVshBridge", 0xF4873F4D);
    if (function && (void *)function != (void *)_vsh_loadexec_ms2) {
      _vsh_loadexec_disc = (void *)function;
      sctrlHENPatchSyscall((void *)function, VshLoadExecDiscPatched);
    }
  }

  if (!_loadexec_ms2) {
    function = bgm_find_func("sceLoadExec", "LoadExecForKernel", 0x28D0D249);
    if (!function)
      function =
          bgm_find_func("sceLoadExec", "LoadExecForKernel", 0xD940C83C);
    if (function) {
      _loadexec_ms2 = (void *)function;
      sctrlHENPatchSyscall((void *)function, LoadExecMs2Patched);
    }
  }

  if (!_loadexec_disc) {
    function = bgm_find_func("sceLoadExec", "LoadExecForKernel", 0xD8320A28);
    if (function && (void *)function != (void *)_loadexec_ms2) {
      _loadexec_disc = (void *)function;
      sctrlHENPatchSyscall((void *)function, LoadExecDiscPatched);
    }
  }

  sceKernelDcacheWritebackAll();
  sceKernelIcacheClearAll();
}

static void CheckPlaybackHotkeys(void) {
  SceCtrlData pad;
  u32 stop_buttons = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE;
  u32 start_buttons = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_CROSS;

  if (sceCtrlPeekBufferPositive(&pad, 1) <= 0)
    return;

  int stop_pressed = (pad.Buttons & stop_buttons) == stop_buttons;
  int start_pressed = (pad.Buttons & start_buttons) == start_buttons;

  if (stop_pressed && !stop_hotkey_down)
    playback_enabled = 0;
  else if (start_pressed && !start_hotkey_down)
    playback_enabled = 1;

  stop_hotkey_down = stop_pressed;
  start_hotkey_down = start_pressed;
}

static int vshbgm_thread(SceSize args, void *argp) {
  DecodeData mp3;
  int mp3_open = 0;
  int max_vol = 0;
  int mode = PLAYBACK_IDLE;
  u32 retry_time = 0;
  while (!sceKernelFindModuleByName("sceVshCommonUtil_Module"))
    sceKernelDelayThread(100000);
  while (!sceKernelFindModuleByName("scePaf_Module"))
    sceKernelDelayThread(100000);
  while (!sceKernelFindModuleByName("sceVshCommonGui_Module"))
    sceKernelDelayThread(100000);
  sceKernelDelayThread(1000000);
  sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);

  Hook();
  sceKernelDelayThread(3000000);

restart:;
  LoadConfig(&config);
  HookGameLaunch();
  LoadPreferredPlaylist(&playlist);
  max_vol = (0x8000 * config.volume) / 100;

  while (chan < 0 || chan > 7) {
    chan = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152,
                             PSP_AUDIO_FORMAT_STEREO);
    if (chan < 0)
      sceKernelDelayThread(500000);
  }
  r_flg = 0;
  int in_media = 0;
  int check_counter = 0;

  mode = PLAYBACK_IDLE;
  if (!startup_played) {
    startup_played = 1;
    if (config.startup_sound[0] && MP3_Init(config.startup_sound, &mp3) == 0) {
      mp3_open = 1;
      mode = PLAYBACK_STARTUP;
    }
  }
  if (!mp3_open && OpenNextTrack(&playlist, &mp3) == 0)
    mp3_open = 1;
  retry_time = sceKernelGetSystemTimeLow();

  while (!stop) {
    sceKernelDelayThread(10000);
    CheckPlaybackHotkeys();

    if (check_counter++ >= 3) {
      check_counter = 0;
      if (bgm_check_audio_active(_glen, chan)) {
        actv = 1;
        l_tim = sceKernelGetSystemTimeLow();
      } else if (actv && sceKernelGetSystemTimeLow() - l_tim > 200000) {
        actv = 0;
      }

      if (is_player_active()) {
        actv = 1;
        l_tim = sceKernelGetSystemTimeLow();
        in_media = 1;
      } else if (in_media) {
        actv = 0;
        in_media = 0;
      }
    }
    if (r_flg) {
      if (mp3_open)
        MP3_End(&mp3);
      mp3_open = 0;
      game_launch_requested = 0;
      game_launch_complete = 1;
      r_flg = 0;
      sceKernelDelayThread(2000000);
      goto restart;
    }

    if (game_launch_requested) {
      if (mp3_open)
        MP3_End(&mp3);
      mp3_open = 0;
      game_launch_requested = 0;
      mode = PLAYBACK_GAMEBOOT;

      if (playback_enabled && config.gameboot_sound[0] &&
          MP3_Init(config.gameboot_sound, &mp3) == 0) {
        mp3_open = 1;
      } else {
        game_launch_complete = 1;
        mode = PLAYBACK_IDLE;
        retry_time = sceKernelGetSystemTimeLow();
      }
    }

    if (actv && mode != PLAYBACK_GAMEBOOT) {
      sceKernelDelayThread(100000);
      continue;
    }

    if (!playback_enabled) {
      sceKernelDelayThread(100000);
      continue;
    }

    if (!mp3_open) {
      if (mode == PLAYBACK_GAMEBOOT) {
        game_launch_complete = 1;
        mode = PLAYBACK_IDLE;
      }
      if (sceKernelGetSystemTimeLow() - retry_time >= 2000000) {
        LoadPreferredPlaylist(&playlist);
        if (OpenNextTrack(&playlist, &mp3) == 0)
          mp3_open = 1;
        retry_time = sceKernelGetSystemTimeLow();
      }
      sceKernelDelayThread(100000);
      continue;
    }

    int res = MP3_Decode(&mp3);
    if (res != 0) {
      int finished_mode = mode;
      MP3_End(&mp3);
      mp3_open = 0;

      if (finished_mode == PLAYBACK_GAMEBOOT)
        game_launch_complete = 1;
      mode = PLAYBACK_IDLE;

      if (finished_mode != PLAYBACK_GAMEBOOT &&
          OpenNextTrack(&playlist, &mp3) == 0)
        mp3_open = 1;
      retry_time = sceKernelGetSystemTimeLow();
      continue;
    }
    sceAudioOutputBlocking(chan, max_vol, mp3.o_buf[mp3.o_num]);
  }
  if (mp3_open)
    MP3_End(&mp3);
  return 0;
}

int module_start(SceSize args, void *argp) {
  sceKernelRegisterSysEventHandler(&bgm_events);
  if ((bgm_thid = sceKernelCreateThread("vshbgm", vshbgm_thread, 0x12, 0x1000,
                                        0, NULL)) >= 0)
    sceKernelStartThread(bgm_thid, 0, 0);
  return 0;
}

int module_stop(SceSize args, void *argp) {
  stop = 1;
  game_launch_requested = 0;
  game_launch_complete = 1;
  sceKernelDelayThread(100000);
  if (chan >= 0) {
    sceAudioChRelease(chan);
    chan = -1;
  }
  if (_acd) {
    sctrlHENPatchSyscall((void *)_acd, (void *)_acd);
    _acd = NULL;
  }
  if (_loadexec_ms2) {
    sctrlHENPatchSyscall((void *)_loadexec_ms2, (void *)_loadexec_ms2);
    _loadexec_ms2 = NULL;
  }
  if (_loadexec_disc) {
    sctrlHENPatchSyscall((void *)_loadexec_disc, (void *)_loadexec_disc);
    _loadexec_disc = NULL;
  }
  if (_vsh_loadexec_ms2) {
    sctrlHENPatchSyscall((void *)_vsh_loadexec_ms2,
                         (void *)_vsh_loadexec_ms2);
    _vsh_loadexec_ms2 = NULL;
  }
  if (_vsh_loadexec_disc) {
    sctrlHENPatchSyscall((void *)_vsh_loadexec_disc,
                         (void *)_vsh_loadexec_disc);
    _vsh_loadexec_disc = NULL;
  }
  sceKernelDcacheWritebackAll();
  sceKernelIcacheClearAll();
  if (bgm_thid >= 0) {
    sceKernelWaitThreadEnd(bgm_thid, NULL);
    sceKernelDeleteThread(bgm_thid);
    bgm_thid = -1;
  }
  sceKernelUnregisterSysEventHandler(&bgm_events);
  return 0;
}
