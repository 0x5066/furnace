/**
 * Furnace Tracker - Winamp Input Plugin Config Manager
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * Licensed under the GNU General Public License v2 or later.
 *
 * Win32 INI file reader/writer for plugin configuration.
 */

#include "in_furnace_config.h"
#include <stdio.h>
#include <string.h>

// Static member initialization
DivEngine* FurnaceConfigManager::gEngine = NULL;
wchar_t FurnaceConfigManager::gConfigPath[MAX_PATH] = {0};
const wchar_t* FurnaceConfigManager::SECTION_NAME = L"FurnaceConfig";

// Global configuration variables
// ============================================================================
int FURNACE_SAMPLERATE;

// Audio settings
int g_audioHiPass = 0;
int g_audioQuality = 0;
int g_forceMono = 0;

// Playback behaviour
int g_loopForever = 0;
int g_endAt0BXX = 1;
int g_channelCountAsKbps = 0;
int g_useModuleTime = 0;

// System cores
int g_arcadeCore = 0;
int g_ayCore = 0;
int g_bubsysQuality = 3;
int g_c64Core = 1;
int g_dsidQuality = 3;
int g_esfmCore = 0;
int g_fdsCore = 1;
int g_gbQuality = 3;
int g_ndsQuality = 3;
int g_nesCore = 1;
int g_opl2Core = 0;
int g_opl3Core = 0;
int g_opl4Core = 0;
int g_opllCore = 0;
int g_opn1Core = 1;
int g_opnCore = 1;
int g_opnaCore = 1;
int g_opnbCore = 1;
int g_pcSpeakerOutMethod = 4;
int g_pceQuality = 3;
int g_pnQuality = 3;
int g_pokeyCore = 1;
int g_s3mOPL3 = 1;
int g_saaCore = 0;
int g_saaQuality = 3;
int g_smQuality = 3;
int g_snCore = 0;
int g_swanCore = 0;
int g_swanQuality = 3;
int g_vbQuality = 3;
int g_ym2612Core = 0;

// List of all configurable options that should be saved/loaded
static const FurnaceConfigManager::ConfigEntry CONFIG_ENTRIES[] = {
  // Audio settings
  { "audioHiPass", 0 },
  { "audioQuality", 0 },
  { "audioRate", 44100 },
  { "forceMono", 0 },

  // Playback behaviour (loopForever, endAt0BXX, channelCountAsKbps, useModuleTime) are
  // plugin-only flags — they don't exist in the engine. Saved/loaded directly via Win32
  // INI functions in loadConfig/saveConfig, not through this table.

  // System cores
  { "arcadeCore", 0 },
  { "ayCore", 0 },
  { "bubsysQuality", 3 },
  { "c64Core", 1 },
  { "dsidQuality", 3 },
  { "esfmCore", 0 },
  { "fdsCore", 1 },
  { "gbQuality", 3 },
  { "ndsQuality", 3 },
  { "nesCore", 1 },
  { "opl2Core", 0 },
  { "opl3Core", 0 },
  { "opl4Core", 0 },
  { "opllCore", 0 },
  { "opn1Core", 1 },
  { "opnCore", 1 },
  { "opnaCore", 1 },
  { "opnbCore", 1 },
  { "pcSpeakerOutMethod", 4 },
  { "pceQuality", 3 },
  { "pnQuality", 3 },
  { "pokeyCore", 1 },
  { "s3mOPL3", 1 },
  { "saaCore", 0 },
  { "saaQuality", 3 },
  { "smQuality", 3 },
  { "snCore", 0 },
  { "swanCore", 0 },
  { "swanQuality", 3 },
  { "vbQuality", 3 },
  { "ym2612Core", 0 },

  // Terminator
  { NULL, 0 }
};

