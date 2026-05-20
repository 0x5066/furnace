/**
 * Furnace Tracker - Winamp Input Plugin
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * Licensed under the GNU General Public License v2 or later.
 *
 * Drop into Furnace's src/ tree beside in2.h.
 * Build as a Windows DLL; place in <WinampDir>/Plugins/.
 * Supported file extension: .fur
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>           // _beginthreadex
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "thirdparty/wacup/wa_ipc.h"
#include "thirdparty/wacup/in2_extra.h"
#include "ta-log.h"
#include "fileutils.h"
#include "engine/engine.h"
#include "utfutils.h"          // utf16To8 / utf8To16
#include "in_furnace_config.h"
#include <zlib.h>

#include "resource.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define IN_FURNACE_VERSION    L"Furnace for Winamp " TEXT(DIV_VERSION)
#define FURNACE_BITDEPTH      16
#define RENDER_CHUNK_SAMPLES  576   // must be >= 576 for SAAddPCMData

// ---------------------------------------------------------------------------
// External Config Variables (defined in in_furnace_config.cpp)
// ---------------------------------------------------------------------------
extern int FURNACE_SAMPLERATE;
extern int g_audioHiPass;
extern int g_audioQuality;

extern int g_forceMono;
extern int g_loopForever;
extern int g_endAt0BXX;
extern int g_channelCountAsKbps;
extern int g_useModuleTime;

extern int g_arcadeCore;
extern int g_ayCore;
extern int g_bubsysQuality;
extern int g_c64Core;
extern int g_dsidQuality;
extern int g_esfmCore;
extern int g_fdsCore;
extern int g_gbQuality;
extern int g_ndsQuality;
extern int g_nesCore;
extern int g_opl2Core;
extern int g_opl3Core;
extern int g_opl4Core;
extern int g_opllCore;
extern int g_opn1Core;
extern int g_opnCore;
extern int g_opnaCore;
extern int g_opnbCore;
extern int g_pcSpeakerOutMethod;
extern int g_pceQuality;
extern int g_pnQuality;
extern int g_pokeyCore;
extern int g_s3mOPL3;
extern int g_saaCore;
extern int g_saaQuality;
extern int g_smQuality;
extern int g_snCore;
extern int g_swanCore;
extern int g_swanQuality;
extern int g_vbQuality;
extern int g_ym2612Core;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static DivEngine    gEngine;
static HANDLE       gPlayThread  = NULL;
static volatile int gStopFlag   = 0;
static volatile int gPauseFlag  = 0;
static volatile int gSeekTo     = -1;
static volatile int gTimestampsReady = 0;
static in_char      gCurrentFile[4096] = {0};
static int          gCurrentSubsong    = 0;   // subsong index of the currently-playing track
static volatile int gTargetSubsong     = 0;   // desired subsong, communicated to play thread

extern In_Module plugin;   // forward declaration

// Forward declarations for helpers used before their definitions
static int songLengthMs();
static int positionMs();
static void FURNACE_Stop();
static int initFurnace();
static void quitFurnace();

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------
static String wideToStr(const in_char* path) {
  WString ws(path);
  return utf16To8(ws);
}

// ---------------------------------------------------------------------------
// Subsong virtual-path helpers
// Convention: "path/to/file.fur?subsong=N"  (N >= 0)
// subsong=0 is the default; paths without the suffix always mean subsong 0.
// ---------------------------------------------------------------------------

// Strip "?subsong=N" from path, write the clean base path into basePath, and
// return the subsong index (0 if the suffix is absent).
static int parseSubsongIndex(const wchar_t* path,
                              wchar_t*       basePath,
                              int            basePathLen) {
  if (basePath && basePathLen > 0) basePath[0] = L'\0';
  if (!path || !path[0]) return 0;

  const wchar_t* q = wcsstr(path, L"?subsong=");
  if (q) {
    int idx = _wtoi(q + 9);
    int copyLen = (int)(q - path);
    if (copyLen >= basePathLen) copyLen = basePathLen - 1;
    if (basePath) { wcsncpy(basePath, path, copyLen); basePath[copyLen] = L'\0'; }
    return (idx >= 0) ? idx : 0;
  }
  if (basePath) {
    wcsncpy(basePath, path, basePathLen - 1);
    basePath[basePathLen - 1] = L'\0';
  }
  return 0;
}

// Equivalent of the private DivEngine::changeSong() implemented entirely
// through public members.
//
// In Furnace, pattern data is stored globally per-channel and shared by
// pattern-ID across all subsongs — only the order matrix (curOrders) and the
// subsong descriptor (curSubSong) change when switching subsongs.
// curPat therefore does not need to be updated.
static void selectSubsong(DivEngine* eng, int idx) {
  if (!eng) return;
  if (idx < 0 || idx >= (int)eng->song.subsong.size()) return;
  eng->curSubSong = eng->song.subsong[idx];
  eng->curOrders  = &eng->curSubSong->orders;
  // Each DivSubSong owns its own DivChannelData pat[] array; curPat is a
  // DivChannelData* pointing into it.  Without this update the render loop
  // reads patterns from subsong 0 even though curOrders is correct.
  eng->curPat     = eng->curSubSong->pat;
}

// Return true if the playlist already contains a "?subsong=1" entry for
// basePath, meaning this module was already expanded on a previous play.
static bool subsongAlreadyInPlaylist(const wchar_t* basePath) {
  wchar_t expected[4096 + 32] = {0};
  _snwprintf(expected, (4096 + 32) - 1, L"%s?subsong=1", basePath);

  int listLen = (int)SendMessage(plugin.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
  for (int i = 0; i < listLen; i++) {
    wchar_t* entry = (wchar_t*)SendMessage(
        plugin.hMainWindow, WM_WA_IPC, (WPARAM)i, IPC_GETPLAYLISTFILEW);
    if (entry && _wcsicmp(entry, expected) == 0)
      return true;
  }
  return false;
}

// Insert subsong entries 1 … N-1 into the Winamp playlist immediately after
// the current item by calling IPC_PE_INSERTFILENAME in reverse order
// (each insertion pushes previous entries one step further down the list,
// so inserting highest-index first yields ascending order in the final list).
// NOTE: This Winamp SDK only provides IPC_ENQUEUEFILEW (append-to-end).
// Entries will appear after any pre-existing items in the playlist.
static void expandSubsongsIntoPlaylist(const wchar_t* basePath) {
  int n = (int)gEngine.song.subsong.size();
  if (n <= 1) return;

  for (int i = 1; i < n; i++) {
    wchar_t vpath[4096 + 32] = {0};
    _snwprintf(vpath, (4096 + 32) - 1, L"%s?subsong=%d", basePath, i);

    // IPC_ENQUEUEFILEW takes an enqueueFileWithMetaStructW*
    enqueueFileWithMetaStructW ef = {0};
    ef.filename = vpath;
    ef.title    = nullptr;  // NULL → Winamp queries it via GetFileInfo
    ef.length   = -1;       // -1  → unknown; Winamp will query it
    SendMessage(plugin.hMainWindow,
                WM_WA_IPC,
                (WPARAM)&ef,
                IPC_ENQUEUEFILEW);
  }
}

bool restartFurnace(){
  FURNACE_Stop();
  quitFurnace();
  if (!initFurnace()) return 0; //do nothing for now
  SendMessage(plugin.hMainWindow, WM_COMMAND, WINAMP_BUTTON2, 0); // this works
  return 1;
}

const wchar_t* msToMMSS(int milliseconds)
{
  // Static buffer persists after function returns
  static wchar_t buffer[6];

  // Convert milliseconds to total seconds
  int totalSeconds = milliseconds / 1000;

  // Calculate minutes and seconds
  int minutes = totalSeconds / 60;
  int seconds = totalSeconds % 60;

  // Format as MM:SS
  _snwprintf(buffer, sizeof(buffer), L"%d:%02d", minutes, seconds);

  return buffer;
}

float calcBPM(DivEngine* e, const DivGroovePattern& speeds, float hz, int vN, int vD) {
  float hl=e->curSubSong->hilightA;
  if (hl<=0.0f) hl=4.0f;
  float speedSum=0;
  for (int i=0; i<MIN(16,speeds.len); i++) {
    speedSum+=speeds.val[i];
  }
  speedSum/=MAX(1,speeds.len);
  if (speedSum<1.0f) speedSum=1.0f;
  if (vD<1) vD=1;
  return (60.0f*hz/(hl*speedSum))*(float)vN/(float)vD;
}

float calcSpeed(DivEngine* e, const DivGroovePattern& speeds) {
  float speedSum=0;
  for (int i=0; i<MIN(16,speeds.len); i++) {
    speedSum+=speeds.val[i];
  }
  speedSum/=MAX(1,speeds.len);
  if (speedSum<1.0f) speedSum=1.0f;
  return (speedSum);
}

static bool looksLikeZlib(const unsigned char* buf, size_t len) {
  if (len < 2) return false;
  if ((buf[0] & 0x0F) != 0x08) return false;          // CM must be deflate
  return ((buf[0] * 256u + buf[1]) % 31 == 0);         // FCHECK must hold
}

#define FURNACE_MAX_CHANNELS 2

static int FURNACE_CHANNELS() {
  return g_forceMono ? 1 : 2;
}

// ---------------------------------------------------------------------------
// Compression helper to convert raw .fur files to zlib format
// ---------------------------------------------------------------------------
static unsigned char* compressRawFur(const unsigned char* rawBuf, size_t rawLen, size_t& outLen) {
  if (!rawBuf || rawLen == 0) {
    outLen = 0;
    return nullptr;
  }

  // Use compressBound to get the maximum possible compressed size
  uLongf compressedLen = compressBound(rawLen);
  unsigned char* compressed = new unsigned char[compressedLen];

  int result = compress2(compressed, &compressedLen, rawBuf, rawLen, Z_DEFAULT_COMPRESSION);
  if (result != Z_OK) {
    delete[] compressed;
    outLen = 0;
    return nullptr;
  }

  outLen = compressedLen;
  return compressed;
}

// ---------------------------------------------------------------------------
// renderChunk()
// ---------------------------------------------------------------------------
static bool renderChunk(short* pcmOut, int samplesPerChan) {
  static float chL[RENDER_CHUNK_SAMPLES];
  static float chR[RENDER_CHUNK_SAMPLES];
  float* outs[2] = { chL, chR };

  gEngine.nextBuf(NULL, outs, 0, 2, (unsigned int)samplesPerChan);

  for (int i = 0; i < samplesPerChan; i++) {
    if (FURNACE_CHANNELS() == 2) {
      float l = chL[i]; if (l >  1.f) l =  1.f; if (l < -1.f) l = -1.f;
      float r = chR[i]; if (r >  1.f) r =  1.f; if (r < -1.f) r = -1.f;
      pcmOut[i * 2 + 0] = (short)(l * 32767.f);
      pcmOut[i * 2 + 1] = (short)(r * 32767.f);
    } else { // there simply is no conceivable way we'll have more than 2 channels
      // and if you think otherwise, you're wrong and stupid
      float l = chL[i]; if (l >  1.f) l =  1.f; if (l < -1.f) l = -1.f;
      float r = chR[i]; if (r >  1.f) r =  1.f; if (r < -1.f) r = -1.f;
      pcmOut[i] = ((short)(l * 32767.f) + (short)(r * 32767.f)) / 2;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Effect Detection Helper
// Detects if a specific effect code exists in the current row
// ---------------------------------------------------------------------------
static bool hasEffectInCurrentRow(unsigned char effectCode) {
  DivSubSong* sub = gEngine.curSubSong;
  if (!sub) return false;

  int order = 0, row = 0, tick = 0, speed = 0;
  gEngine.getPlayPosTick(order, row, tick, speed);

  // Check all channels for the effect
  for (int chan = 0; chan < gEngine.song.chans; chan++) {
    DivPattern* pat = gEngine.curPat[chan].getPattern(
      gEngine.curOrders->ord[chan][order], false);

    if (!pat) continue;

    // Check all effect columns
    for (int effCol = 0; effCol < gEngine.curPat[chan].effectCols; effCol++) {
      short effect = pat->newData[row][DIV_PAT_FX(effCol)];

      if (effect == effectCode) {
        return true;  // Found the effect
      }
    }
  }

  return false;  // Effect not found
}

static bool hasEffectInCurrentRowExt(DivEngine* eng, unsigned char effectCode) {
  DivSubSong* sub = eng->curSubSong;
  if (!sub) return false;

  int order = 0, row = 0, tick = 0, speed = 0;
  eng->getPlayPosTick(order, row, tick, speed);

  for (int chan = 0; chan < eng->song.chans; chan++) {
    DivPattern* pat = eng->curPat[chan].getPattern(
      eng->curOrders->ord[chan][order], false);

    if (!pat) continue;

    for (int effCol = 0; effCol < eng->curPat[chan].effectCols; effCol++) {
      short effect = pat->newData[row][DIV_PAT_FX(effCol)];
      if (effect == effectCode)
        return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Playback thread
// ---------------------------------------------------------------------------
static unsigned __stdcall playThreadProc(void*) {
  static short pcmBuf[RENDER_CHUNK_SAMPLES * FURNACE_MAX_CHANNELS];
  const  int   chunkBytes =
  RENDER_CHUNK_SAMPLES * FURNACE_CHANNELS() * (FURNACE_BITDEPTH / 8);

  int order = 0, row = 0, tick = 0, speed = 0;
  int prevOrder = -1;
  int prevRow   = -1;
  bool skipped = false;
  int skipRowsRemaining = 0;  // Counter to persist skip for 1-2 more rows

  // Capture sub only after the selectSubsong override so it reflects the
  // correct subsong for wrap detection, seeking, and EOF logic.
  DivSubSong* sub = gEngine.curSubSong;

  while (!gStopFlag) {

    gEngine.getPlayPosTick(order, row, tick, speed);
    plugin.SetInfo(g_channelCountAsKbps ? gEngine.song.chans : row, g_channelCountAsKbps ? FURNACE_SAMPLERATE / 1000 : order, g_channelCountAsKbps ? FURNACE_CHANNELS() : 0, !skipped);
    // --- seek ---
    int seekTo = gSeekTo;
    if (seekTo >= 0) {
      gSeekTo = -1;
      if (sub && sub->ordersLen > 0) {
        int64_t targetUs = (int64_t)seekTo * 1000;
        int bestOrd = 0;
        for (int o = 1; o < sub->ordersLen; o++) {
          TimeMicros t = sub->ts.getTimes(o, 0);
          int64_t us = (int64_t)t.seconds * 1000000LL + t.micros;
          if (us > targetUs) break;
          bestOrd = o;
        }
        gEngine.setOrder((unsigned char)bestOrd);
      }
      plugin.outMod->Flush(seekTo);
    }

    // --- pause ---
    if (gPauseFlag) { Sleep(50); continue; }

    // --- throttle ---
    if (plugin.outMod->CanWrite() < chunkBytes) { Sleep(10); continue; }

    // --- loop / stop detection (pre-render) --------------------------------
    // Check for stop effects on the current row before we render.
    if (sub) {
      if (hasEffectInCurrentRow(0xFF)) {
        // Handle FFxx (stop song) detection
        while (!gStopFlag && plugin.outMod->IsPlaying()) Sleep(11);
        PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
        break;
      }

      if ( (g_endAt0BXX && hasEffectInCurrentRow(0x0B)) && (order == sub->ordersLen - 1) ) {
        // Handle 0Bxx detection
        while (!gStopFlag && plugin.outMod->IsPlaying()) Sleep(11);
        PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
        break;
      }
    }

    // --- render ---
    // Snapshot position before and after the chunk.  If the engine crossed
    // the loop boundary during renderChunk (order or row wrapped), discard
    // the chunk entirely rather than letting beginning-of-song audio leak
    // into the output buffer.
    int orderBefore = order, rowBefore = row;
    if (!renderChunk(pcmBuf, RENDER_CHUNK_SAMPLES)) {
      while (!gStopFlag && plugin.outMod->IsPlaying()) Sleep(10);
      PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
      break;
    }

    if (sub) {
      int orderAfter = 0, rowAfter = 0, tickAfter = 0, speedAfter = 0;
      gEngine.getPlayPosTick(orderAfter, rowAfter, tickAfter, speedAfter);

      bool wrapped = false;
      if (sub->ordersLen > 1) {
        // multi-order: order went backwards (looped)
        if (orderBefore == sub->ordersLen - 1 && orderAfter < orderBefore)
          wrapped = true;
      } else {
        // single-order: row went backwards within the pattern
        if (rowAfter < rowBefore)
          wrapped = true;
      }

      if (wrapped && !g_loopForever) {
        // Don't write the chunk — it contains audio from the start of the loop.
        while (!gStopFlag && plugin.outMod->IsPlaying()) Sleep(11);
        PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
        break;
      }

      prevOrder = orderAfter;
      prevRow   = rowAfter;
      order     = orderAfter;
      row       = rowAfter;
    }

    if (hasEffectInCurrentRow(0x0D)) {
      skipRowsRemaining = 16;  // Keep skip active for 16 more rows
      skipped = true;
    } else if (hasEffectInCurrentRow(0x0B)) {
      skipRowsRemaining = 16;  // Keep skip active for 16 more rows
      skipped = true;
    } else {
      if (skipRowsRemaining > 0) {
        skipped = true;
        skipRowsRemaining--;
      } else {
        skipped = false;
      }
    }

    // DSP chain
    int writeSamples = RENDER_CHUNK_SAMPLES;
    if (plugin.dsp_isactive()) {
      writeSamples = plugin.dsp_dosamples(
        pcmBuf, RENDER_CHUNK_SAMPLES,
        FURNACE_BITDEPTH, FURNACE_CHANNELS(), FURNACE_SAMPLERATE);
    }

    int ts = plugin.outMod->GetWrittenTime();
    plugin.SAAddPCMData (pcmBuf, FURNACE_CHANNELS(), FURNACE_BITDEPTH, ts);
    plugin.VSAAddPCMData(pcmBuf, FURNACE_CHANNELS(), FURNACE_BITDEPTH, ts);
    plugin.outMod->Write((char*)pcmBuf,
                         writeSamples * FURNACE_CHANNELS() * (FURNACE_BITDEPTH / 8));
  }

  plugin.outMod->Close();
  plugin.SAVSADeInit();
  gEngine.stop();
  gEngine.song.unload();
  return 0;
}

// ---------------------------------------------------------------------------
// In_Module callbacks
// ---------------------------------------------------------------------------
static void FURNACE_Config(HWND hwndParent) {
  MessageBoxW(hwndParent,
              L"No configuration options yet.",
              L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
}

static void FURNACE_About(HWND hwndParent) {
  MessageBoxW(hwndParent,
              L"Furnace for Winamp\n"
              L"Version " TEXT(DIV_VERSION) L"\n\n"
              L"Copyright (C) 2021-2026 tildearrow and contributors\n"
              L"GNU General Public License v2 or later.",
              L"About Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
}


int initFurnace(){

  //initLog(stderr);
  //gEngine.everythingOK();
  //gEngine.setAudio(DIV_AUDIO_DUMMY);

  // Initialize config manager and load settings
  FurnaceConfigManager::init(&gEngine, plugin.hMainWindow);
  FurnaceConfigManager::loadConfig();

  return gEngine.init() ? 1 : 0;
}

void quitFurnace() {
  FurnaceConfigManager::saveConfig();
  gEngine.quit(false);
  finishLogFile();

  // Explicitly destroy and reconstruct the engine in place.
  // quit() leaves internal state that init() can't safely start from —
  // this gives us a genuinely clean object without changing all the
  // call sites from gEngine. to gEngine->
  gEngine.~DivEngine();
  new (&gEngine) DivEngine();
}

prefsDlgRecW *prefsRec = NULL;

// ---------------------------------------------------------------------------
// Prefs Dialog Helper Functions
// ---------------------------------------------------------------------------

// Populate sample rate combo box
static void populateSampleRateCombo(HWND hwnd) {
  int rates[] = { 8000, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000 };
  for (int i = 0; i < 9; i++) {
    wchar_t buf[32];
    _snwprintf(buf, 31, L"%d Hz", rates[i]);
    SendDlgItemMessageW(hwnd, IDC_COMBO_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)buf);
  }
  // Set current selection
  int idx = 4; // default 44100
  if (FURNACE_SAMPLERATE == 8000) idx = 0;
  else if (FURNACE_SAMPLERATE == 16000) idx = 1;
  else if (FURNACE_SAMPLERATE == 22050) idx = 2;
  else if (FURNACE_SAMPLERATE == 32000) idx = 3;
  else if (FURNACE_SAMPLERATE == 44100) idx = 4;
  else if (FURNACE_SAMPLERATE == 48000) idx = 5;
  else if (FURNACE_SAMPLERATE == 88200) idx = 6;
  else if (FURNACE_SAMPLERATE == 96000) idx = 7;
  else if (FURNACE_SAMPLERATE == 192000) idx = 8;
  SendDlgItemMessageW(hwnd, IDC_COMBO_SAMPLERATE, CB_SETCURSEL, idx, 0);
}

// Populate quality combo (0-3)
static void populateQualityCombo(HWND hwnd, int ctrlId, int currentVal, int count) {
  const wchar_t* qualityHiLo[] = { L"High", L"Low" };
  const wchar_t* qualitySixLev[] = { L"Lower", L"Low", L"Medium", L"High", L"Ultra", L"Ultimate"};

  const wchar_t** quality = nullptr;

  if (ctrlId == IDC_COMBO_QUALITY) {
    quality = qualityHiLo;
  } else if (ctrlId == IDC_COMBO_GB) {
    quality = qualitySixLev;
  } else if (ctrlId == IDC_COMBO_POWERNOISE) {
    quality = qualitySixLev;
  } else if (ctrlId == IDC_COMBO_SAA1099) {
    quality = qualitySixLev;
  } else if (ctrlId == IDC_COMBO_DSID) {
    quality = qualitySixLev;
  }

  for (int i = 0; i < count; i++) {
    SendDlgItemMessageW(hwnd, ctrlId, CB_ADDSTRING, 0, (LPARAM)quality[i]);
  }
  SendDlgItemMessageW(hwnd, ctrlId, CB_SETCURSEL, currentVal, 0);
}

// Populate chip core combo (0-1 or 0-2 depending on chip)
static void populateCoreCombo(HWND hwnd, int ctrlId, int currentVal, const wchar_t** options, int count) {
  for (int i = 0; i < count; i++) {
    SendDlgItemMessageW(hwnd, ctrlId, CB_ADDSTRING, 0, (LPARAM)options[i]);
  }
  SendDlgItemMessageW(hwnd, ctrlId, CB_SETCURSEL, currentVal, 0);
}

// Initialize dialog controls from global config
static void initDialogFromConfig(HWND hwnd) {
  // Audio settings
  CheckDlgButton(hwnd, IDC_AUDIO_DCOFFSET_CORRECTION, g_audioHiPass ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_AUDIO_FORCE_MONO, g_forceMono ? BST_CHECKED : BST_UNCHECKED);

  // Plugin-specific settings
  CheckDlgButton(hwnd, IDC_AUDIO_LOOP_FOREVER,          g_loopForever        ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_AUDIO_END_AT_0BXX,           g_endAt0BXX          ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_AUDIO_CHANNEL_COUNT_AS_KBPS, g_channelCountAsKbps ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_AUDIO_USE_MODULE_TIME,        g_useModuleTime      ? BST_CHECKED : BST_UNCHECKED);

  // Sample rate combo
  populateSampleRateCombo(hwnd);

  // Playback quality combo
  populateQualityCombo(hwnd, IDC_COMBO_QUALITY, g_audioQuality, 2);

  // Chip cores
  const wchar_t* ym2151Options[] = { L"ymfm", L"Nuked-OPN" };
  populateCoreCombo(hwnd, IDC_COMBO_YM2151, g_opn1Core, ym2151Options, 2);

  const wchar_t* ym2612Options[] = { L"Nuked-OPN2", L"ymfm", L"YMF276-LLE" };
  populateCoreCombo(hwnd, IDC_COMBO_YM2612, g_ym2612Core, ym2612Options, 3);

  const wchar_t* sn76489Options[] = { L"MAME", L"Nuked-PSG Mod" };
  populateCoreCombo(hwnd, IDC_COMBO_SN76489, g_snCore, sn76489Options, 2);

  const wchar_t* nesOptions[] = { L"puNES", L"NSFplay" };
  populateCoreCombo(hwnd, IDC_COMBO_NES, g_nesCore, nesOptions, 2);

  const wchar_t* fdsOptions[] = { L"puNES", L"NSFplay" };
  populateCoreCombo(hwnd, IDC_COMBO_FDS, g_fdsCore, fdsOptions, 2);

  const wchar_t* sidOptions[] = { L"reSID", L"reSIDfp", L"dSID" };
  populateCoreCombo(hwnd, IDC_COMBO_SID, g_c64Core, sidOptions, 3);

  const wchar_t* pokeyOptions[] = { L"Atari800 (mzpokeysnd)", L"ASAP (C++ port)" };
  populateCoreCombo(hwnd, IDC_COMBO_POKEY, g_pokeyCore, pokeyOptions, 2);

  // OPN family
  const wchar_t* opnOptions[] = { L"ymfm", L"Nuked-OPN2 (FM) + ymfm (SSG/ADPCM)", L"YM2608-LLE" };
  populateCoreCombo(hwnd, IDC_COMBO_OPN, g_opnCore, opnOptions, 3);

  // OPL family
  const wchar_t* oplOptions[] = { L"Nuked OPL3 (FM) + openMSX (PCM)", L"ymfm" };
  populateCoreCombo(hwnd, IDC_COMBO_OPL, g_opl2Core, oplOptions, 2);

  const wchar_t* opl4Options[] = { L"MAME" };
  populateCoreCombo(hwnd, IDC_COMBO_OPL4, g_opl4Core, opl4Options, 1);

  const wchar_t* esOptions[] = { L"ESFMu", L"ESFMu (fast)" };
  populateCoreCombo(hwnd, IDC_COMBO_ESFM, g_esfmCore, esOptions, 2);

  const wchar_t* opllOptions[] = { L"Nuked-OPLL", L"emu2413" };
  populateCoreCombo(hwnd, IDC_COMBO_OPLL, g_opllCore, opllOptions, 2);

  const wchar_t* ayOptions[] = { L"MAME", L"AtomicSSG" };
  populateCoreCombo(hwnd, IDC_COMBO_AY, g_ayCore, ayOptions, 2);

  const wchar_t* swanOptions[] = { L"asiekierka new core", L"Mednafen" };
  populateCoreCombo(hwnd, IDC_COMBO_WONDERSWAN, g_swanCore, swanOptions, 2);

  // Quality settings
  populateQualityCombo(hwnd, IDC_COMBO_GB, g_gbQuality, 6);
  populateQualityCombo(hwnd, IDC_COMBO_POWERNOISE, g_pnQuality, 6);
  populateQualityCombo(hwnd, IDC_COMBO_SAA1099, g_saaQuality, 6);
  populateQualityCombo(hwnd, IDC_COMBO_DSID, g_dsidQuality, 6);
}

INT_PTR CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            initDialogFromConfig(hwnd);
            return TRUE;
        }

        case WM_COMMAND: {
            int ctrlId = LOWORD(wParam);
            int notif = HIWORD(wParam);

            // Handle checkbox changes
            if (notif == BN_CLICKED) {
                switch (ctrlId) {
                    case IDC_AUDIO_DCOFFSET_CORRECTION:
                        MessageBoxW(hwnd, L"Enabling/Disabling DC offset correction will restart playback.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
                        g_audioHiPass = IsDlgButtonChecked(hwnd, IDC_AUDIO_DCOFFSET_CORRECTION) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("audioHiPass");
                        restartFurnace();
                        break;
                    case IDC_AUDIO_FORCE_MONO:
                        MessageBoxW(hwnd, L"Switching from/to mono will restart playback.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
                        g_forceMono = IsDlgButtonChecked(hwnd, IDC_AUDIO_FORCE_MONO) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("forceMono");
                        restartFurnace();
                        break;
                    case IDC_AUDIO_LOOP_FOREVER:
                        g_loopForever = IsDlgButtonChecked(hwnd, IDC_AUDIO_LOOP_FOREVER) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("loopForever");
                        break;
                    case IDC_AUDIO_END_AT_0BXX:
                        if (g_endAt0BXX) MessageBoxW(hwnd, L"Unchecking this will cause songs with 0Bxx to be played forever, regardless of the 'Loop forever' setting.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
                        g_endAt0BXX = IsDlgButtonChecked(hwnd, IDC_AUDIO_END_AT_0BXX) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("endAt0BXX");
                        break;
                    case IDC_AUDIO_CHANNEL_COUNT_AS_KBPS:
                        g_channelCountAsKbps = IsDlgButtonChecked(hwnd, IDC_AUDIO_CHANNEL_COUNT_AS_KBPS) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("channelCountAsKbps");
                        break;
                    case IDC_AUDIO_USE_MODULE_TIME:
                        if (!g_useModuleTime) MessageBoxW(hwnd, L"This will cause the visualization to be Out Of Sync and appear stuttery/laggy.\nOnly enable this option if you know of the consequences.\n\nThis will use furnace for time keeping instead of relying on the output plugin to do it for us.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);

                        g_useModuleTime = IsDlgButtonChecked(hwnd, IDC_AUDIO_USE_MODULE_TIME) ? 1 : 0;
                        FurnaceConfigManager::applyAndSaveGlobalChange("useModuleTime");
                        break;
                }
            }

            // Handle combo box changes
            if (notif == CBN_SELCHANGE) {
                int sel = SendDlgItemMessageW(hwnd, ctrlId, CB_GETCURSEL, 0, 0);
                if (sel < 0) sel = 0;

                switch (ctrlId) {
                    case IDC_COMBO_SAMPLERATE: {
                        int rates[] = { 8000, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000 };
                        MessageBoxW(hwnd, L"Sample rate change will restart playback.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
                        FURNACE_SAMPLERATE = rates[sel];
                        FurnaceConfigManager::applyAndSaveGlobalChange("audioRate");
                        restartFurnace();
                        break;
                    }
                    case IDC_COMBO_QUALITY:
                        MessageBoxW(hwnd, L"Changing the quality will restart playback.", L"Furnace for Winamp", MB_OK | MB_ICONINFORMATION);
                        g_audioQuality = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("audioQuality");
                        restartFurnace();
                        break;
                    case IDC_COMBO_YM2151:
                        g_opn1Core = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("opn1Core");
                        break;
                    case IDC_COMBO_YM2612:
                        g_ym2612Core = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("ym2612Core");
                        break;
                    case IDC_COMBO_SN76489:
                        g_snCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("snCore");
                        break;
                    case IDC_COMBO_NES:
                        g_nesCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("nesCore");
                        break;
                    case IDC_COMBO_FDS:
                        g_fdsCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("fdsCore");
                        break;
                    case IDC_COMBO_SID:
                        g_c64Core = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("c64Core");
                        break;
                    case IDC_COMBO_POKEY:
                        g_pokeyCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("pokeyCore");
                        break;
                    case IDC_COMBO_OPN:
                        g_opnCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("opnCore");
                        break;
                    case IDC_COMBO_OPL:
                        g_opl2Core = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("opl2Core");
                        break;
                    case IDC_COMBO_OPL4:
                        g_opl4Core = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("opl4Core");
                        break;
                    case IDC_COMBO_ESFM:
                        g_esfmCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("esfmCore");
                        break;
                    case IDC_COMBO_OPLL:
                        g_opllCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("opllCore");
                        break;
                    case IDC_COMBO_AY:
                        g_ayCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("ayCore");
                        break;
                    case IDC_COMBO_WONDERSWAN:
                        g_swanCore = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("swanCore");
                        break;
                    case IDC_COMBO_GB:
                        g_gbQuality = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("gbQuality");
                        break;
                    case IDC_COMBO_POWERNOISE:
                        g_pnQuality = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("pnQuality");
                        break;
                    case IDC_COMBO_SAA1099:
                        g_saaQuality = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("saaQuality");
                        break;
                    case IDC_COMBO_DSID:
                        g_dsidQuality = sel;
                        FurnaceConfigManager::applyAndSaveGlobalChange("dsidQuality");
                        break;
                }
            }
            break;
        }

        case WM_DESTROY:
            break;

        default:
            return FALSE;
    }

    return FALSE;
}

static int FURNACE_Init() {

  int version = SendMessage(plugin.hMainWindow,WM_WA_IPC,0,IPC_GETVERSION);

  if (version > 0x5066){
    MessageBoxW(plugin.hMainWindow, L"Did you know that you suck for not using Winamp 5.666?\n\nThis plugin will not support anything above 5.666.\nDowngrade to Winamp 5.666 (or get WACUP) and stop being a pleb.\nOnly plebs use shit made by Llama Group- I mean Winamp Group.", L"Furnace for Winamp", MB_OK | MB_ICONERROR);
    ExitProcess(0);
  }

  // Allocate memory for the preferences dialog structure
  prefsRec = (prefsDlgRecW*)GlobalAlloc(GPTR, sizeof(prefsDlgRecW));

  // Populate the preferences dialog structure
  prefsRec->hInst = plugin.hDllInstance; // Assuming your plugin instance
  prefsRec->dlgID = IDC_PREFSPAGE; // Resource identifier of your dialog
  prefsRec->proc = (void *)TestWndProc; // Cast the function pointer to void*
  prefsRec->name = const_cast<wchar_t*>(L"FUR | FURNACE"); // Use wide string literal by prefixing with L
  prefsRec->where = 10; // Add to General Preferences

  SendMessage(plugin.hMainWindow, WM_WA_IPC, reinterpret_cast<WPARAM>(prefsRec), IPC_ADD_PREFS_DLGW);

  initFurnace();

  return IN_INIT_SUCCESS;
}

static void FURNACE_Quit() {
  quitFurnace();
}

// ---------------------------------------------------------------------------
// songLengthMs() / positionMs()
// ---------------------------------------------------------------------------
static int songLengthMs() {
  if (!gTimestampsReady) return 0;
  DivSubSong* sub = gEngine.curSubSong;
  double hz = (double)gEngine.getCurHz();
  if (!sub || hz <= 0.0 || sub->ts.totalTicks == 0) return 0;
  return (int)((double)sub->ts.totalTicks / hz * 1000.0);
}

static int positionMs() {
  DivSubSong* sub = gEngine.curSubSong;
  if (!sub) return 0;

  int order = 0, row = 0, tick = 0, speed = 0;
  gEngine.getPlayPosTick(order, row, tick, speed);

  TimeMicros rowTime = sub->ts.getTimes(order, row);
  int ms = (int)(rowTime.toDouble() * 1000.0);

  double hz = (double)gEngine.getCurHz();
  if (speed > 0 && hz > 0.0) {
    ms += (int)((double)tick / (double)speed / hz * 1000.0);
  }

  return ms;
}

// ---------------------------------------------------------------------------
// Per-file info helper
// Loads a .fur file into a temporary DivEngine (independent of the global
// gEngine that may be playing), calculates timestamps, and returns metadata.
// This is the only correct way to query length/title for playlist entries
// that are NOT the currently-playing track.
// ---------------------------------------------------------------------------
struct FurFileInfo {
  std::wstring title;
  std::wstring author;
  std::wstring album;
  std::wstring systemName;
  std::wstring notes;
  std::wstring subsongName;
  int subsongCount;
  int chans;
  int lengthMs;
  int patterns;
  int patternLength;
  float Hz;
  float BPM;
  int speed;
  int tuning;
};

static bool getFurFileInfo(const in_char* wpath, FurFileInfo& out) {
  // Strip "?subsong=N" before doing any file I/O.
  wchar_t basePath[4096] = {0};
  int subsongIdx = parseSubsongIndex(wpath, basePath, 4096);
  const wchar_t* actualPath = (subsongIdx > 0 || wcsstr(wpath, L"?subsong="))
                              ? basePath : wpath;

  String path = wideToStr(actualPath);

  FILE* f = ps_fopen(path.c_str(), "rb");
  if (!f) return false;

  fseek(f, 0, SEEK_END);
  long flen = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (flen <= 0) { fclose(f); return false; }

  unsigned char* buf = new unsigned char[flen];
  bool ok = (fread(buf, 1, (size_t)flen, f) == (size_t)flen);
  fclose(f);
  if (!ok) { delete[] buf; return false; }

  // Quick magic check — same logic as FURNACE_Play — so we never pass a
  // corrupt buffer to load() and risk a mis-handled exception.
  // Use the clean base path (subsong suffix already stripped) for extension checks.
  const wchar_t* ext = wcsrchr(actualPath, L'.');

  bool isFur = false;
  bool isRaw = false;

  if ((size_t)flen >= 16) {
    if (memcmp(buf, "-Furnace module-", 16) == 0) {
      isFur = true;
      isRaw = true;
    } else if (looksLikeZlib(buf, (size_t)flen)) {
      isFur = true;
    }
  }

  // .fur files must pass the header check — if not, they're corrupt.
  // Other formats (dnm, ftm, etc.) are always raw and get compressed unconditionally.
  if (ext && _wcsicmp(ext, L".fur") == 0) {
    if (!isFur) {
      MessageBoxW(plugin.hMainWindow,
                  L"Error while loading file!\n(Not a valid Furnace module.)",
                  L"Furnace for Winamp", MB_OK | MB_ICONERROR);
      delete[] buf;
      return 1;
    }
  }

  size_t bufLen = (size_t)flen;
  if (isRaw || !isFur) {  // raw .fur or any other format — compress before loading
    size_t compressedLen = 0;
    unsigned char* compressed = compressRawFur(buf, flen, compressedLen);
    if (!compressed || compressedLen == 0) { delete[] buf; return 1; }
    delete[] buf;
    buf = compressed;
    bufLen = compressedLen;
  }

  // Temporary engine — fully isolated from gEngine.
  // IMPORTANT: DivEngine is ~870 KB. Declaring it as a local variable would
  // blow the default Windows thread stack (~1 MB) the moment this function
  // is entered — __chkstk_ms would fault before a single line of our code
  // executes. Always heap-allocate it.
  static DivEngine* eng    = nullptr;
  static bool       engOK  = false;

  if (!eng) {
    eng = new DivEngine();
    engOK = eng->init();
  }
  if (!engOK) { delete[] buf; return false; }

  if (!eng->load(buf, bufLen, path.c_str())) { delete[] buf; return false; }

  // calcSongTimestamps ONLY computes ts.totalTicks for whichever subsong
  // curSubSong points to at call time — it does NOT iterate all subsongs.
  // Select the target subsong first so timestamps are computed for it.
  // Then re-select after, because calcSongTimestamps may reset curSubSong
  // as a side-effect of its internal tick simulation.
  int targetIdx = (subsongIdx > 0 && subsongIdx < (int)eng->song.subsong.size())
                  ? subsongIdx : 0;
  selectSubsong(eng, targetIdx);
  eng->calcSongTimestamps();
  selectSubsong(eng, targetIdx);  // re-select; calcSongTimestamps may have changed it

  // Song metadata
  WString wname   = utf8To16(eng->song.name.c_str());
  WString wauthor = utf8To16(eng->song.author.c_str());
  WString walbum  = utf8To16(eng->song.category.c_str());
  WString wsys    = utf8To16(eng->song.systemName.c_str());
  WString wnotes  = utf8To16(eng->song.notes.c_str());

  out.title        = wname.c_str();
  out.author       = wauthor.c_str();
  out.album        = walbum.c_str();
  out.systemName   = wsys.c_str();
  out.notes        = wnotes.c_str();
  out.subsongCount = (int)eng->song.subsong.size();
  out.subsongName  = utf8To16(eng->curSubSong->name.c_str()).c_str();
  out.chans        = eng->song.chans;
  out.patterns     = eng->curSubSong->ordersLen;
  out.patternLength= eng->curSubSong->patLen * out.patterns;
  out.Hz           = eng->curSubSong->hz;
  out.BPM          = calcBPM(eng, eng->curSubSong->speeds,eng->curSubSong->hz,eng->curSubSong->virtualTempoN,eng->curSubSong->virtualTempoD);
  out.speed        = calcSpeed(eng, eng->curSubSong->speeds);
  out.tuning       = eng->song.tuning;

  // Length — use curSubSong->hz directly; getCurHz() reads dispatch state
  // that is never initialised in this headless engine (play() is never called).
  DivSubSong* sub = eng->curSubSong;
  double hz = (sub && sub->hz > 0.0f) ? (double)sub->hz : 60.0;
  out.lengthMs = 0;
  if (sub && hz > 0.0 && sub->ts.totalTicks > 0)
    out.lengthMs = (int)((double)sub->ts.totalTicks / hz * 1000.0);

  //eng->quit(false);
  //free(eng);
  //delete eng;
  return true;
}

static void FURNACE_GetFileInfo(const in_char* file, in_char* title, int* length_in_ms) {
  // Strip "?subsong=N" so isCurrent comparisons and file I/O work correctly.
  wchar_t basePath[4096] = {0};
  int subsongIdx = 0;
  const in_char* resolvedFile = file;
  if (file && file[0] && wcsstr(file, L"?subsong=")) {
    subsongIdx   = parseSubsongIndex(file, basePath, 4096);
    resolvedFile = basePath;
  }

  const in_char* target = (resolvedFile && resolvedFile[0]) ? resolvedFile : gCurrentFile;

  bool isCurrent = (!resolvedFile || !resolvedFile[0] ||
                    (wcscmp(resolvedFile, gCurrentFile) == 0 &&
                     subsongIdx == gCurrentSubsong));

  if (isCurrent) {
    // Fast path: data is already in gEngine — no disk I/O needed.
    if (title) {
      WString wname   = utf8To16(gEngine.song.name.c_str());
      WString wauthor = utf8To16(gEngine.song.author.c_str());
      // When the module has multiple subsongs, prefix the title with the
      // subsong name so each playlist entry is individually identifiable.
      bool multiSub = ((int)gEngine.song.subsong.size() > 1);
      WString wsubname = multiSub
                         ? utf8To16(gEngine.curSubSong->name.c_str())
                         : WString();

      if (!wname.empty()) {
        if (multiSub) {
          const wchar_t* sname = wsubname.empty() ? L"<no name>" : wsubname.c_str();
          if (!wauthor.empty())
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s - %s [%s]", wauthor.c_str(), wname.c_str(), sname);
          else
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s [%s]", wname.c_str(), sname);
        } else {
          if (!wauthor.empty())
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s - %s", wauthor.c_str(), wname.c_str());
          else
            wcsncpy(title, wname.c_str(), GETFILEINFO_TITLE_LENGTH - 1);
        }
      } else {
        // Fall back to filename stem when the module has no internal name.
        const in_char* slash = wcsrchr(target, L'\\');
        if (!slash) slash = wcsrchr(target, L'/');
        wcsncpy(title, slash ? slash + 1 : target, GETFILEINFO_TITLE_LENGTH - 1);
        in_char* dot = wcsrchr(title, L'.');
        if (dot) *dot = L'\0';
      }
      title[GETFILEINFO_TITLE_LENGTH - 1] = L'\0';
    }
    if (length_in_ms) *length_in_ms = songLengthMs();
    return;
  }

  // Slow path: a different playlist entry — load it in a temporary engine.
  // Pass the original virtual path (with ?subsong=N if present) so that
  // getFurFileInfo switches to the correct subsong before reading metadata.
  const in_char* virtualPath = (file && file[0]) ? file : gCurrentFile;
  FurFileInfo info = {};
  if (getFurFileInfo(virtualPath, info)) {
    if (title) {
      if (!info.title.empty()) {
        bool multiSub = (info.subsongCount > 1);
        if (multiSub) {
          const wchar_t* sname = info.subsongName.empty()
                                 ? L"<no name>" : info.subsongName.c_str();
          if (!info.author.empty())
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s - %s [%s]", info.author.c_str(), info.title.c_str(), sname);
          else
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s [%s]", info.title.c_str(), sname);
        } else {
          if (!info.author.empty())
            _snwprintf(title, GETFILEINFO_TITLE_LENGTH - 1,
                       L"%s - %s", info.author.c_str(), info.title.c_str());
          else
            wcsncpy(title, info.title.c_str(), GETFILEINFO_TITLE_LENGTH - 1);
        }
      } else {
        const in_char* slash = wcsrchr(target, L'\\');
        if (!slash) slash = wcsrchr(target, L'/');
        wcsncpy(title, slash ? slash + 1 : target, GETFILEINFO_TITLE_LENGTH - 1);
        in_char* dot = wcsrchr(title, L'.');
        if (dot) *dot = L'\0';
      }
      title[GETFILEINFO_TITLE_LENGTH - 1] = L'\0';
    }
    if (length_in_ms) *length_in_ms = info.lengthMs;
  } else {
    // Could not read the file — provide a safe fallback.
    if (title) {
      const in_char* slash = wcsrchr(target, L'\\');
      if (!slash) slash = wcsrchr(target, L'/');
      wcsncpy(title, slash ? slash + 1 : target, GETFILEINFO_TITLE_LENGTH - 1);
      in_char* dot = wcsrchr(title, L'.');
      if (dot) *dot = L'\0';
      title[GETFILEINFO_TITLE_LENGTH - 1] = L'\0';
    }
    if (length_in_ms) *length_in_ms = -1;   // unknown
  }
}

static int FURNACE_InfoBox(const in_char* file, HWND hwndParent) {
  // Strip "?subsong=N" so the correct file is opened and the correct
  // subsong is shown.  getFurFileInfo handles the full virtual path itself.
  wchar_t basePath[4096] = {0};
  const in_char* resolvedFile = file;
  if (file && file[0] && wcsstr(file, L"?subsong=")) {
    parseSubsongIndex(file, basePath, 4096);
    resolvedFile = basePath;
  }

  const in_char* target = (resolvedFile && resolvedFile[0]) ? resolvedFile : gCurrentFile;
  bool isCurrent = (!resolvedFile || !resolvedFile[0] ||
                    (wcscmp(resolvedFile, gCurrentFile) == 0));

  FurFileInfo info = {};

  if (isCurrent) {
    // Pull directly from the already-loaded engine.
    WString wname   = utf8To16(gEngine.song.name.c_str());
    WString wauthor = utf8To16(gEngine.song.author.c_str());
    WString walbum  = utf8To16(gEngine.song.category.c_str());
    WString wsys    = utf8To16(gEngine.song.systemName.c_str());
    WString wnotes  = utf8To16(gEngine.song.notes.c_str());
    info.title       = wname.c_str();
    info.author      = wauthor.c_str();
    info.album       = walbum.c_str();
    info.systemName  = wsys.c_str();
    info.notes       = wnotes.c_str();
    info.subsongCount = (int)gEngine.song.subsong.size();
    info.chans        = gEngine.song.chans;
    info.lengthMs     = songLengthMs();
  } else {
    // Pass the full virtual path (with ?subsong=N) so getFurFileInfo
    // switches to the right subsong before reading metadata.
    const in_char* infoPath = (file && file[0]) ? file : gCurrentFile;
    getFurFileInfo(infoPath, info);
  }

  int ms   = info.lengthMs;
  int secs = ms / 1000;

  // Build the display title: internal name if present, else filename stem.
  wchar_t displayTitle[GETFILEINFO_TITLE_LENGTH] = {0};
  if (!info.title.empty()) {
    wcsncpy(displayTitle, info.title.c_str(), GETFILEINFO_TITLE_LENGTH - 1);
  } else {
    const in_char* slash = wcsrchr(target, L'\\');
    if (!slash) slash = wcsrchr(target, L'/');
    wcsncpy(displayTitle, slash ? slash + 1 : target, GETFILEINFO_TITLE_LENGTH - 1);
    wchar_t* dot = wcsrchr(displayTitle, L'.');
    if (dot) *dot = L'\0';
  }

  wchar_t msg[2048] = {0};
  int pos = 0;

  pos += _snwprintf(msg + pos, 2047 - pos,
                    L"File:       %s\n"
                    L"Title:      %s\n",
                    target,
                    displayTitle);

  if (!info.author.empty())
    pos += _snwprintf(msg + pos, 2047 - pos, L"Author:     %s\n", info.author.c_str());

  if (!info.album.empty())
    pos += _snwprintf(msg + pos, 2047 - pos, L"Album:      %s\n", info.album.c_str());

  if (!info.systemName.empty())
    pos += _snwprintf(msg + pos, 2047 - pos, L"System:     %s\n", info.systemName.c_str());

  pos += _snwprintf(msg + pos, 2047 - pos,
                    L"Channels:   %d\n"
                    L"Subsongs:   %d\n"
                    L"Length:     %d:%02d\n",
                    info.chans,
                    info.subsongCount,
                    secs / 60, secs % 60);

  if (!info.notes.empty()) {
    std::wstring trimmed = info.notes;
    pos += _snwprintf(msg + pos, 2047 - pos, L"\nNotes:\n%s", trimmed.c_str());
  }

  msg[2047] = L'\0';
  MessageBoxW(hwndParent, msg, L"Furnace Track Info", MB_OK);
  return INFOBOX_UNCHANGED;
}

static int FURNACE_IsOurFile(const in_char* fn) {
  // Strip "?subsong=N" before the extension check so virtual paths like
  // "song.fur?subsong=2" are correctly recognised.
  wchar_t base[4096] = {0};
  const wchar_t* checkPath = fn;
  if (fn && wcsstr(fn, L"?subsong=")) {
    parseSubsongIndex(fn, base, 4096);
    checkPath = base;
  }
  const in_char* ext = wcsrchr(checkPath, L'.');
  if (!ext) return 0;
  return (_wcsicmp(ext, L".fur") == 0 ||
  _wcsicmp(ext, L".0cc") == 0 ||
  _wcsicmp(ext, L".dnm") == 0 ||
  _wcsicmp(ext, L".ftm") == 0 ||
  _wcsicmp(ext, L".dmf") == 0) ? 1 : 0;
}

void reportError(String what) {
  logE("Furnace plugin: %s", what.c_str());
}

static int FURNACE_Play(const in_char* fn) {
  //MessageBoxW(plugin.hMainWindow, L"FURNACE_Play", L"", MB_OK);

  // Parse "?subsong=N" from the path so virtual playlist entries work.
  wchar_t basePath[4096] = {0};
  int subsongIdx = parseSubsongIndex(fn, basePath, 4096);
  // actualFn is the real file path without the virtual suffix.
  const wchar_t* actualFn = (subsongIdx > 0 || wcsstr(fn, L"?subsong="))
                            ? basePath : fn;

  if (gPlayThread) {
    HANDLE h = gPlayThread;
    gStopFlag = 1;
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    gPlayThread = NULL;
    gEngine.quitDispatch();
  }
  // Always quit the dispatch before loading a new song. If a play thread was
  // running we need it to tear down the previous chip state; if this is the
  // first play, gEngine.init() left a dispatch live that load() must not
  // double-initialize on top of.
  //gEngine.quitDispatch();
  gStopFlag = 0; gPauseFlag = 0; gSeekTo = -1; gTimestampsReady = 0;

  // Store the base file path (without ?subsong=N) so isCurrent checks work.
  wcsncpy(gCurrentFile, actualFn, 4095);
  gCurrentFile[4095] = L'\0';
  gCurrentSubsong = subsongIdx;

  String path = wideToStr(actualFn);
  FILE* f = ps_fopen(path.c_str(), "rb");
  if (!f) return -1;

  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
  if (len <= 0) { fclose(f); return 1; }

  unsigned char* buf = new unsigned char[len];
  bool ok = (fread(buf, 1, (size_t)len, f) == (size_t)len);
  fclose(f);

  if (!ok) { delete[] buf; return 1; }

  // Validate the file header before calling gEngine.load().
  // gEngine.load() throws NotZlibException for corrupt/non-fur files, and
  // due to a mingw-w64 SEH/DWARF mismatch the unwinder crashes before it
  // can reach any catch block in our code. Rejecting bad files here means
  // load() is never called and the throw never happens.
  //
  // Valid .fur files start with either:
  //   "-Furnace module-"  (raw, uncompressed)
  //   0x78 ??             (zlib-compressed; CMF low nibble == 8 = deflate)
  const wchar_t* ext = wcsrchr(actualFn, L'.');  // use clean path, no ?subsong= suffix

  bool isFur = false;
  bool isRaw = false;

  if ((size_t)len >= 16) {
    if (memcmp(buf, "-Furnace module-", 16) == 0) {
      isFur = true;
      isRaw = true;
    } else if (looksLikeZlib(buf, (size_t)len)) {
      isFur = true;
    }
  }

  // .fur files must pass the header check — if not, they're corrupt.
  // Other formats (dnm, ftm, etc.) are always raw and get compressed unconditionally.
  if (ext && _wcsicmp(ext, L".fur") == 0) {
    if (!isFur) {
      MessageBoxW(plugin.hMainWindow,
                  L"Error while loading file!\n(Not a valid Furnace module.)",
                  L"Furnace for Winamp", MB_OK | MB_ICONERROR);
      delete[] buf;
      return 1; // or false/nullptr depending on context
    }
  }

  size_t bufLen = (size_t)len;
  if (isRaw || !isFur) {  // raw .fur or any other format — compress before loading
    size_t compressedLen = 0;
    unsigned char* compressed = compressRawFur(buf, len, compressedLen);
    if (!compressed || compressedLen == 0) { delete[] buf; return 1; }
    delete[] buf;
    buf = compressed;
    bufLen = compressedLen;
  }

  bool loaded = gEngine.load(buf, bufLen, path.c_str());

  if (!loaded) {
    wchar_t msg[512];
    _snwprintf(msg, 511, L"Error while loading file!\n(Failed to load for some reason.)");
    msg[511] = L'\0';
    MessageBoxW(plugin.hMainWindow, msg, L"Furnace for Winamp", MB_OK | MB_ICONERROR);
    delete[] buf;
    gEngine.everythingOK();
    return 1;
  }

  int maxLatencyMs = plugin.outMod->Open(FURNACE_SAMPLERATE, FURNACE_CHANNELS(), FURNACE_BITDEPTH, -1, -1);
  if (maxLatencyMs < 0) return 0;

  CHAR info[32];
  sprintf(info, "maxlatency: %d\n", maxLatencyMs);
  //MessageBox(plugin.hMainWindow, info, "", MB_OK | MB_ICONERROR);

  OutputDebugString(info);

  plugin.SAVSAInit(maxLatencyMs, FURNACE_SAMPLERATE);
  plugin.VSASetInfo(FURNACE_SAMPLERATE, FURNACE_CHANNELS());

  //plugin.SetInfo(gEngine.song.chans, FURNACE_SAMPLERATE / 1000, FURNACE_CHANNELS, 1);
  // Select the target subsong BEFORE calcSongTimestamps so timestamps are
  // computed for the correct subsong. calcSongTimestamps only runs for
  // curSubSong at call time — it does not iterate all subsongs.
  // After calcSongTimestamps, re-select because it may reset curSubSong.
  // The final selectSubsong before play() ensures play()/reset() commits
  // the correct subsong's speed, groove, hz, and virtualTempo to chip state.
  if (subsongIdx > 0 && subsongIdx < (int)gEngine.song.subsong.size())
    selectSubsong(&gEngine, subsongIdx);

  gEngine.calcSongTimestamps();
  gTimestampsReady = 1;

  // Expand multi-subsong modules into the playlist (subsong 0 only, once).
  if (subsongIdx == 0 && (int)gEngine.song.subsong.size() > 1
      && !subsongAlreadyInPlaylist(actualFn))
    expandSubsongsIntoPlaylist(actualFn);

  // Re-select after calcSongTimestamps (which may have reset curSubSong),
  // so play() initialises the chip from the correct subsong.
  selectSubsong(&gEngine, subsongIdx < (int)gEngine.song.subsong.size()
                          ? subsongIdx : 0);

  gEngine.play();

  unsigned tid = 0;
  gPlayThread = (HANDLE)_beginthreadex(NULL, 0, playThreadProc, NULL, 0, &tid);
  if (!gPlayThread) return 1;
  SetThreadPriority(gPlayThread, THREAD_PRIORITY_ABOVE_NORMAL);
  return 0;
}

static void FURNACE_Pause()    { gPauseFlag = 1; plugin.outMod->Pause(1); }
static void FURNACE_UnPause()  { gPauseFlag = 0; plugin.outMod->Pause(0); }
static int  FURNACE_IsPaused() { return gPauseFlag; }

static void FURNACE_Stop() {
  if (!gPlayThread) return;
  HANDLE h = gPlayThread;
  gStopFlag = 1;
  WaitForSingleObject(h, 5000);
  CloseHandle(h);
  gPlayThread = NULL;
  gStopFlag   = 0;
}

static int  FURNACE_GetLength()     { return songLengthMs(); }
static int  FURNACE_GetOutputTime() {
  return g_useModuleTime ? positionMs() : plugin.outMod->GetOutputTime();
}
static void FURNACE_SetOutputTime(int ms) { gSeekTo = ms; }
static void FURNACE_SetVolume(int v) { plugin.outMod->SetVolume(v); }
static void FURNACE_SetPan(int p)    { plugin.outMod->SetPan(p); }
static void FURNACE_EQSet(int on, char data[10], int preamp) {}

// ---------------------------------------------------------------------------
// In_Module definition
// ---------------------------------------------------------------------------
In_Module plugin = {
  (int)IN_VER_RET,
  const_cast<wchar_t*>(IN_FURNACE_VERSION),
  0,
  0,
  const_cast<char*>(
    "fur\0Furnace Tracker Module (*.fur)\0"
    "dmf\0DefleMask Module (*.dmf)\0"
    "ftm\0FamiTracker Module (*.ftm)\0"
    "0cc\0OCC-FamiTracker Module (*.0cc)\0"
    "dnm\0Dn-FamiTracker Module (*.dnm)\0\0"
  ),
  1,
  IN_MODULE_FLAG_USES_OUTPUT_PLUGIN,
  FURNACE_Config,
  FURNACE_About,
  FURNACE_Init,
  FURNACE_Quit,
  FURNACE_GetFileInfo,
  FURNACE_InfoBox,
  FURNACE_IsOurFile,
  FURNACE_Play,
  FURNACE_Pause,
  FURNACE_UnPause,
  FURNACE_IsPaused,
  FURNACE_Stop,
  FURNACE_GetLength,
  FURNACE_GetOutputTime,
  FURNACE_SetOutputTime,
  FURNACE_SetVolume,
  FURNACE_SetPan,
  0,0,0,0,0,0,0,0,0, // visualization calls filled in by winamp

  0,0, // dsp calls filled in by winamp

  FURNACE_EQSet,
  NULL,
  0,
  NULL
};

// ---------------------------------------------------------------------------
// Shared metadata resolver (WIDE)
// ---------------------------------------------------------------------------
static int getExtendedFileInfoW_internal(
  const wchar_t* fn,
  const char* key,
  wchar_t* ret,
  int retlen)
{
  if (!ret || retlen <= 0) return 0;
  ret[0] = L'\0';

  // Strip "?subsong=N" so file I/O and isCurrent comparisons work correctly.
  wchar_t basePath[4096] = {0};
  int subsongIdx = 0;
  const wchar_t* resolvedFn = fn;
  if (fn && fn[0] && wcsstr(fn, L"?subsong=")) {
    subsongIdx  = parseSubsongIndex(fn, basePath, 4096);
    resolvedFn  = basePath;
  }

  bool isCurrent = (!resolvedFn || !resolvedFn[0] ||
  (gCurrentFile[0] && wcscmp(resolvedFn, gCurrentFile) == 0 &&
   subsongIdx == gCurrentSubsong));

  FurFileInfo info = {};

  std::wstring fileName;

  // Build filename stem from path
  if (resolvedFn && resolvedFn[0]) {
    const wchar_t* slash = wcsrchr(resolvedFn, L'\\');
    if (!slash) slash = wcsrchr(resolvedFn, L'/');

    fileName = slash ? slash + 1 : resolvedFn;

    size_t dot = fileName.find_last_of(L'.');
    if (dot != std::wstring::npos)
      fileName.erase(dot);
  }

  if (isCurrent && gCurrentFile[0]) {
    // fast path: pull directly from the running engine
    info.title        = utf8To16(gEngine.song.name.c_str()).c_str();
    info.author       = utf8To16(gEngine.song.author.c_str()).c_str();
    info.album        = utf8To16(gEngine.song.category.c_str()).c_str();
    info.systemName   = utf8To16(gEngine.song.systemName.c_str()).c_str();
    info.notes        = utf8To16(gEngine.song.notes.c_str()).c_str();
    info.lengthMs     = songLengthMs();
    info.subsongCount = (int)gEngine.song.subsong.size();
    info.subsongName  = utf8To16(gEngine.curSubSong->name.c_str()).c_str();
    info.chans        = gEngine.song.chans;
    info.patterns     = gEngine.curSubSong->ordersLen;
    info.patternLength= gEngine.curSubSong->patLen * info.patterns;
    info.Hz           = gEngine.curSubSong->hz;
    info.BPM          = calcBPM(&gEngine, gEngine.curSubSong->speeds,gEngine.curSubSong->hz,gEngine.curSubSong->virtualTempoN,gEngine.curSubSong->virtualTempoD);
    info.speed        = calcSpeed(&gEngine, gEngine.curSubSong->speeds);
    info.tuning       = gEngine.song.tuning;
  } else if (fn && fn[0]) {
    // non-current file: read from disk.
    // Pass the original virtual path (with ?subsong=N if present) so that
    // getFurFileInfo switches to the correct subsong.
    if (!getFurFileInfo(fn, info))
      return 0;
  } else {
    // fn is null/empty and nothing is loaded
    return 0;
  }

  // ------------------------------------------------------------
  // key dispatch
  // ------------------------------------------------------------
  CHAR keyy[32];
  sprintf(keyy, key, 0);

  // possible keys (in WACUP and Winamp)
  // artist
  // album
  // albumartist
  // title
  // year
  // genre
  // comment
  // composer
  // publisher
  // disc
  // track
  // bpm
  // GracenoteFileID (Winamp)
  // GracenoteExtData (Winamp)
  // formatinformation
  // replaygain_album_peak (WACUP)
  // replaygain_album_gain
  // replaygain_track_peak (WACUP)
  // replaygain_track_gain
  // rawtag (WACUP)

  OutputDebugString(keyy);
  if (!_stricmp(key, "title")) {
    const wchar_t* finalTitle = !info.title.empty() ? info.title.c_str() : fileName.c_str();

    if (info.subsongCount > 1) {
      if (!info.subsongName.empty())
        _snwprintf(ret, retlen - 1, L"%s [%s]", info.subsongName.c_str(), finalTitle);
        else
          _snwprintf(ret, retlen - 1, L"<no name> [%s]", finalTitle);
    } else {
      _snwprintf(ret, retlen - 1, L"%s", finalTitle);
    }
  } else if (!_stricmp(key, "artist")) {
    _snwprintf(ret, retlen - 1, L"%s", info.author.c_str());

  } else if (!_stricmp(key, "album")) {
    _snwprintf(ret, retlen - 1, L"%s", info.album.c_str());

  } else if (!_stricmp(key, "comment")) {
    _snwprintf(ret, retlen - 1, L"%s", info.notes.c_str());

  } else if (!_stricmp(key, "length")) {
    _snwprintf(ret, retlen - 1, L"%d", info.lengthMs);

  } else if (!_stricmp(key, "bpm")) {
      _snwprintf(ret, retlen - 1, L"%.2f", info.BPM);

  } else if (!_stricmp(key, "rawtag")) {
    std::wstring out;
    wchar_t tmp[128];

    _snwprintf(tmp, 127, L"%.2f", info.BPM);
    out += std::wstring(L"bpm=") + tmp + L"\n";
    out += std::wstring(L"system=") + info.systemName.c_str() + L"\n";
    _snwprintf(tmp, 127, L"%d", info.chans);
    out += std::wstring(L"channels=") + tmp + L"\n";
    _snwprintf(tmp, 127, L"%.2f", info.Hz);
    out += std::wstring(L"tickrate=") + tmp + L"\n";

    wcsncpy(ret, out.c_str(), retlen - 1);
    ret[retlen - 1] = L'\0';

  } else if (!_stricmp(key, "formatinformation")) {
    wchar_t fmtbuf[256] = {0};
    if (info.subsongCount > 1) {
      _snwprintf(fmtbuf, 255, L"System: %s\nSubsongs: %d\nChannels: %d\n\nEstimated length: %s\nPatterns: %d\nTotal length (in rows): %d\n\nTick rate: %.2fHz\nSpeed: %d\nTuning (A-4): %dHz",
                 info.systemName.c_str(), info.subsongCount, info.chans, msToMMSS(info.lengthMs), info.patterns, info.patternLength, info.Hz, info.speed, info.tuning);
    } else {
      _snwprintf(fmtbuf, 255, L"System: %s\nChannels: %d\n\nEstimated length: %s\nPatterns: %d\nTotal length (in rows): %d\n\nTick rate: %.2fHz\nSpeed: %d\nTuning (A-4): %dHz",
                 info.systemName.c_str(), info.chans, msToMMSS(info.lengthMs), info.patterns, info.patternLength, info.Hz, info.speed, info.tuning);
    }
    _snwprintf(ret, retlen - 1, L"%s", fmtbuf);

  } else {
    return 0;
  }

  ret[retlen - 1] = L'\0';
  return 1;
}

// ---------------------------------------------------------------------------
// Extended read handle — used for transcoding / library ripping.
// ---------------------------------------------------------------------------
struct FurExtReadHandle {
  DivEngine* eng;
  int        channels;
  int        sampleRate;
  bool       done;
  int        prevOrder;
  int        prevRow;
  float      chL[RENDER_CHUNK_SAMPLES];
  float      chR[RENDER_CHUNK_SAMPLES];
  int        samplesGenerated;
};

static FurExtReadHandle* extReadOpenW(const wchar_t* fn,
                                      int* size, int* bps,
                                      int* nch,  int* srate) {
  if (!fn || !fn[0]) return nullptr;

  // Strip "?subsong=N" so file I/O operates on the real path.
  wchar_t basePath[4096] = {0};
  int subsongIdx = parseSubsongIndex(fn, basePath, 4096);
  const wchar_t* actualFn = (subsongIdx > 0 || wcsstr(fn, L"?subsong="))
                            ? basePath : fn;

  String path = wideToStr(actualFn);
  FILE* f = ps_fopen(path.c_str(), "rb");
  if (!f) return nullptr;

  fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
  if (flen <= 0) { fclose(f); return nullptr; }

  unsigned char* buf = new unsigned char[flen];
  bool ok = (fread(buf, 1, (size_t)flen, f) == (size_t)flen);
  fclose(f);
  if (!ok) { delete[] buf; return nullptr; }

  bool isFur = false;
  bool isRaw = false;
  if ((size_t)flen >= 16) {
    if (memcmp(buf, "-Furnace module-", 16) == 0) {
      isFur = true;
      isRaw = true;
    } else if ((buf[0] & 0x0F) == 0x08) {
      isFur = true;
    }
  }

  // If raw, compress it before loading
  size_t bufLen = flen;
  if (isRaw || !isFur) {
    size_t compressedLen = 0;
    unsigned char* compressed = compressRawFur(buf, flen, compressedLen);
    if (!compressed || compressedLen == 0) {
      delete[] buf;
      return nullptr;
    }
    delete[] buf;
    buf = compressed;
    bufLen = compressedLen;
  }

  // Fresh engine per handle — each transcoding session is fully independent.
  // NOT static: the handle owns this engine and destroys it on close/EOF.
  // Heap-allocated: DivEngine is ~870 KB and would overflow the stack.
  static DivEngine* eng    = nullptr;
  static bool       engOK  = false;
  if (!eng) {
    eng = new DivEngine();
    FurnaceConfigManager::applyGlobalConfigToEngine(eng);
    engOK = eng->init();
  }
  FurnaceConfigManager::applyGlobalConfigToEngine(eng);
  if (!engOK) { delete[] buf; return nullptr; }

  if (!eng->load(buf, bufLen, path.c_str())) { delete[] buf; return nullptr; }

  int extTargetIdx = (subsongIdx > 0 && subsongIdx < (int)eng->song.subsong.size())
                     ? subsongIdx : 0;
  selectSubsong(eng, extTargetIdx);
  eng->calcSongTimestamps();
  selectSubsong(eng, extTargetIdx);

  int channels   = 2;
  int sampleRate = 44100;

  DivSubSong* sub = eng->curSubSong;
  double      hz  = (sub && sub->hz > 0.0f) ? (double)sub->hz : 60.0;
  int lengthMs = (sub && hz > 0.0 && sub->ts.totalTicks > 0)
    ? (int)((double)sub->ts.totalTicks / hz * 1000.0) : 0;

  int totalBytes = (lengthMs > 0)
    ? (int)((int64_t)lengthMs * sampleRate / 1000 * channels * (FURNACE_BITDEPTH / 8))
    : -1;

  FurExtReadHandle* h = new FurExtReadHandle();
  h->eng              = eng;
  h->channels         = channels;
  h->sampleRate       = sampleRate;
  h->done             = false;
  h->prevOrder        = -1;
  h->prevRow          = -1;
  h->samplesGenerated = 0;
  memset(h->chL, 0, sizeof(h->chL));
  memset(h->chR, 0, sizeof(h->chR));

  if (size)  *size  = totalBytes;
  if (bps)   *bps   = FURNACE_BITDEPTH;
  if (nch)   *nch   = channels;
  if (srate) *srate = sampleRate;

  eng->play();
  return h;
}

// ---------------------------------------------------------------------------
// DLL exports
// ---------------------------------------------------------------------------
extern "C" {

  __declspec(dllexport) In_Module* winampGetInModule2() { return &plugin; }

  __declspec(dllexport) int winampUninstallPlugin(
    HINSTANCE, HWND, int) { return IN_PLUGIN_UNINSTALL_NOW; }

  __declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t*) {
    return 1;
  }

  __declspec(dllexport) int winampGetExtendedFileInfoW(
    wchar_t* filename, char* metadata, wchar_t* ret, int retlen) {
    return getExtendedFileInfoW_internal(filename, metadata, ret, retlen);  // ← was getMetadataW
  }

  __declspec(dllexport) int winampGetExtendedFileInfo(
    char* filename, char* metadata, char* ret, int retlen) {
    if (!ret || retlen <= 0) return 0;
    ret[0] = '\0';
    wchar_t wpath[4096] = {0};
    if (filename && filename[0])
      MultiByteToWideChar(CP_ACP, 0, filename, -1, wpath, 4095);
    wchar_t wret[4096] = {0};
    int r = getExtendedFileInfoW_internal(filename ? wpath : nullptr, metadata, wret, 4095);  // ← was getMetadataW
    if (r)
      WideCharToMultiByte(CP_ACP, 0, wret, -1, ret, retlen - 1, NULL, NULL);
    ret[retlen - 1] = '\0';
    return r;
  }

  __declspec(dllexport) void* winampGetExtendedRead_openW(
      const wchar_t* fn, int* size, int* bps, int* nch, int* srate) {
    return (void*)extReadOpenW(fn, size, bps, nch, srate);
  }

  __declspec(dllexport) void* winampGetExtendedRead_open(
      const char* fn, int* size, int* bps, int* nch, int* srate) {
    if (!fn || !fn[0]) return nullptr;
    wchar_t wpath[4096] = {0};
    MultiByteToWideChar(CP_ACP, 0, fn, -1, wpath, 4095);
    return (void*)extReadOpenW(wpath, size, bps, nch, srate);
  }

  __declspec(dllexport) size_t winampGetExtendedRead_getData(
    void* handle,
    char* dest,
    size_t len,
    int* killswitch)
  {
    FurExtReadHandle* h = (FurExtReadHandle*)handle;

    int order = 0, row = 0, tick = 0, speed = 0;

    // -------------------------------------------------------------------------
    // Validate arguments.
    // -------------------------------------------------------------------------
    if (!h || !dest || len == 0)
      return 0;

    // -------------------------------------------------------------------------
    // Already finished previously.
    // Return 0 so Winamp treats this as EOF.
    // -------------------------------------------------------------------------
    if (h->done)
      return 0;

    const int frameBytes =
    h->channels * (FURNACE_BITDEPTH / 8);

    size_t written = 0;

    DivSubSong* sub = h->eng->curSubSong;

    //double hz = (double)h->eng->getCurHz();

    //int totalSamples = (sub && hz > 0.0 && sub->ts.totalTicks > 0) ? (int)((double)sub->ts.totalTicks / hz * h->sampleRate) : 0;

    // -------------------------------------------------------------------------
    // Fill buffer until:
    // - buffer full
    // - EOF reached
    // - Winamp requests abort
    // -------------------------------------------------------------------------
    while (written + (size_t)(RENDER_CHUNK_SAMPLES * frameBytes) <= len) {
      if (killswitch && *killswitch)
        break;

      float* outs[2] = {
        h->chL,
        h->chR
      };


      // ---------------------------------------------------------------------
      // query playback position.
      // ---------------------------------------------------------------------
        h->eng->getPlayPosTick(order, row, tick, speed);

        // we dont seek, or pause, or throttle

        if (sub) {
          if (hasEffectInCurrentRowExt(h->eng, 0xFF)) {
            // Handle FFxx (stop song) detection
            h->done = true;
            break;
          }

          if ( (hasEffectInCurrentRowExt(h->eng, 0x0B)) && (order == sub->ordersLen - 1) ) {
            // Handle 0Bxx detection
            h->done = true;
            break;
          }
        }

      // ---------------------------------------------------------------------
      // Generate audio.
      // ---------------------------------------------------------------------
      int orderBefore = order, rowBefore = row;
      h->eng->nextBuf(NULL, outs, 0, h->channels, RENDER_CHUNK_SAMPLES);

      short* out16 = (short*)(dest + written);

      for (int i = 0; i < RENDER_CHUNK_SAMPLES; i++) {
        if (h->channels == 2) {
          float l = h->chL[i]; if (l > 1.f)  l = 1.f; if (l < -1.f) l = -1.f;
          float r = h->chR[i]; if (r > 1.f)  r = 1.f; if (r < -1.f) r = -1.f;
          out16[i * 2 + 0] = (short)(l * 32767.f);
          out16[i * 2 + 1] = (short)(r * 32767.f);
        } else { // there simply is no conceivable way we'll have more than 2 channels
          // and if you think otherwise, you're wrong and stupid
          float l = h->chL[i]; if (l > 1.f)  l = 1.f; if (l < -1.f) l = -1.f;
          float r = h->chR[i]; if (r > 1.f)  r = 1.f; if (r < -1.f) r = -1.f;
          out16[i] = ((short)(l * 32767.f) + (short)(r * 32767.f)) / 2;
        }
      }

      if (sub) {
        int orderAfter = 0, rowAfter = 0, tickAfter = 0, speedAfter = 0;
        h->eng->getPlayPosTick(orderAfter, rowAfter, tickAfter, speedAfter);

        bool wrapped = false;
        if (sub->ordersLen > 1) {
          // multi-order: order went backwards (looped)
          if (orderBefore == sub->ordersLen - 1 && orderAfter < orderBefore)
            wrapped = true;
        } else {
          // single-order: row went backwards within the pattern
          if (rowAfter < rowBefore)
            wrapped = true;
        }

        if (wrapped) {
          h->done = true;
          break;
        }

        h->prevOrder = orderAfter;
        h->prevRow   = rowAfter;
        order     = orderAfter;
        row       = rowAfter;
      }


      written +=
      RENDER_CHUNK_SAMPLES * frameBytes;

      // -----------------------------------------------------------------------
      // Stop at computed song duration.
      // -----------------------------------------------------------------------
      /*h->samplesGenerated +=
      RENDER_CHUNK_SAMPLES;

      if (totalSamples > 0 &&
        h->samplesGenerated >= totalSamples)
      {
        int excess =
        h->samplesGenerated - totalSamples;

        int bytesToRemove =
        excess * frameBytes;

        if ((size_t)bytesToRemove <= written)
          written -= bytesToRemove;

        h->done = true;

        // ---------------------------------------------------------------
        // Return short read / partial buffer.
        // ---------------------------------------------------------------
        break;
      }*/

      // -----------------------------------------------------------------------
      // Winamp requested abort/cancel.
      // -----------------------------------------------------------------------
      if (killswitch && *killswitch)
        break;
    }

    // -------------------------------------------------------------------------
    // Do NOT destroy engine here.
    //
    // Winamp may call getData() again after EOF.
    // Cleanup belongs in close().
    // -------------------------------------------------------------------------

    return written;
  }

  __declspec(dllexport) int winampGetExtendedRead_setTime(
      void* handle, int time_in_ms) {
    FurExtReadHandle* h = (FurExtReadHandle*)handle;
    if (!h || !h->eng) return 0;
    DivSubSong* sub = h->eng->curSubSong;
    if (!sub || sub->ordersLen == 0) return 0;
    int64_t targetUs = (int64_t)time_in_ms * 1000;
    int bestOrd = 0;
    for (int o = 1; o < sub->ordersLen; o++) {
      TimeMicros t = sub->ts.getTimes(o, 0);
      if ((int64_t)t.seconds * 1000000LL + t.micros > targetUs) break;
      bestOrd = o;
    }
    h->eng->setOrder((unsigned char)bestOrd);
    h->prevOrder = -1; h->prevRow = -1; h->done = false;
    return 1;
  }

  __declspec(dllexport) void winampGetExtendedRead_close(void* handle) {
    FurExtReadHandle* h = (FurExtReadHandle*)handle;
    if (!h) return;
    if (h->eng) {
      h->eng->stop();
      //h->eng->quit(false);
      //delete h->eng;
    }
    //delete h;
  }

}
