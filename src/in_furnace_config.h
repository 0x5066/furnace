/**
 * Furnace Tracker - Winamp Input Plugin Config Manager
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * Licensed under the GNU General Public License v2 or later.
 *
 * Win32 INI file reader/writer for plugin configuration.
 */

#ifndef _IN_FURNACE_CONFIG_H
#define _IN_FURNACE_CONFIG_H

#include <windows.h>
#include <wchar.h>
#include "engine/engine.h"
#include "thirdparty/wacup/wa_ipc.h"

// Global configuration variables (extern; defined in in_furnace_config.cpp)
// ============================================================================
extern int FURNACE_SAMPLERATE;

// Audio settings
extern int g_audioHiPass;
extern int g_audioQuality;
extern int g_forceMono;

// Playback behaviour
extern int g_loopForever;         // loop indefinitely instead of stopping at song end
extern int g_endAt0BXX;           // treat 0Bxx (jump-to-order) as a stop signal
extern int g_channelCountAsKbps;  // show chip channel count in Winamp's kbps field
extern int g_useModuleTime;       // use module timestamp for GetOutputTime instead of outMod

// System cores
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

/**
 * FurnaceConfigManager - Manages reading/writing plugin config to INI file.
 * Uses Windows API (WritePrivateProfileString, GetPrivateProfileString) to
 * interface with a .ini file stored in the Winamp INI directory.
 */
class FurnaceConfigManager {
public:
  // Default config keys and their default values
  struct ConfigEntry {
    const char* key;
    int defaultValue;
  };
    static DivEngine* gEngine;
  /**
   * Initialize the config manager with a DivEngine pointer and Winamp window handle.
   * Uses IPC_GETINIDIRECTORYW to get the directory from Winamp where INI files are stored.
   */
  static void init(DivEngine* engine, HWND hwnd_winamp);

  /**
   * Load all configuration from the INI file into the engine.
   * Creates the INI file with defaults if it doesn't exist.
   * Returns true on success.
   */
  static bool loadConfig();

  /**
   * Save all current engine configuration to the INI file.
   * Returns true on success.
   */
  static bool saveConfig();

  /**
   * Get a single config option from the INI file.
   * Does not modify the engine; purely reads from disk.
   * Returns the value as a wide string, or defaultValue if not found.
   */
  static std::wstring getConfigValue(const wchar_t* key, const wchar_t* defaultValue = L"");

  /**
   * Set a single config option in the INI file.
   * Immediately writes to disk; does not require saveConfig() call.
   */
  static void setConfigValue(const wchar_t* key, const wchar_t* value);

  /**
   * Helper for UI: update a config variable and immediately persist it.
   * This is useful when a UI element changes a global variable.
   * The global is assumed to already be updated; this syncs engine and saves INI.
   */
  static void applyAndSaveGlobalChange(const char* configKey);

  static void applyGlobalConfigToEngine(DivEngine* eng);

private:
  static wchar_t gConfigPath[MAX_PATH];

  // Helper: convert wide string to int
  static int wstoi(const wchar_t* str);

  // Helper: convert int to wide string
  static void itows(int val, wchar_t* buf, size_t buflen);

  // INI section name
  static const wchar_t* SECTION_NAME;
};

#endif