void FurnaceConfigManager::init(DivEngine* engine, HWND hwnd_winamp) {
  gEngine = engine;

  // Get INI directory from Winamp via IPC message
  wchar_t iniDir[MAX_PATH] = {0};
  wchar_t* dirResult = (wchar_t*)SendMessageW(hwnd_winamp, WM_USER, (WPARAM)iniDir, (LPARAM)IPC_GETINIDIRECTORYW);

  // If SendMessage returned a pointer to a string, use it; otherwise use the buffer we passed
  if (dirResult && dirResult != (wchar_t*)1) {
    wcsncpy(iniDir, dirResult, MAX_PATH - 1);
  }

  // If we got a valid directory, construct the full INI path
  if (iniDir[0] != L'\0') {
    // Ensure the directory path ends with a backslash
    size_t len = wcslen(iniDir);
    if (len > 0 && iniDir[len - 1] != L'\\') {
      iniDir[len] = L'\\';
      iniDir[len + 1] = L'\0';
    }
    _snwprintf(gConfigPath, MAX_PATH - 1, L"%sin_furnace.ini", iniDir);
  } else {
    // Fallback: if IPC call failed, use a default path in current directory
    _snwprintf(gConfigPath, MAX_PATH - 1, L"in_furnace.ini");
  }
  gConfigPath[MAX_PATH - 1] = L'\0';
}

int FurnaceConfigManager::wstoi(const wchar_t* str) {
  if (!str || !str[0]) return 0;
  return _wtoi(str);
}

void FurnaceConfigManager::itows(int val, wchar_t* buf, size_t buflen) {
  _snwprintf(buf, buflen - 1, L"%d", val);
  buf[buflen - 1] = L'\0';
}

// Helper: Sync all global config variables from engine
static void syncGlobalsFromEngine() {
  FURNACE_SAMPLERATE = FurnaceConfigManager::gEngine->getConfInt("audioRate", 44100);

  // Audio settings
  g_audioHiPass = FurnaceConfigManager::gEngine->getConfInt("audioHiPass", 0);
  g_audioQuality = FurnaceConfigManager::gEngine->getConfInt("audioQuality", 0);
  g_forceMono = FurnaceConfigManager::gEngine->getConfInt("forceMono", 0);

  // Playback behaviour flags are not in the engine; loaded directly from INI in loadConfig.

  // System cores
  g_arcadeCore = FurnaceConfigManager::gEngine->getConfInt("arcadeCore", 0);
  g_ayCore = FurnaceConfigManager::gEngine->getConfInt("ayCore", 0);
  g_bubsysQuality = FurnaceConfigManager::gEngine->getConfInt("bubsysQuality", 3);
  g_c64Core = FurnaceConfigManager::gEngine->getConfInt("c64Core", 1);
  g_dsidQuality = FurnaceConfigManager::gEngine->getConfInt("dsidQuality", 3);
  g_esfmCore = FurnaceConfigManager::gEngine->getConfInt("esfmCore", 0);
  g_fdsCore = FurnaceConfigManager::gEngine->getConfInt("fdsCore", 1);
  g_gbQuality = FurnaceConfigManager::gEngine->getConfInt("gbQuality", 3);
  g_ndsQuality = FurnaceConfigManager::gEngine->getConfInt("ndsQuality", 3);
  g_nesCore = FurnaceConfigManager::gEngine->getConfInt("nesCore", 1);
  g_opl2Core = FurnaceConfigManager::gEngine->getConfInt("opl2Core", 0);
  g_opl3Core = FurnaceConfigManager::gEngine->getConfInt("opl3Core", 0);
  g_opl4Core = FurnaceConfigManager::gEngine->getConfInt("opl4Core", 0);
  g_opllCore = FurnaceConfigManager::gEngine->getConfInt("opllCore", 0);
  g_opn1Core = FurnaceConfigManager::gEngine->getConfInt("opn1Core", 1);
  g_opnCore = FurnaceConfigManager::gEngine->getConfInt("opnCore", 1);
  g_opnaCore = FurnaceConfigManager::gEngine->getConfInt("opnaCore", 1);
  g_opnbCore = FurnaceConfigManager::gEngine->getConfInt("opnbCore", 1);
  g_pcSpeakerOutMethod = FurnaceConfigManager::gEngine->getConfInt("pcSpeakerOutMethod", 4);
  g_pceQuality = FurnaceConfigManager::gEngine->getConfInt("pceQuality", 3);
  g_pnQuality = FurnaceConfigManager::gEngine->getConfInt("pnQuality", 3);
  g_pokeyCore = FurnaceConfigManager::gEngine->getConfInt("pokeyCore", 1);
  g_s3mOPL3 = FurnaceConfigManager::gEngine->getConfInt("s3mOPL3", 1);
  g_saaCore = FurnaceConfigManager::gEngine->getConfInt("saaCore", 0);
  g_saaQuality = FurnaceConfigManager::gEngine->getConfInt("saaQuality", 3);
  g_smQuality = FurnaceConfigManager::gEngine->getConfInt("smQuality", 3);
  g_snCore = FurnaceConfigManager::gEngine->getConfInt("snCore", 0);
  g_swanCore = FurnaceConfigManager::gEngine->getConfInt("swanCore", 0);
  g_swanQuality = FurnaceConfigManager::gEngine->getConfInt("swanQuality", 3);
  g_vbQuality = FurnaceConfigManager::gEngine->getConfInt("vbQuality", 3);
  g_ym2612Core = FurnaceConfigManager::gEngine->getConfInt("ym2612Core", 0);
}

// Helper: Sync engine config from global variables
static void syncEngineFromGlobals() {
  FurnaceConfigManager::gEngine->setConf("audioRate", FURNACE_SAMPLERATE);

  // Audio settings
  FurnaceConfigManager::gEngine->setConf("audioHiPass", g_audioHiPass);
  FurnaceConfigManager::gEngine->setConf("audioQuality", g_audioQuality);
  FurnaceConfigManager::gEngine->setConf("forceMono", g_forceMono);

  // Playback behaviour flags are not in the engine; saved directly to INI in saveConfig.

  // System cores
  FurnaceConfigManager::gEngine->setConf("arcadeCore", g_arcadeCore);
  FurnaceConfigManager::gEngine->setConf("ayCore", g_ayCore);
  FurnaceConfigManager::gEngine->setConf("bubsysQuality", g_bubsysQuality);
  FurnaceConfigManager::gEngine->setConf("c64Core", g_c64Core);
  FurnaceConfigManager::gEngine->setConf("dsidQuality", g_dsidQuality);
  FurnaceConfigManager::gEngine->setConf("esfmCore", g_esfmCore);
  FurnaceConfigManager::gEngine->setConf("fdsCore", g_fdsCore);
  FurnaceConfigManager::gEngine->setConf("gbQuality", g_gbQuality);
  FurnaceConfigManager::gEngine->setConf("ndsQuality", g_ndsQuality);
  FurnaceConfigManager::gEngine->setConf("nesCore", g_nesCore);
  FurnaceConfigManager::gEngine->setConf("opl2Core", g_opl2Core);
  FurnaceConfigManager::gEngine->setConf("opl3Core", g_opl3Core);
  FurnaceConfigManager::gEngine->setConf("opl4Core", g_opl4Core);
  FurnaceConfigManager::gEngine->setConf("opllCore", g_opllCore);
  FurnaceConfigManager::gEngine->setConf("opn1Core", g_opn1Core);
  FurnaceConfigManager::gEngine->setConf("opnCore", g_opnCore);
  FurnaceConfigManager::gEngine->setConf("opnaCore", g_opnaCore);
  FurnaceConfigManager::gEngine->setConf("opnbCore", g_opnbCore);
  FurnaceConfigManager::gEngine->setConf("pcSpeakerOutMethod", g_pcSpeakerOutMethod);
  FurnaceConfigManager::gEngine->setConf("pceQuality", g_pceQuality);
  FurnaceConfigManager::gEngine->setConf("pnQuality", g_pnQuality);
  FurnaceConfigManager::gEngine->setConf("pokeyCore", g_pokeyCore);
  FurnaceConfigManager::gEngine->setConf("s3mOPL3", g_s3mOPL3);
  FurnaceConfigManager::gEngine->setConf("saaCore", g_saaCore);
  FurnaceConfigManager::gEngine->setConf("saaQuality", g_saaQuality);
  FurnaceConfigManager::gEngine->setConf("smQuality", g_smQuality);
  FurnaceConfigManager::gEngine->setConf("snCore", g_snCore);
  FurnaceConfigManager::gEngine->setConf("swanCore", g_swanCore);
  FurnaceConfigManager::gEngine->setConf("swanQuality", g_swanQuality);
  FurnaceConfigManager::gEngine->setConf("vbQuality", g_vbQuality);
  FurnaceConfigManager::gEngine->setConf("ym2612Core", g_ym2612Core);
}

// Apply the current global plugin config to an arbitrary DivEngine.
// Safe to call from the transcoding thread: no IPC, no SendMessageW,
// no mutation of FurnaceConfigManager state.
void FurnaceConfigManager::applyGlobalConfigToEngine(DivEngine* eng) {
  if (!eng) return;
  eng->setConf("audioRate",          FURNACE_SAMPLERATE);
  eng->setConf("audioHiPass",        g_audioHiPass);
  eng->setConf("audioQuality",       g_audioQuality);
  eng->setConf("forceMono",          g_forceMono);
  eng->setConf("arcadeCore",         g_arcadeCore);
  eng->setConf("ayCore",             g_ayCore);
  eng->setConf("bubsysQuality",      g_bubsysQuality);
  eng->setConf("c64Core",            g_c64Core);
  eng->setConf("dsidQuality",        g_dsidQuality);
  eng->setConf("esfmCore",           g_esfmCore);
  eng->setConf("fdsCore",            g_fdsCore);
  eng->setConf("gbQuality",          g_gbQuality);
  eng->setConf("ndsQuality",         g_ndsQuality);
  eng->setConf("nesCore",            g_nesCore);
  eng->setConf("opl2Core",           g_opl2Core);
  eng->setConf("opl3Core",           g_opl3Core);
  eng->setConf("opl4Core",           g_opl4Core);
  eng->setConf("opllCore",           g_opllCore);
  eng->setConf("opn1Core",           g_opn1Core);
  eng->setConf("opnCore",            g_opnCore);
  eng->setConf("opnaCore",           g_opnaCore);
  eng->setConf("opnbCore",           g_opnbCore);
  eng->setConf("pcSpeakerOutMethod", g_pcSpeakerOutMethod);
  eng->setConf("pceQuality",         g_pceQuality);
  eng->setConf("pnQuality",          g_pnQuality);
  eng->setConf("pokeyCore",          g_pokeyCore);
  eng->setConf("s3mOPL3",            g_s3mOPL3);
  eng->setConf("saaCore",            g_saaCore);
  eng->setConf("saaQuality",         g_saaQuality);
  eng->setConf("smQuality",          g_smQuality);
  eng->setConf("snCore",             g_snCore);
  eng->setConf("swanCore",           g_swanCore);
  eng->setConf("swanQuality",        g_swanQuality);
  eng->setConf("vbQuality",          g_vbQuality);
  eng->setConf("ym2612Core",         g_ym2612Core);
}

bool FurnaceConfigManager::loadConfig() {
  if (!gEngine || !gConfigPath[0]) return false;

  // Iterate through all known config entries
  for (int i = 0; CONFIG_ENTRIES[i].key != NULL; i++) {
    const char* key = CONFIG_ENTRIES[i].key;
    int defaultVal = CONFIG_ENTRIES[i].defaultValue;

    // Convert key to wide string
    wchar_t wideKey[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wideKey, 255);

    // Read from INI file
    wchar_t valueBuf[256] = {0};
    GetPrivateProfileStringW(SECTION_NAME, wideKey, L"", valueBuf, 255, gConfigPath);

    // If found, use it; otherwise use default
    int configValue;
    if (valueBuf[0] != L'\0') {
      configValue = wstoi(valueBuf);
    } else {
      configValue = defaultVal;
    }

    // Apply to engine
    gEngine->setConf(key, configValue);
  }

  // Sync all global config variables from the loaded engine config
  syncGlobalsFromEngine();

  // Plugin-only playback flags — read directly from INI, bypassing the engine.
  g_loopForever        = GetPrivateProfileIntW(SECTION_NAME, L"loopForever",        0, gConfigPath);
  g_endAt0BXX          = GetPrivateProfileIntW(SECTION_NAME, L"endAt0BXX",          1, gConfigPath);
  g_channelCountAsKbps = GetPrivateProfileIntW(SECTION_NAME, L"channelCountAsKbps", 0, gConfigPath);
  g_useModuleTime      = GetPrivateProfileIntW(SECTION_NAME, L"useModuleTime",       0, gConfigPath);

  return true;
}

bool FurnaceConfigManager::saveConfig() {
  if (!gEngine || !gConfigPath[0]) return false;

  // Sync engine config from global variables before saving
  syncEngineFromGlobals();

  // Iterate through all known config entries and save them
  for (int i = 0; CONFIG_ENTRIES[i].key != NULL; i++) {
    const char* key = CONFIG_ENTRIES[i].key;

    // Convert key to wide string
    wchar_t wideKey[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wideKey, 255);

    // Get current value from engine
    int value = gEngine->getConfInt(key, 0);

    // Convert value to wide string
    wchar_t wideValue[256] = {0};
    itows(value, wideValue, 255);

    // Write to INI file
    WritePrivateProfileStringW(SECTION_NAME, wideKey, wideValue, gConfigPath);
  }

  // Plugin-only playback flags — written directly to INI, bypassing the engine.
  wchar_t buf[32];
  _snwprintf(buf, 31, L"%d", g_loopForever);        WritePrivateProfileStringW(SECTION_NAME, L"loopForever",        buf, gConfigPath);
  _snwprintf(buf, 31, L"%d", g_endAt0BXX);          WritePrivateProfileStringW(SECTION_NAME, L"endAt0BXX",          buf, gConfigPath);
  _snwprintf(buf, 31, L"%d", g_channelCountAsKbps); WritePrivateProfileStringW(SECTION_NAME, L"channelCountAsKbps", buf, gConfigPath);
  _snwprintf(buf, 31, L"%d", g_useModuleTime);      WritePrivateProfileStringW(SECTION_NAME, L"useModuleTime",      buf, gConfigPath);

  return true;
}

std::wstring FurnaceConfigManager::getConfigValue(const wchar_t* key, const wchar_t* defaultValue) {
  if (!gConfigPath[0]) return defaultValue;

  wchar_t valueBuf[1024] = {0};
  GetPrivateProfileStringW(SECTION_NAME, key, defaultValue, valueBuf, 1023, gConfigPath);

  return std::wstring(valueBuf);
}

void FurnaceConfigManager::setConfigValue(const wchar_t* key, const wchar_t* value) {
  if (!gConfigPath[0]) return;

  WritePrivateProfileStringW(SECTION_NAME, key, value, gConfigPath);
}

void FurnaceConfigManager::applyAndSaveGlobalChange(const char* configKey) {
  if (!gEngine || !gConfigPath[0]) return;

  // Plugin-only flags: write directly to INI, never touch the engine.
  static const struct { const char* key; int* var; } pluginFlags[] = {
    { "loopForever",        &g_loopForever        },
    { "endAt0BXX",          &g_endAt0BXX          },
    { "channelCountAsKbps", &g_channelCountAsKbps },
    { "useModuleTime",      &g_useModuleTime      },
    { NULL, NULL }
  };
  for (int i = 0; pluginFlags[i].key != NULL; i++) {
    if (strcmp(configKey, pluginFlags[i].key) == 0) {
      wchar_t wideKey[256] = {0};
      MultiByteToWideChar(CP_UTF8, 0, configKey, -1, wideKey, 255);
      wchar_t wideValue[32] = {0};
      _snwprintf(wideValue, 31, L"%d", *pluginFlags[i].var);
      WritePrivateProfileStringW(SECTION_NAME, wideKey, wideValue, gConfigPath);
      return;
    }
  }

  // Engine-backed options: determine the current value and sync through the engine.
  int value = 0;

  if (strcmp(configKey, "audioRate") == 0) {
    value = FURNACE_SAMPLERATE;
  } else if (strcmp(configKey, "audioHiPass") == 0) {
    value = g_audioHiPass;
  } else if (strcmp(configKey, "audioQuality") == 0) {
    value = g_audioQuality;
  } else if (strcmp(configKey, "forceMono") == 0) {
    value = g_forceMono;
  } else {
    // For other keys, sync globals and read from engine
    syncEngineFromGlobals();
    value = gEngine->getConfInt(configKey, 0);
  }

  // Set the value in the engine config
  syncEngineFromGlobals();
  gEngine->setConf(configKey, value);

  // Convert config key to wide string
  wchar_t wideKey[256] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, configKey, -1, wideKey, 255) == 0) {
    return;  // Conversion failed
  }

  // Convert value to wide string and write to INI file
  wchar_t wideValue[256] = {0};
  _snwprintf(wideValue, 255, L"%d", value);
  wideValue[255] = L'\0';

  WritePrivateProfileStringW(SECTION_NAME, wideKey, wideValue, gConfigPath);
}
