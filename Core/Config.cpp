// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "ppsspp_config.h"

#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/URL.h"

#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/CPUDetect.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/LogManager.h"
#include "Common/OSVersion.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/VR/PPSSPPVR.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Loaders.h"
#include "Core/KeyMap.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Instance.h"
#include "GPU/Common/FramebufferManagerCommon.h"

// TODO: Find a better place for this.
http::Downloader g_DownloadManager;

Config g_Config;

static bool jitForcedOff;

// Not in Config.h because it's #included a lot.
struct ConfigPrivate {
	std::mutex recentIsosLock;
	std::mutex recentIsosThreadLock;
	std::thread recentIsosThread;
	bool recentIsosThreadPending = false;

	void ResetRecentIsosThread();
	void SetRecentIsosThread(std::function<void()> f);
};

#ifdef _DEBUG
static const char *logSectionName = "LogDebug";
#else
static const char *logSectionName = "Log";
#endif

struct ConfigSetting {
	enum Type {
		TYPE_TERMINATOR,
		TYPE_BOOL,
		TYPE_INT,
		TYPE_UINT32,
		TYPE_UINT64,
		TYPE_FLOAT,
		TYPE_STRING,
		TYPE_TOUCH_POS,
		TYPE_PATH,
		TYPE_CUSTOM_BUTTON
	};
	union DefaultValue {
		bool b;
		int i;
		uint32_t u;
		uint64_t lu;
		float f;
		const char *s;
		const char *p;  // not sure how much point..
		ConfigTouchPos touchPos;
		ConfigCustomButton customButton;
	};
	union SettingPtr {
		bool *b;
		int *i;
		uint32_t *u;
		uint64_t *lu;
		float *f;
		std::string *s;
		Path *p;
		ConfigTouchPos *touchPos;
		ConfigCustomButton *customButton;
	};

	typedef bool (*BoolDefaultCallback)();
	typedef int (*IntDefaultCallback)();
	typedef uint32_t (*Uint32DefaultCallback)();
	typedef uint64_t (*Uint64DefaultCallback)();
	typedef float (*FloatDefaultCallback)();
	typedef const char *(*StringDefaultCallback)();
	typedef ConfigTouchPos (*TouchPosDefaultCallback)();
	typedef const char *(*PathDefaultCallback)();
	typedef ConfigCustomButton (*CustomButtonDefaultCallback)();

	union DefaultCallback {
		BoolDefaultCallback b;
		IntDefaultCallback i;
		Uint32DefaultCallback u;
		Uint64DefaultCallback lu;
		FloatDefaultCallback f;
		StringDefaultCallback s;
		PathDefaultCallback p;
		TouchPosDefaultCallback touchPos;
		CustomButtonDefaultCallback customButton;
	};

	ConfigSetting(const char *ini, bool *v, bool def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_BOOL), report_(false), save_(save), perGame_(perGame) {
		ptr_.b = v;
		cb_.b = nullptr;
		default_.b = def;
	}

	ConfigSetting(const char *ini, int *v, int def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_INT), report_(false), save_(save), perGame_(perGame) {
		ptr_.i = v;
		cb_.i = nullptr;
		default_.i = def;
	}

	ConfigSetting(const char *ini, int *v, int def, std::function<std::string(int)> transTo, std::function<int(const std::string &)> transFrom, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_INT), report_(false), save_(save), perGame_(perGame), translateTo_(transTo), translateFrom_(transFrom) {
		ptr_.i = v;
		cb_.i = nullptr;
		default_.i = def;
	}

	ConfigSetting(const char *ini, uint32_t *v, uint32_t def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_UINT32), report_(false), save_(save), perGame_(perGame) {
		ptr_.u = v;
		cb_.u = nullptr;
		default_.u = def;
	}

	ConfigSetting(const char *ini, uint64_t *v, uint64_t def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_UINT64), report_(false), save_(save), perGame_(perGame) {
		ptr_.lu = v;
		cb_.lu = nullptr;
		default_.lu = def;
	}

	ConfigSetting(const char *ini, float *v, float def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_FLOAT), report_(false), save_(save), perGame_(perGame) {
		ptr_.f = v;
		cb_.f = nullptr;
		default_.f = def;
	}

	ConfigSetting(const char *ini, std::string *v, const char *def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_STRING), report_(false), save_(save), perGame_(perGame) {
		ptr_.s = v;
		cb_.s = nullptr;
		default_.s = def;
	}

	ConfigSetting(const char *ini, Path *p, const char *def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_PATH), report_(false), save_(save), perGame_(perGame) {
		ptr_.p = p;
		cb_.p = nullptr;
		default_.p = def;
	}

	ConfigSetting(const char *iniX, const char *iniY, const char *iniScale, const char *iniShow, ConfigTouchPos *v, ConfigTouchPos def, bool save = true, bool perGame = false)
		: iniKey_(iniX), ini2_(iniY), ini3_(iniScale), ini4_(iniShow), type_(TYPE_TOUCH_POS), report_(false), save_(save), perGame_(perGame) {
		ptr_.touchPos = v;
		cb_.touchPos = nullptr;
		default_.touchPos = def;
	}

	ConfigSetting(const char *iniKey, const char *iniImage, const char *iniShape, const char *iniToggle, const char *iniRepeat, ConfigCustomButton *v, ConfigCustomButton def, bool save = true, bool perGame = false)
		: iniKey_(iniKey), ini2_(iniImage), ini3_(iniShape), ini4_(iniToggle), ini5_(iniRepeat), type_(TYPE_CUSTOM_BUTTON), report_(false), save_(save), perGame_(perGame) {
		ptr_.customButton = v;
		cb_.customButton = nullptr;
		default_.customButton = def;
	}

	ConfigSetting(const char *ini, bool *v, BoolDefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_BOOL), report_(false), save_(save), perGame_(perGame) {
		ptr_.b = v;
		cb_.b = def;
	}

	ConfigSetting(const char *ini, int *v, IntDefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_INT), report_(false), save_(save), perGame_(perGame) {
		ptr_ .i = v;
		cb_.i = def;
	}

	ConfigSetting(const char *ini, int *v, IntDefaultCallback def, std::function<std::string(int)> transTo, std::function<int(const std::string &)> transFrom, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_INT), report_(false), save_(save), perGame_(perGame), translateTo_(transTo), translateFrom_(transFrom) {
		ptr_.i = v;
		cb_.i = def;
	}

	ConfigSetting(const char *ini, uint32_t *v, Uint32DefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_UINT32), report_(false), save_(save), perGame_(perGame) {
		ptr_ .u = v;
		cb_.u = def;
	}

	ConfigSetting(const char *ini, float *v, FloatDefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_FLOAT), report_(false), save_(save), perGame_(perGame) {
		ptr_.f = v;
		cb_.f = def;
	}

	ConfigSetting(const char *ini, std::string *v, StringDefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(ini), type_(TYPE_STRING), report_(false), save_(save), perGame_(perGame) {
		ptr_.s = v;
		cb_.s = def;
	}

	ConfigSetting(const char *iniX, const char *iniY, const char *iniScale, const char *iniShow, ConfigTouchPos *v, TouchPosDefaultCallback def, bool save = true, bool perGame = false)
		: iniKey_(iniX), ini2_(iniY), ini3_(iniScale), ini4_(iniShow), type_(TYPE_TOUCH_POS), report_(false), save_(save), perGame_(perGame) {
		ptr_.touchPos = v;
		cb_.touchPos = def;
	}

	// Should actually be called ReadFromIni or something.
	bool Get(const Section *section) const {
		switch (type_) {
		case TYPE_BOOL:
			return section->Get(iniKey_, ptr_.b, cb_.b ? cb_.b() : default_.b);

		case TYPE_INT:
			if (translateFrom_) {
				std::string value;
				if (section->Get(iniKey_, &value, nullptr)) {
					*ptr_.i = translateFrom_(value);
					return true;
				}
			}
			return section->Get(iniKey_, ptr_.i, cb_.i ? cb_.i() : default_.i);
		case TYPE_UINT32:
			return section->Get(iniKey_, ptr_.u, cb_.u ? cb_.u() : default_.u);
		case TYPE_UINT64:
			return section->Get(iniKey_, ptr_.lu, cb_.lu ? cb_.lu() : default_.lu);
		case TYPE_FLOAT:
			return section->Get(iniKey_, ptr_.f, cb_.f ? cb_.f() : default_.f);
		case TYPE_STRING:
			return section->Get(iniKey_, ptr_.s, cb_.s ? cb_.s() : default_.s);
		case TYPE_TOUCH_POS:
		{
			ConfigTouchPos defaultTouchPos = cb_.touchPos ? cb_.touchPos() : default_.touchPos;
			section->Get(iniKey_, &ptr_.touchPos->x, defaultTouchPos.x);
			section->Get(ini2_, &ptr_.touchPos->y, defaultTouchPos.y);
			section->Get(ini3_, &ptr_.touchPos->scale, defaultTouchPos.scale);
			if (ini4_) {
				section->Get(ini4_, &ptr_.touchPos->show, defaultTouchPos.show);
			} else {
				ptr_.touchPos->show = defaultTouchPos.show;
			}
			return true;
		}
		case TYPE_PATH:
		{
			std::string tmp;
			bool result = section->Get(iniKey_, &tmp, cb_.p ? cb_.p() : default_.p);
			if (result) {
				*ptr_.p = Path(tmp);
			}
			return result;
		}
		case TYPE_CUSTOM_BUTTON:
		{
			ConfigCustomButton defaultCustomButton = cb_.customButton ? cb_.customButton() : default_.customButton;
			section->Get(iniKey_, &ptr_.customButton->key, defaultCustomButton.key);
			section->Get(ini2_, &ptr_.customButton->image, defaultCustomButton.image);
			section->Get(ini3_, &ptr_.customButton->shape, defaultCustomButton.shape);
			section->Get(ini4_, &ptr_.customButton->toggle, defaultCustomButton.toggle);
			section->Get(ini5_, &ptr_.customButton->repeat, defaultCustomButton.repeat);
			return true;
		}
		default:
			_dbg_assert_msg_(false, "Get(%s): Unexpected ini setting type: %d", iniKey_, (int)type_);
			return false;
		}
	}

	// Yes, this can be const because what's modified is not the ConfigSetting struct, but the value which is stored elsewhere.
	// Should actually be called WriteToIni or something.
	void Set(Section *section) const {
		if (!save_) {
			return;
		}

		switch (type_) {
		case TYPE_BOOL:
			return section->Set(iniKey_, *ptr_.b);
		case TYPE_INT:
			if (translateTo_) {
				std::string value = translateTo_(*ptr_.i);
				return section->Set(iniKey_, value);
			}
			return section->Set(iniKey_, *ptr_.i);
		case TYPE_UINT32:
			return section->Set(iniKey_, *ptr_.u);
		case TYPE_UINT64:
			return section->Set(iniKey_, *ptr_.lu);
		case TYPE_FLOAT:
			return section->Set(iniKey_, *ptr_.f);
		case TYPE_STRING:
			return section->Set(iniKey_, *ptr_.s);
		case TYPE_PATH:
			return section->Set(iniKey_, ptr_.p->ToString());
		case TYPE_TOUCH_POS:
			section->Set(iniKey_, ptr_.touchPos->x);
			section->Set(ini2_, ptr_.touchPos->y);
			section->Set(ini3_, ptr_.touchPos->scale);
			if (ini4_) {
				section->Set(ini4_, ptr_.touchPos->show);
			}
			return;
		case TYPE_CUSTOM_BUTTON:
			section->Set(iniKey_, ptr_.customButton->key);
			section->Set(ini2_, ptr_.customButton->image);
			section->Set(ini3_, ptr_.customButton->shape);
			section->Set(ini4_, ptr_.customButton->toggle);
			section->Set(ini5_, ptr_.customButton->repeat);
			return;
		default:
			_dbg_assert_msg_(false, "Set(%s): Unexpected ini setting type: %d", iniKey_, (int)type_);
			return;
		}
	}

	void RestoreToDefault() const {
		switch (type_) {
		case TYPE_BOOL:   *ptr_.b = cb_.b ? cb_.b() : default_.b; break;
		case TYPE_INT:    *ptr_.i = cb_.i ? cb_.i() : default_.i; break;
		case TYPE_UINT32: *ptr_.u = cb_.u ? cb_.u() : default_.u; break;
		case TYPE_UINT64: *ptr_.lu = cb_.lu ? cb_.lu() : default_.lu; break;
		case TYPE_FLOAT:  *ptr_.f = cb_.f ? cb_.f() : default_.f; break;
		case TYPE_STRING: *ptr_.s = cb_.s ? cb_.s() : default_.s; break;
		case TYPE_TOUCH_POS: *ptr_.touchPos = cb_.touchPos ? cb_.touchPos() : default_.touchPos; break;
		case TYPE_PATH: *ptr_.p = Path(cb_.p ? cb_.p() : default_.p); break;
		case TYPE_CUSTOM_BUTTON: *ptr_.customButton = cb_.customButton ? cb_.customButton() : default_.customButton; break;
		default:
			_dbg_assert_msg_(false, "RestoreToDefault(%s): Unexpected ini setting type: %d", iniKey_, (int)type_);
		}
	}

	void Report(UrlEncoder &data, const std::string &prefix) const {
		if (!report_)
			return;

		switch (type_) {
		case TYPE_BOOL:   return data.Add(prefix + iniKey_, *ptr_.b);
		case TYPE_INT:    return data.Add(prefix + iniKey_, *ptr_.i);
		case TYPE_UINT32: return data.Add(prefix + iniKey_, *ptr_.u);
		case TYPE_UINT64: return data.Add(prefix + iniKey_, *ptr_.lu);
		case TYPE_FLOAT:  return data.Add(prefix + iniKey_, *ptr_.f);
		case TYPE_STRING: return data.Add(prefix + iniKey_, *ptr_.s);
		case TYPE_PATH:   return data.Add(prefix + iniKey_, ptr_.p->ToString());
		case TYPE_TOUCH_POS: return;   // Doesn't report.
		case TYPE_CUSTOM_BUTTON: return; // Doesn't report.
		default:
			_dbg_assert_msg_(false, "Report(%s): Unexpected ini setting type: %d", iniKey_, (int)type_);
			return;
		}
	}

	const char *iniKey_ = nullptr;
	const char *ini2_ = nullptr;
	const char *ini3_ = nullptr;
	const char *ini4_ = nullptr;
	const char *ini5_ = nullptr;

	const Type type_;

	bool report_;
	const bool save_;
	const bool perGame_;
	SettingPtr ptr_;
	DefaultValue default_{};
	DefaultCallback cb_;

	// We only support transform for ints.
	std::function<std::string(int)> translateTo_;
	std::function<int(const std::string &)> translateFrom_;
};

struct ReportedConfigSetting : public ConfigSetting {
	template <typename T1, typename T2>
	ReportedConfigSetting(const char *ini, T1 *v, T2 def, bool save = true, bool perGame = false)
		: ConfigSetting(ini, v, def, save, perGame) {
		report_ = true;
	}

	template <typename T1, typename T2>
	ReportedConfigSetting(const char *ini, T1 *v, T2 def, std::function<std::string(int)> transTo, std::function<int(const std::string &)> transFrom, bool save = true, bool perGame = false)
		: ConfigSetting(ini, v, def, transTo, transFrom, save, perGame) {
		report_ = true;
	}
};

const char *DefaultLangRegion() {
	// Unfortunate default.  There's no need to use bFirstRun, since this is only a default.
	static std::string defaultLangRegion = "en_US";
	std::string langRegion = System_GetProperty(SYSPROP_LANGREGION);
	if (i18nrepo.IniExists(langRegion)) {
		defaultLangRegion = langRegion;
	} else if (langRegion.length() >= 3) {
		// Don't give up.  Let's try a fuzzy match - so nl_BE can match nl_NL.
		IniFile mapping;
		mapping.LoadFromVFS(g_VFS, "langregion.ini");
		std::vector<std::string> keys;
		mapping.GetKeys("LangRegionNames", keys);

		for (std::string key : keys) {
			if (startsWithNoCase(key, langRegion)) {
				// Exact submatch, or different case.  Let's use it.
				defaultLangRegion = key;
				break;
			} else if (startsWithNoCase(key, langRegion.substr(0, 3))) {
				// Best so far.
				defaultLangRegion = key;
			}
		}
	}

	return defaultLangRegion.c_str();
}

std::string CreateRandMAC() {
	std::stringstream randStream;
	srand(time(nullptr));
	for (int i = 0; i < 6; i++) {
		u32 value = rand() % 256;
		if (i == 0) {
			// Making sure the 1st 2-bits on the 1st byte of OUI are zero to prevent issue with some games (ie. Gran Turismo)
			value &= 0xfc;
		}
		if (value <= 15)
			randStream << '0' << std::hex << value;
		else
			randStream << std::hex << value;
		if (i < 5) {
			randStream << ':'; //we need a : between every octet
		}
	}
	return randStream.str();
}

static int DefaultCpuCore() {
#if PPSSPP_ARCH(ARM) || PPSSPP_ARCH(ARM64) || PPSSPP_ARCH(X86) || PPSSPP_ARCH(AMD64)
	if (System_GetPropertyBool(SYSPROP_CAN_JIT))
		return (int)CPUCore::JIT;
	return (int)CPUCore::IR_JIT;
#else
	return (int)CPUCore::IR_JIT;
#endif
}

static bool DefaultCodeGen() {
#if PPSSPP_ARCH(ARM) || PPSSPP_ARCH(ARM64) || PPSSPP_ARCH(X86) || PPSSPP_ARCH(AMD64)
	return true;
#else
	return false;
#endif
}

static bool DefaultEnableStateUndo() {
#ifdef MOBILE_DEVICE
	// Off on mobile to save disk space.
	return false;
#endif
	return true;
}

static float DefaultUISaturation() {
	return IsVREnabled() ? 1.5f : 1.0f;
}

static const ConfigSetting generalSettings[] = {
	ConfigSetting("FirstRun", &g_Config.bFirstRun, true),
	ConfigSetting("RunCount", &g_Config.iRunCount, 0),
	ConfigSetting("Enable Logging", &g_Config.bEnableLogging, true),
	ConfigSetting("AutoRun", &g_Config.bAutoRun, true),
	ConfigSetting("Browse", &g_Config.bBrowse, false),
	ConfigSetting("IgnoreBadMemAccess", &g_Config.bIgnoreBadMemAccess, true, true),
	ConfigSetting("CurrentDirectory", &g_Config.currentDirectory, ""),
	ConfigSetting("ShowDebuggerOnLoad", &g_Config.bShowDebuggerOnLoad, false),
	ConfigSetting("CheckForNewVersion", &g_Config.bCheckForNewVersion, true),
	ConfigSetting("Language", &g_Config.sLanguageIni, &DefaultLangRegion),
	ConfigSetting("ForceLagSync2", &g_Config.bForceLagSync, false, true, true),
	ConfigSetting("DiscordPresence", &g_Config.bDiscordPresence, true, true, false),  // Or maybe it makes sense to have it per-game? Race conditions abound...
	ConfigSetting("UISound", &g_Config.bUISound, false, true, false),

	ConfigSetting("AutoLoadSaveState", &g_Config.iAutoLoadSaveState, 0, true, true),
	ReportedConfigSetting("EnableCheats", &g_Config.bEnableCheats, false, true, true),
	ConfigSetting("CwCheatRefreshRate", &g_Config.iCwCheatRefreshRate, 77, true, true),
	ConfigSetting("CwCheatScrollPosition", &g_Config.fCwCheatScrollPosition, 0.0f, true, true),
	ConfigSetting("GameListScrollPosition", &g_Config.fGameListScrollPosition, 0.0f),

	ConfigSetting("ScreenshotsAsPNG", &g_Config.bScreenshotsAsPNG, false, true, true),
	ConfigSetting("UseFFV1", &g_Config.bUseFFV1, false),
	ConfigSetting("DumpFrames", &g_Config.bDumpFrames, false),
	ConfigSetting("DumpVideoOutput", &g_Config.bDumpVideoOutput, false),
	ConfigSetting("DumpAudio", &g_Config.bDumpAudio, false),
	ConfigSetting("SaveLoadResetsAVdumping", &g_Config.bSaveLoadResetsAVdumping, false),
	ConfigSetting("StateSlot", &g_Config.iCurrentStateSlot, 0, true, true),
	ConfigSetting("EnableStateUndo", &g_Config.bEnableStateUndo, &DefaultEnableStateUndo, true, true),
	ConfigSetting("StateLoadUndoGame", &g_Config.sStateLoadUndoGame, "NA", true, false),
	ConfigSetting("StateUndoLastSaveGame", &g_Config.sStateUndoLastSaveGame, "NA", true, false),
	ConfigSetting("StateUndoLastSaveSlot", &g_Config.iStateUndoLastSaveSlot, -5, true, false), // Start with an "invalid" value
	ConfigSetting("RewindSnapshotInterval", &g_Config.iRewindSnapshotInterval, 0, true, true),

	ConfigSetting("ShowOnScreenMessage", &g_Config.bShowOnScreenMessages, true, true, false),
	ConfigSetting("ShowRegionOnGameIcon", &g_Config.bShowRegionOnGameIcon, false),
	ConfigSetting("ShowIDOnGameIcon", &g_Config.bShowIDOnGameIcon, false),
	ConfigSetting("GameGridScale", &g_Config.fGameGridScale, 1.0),
	ConfigSetting("GridView1", &g_Config.bGridView1, true),
	ConfigSetting("GridView2", &g_Config.bGridView2, true),
	ConfigSetting("GridView3", &g_Config.bGridView3, false),
	ConfigSetting("RightAnalogUp", &g_Config.iRightAnalogUp, 0, true, true),
	ConfigSetting("RightAnalogDown", &g_Config.iRightAnalogDown, 0, true, true),
	ConfigSetting("RightAnalogLeft", &g_Config.iRightAnalogLeft, 0, true, true),
	ConfigSetting("RightAnalogRight", &g_Config.iRightAnalogRight, 0, true, true),
	ConfigSetting("RightAnalogPress", &g_Config.iRightAnalogPress, 0, true, true),
	ConfigSetting("RightAnalogCustom", &g_Config.bRightAnalogCustom, false, true, true),
	ConfigSetting("RightAnalogDisableDiagonal", &g_Config.bRightAnalogDisableDiagonal, false, true, true),
	ConfigSetting("SwipeUp", &g_Config.iSwipeUp, 0, true, true),
	ConfigSetting("SwipeDown", &g_Config.iSwipeDown, 0, true, true),
	ConfigSetting("SwipeLeft", &g_Config.iSwipeLeft, 0, true, true),
	ConfigSetting("SwipeRight", &g_Config.iSwipeRight, 0, true, true),
	ConfigSetting("SwipeSensitivity", &g_Config.fSwipeSensitivity, 1.0f, true, true),
	ConfigSetting("SwipeSmoothing", &g_Config.fSwipeSmoothing, 0.3f, true, true),
	ConfigSetting("DoubleTapGesture", &g_Config.iDoubleTapGesture, 0, true, true),
	ConfigSetting("GestureControlEnabled", &g_Config.bGestureControlEnabled, false, true, true),

	// "default" means let emulator decide, "" means disable.
	ConfigSetting("ReportingHost", &g_Config.sReportHost, "default"),
	ConfigSetting("AutoSaveSymbolMap", &g_Config.bAutoSaveSymbolMap, false, true, true),
	ConfigSetting("CacheFullIsoInRam", &g_Config.bCacheFullIsoInRam, false, true, true),
	ConfigSetting("RemoteISOPort", &g_Config.iRemoteISOPort, 0, true, false),
	ConfigSetting("LastRemoteISOServer", &g_Config.sLastRemoteISOServer, ""),
	ConfigSetting("LastRemoteISOPort", &g_Config.iLastRemoteISOPort, 0),
	ConfigSetting("RemoteISOManualConfig", &g_Config.bRemoteISOManual, false),
	ConfigSetting("RemoteShareOnStartup", &g_Config.bRemoteShareOnStartup, false),
	ConfigSetting("RemoteISOSubdir", &g_Config.sRemoteISOSubdir, "/"),
	ConfigSetting("RemoteDebuggerOnStartup", &g_Config.bRemoteDebuggerOnStartup, false),

#ifdef __ANDROID__
	ConfigSetting("ScreenRotation", &g_Config.iScreenRotation, ROTATION_AUTO_HORIZONTAL),
#endif
	ConfigSetting("InternalScreenRotation", &g_Config.iInternalScreenRotation, ROTATION_LOCKED_HORIZONTAL, true, true),

	ConfigSetting("BackgroundAnimation", &g_Config.iBackgroundAnimation, 1, true, false),
	ConfigSetting("TransparentBackground", &g_Config.bTransparentBackground, true, true, false),
	ConfigSetting("UITint", &g_Config.fUITint, 0.0, true, false),
	ConfigSetting("UISaturation", &g_Config.fUISaturation, &DefaultUISaturation, true, false),

#if defined(USING_WIN_UI)
	ConfigSetting("TopMost", &g_Config.bTopMost, false),
	ConfigSetting("WindowX", &g_Config.iWindowX, -1), // -1 tells us to center the window.
	ConfigSetting("WindowY", &g_Config.iWindowY, -1),
	ConfigSetting("WindowWidth", &g_Config.iWindowWidth, 0),   // 0 will be automatically reset later (need to do the AdjustWindowRect dance).
	ConfigSetting("WindowHeight", &g_Config.iWindowHeight, 0),
	ConfigSetting("PauseOnLostFocus", &g_Config.bPauseOnLostFocus, false, true, true),
#endif
	ConfigSetting("PauseWhenMinimized", &g_Config.bPauseWhenMinimized, false, true, true),
	ConfigSetting("DumpDecryptedEboots", &g_Config.bDumpDecryptedEboot, false, true, true),
	ConfigSetting("FullscreenOnDoubleclick", &g_Config.bFullscreenOnDoubleclick, true, false, false),
	ConfigSetting("ShowMenuBar", &g_Config.bShowMenuBar, true, true, false),

	ReportedConfigSetting("MemStickInserted", &g_Config.bMemStickInserted, true, true, true),
	ConfigSetting("EnablePlugins", &g_Config.bLoadPlugins, true, true, true),

	ReportedConfigSetting("IgnoreCompatSettings", &g_Config.sIgnoreCompatSettings, "", true, true),
};

static bool DefaultSasThread() {
	return cpu_info.num_cores > 1;
}

static const ConfigSetting cpuSettings[] = {
	ReportedConfigSetting("CPUCore", &g_Config.iCpuCore, &DefaultCpuCore, true, true),
	ReportedConfigSetting("SeparateSASThread", &g_Config.bSeparateSASThread, &DefaultSasThread, true, true),
	ReportedConfigSetting("IOTimingMethod", &g_Config.iIOTimingMethod, IOTIMING_FAST, true, true),
	ConfigSetting("FastMemoryAccess", &g_Config.bFastMemory, true, true, true),
	ReportedConfigSetting("FunctionReplacements", &g_Config.bFuncReplacements, true, true, true),
	ConfigSetting("HideSlowWarnings", &g_Config.bHideSlowWarnings, false, true, false),
	ConfigSetting("HideStateWarnings", &g_Config.bHideStateWarnings, false, true, false),
	ConfigSetting("PreloadFunctions", &g_Config.bPreloadFunctions, false, true, true),
	ConfigSetting("JitDisableFlags", &g_Config.uJitDisableFlags, (uint32_t)0, true, true),
	ReportedConfigSetting("CPUSpeed", &g_Config.iLockedCPUSpeed, 0, true, true),
};

static int DefaultInternalResolution() {
	// Auto on Windows and Linux, 2x on large screens, 1x elsewhere.
#if defined(USING_WIN_UI) || defined(USING_QT_UI)
	return 0;
#else
	if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_VR) {
		return 4;
	}
	int longestDisplaySide = std::max(System_GetPropertyInt(SYSPROP_DISPLAY_XRES), System_GetPropertyInt(SYSPROP_DISPLAY_YRES));
	int scale = longestDisplaySide >= 1000 ? 2 : 1;
	INFO_LOG(G3D, "Longest display side: %d pixels. Choosing scale %d", longestDisplaySide, scale);
	return scale;
#endif
}

static int DefaultFastForwardMode() {
#if PPSSPP_PLATFORM(ANDROID) || defined(USING_QT_UI) || PPSSPP_PLATFORM(UWP) || PPSSPP_PLATFORM(IOS)
	return (int)FastForwardMode::SKIP_FLIP;
#else
	return (int)FastForwardMode::CONTINUOUS;
#endif
}

// See issue 14439. Should possibly even block these devices from selecting VK.
const char * const vulkanDefaultBlacklist[] = {
	"Sony:BRAVIA VH1",
};

static int DefaultGPUBackend() {
	if (IsVREnabled()) {
		return (int)GPUBackend::OPENGL;
	}

#if PPSSPP_PLATFORM(WINDOWS)
	// If no Vulkan, use Direct3D 11 on Windows 8+ (most importantly 10.)
	if (DoesVersionMatchWindows(6, 2, 0, 0, true)) {
		return (int)GPUBackend::DIRECT3D11;
	}
#elif PPSSPP_PLATFORM(ANDROID)
	// Check blacklist.
	for (size_t i = 0; i < ARRAY_SIZE(vulkanDefaultBlacklist); i++) {
		if (System_GetProperty(SYSPROP_NAME) == vulkanDefaultBlacklist[i]) {
			return (int)GPUBackend::OPENGL;
		}
	}

	// Default to Vulkan only on Oreo 8.1 (level 27) devices or newer, and only
	// on ARM64 and x86-64. Drivers before, and on other archs, are generally too
	// unreliable to default to (with some exceptions, of course).
#if PPSSPP_ARCH(64BIT)
	if (System_GetPropertyInt(SYSPROP_SYSTEMVERSION) >= 27) {
		return (int)GPUBackend::VULKAN;
	}
#endif

#endif
	// TODO: On some additional Linux platforms, we should also default to Vulkan.
	return (int)GPUBackend::OPENGL;
}

int Config::NextValidBackend() {
	std::vector<std::string> split;
	std::set<GPUBackend> failed;

	SplitString(sFailedGPUBackends, ',', split);
	for (const auto &str : split) {
		if (!str.empty() && str != "ALL") {
			failed.insert(GPUBackendFromString(str));
		}
	}

	// Count these as "failed" too so we don't pick them.
	SplitString(sDisabledGPUBackends, ',', split);
	for (const auto &str : split) {
		if (!str.empty()) {
			failed.insert(GPUBackendFromString(str));
		}
	}

	if (failed.count((GPUBackend)iGPUBackend)) {
		ERROR_LOG(LOADER, "Graphics backend failed for %d, trying another", iGPUBackend);

#if !PPSSPP_PLATFORM(UWP)
		if (!failed.count(GPUBackend::VULKAN) && VulkanMayBeAvailable()) {
			return (int)GPUBackend::VULKAN;
		}
#endif
#if PPSSPP_PLATFORM(WINDOWS)
		if (!failed.count(GPUBackend::DIRECT3D11) && DoesVersionMatchWindows(6, 1, 0, 0, true)) {
			return (int)GPUBackend::DIRECT3D11;
		}
#endif
#if PPSSPP_API(ANY_GL)
		if (!failed.count(GPUBackend::OPENGL)) {
			return (int)GPUBackend::OPENGL;
		}
#endif
#if PPSSPP_API(D3D9)
		if (!failed.count(GPUBackend::DIRECT3D9)) {
			return (int)GPUBackend::DIRECT3D9;
		}
#endif

		// They've all failed.  Let them try the default - or on Android, OpenGL.
		sFailedGPUBackends += ",ALL";
		ERROR_LOG(LOADER, "All graphics backends failed");
#if PPSSPP_PLATFORM(ANDROID)
		return (int)GPUBackend::OPENGL;
#else
		return DefaultGPUBackend();
#endif
	}

	return iGPUBackend;
}

bool Config::IsBackendEnabled(GPUBackend backend, bool validate) {
	std::vector<std::string> split;

	SplitString(sDisabledGPUBackends, ',', split);
	for (const auto &str : split) {
		if (str.empty())
			continue;
		auto match = GPUBackendFromString(str);
		if (match == backend)
			return false;
	}

#if PPSSPP_PLATFORM(UWP)
	if (backend != GPUBackend::DIRECT3D11)
		return false;
#elif PPSSPP_PLATFORM(SWITCH)
	if (backend != GPUBackend::OPENGL)
		return false;
#elif PPSSPP_PLATFORM(WINDOWS)
	if (validate) {
		if (backend == GPUBackend::DIRECT3D11 && !DoesVersionMatchWindows(6, 0, 0, 0, true))
			return false;
	}
#else
	if (backend == GPUBackend::DIRECT3D11 || backend == GPUBackend::DIRECT3D9)
		return false;
#endif

#if !PPSSPP_API(ANY_GL)
	if (backend == GPUBackend::OPENGL)
		return false;
#endif
	if (validate) {
		if (backend == GPUBackend::VULKAN && !VulkanMayBeAvailable())
			return false;
	}

	return true;
}

template <typename T, std::string (*FTo)(T), T (*FFrom)(const std::string &)>
struct ConfigTranslator {
	static std::string To(int v) {
		return StringFromInt(v) + " (" + FTo(T(v)) + ")";
	}

	static int From(const std::string &v) {
		int result;
		if (TryParse(v, &result)) {
			return result;
		}
		return (int)FFrom(v);
	}
};

typedef ConfigTranslator<GPUBackend, GPUBackendToString, GPUBackendFromString> GPUBackendTranslator;

static int FastForwardModeFromString(const std::string &s) {
	if (!strcasecmp(s.c_str(), "CONTINUOUS"))
		return (int)FastForwardMode::CONTINUOUS;
	if (!strcasecmp(s.c_str(), "SKIP_FLIP"))
		return (int)FastForwardMode::SKIP_FLIP;
	return DefaultFastForwardMode();
}

std::string FastForwardModeToString(int v) {
	switch (FastForwardMode(v)) {
	case FastForwardMode::CONTINUOUS:
		return "CONTINUOUS";
	case FastForwardMode::SKIP_FLIP:
		return "SKIP_FLIP";
	}
	return "CONTINUOUS";
}

static const ConfigSetting graphicsSettings[] = {
	ConfigSetting("EnableCardboardVR", &g_Config.bEnableCardboardVR, false, true, true),
	ConfigSetting("CardboardScreenSize", &g_Config.iCardboardScreenSize, 50, true, true),
	ConfigSetting("CardboardXShift", &g_Config.iCardboardXShift, 0, true, true),
	ConfigSetting("CardboardYShift", &g_Config.iCardboardYShift, 0, true, true),
	ConfigSetting("iShowStatusFlags", &g_Config.iShowStatusFlags, 0, true, true),
	ReportedConfigSetting("GraphicsBackend", &g_Config.iGPUBackend, &DefaultGPUBackend, &GPUBackendTranslator::To, &GPUBackendTranslator::From, true, false),
	ConfigSetting("FailedGraphicsBackends", &g_Config.sFailedGPUBackends, ""),
	ConfigSetting("DisabledGraphicsBackends", &g_Config.sDisabledGPUBackends, ""),
	ConfigSetting("VulkanDevice", &g_Config.sVulkanDevice, "", true, false),
#ifdef _WIN32
	ConfigSetting("D3D11Device", &g_Config.sD3D11Device, "", true, false),
#endif
	ConfigSetting("CameraDevice", &g_Config.sCameraDevice, "", true, false),
	ConfigSetting("VendorBugChecksEnabled", &g_Config.bVendorBugChecksEnabled, true, false, false),
	ConfigSetting("UseGeometryShader", &g_Config.bUseGeometryShader, false, true, true),
	ReportedConfigSetting("SkipBufferEffects", &g_Config.bSkipBufferEffects, false, true, true),
	ConfigSetting("SoftwareRenderer", &g_Config.bSoftwareRendering, false, true, true),
	ConfigSetting("SoftwareRendererJit", &g_Config.bSoftwareRenderingJit, true, true, true),
	ReportedConfigSetting("HardwareTransform", &g_Config.bHardwareTransform, true, true, true),
	ReportedConfigSetting("SoftwareSkinning", &g_Config.bSoftwareSkinning, true, true, true),
	ReportedConfigSetting("TextureFiltering", &g_Config.iTexFiltering, 1, true, true),
	ReportedConfigSetting("BufferFiltering", &g_Config.iBufFilter, SCALE_LINEAR, true, true),
	ReportedConfigSetting("InternalResolution", &g_Config.iInternalResolution, &DefaultInternalResolution, true, true),
	ReportedConfigSetting("HighQualityDepth", &g_Config.bHighQualityDepth, true, true, true),
	ReportedConfigSetting("FrameSkip", &g_Config.iFrameSkip, 0, true, true),
	ReportedConfigSetting("FrameSkipType", &g_Config.iFrameSkipType, 0, true, true),
	ReportedConfigSetting("AutoFrameSkip", &g_Config.bAutoFrameSkip, IsVREnabled(), true, true),
	ConfigSetting("StereoRendering", &g_Config.bStereoRendering, false, true, true),
	ConfigSetting("StereoToMonoShader", &g_Config.sStereoToMonoShader, "RedBlue", true, true),
	ConfigSetting("FrameRate", &g_Config.iFpsLimit1, 0, true, true),
	ConfigSetting("FrameRate2", &g_Config.iFpsLimit2, -1, true, true),
	ConfigSetting("AnalogFrameRate", &g_Config.iAnalogFpsLimit, 240, true, true),
	ConfigSetting("AnalogFrameRateMode", &g_Config.iAnalogFpsMode, 0, true, true),
	ConfigSetting("UnthrottlingMode", &g_Config.iFastForwardMode, &DefaultFastForwardMode, &FastForwardModeToString, &FastForwardModeFromString, true, true),
#if defined(USING_WIN_UI)
	ConfigSetting("RestartRequired", &g_Config.bRestartRequired, false, false),
#endif

	// Most low-performance (and many high performance) mobile GPUs do not support aniso anyway so defaulting to 4 is fine.
	ConfigSetting("AnisotropyLevel", &g_Config.iAnisotropyLevel, 4, true, true),
	ConfigSetting("MultiSampleLevel", &g_Config.iMultiSampleLevel, 0, true, true),  // Number of samples is 1 << iMultiSampleLevel

	ReportedConfigSetting("VertexDecCache", &g_Config.bVertexCache, false, true, true),
	ReportedConfigSetting("TextureBackoffCache", &g_Config.bTextureBackoffCache, false, true, true),
	ReportedConfigSetting("VertexDecJit", &g_Config.bVertexDecoderJit, &DefaultCodeGen, false),

#ifndef MOBILE_DEVICE
	ConfigSetting("FullScreen", &g_Config.bFullScreen, false),
	ConfigSetting("FullScreenMulti", &g_Config.bFullScreenMulti, false),
#endif

	ConfigSetting("DisplayOffsetX", &g_Config.fDisplayOffsetX, 0.5f, true, true),
	ConfigSetting("DisplayOffsetY", &g_Config.fDisplayOffsetY, 0.5f, true, true),
	ConfigSetting("DisplayScale", &g_Config.fDisplayScale, 1.0f, true, true),
	ConfigSetting("DisplayAspectRatio", &g_Config.fDisplayAspectRatio, 1.0f, true, true),
	ConfigSetting("DisplayStretch", &g_Config.bDisplayStretch, false, true, true),

	ConfigSetting("ImmersiveMode", &g_Config.bImmersiveMode, true, true, true),
	ConfigSetting("SustainedPerformanceMode", &g_Config.bSustainedPerformanceMode, false, true, true),
	ConfigSetting("IgnoreScreenInsets", &g_Config.bIgnoreScreenInsets, true, true, false),

	ReportedConfigSetting("ReplaceTextures", &g_Config.bReplaceTextures, true, true, true),
	ReportedConfigSetting("SaveNewTextures", &g_Config.bSaveNewTextures, false, true, true),
	ConfigSetting("IgnoreTextureFilenames", &g_Config.bIgnoreTextureFilenames, false, true, true),
	ConfigSetting("ReplaceTexturesAllowLate", &g_Config.bReplaceTexturesAllowLate, true, true, true),

	ReportedConfigSetting("TexScalingLevel", &g_Config.iTexScalingLevel, 1, true, true),
	ReportedConfigSetting("TexScalingType", &g_Config.iTexScalingType, 0, true, true),
	ReportedConfigSetting("TexDeposterize", &g_Config.bTexDeposterize, false, true, true),
	ReportedConfigSetting("TexHardwareScaling", &g_Config.bTexHardwareScaling, false, true, true),
	ConfigSetting("VSyncInterval", &g_Config.bVSync, false, true, true),
	ReportedConfigSetting("BloomHack", &g_Config.iBloomHack, 0, true, true),

	// Not really a graphics setting...
	ReportedConfigSetting("SplineBezierQuality", &g_Config.iSplineBezierQuality, 2, true, true),
	ReportedConfigSetting("HardwareTessellation", &g_Config.bHardwareTessellation, false, true, true),
	ConfigSetting("TextureShader", &g_Config.sTextureShaderName, "Off", true, true),
	ConfigSetting("ShaderChainRequires60FPS", &g_Config.bShaderChainRequires60FPS, false, true, true),

	ReportedConfigSetting("SkipGPUReadbacks", &g_Config.bSkipGPUReadbacks, false, true, true),

	ConfigSetting("GfxDebugOutput", &g_Config.bGfxDebugOutput, false, false, false),
	ConfigSetting("LogFrameDrops", &g_Config.bLogFrameDrops, false, true, false),

	ConfigSetting("InflightFrames", &g_Config.iInflightFrames, 3, true, false),
	ConfigSetting("RenderDuplicateFrames", &g_Config.bRenderDuplicateFrames, false, true, true),

	ConfigSetting("ShaderCache", &g_Config.bShaderCache, true, false, false),  // Doesn't save. Ini-only.
	ConfigSetting("GpuLogProfiler", &g_Config.bGpuLogProfiler, false, true, false),
};

static const ConfigSetting soundSettings[] = {
	ConfigSetting("Enable", &g_Config.bEnableSound, true, true, true),
	ConfigSetting("AudioBackend", &g_Config.iAudioBackend, 0, true, true),
	ConfigSetting("ExtraAudioBuffering", &g_Config.bExtraAudioBuffering, false, true, false),
	ConfigSetting("GlobalVolume", &g_Config.iGlobalVolume, VOLUME_FULL, true, true),
	ConfigSetting("ReverbVolume", &g_Config.iReverbVolume, VOLUME_FULL, true, true),
	ConfigSetting("AltSpeedVolume", &g_Config.iAltSpeedVolume, -1, true, true),
	ConfigSetting("AudioDevice", &g_Config.sAudioDevice, "", true, false),
	ConfigSetting("AutoAudioDevice", &g_Config.bAutoAudioDevice, true, true, false),
};

static bool DefaultShowTouchControls() {
	int deviceType = System_GetPropertyInt(SYSPROP_DEVICE_TYPE);
	if (deviceType == DEVICE_TYPE_MOBILE) {
		std::string name = System_GetProperty(SYSPROP_NAME);
		if (KeyMap::HasBuiltinController(name)) {
			return false;
		} else {
			return true;
		}
	} else if (deviceType == DEVICE_TYPE_TV) {
		return false;
	} else if (deviceType == DEVICE_TYPE_DESKTOP) {
		return false;
	} else if (deviceType == DEVICE_TYPE_VR) {
		return false;
	} else {
		return false;
	}
}

static const float defaultControlScale = 1.15f;
static const ConfigTouchPos defaultTouchPosShow = { -1.0f, -1.0f, defaultControlScale, true };
static const ConfigTouchPos defaultTouchPosHide = { -1.0f, -1.0f, defaultControlScale, false };

static const ConfigSetting controlSettings[] = {
	ConfigSetting("HapticFeedback", &g_Config.bHapticFeedback, false, true, true),
	ConfigSetting("ShowTouchCross", &g_Config.bShowTouchCross, true, true, true),
	ConfigSetting("ShowTouchCircle", &g_Config.bShowTouchCircle, true, true, true),
	ConfigSetting("ShowTouchSquare", &g_Config.bShowTouchSquare, true, true, true),
	ConfigSetting("ShowTouchTriangle", &g_Config.bShowTouchTriangle, true, true, true),

	ConfigSetting("Custom0Mapping", "Custom0Image", "Custom0Shape", "Custom0Toggle", "Custom0Repeat", &g_Config.CustomKey0, {0, 0, 0, false, false}, true, true),
	ConfigSetting("Custom1Mapping", "Custom1Image", "Custom1Shape", "Custom1Toggle", "Custom1Repeat", &g_Config.CustomKey1, {0, 1, 0, false, false}, true, true),
	ConfigSetting("Custom2Mapping", "Custom2Image", "Custom2Shape", "Custom2Toggle", "Custom2Repeat", &g_Config.CustomKey2, {0, 2, 0, false, false}, true, true),
	ConfigSetting("Custom3Mapping", "Custom3Image", "Custom3Shape", "Custom3Toggle", "Custom3Repeat", &g_Config.CustomKey3, {0, 3, 0, false, false}, true, true),
	ConfigSetting("Custom4Mapping", "Custom4Image", "Custom4Shape", "Custom4Toggle", "Custom4Repeat", &g_Config.CustomKey4, {0, 4, 0, false, false}, true, true),
	ConfigSetting("Custom5Mapping", "Custom5Image", "Custom5Shape", "Custom5Toggle", "Custom5Repeat", &g_Config.CustomKey5, {0, 0, 1, false, false}, true, true),
	ConfigSetting("Custom6Mapping", "Custom6Image", "Custom6Shape", "Custom6Toggle", "Custom6Repeat", &g_Config.CustomKey6, {0, 1, 1, false, false}, true, true),
	ConfigSetting("Custom7Mapping", "Custom7Image", "Custom7Shape", "Custom7Toggle", "Custom7Repeat", &g_Config.CustomKey7, {0, 2, 1, false, false}, true, true),
	ConfigSetting("Custom8Mapping", "Custom8Image", "Custom8Shape", "Custom8Toggle", "Custom8Repeat", &g_Config.CustomKey8, {0, 3, 1, false, false}, true, true),
	ConfigSetting("Custom9Mapping", "Custom9Image", "Custom9Shape", "Custom9Toggle", "Custom9Repeat", &g_Config.CustomKey9, {0, 4, 1, false, false}, true, true),

#if defined(_WIN32)
	// A win32 user seeing touch controls is likely using PPSSPP on a tablet. There it makes
	// sense to default this to on.
	ConfigSetting("ShowTouchPause", &g_Config.bShowTouchPause, true, true, false),
#else
	ConfigSetting("ShowTouchPause", &g_Config.bShowTouchPause, false, true, false),
#endif
#if defined(USING_WIN_UI)
	ConfigSetting("IgnoreWindowsKey", &g_Config.bIgnoreWindowsKey, false, true, true),
#endif

	ConfigSetting("ShowTouchControls", &g_Config.bShowTouchControls, &DefaultShowTouchControls, true, true),

	// ConfigSetting("KeyMapping", &g_Config.iMappingMap, 0),

#ifdef MOBILE_DEVICE
	ConfigSetting("TiltBaseAngleY", &g_Config.fTiltBaseAngleY, 0.9f, true, true),
	ConfigSetting("TiltInvertX", &g_Config.bInvertTiltX, false, true, true),
	ConfigSetting("TiltInvertY", &g_Config.bInvertTiltY, false, true, true),
	ConfigSetting("TiltSensitivityX", &g_Config.iTiltSensitivityX, 60, true, true),
	ConfigSetting("TiltSensitivityY", &g_Config.iTiltSensitivityY, 60, true, true),
	ConfigSetting("TiltAnalogDeadzoneRadius", &g_Config.fTiltAnalogDeadzoneRadius, 0.0f, true, true),
	ConfigSetting("TiltInputType", &g_Config.iTiltInputType, 0, true, true),
#endif

	ConfigSetting("DisableDpadDiagonals", &g_Config.bDisableDpadDiagonals, false, true, true),
	ConfigSetting("GamepadOnlyFocused", &g_Config.bGamepadOnlyFocused, false, true, true),
	ConfigSetting("TouchButtonStyle", &g_Config.iTouchButtonStyle, 1, true, true),
	ConfigSetting("TouchButtonOpacity", &g_Config.iTouchButtonOpacity, 65, true, true),
	ConfigSetting("TouchButtonHideSeconds", &g_Config.iTouchButtonHideSeconds, 20, true, true),
	ConfigSetting("AutoCenterTouchAnalog", &g_Config.bAutoCenterTouchAnalog, false, true, true),
	ConfigSetting("AnalogAutoRotSpeed", &g_Config.fAnalogAutoRotSpeed, 8.0f, true, true),

	// Snap touch control position
	ConfigSetting("TouchSnapToGrid", &g_Config.bTouchSnapToGrid, false, true, true),
	ConfigSetting("TouchSnapGridSize", &g_Config.iTouchSnapGridSize, 64, true, true),

	// -1.0f means uninitialized, set in GamepadEmu::CreatePadLayout().
	ConfigSetting("ActionButtonSpacing2", &g_Config.fActionButtonSpacing, 1.0f, true, true),
	ConfigSetting("ActionButtonCenterX", "ActionButtonCenterY", "ActionButtonScale", nullptr, &g_Config.touchActionButtonCenter, defaultTouchPosShow, true, true),
	ConfigSetting("DPadX", "DPadY", "DPadScale", "ShowTouchDpad", &g_Config.touchDpad, defaultTouchPosShow, true, true),

	// Note: these will be overwritten if DPadRadius is set.
	ConfigSetting("DPadSpacing", &g_Config.fDpadSpacing, 1.0f, true, true),
	ConfigSetting("StartKeyX", "StartKeyY", "StartKeyScale", "ShowTouchStart", &g_Config.touchStartKey, defaultTouchPosShow, true, true),
	ConfigSetting("SelectKeyX", "SelectKeyY", "SelectKeyScale", "ShowTouchSelect", &g_Config.touchSelectKey, defaultTouchPosShow, true, true),
	ConfigSetting("UnthrottleKeyX", "UnthrottleKeyY", "UnthrottleKeyScale", "ShowTouchUnthrottle", &g_Config.touchFastForwardKey, defaultTouchPosShow, true, true),
	ConfigSetting("LKeyX", "LKeyY", "LKeyScale", "ShowTouchLTrigger", &g_Config.touchLKey, defaultTouchPosShow, true, true),
	ConfigSetting("RKeyX", "RKeyY", "RKeyScale", "ShowTouchRTrigger", &g_Config.touchRKey, defaultTouchPosShow, true, true),
	ConfigSetting("AnalogStickX", "AnalogStickY", "AnalogStickScale", "ShowAnalogStick", &g_Config.touchAnalogStick, defaultTouchPosShow, true, true),
	ConfigSetting("RightAnalogStickX", "RightAnalogStickY", "RightAnalogStickScale", "ShowRightAnalogStick", &g_Config.touchRightAnalogStick, defaultTouchPosHide, true, true),

	ConfigSetting("fcombo0X", "fcombo0Y", "comboKeyScale0", "ShowComboKey0", &g_Config.touchCombo0, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo1X", "fcombo1Y", "comboKeyScale1", "ShowComboKey1", &g_Config.touchCombo1, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo2X", "fcombo2Y", "comboKeyScale2", "ShowComboKey2", &g_Config.touchCombo2, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo3X", "fcombo3Y", "comboKeyScale3", "ShowComboKey3", &g_Config.touchCombo3, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo4X", "fcombo4Y", "comboKeyScale4", "ShowComboKey4", &g_Config.touchCombo4, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo5X", "fcombo5Y", "comboKeyScale5", "ShowComboKey5", &g_Config.touchCombo5, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo6X", "fcombo6Y", "comboKeyScale6", "ShowComboKey6", &g_Config.touchCombo6, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo7X", "fcombo7Y", "comboKeyScale7", "ShowComboKey7", &g_Config.touchCombo7, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo8X", "fcombo8Y", "comboKeyScale8", "ShowComboKey8", &g_Config.touchCombo8, defaultTouchPosHide, true, true),
	ConfigSetting("fcombo9X", "fcombo9Y", "comboKeyScale9", "ShowComboKey9", &g_Config.touchCombo9, defaultTouchPosHide, true, true),

	ConfigSetting("AnalogDeadzone", &g_Config.fAnalogDeadzone, 0.15f, true, true),
	ConfigSetting("AnalogInverseDeadzone", &g_Config.fAnalogInverseDeadzone, 0.0f, true, true),
	ConfigSetting("AnalogSensitivity", &g_Config.fAnalogSensitivity, 1.1f, true, true),
	ConfigSetting("AnalogIsCircular", &g_Config.bAnalogIsCircular, false , true, true),

	ConfigSetting("AnalogLimiterDeadzone", &g_Config.fAnalogLimiterDeadzone, 0.6f, true, true),

	ConfigSetting("LeftStickHeadScale", &g_Config.fLeftStickHeadScale, 1.0f, true, true),
	ConfigSetting("RightStickHeadScale", &g_Config.fRightStickHeadScale, 1.0f, true, true),
	ConfigSetting("HideStickBackground", &g_Config.bHideStickBackground, false, true, true),

	ConfigSetting("UseMouse", &g_Config.bMouseControl, false, true, true),
	ConfigSetting("MapMouse", &g_Config.bMapMouse, false, true, true),
	ConfigSetting("ConfineMap", &g_Config.bMouseConfine, false, true, true),
	ConfigSetting("MouseSensitivity", &g_Config.fMouseSensitivity, 0.1f, true, true),
	ConfigSetting("MouseSmoothing", &g_Config.fMouseSmoothing, 0.9f, true, true),

	ConfigSetting("SystemControls", &g_Config.bSystemControls, true, true, false),
};

static const ConfigSetting networkSettings[] = {
	ConfigSetting("EnableWlan", &g_Config.bEnableWlan, false, true, true),
	ConfigSetting("EnableAdhocServer", &g_Config.bEnableAdhocServer, false, true, true),
	ConfigSetting("proAdhocServer", &g_Config.proAdhocServer, "socom.cc", true, true),
	ConfigSetting("PortOffset", &g_Config.iPortOffset, 10000, true, true),
	ConfigSetting("MinTimeout", &g_Config.iMinTimeout, 0, true, true),
	ConfigSetting("ForcedFirstConnect", &g_Config.bForcedFirstConnect, false, true, true),
	ConfigSetting("EnableUPnP", &g_Config.bEnableUPnP, false, true, true),
	ConfigSetting("UPnPUseOriginalPort", &g_Config.bUPnPUseOriginalPort, false, true, true),

	ConfigSetting("EnableNetworkChat", &g_Config.bEnableNetworkChat, false, true, true),
	ConfigSetting("ChatButtonPosition",&g_Config.iChatButtonPosition,BOTTOM_LEFT,true,true),
	ConfigSetting("ChatScreenPosition",&g_Config.iChatScreenPosition,BOTTOM_LEFT,true,true),
	ConfigSetting("EnableQuickChat", &g_Config.bEnableQuickChat, true, true, true),
	ConfigSetting("QuickChat1", &g_Config.sQuickChat0, "Quick Chat 1", true, true),
	ConfigSetting("QuickChat2", &g_Config.sQuickChat1, "Quick Chat 2", true, true),
	ConfigSetting("QuickChat3", &g_Config.sQuickChat2, "Quick Chat 3", true, true),
	ConfigSetting("QuickChat4", &g_Config.sQuickChat3, "Quick Chat 4", true, true),
	ConfigSetting("QuickChat5", &g_Config.sQuickChat4, "Quick Chat 5", true, true),
};

static int DefaultSystemParamLanguage() {
	int defaultLang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	if (g_Config.bFirstRun) {
		// TODO: Be smart about same language, different country
		auto &langValuesMapping = g_Config.GetLangValuesMapping();
		auto iter = langValuesMapping.find(g_Config.sLanguageIni);
		if (iter != langValuesMapping.end()) {
			defaultLang = iter->second.second;
		}
	}
	return defaultLang;
}

static const ConfigSetting systemParamSettings[] = {
	ReportedConfigSetting("PSPModel", &g_Config.iPSPModel, PSP_MODEL_SLIM, true, true),
	ReportedConfigSetting("PSPFirmwareVersion", &g_Config.iFirmwareVersion, PSP_DEFAULT_FIRMWARE, true, true),
	ConfigSetting("NickName", &g_Config.sNickName, "PPSSPP", true, true),
	ConfigSetting("MacAddress", &g_Config.sMACAddress, "", true, true),
	ReportedConfigSetting("Language", &g_Config.iLanguage, &DefaultSystemParamLanguage, true, true),
	ConfigSetting("ParamTimeFormat", &g_Config.iTimeFormat, PSP_SYSTEMPARAM_TIME_FORMAT_24HR, true, true),
	ConfigSetting("ParamDateFormat", &g_Config.iDateFormat, PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD, true, true),
	ConfigSetting("TimeZone", &g_Config.iTimeZone, 0, true, true),
	ConfigSetting("DayLightSavings", &g_Config.bDayLightSavings, (bool) PSP_SYSTEMPARAM_DAYLIGHTSAVINGS_STD, true, true),
	ReportedConfigSetting("ButtonPreference", &g_Config.iButtonPreference, PSP_SYSTEMPARAM_BUTTON_CROSS, true, true),
	ConfigSetting("LockParentalLevel", &g_Config.iLockParentalLevel, 0, true, true),
	ConfigSetting("WlanAdhocChannel", &g_Config.iWlanAdhocChannel, PSP_SYSTEMPARAM_ADHOC_CHANNEL_AUTOMATIC, true, true),
#if defined(USING_WIN_UI) || defined(USING_QT_UI) || PPSSPP_PLATFORM(ANDROID)
	ConfigSetting("BypassOSKWithKeyboard", &g_Config.bBypassOSKWithKeyboard, false, true, true),
#endif
	ConfigSetting("WlanPowerSave", &g_Config.bWlanPowerSave, (bool) PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF, true, true),
	ReportedConfigSetting("EncryptSave", &g_Config.bEncryptSave, true, true, true),
	ConfigSetting("SavedataUpgradeVersion", &g_Config.bSavedataUpgrade, true, true, false),
	ConfigSetting("MemStickSize", &g_Config.iMemStickSizeGB, 16, true, false),
};

static const ConfigSetting debuggerSettings[] = {
	ConfigSetting("DisasmWindowX", &g_Config.iDisasmWindowX, -1),
	ConfigSetting("DisasmWindowY", &g_Config.iDisasmWindowY, -1),
	ConfigSetting("DisasmWindowW", &g_Config.iDisasmWindowW, -1),
	ConfigSetting("DisasmWindowH", &g_Config.iDisasmWindowH, -1),
	ConfigSetting("GEWindowX", &g_Config.iGEWindowX, -1),
	ConfigSetting("GEWindowY", &g_Config.iGEWindowY, -1),
	ConfigSetting("GEWindowW", &g_Config.iGEWindowW, -1),
	ConfigSetting("GEWindowH", &g_Config.iGEWindowH, -1),
	ConfigSetting("GEWindowTabsBL", &g_Config.uGETabsLeft, (uint32_t)0),
	ConfigSetting("GEWindowTabsBR", &g_Config.uGETabsRight, (uint32_t)0),
	ConfigSetting("GEWindowTabsTR", &g_Config.uGETabsTopRight, (uint32_t)0),
	ConfigSetting("ConsoleWindowX", &g_Config.iConsoleWindowX, -1),
	ConfigSetting("ConsoleWindowY", &g_Config.iConsoleWindowY, -1),
	ConfigSetting("FontWidth", &g_Config.iFontWidth, 8),
	ConfigSetting("FontHeight", &g_Config.iFontHeight, 12),
	ConfigSetting("DisplayStatusBar", &g_Config.bDisplayStatusBar, true),
	ConfigSetting("ShowBottomTabTitles",&g_Config.bShowBottomTabTitles, true),
	ConfigSetting("ShowDeveloperMenu", &g_Config.bShowDeveloperMenu, false),
	ConfigSetting("ShowAllocatorDebug", &g_Config.bShowAllocatorDebug, false, false),
	ConfigSetting("ShowGpuProfile", &g_Config.bShowGpuProfile, false, false),
	ConfigSetting("SkipDeadbeefFilling", &g_Config.bSkipDeadbeefFilling, false),
	ConfigSetting("FuncHashMap", &g_Config.bFuncHashMap, false),
	ConfigSetting("MemInfoDetailed", &g_Config.bDebugMemInfoDetailed, false),
	ConfigSetting("DrawFrameGraph", &g_Config.bDrawFrameGraph, false),
};

static const ConfigSetting jitSettings[] = {
	ReportedConfigSetting("DiscardRegsOnJRRA", &g_Config.bDiscardRegsOnJRRA, false, false),
};

static const ConfigSetting upgradeSettings[] = {
	ConfigSetting("UpgradeMessage", &g_Config.upgradeMessage, ""),
	ConfigSetting("UpgradeVersion", &g_Config.upgradeVersion, ""),
	ConfigSetting("DismissedVersion", &g_Config.dismissedVersion, ""),
};

static const ConfigSetting themeSettings[] = {
	ConfigSetting("ThemeName", &g_Config.sThemeName, "Default", true, false),
};


static const ConfigSetting vrSettings[] = {
	ConfigSetting("VREnable", &g_Config.bEnableVR, true),
	ConfigSetting("VREnable6DoF", &g_Config.bEnable6DoF, true),
	ConfigSetting("VREnableStereo", &g_Config.bEnableStereo, false),
	ConfigSetting("VREnableMotions", &g_Config.bEnableMotions, true),
	ConfigSetting("VRForce72Hz", &g_Config.bForce72Hz, true),
	ConfigSetting("VRManualForceVR", &g_Config.bManualForceVR, false),
	ConfigSetting("VRCameraDistance", &g_Config.fCameraDistance, 0.0f),
	ConfigSetting("VRCameraHeight", &g_Config.fCameraHeight, 0.0f),
	ConfigSetting("VRCameraSide", &g_Config.fCameraSide, 0.0f),
	ConfigSetting("VRCanvasDistance", &g_Config.fCanvasDistance, 12.0f),
	ConfigSetting("VRFieldOfView", &g_Config.fFieldOfViewPercentage, 100.0f),
	ConfigSetting("VRHeadUpDisplayScale", &g_Config.fHeadUpDisplayScale, 0.3f),
	ConfigSetting("VRMotionLength", &g_Config.fMotionLength, 0.5f),
	ConfigSetting("VRHeadRotationScale", &g_Config.fHeadRotationScale, 5.0f),
	ConfigSetting("VRHeadRotationSmoothing", &g_Config.bHeadRotationSmoothing, false),
	ConfigSetting("VRHeadRotation", &g_Config.iHeadRotation, 0),
};

struct ConfigSectionSettings {
	const char *section;
	const ConfigSetting *settings;
	size_t settingsCount;
};

static const ConfigSectionSettings sections[] = {
	{"General", generalSettings, ARRAY_SIZE(generalSettings)},
	{"CPU", cpuSettings, ARRAY_SIZE(cpuSettings)},
	{"Graphics", graphicsSettings, ARRAY_SIZE(graphicsSettings)},
	{"Sound", soundSettings, ARRAY_SIZE(soundSettings)},
	{"Control", controlSettings, ARRAY_SIZE(controlSettings)},
	{"Network", networkSettings, ARRAY_SIZE(networkSettings)},
	{"SystemParam", systemParamSettings, ARRAY_SIZE(systemParamSettings)},
	{"Debugger", debuggerSettings, ARRAY_SIZE(debuggerSettings)},
	{"JIT", jitSettings, ARRAY_SIZE(jitSettings)},
	{"Upgrade", upgradeSettings, ARRAY_SIZE(upgradeSettings)},
	{"Theme", themeSettings, ARRAY_SIZE(themeSettings)},
	{"VR", vrSettings, ARRAY_SIZE(vrSettings)},
};

static void IterateSettings(IniFile &iniFile, std::function<void(Section *section, const ConfigSetting &setting)> func) {
	for (size_t i = 0; i < ARRAY_SIZE(sections); ++i) {
		Section *section = iniFile.GetOrCreateSection(sections[i].section);
		for (size_t j = 0; j < sections[i].settingsCount; j++) {
			func(section, sections[i].settings[j]);
		}
	}
}

static void IterateSettings(std::function<void(const ConfigSetting &setting)> func) {
	for (size_t i = 0; i < ARRAY_SIZE(sections); ++i) {
		for (size_t j = 0; j < sections[i].settingsCount; j++) {
			func(sections[i].settings[j]);
		}
	}
}

void ConfigPrivate::ResetRecentIsosThread() {
	std::lock_guard<std::mutex> guard(recentIsosThreadLock);
	if (recentIsosThreadPending && recentIsosThread.joinable())
		recentIsosThread.join();
}

void ConfigPrivate::SetRecentIsosThread(std::function<void()> f) {
	std::lock_guard<std::mutex> guard(recentIsosThreadLock);
	if (recentIsosThreadPending && recentIsosThread.joinable())
		recentIsosThread.join();
	recentIsosThread = std::thread(f);
	recentIsosThreadPending = true;
}

Config::Config() {
	private_ = new ConfigPrivate();
}

Config::~Config() {
	if (bUpdatedInstanceCounter) {
		ShutdownInstanceCounter();
	}
	private_->ResetRecentIsosThread();
	delete private_;
}

void Config::LoadLangValuesMapping() {
	IniFile mapping;
	mapping.LoadFromVFS(g_VFS, "langregion.ini");
	std::vector<std::string> keys;
	mapping.GetKeys("LangRegionNames", keys);

	std::map<std::string, int> langCodeMapping;
	langCodeMapping["JAPANESE"] = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
	langCodeMapping["ENGLISH"] = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	langCodeMapping["FRENCH"] = PSP_SYSTEMPARAM_LANGUAGE_FRENCH;
	langCodeMapping["SPANISH"] = PSP_SYSTEMPARAM_LANGUAGE_SPANISH;
	langCodeMapping["GERMAN"] = PSP_SYSTEMPARAM_LANGUAGE_GERMAN;
	langCodeMapping["ITALIAN"] = PSP_SYSTEMPARAM_LANGUAGE_ITALIAN;
	langCodeMapping["DUTCH"] = PSP_SYSTEMPARAM_LANGUAGE_DUTCH;
	langCodeMapping["PORTUGUESE"] = PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE;
	langCodeMapping["RUSSIAN"] = PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN;
	langCodeMapping["KOREAN"] = PSP_SYSTEMPARAM_LANGUAGE_KOREAN;
	langCodeMapping["CHINESE_TRADITIONAL"] = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL;
	langCodeMapping["CHINESE_SIMPLIFIED"] = PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED;

	const Section *langRegionNames = mapping.GetOrCreateSection("LangRegionNames");
	const Section *systemLanguage = mapping.GetOrCreateSection("SystemLanguage");

	for (size_t i = 0; i < keys.size(); i++) {
		std::string langName;
		langRegionNames->Get(keys[i].c_str(), &langName, "ERROR");
		std::string langCode;
		systemLanguage->Get(keys[i].c_str(), &langCode, "ENGLISH");
		int iLangCode = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
		if (langCodeMapping.find(langCode) != langCodeMapping.end())
			iLangCode = langCodeMapping[langCode];
		langValuesMapping_[keys[i]] = std::make_pair(langName, iLangCode);
	}
}

const std::map<std::string, std::pair<std::string, int>> &Config::GetLangValuesMapping() {
	if (langValuesMapping_.empty()) {
		LoadLangValuesMapping();
	}
	return langValuesMapping_;
}

void Config::Reload() {
	reload_ = true;
	Load();
	reload_ = false;
}

// Call this if you change the search path (such as when changing memstick directory. can't
// really think of any other legit uses).
void Config::UpdateIniLocation(const char *iniFileName, const char *controllerIniFilename) {
	const bool useIniFilename = iniFileName != nullptr && strlen(iniFileName) > 0;
	const char *ppssppIniFilename = IsVREnabled() ? "ppssppvr.ini" : "ppsspp.ini";
	iniFilename_ = FindConfigFile(useIniFilename ? iniFileName : ppssppIniFilename);
	const bool useControllerIniFilename = controllerIniFilename != nullptr && strlen(controllerIniFilename) > 0;
	const char *controlsIniFilename = IsVREnabled() ? "controlsvr.ini" : "controls.ini";
	controllerIniFilename_ = FindConfigFile(useControllerIniFilename ? controllerIniFilename : controlsIniFilename);
}

bool Config::LoadAppendedConfig() {
	IniFile iniFile;
	if (!iniFile.Load(appendedConfigFileName_)) {
		ERROR_LOG(LOADER, "Failed to read appended config '%s'.", appendedConfigFileName_.c_str());
		return false;
	}

	IterateSettings(iniFile, [&iniFile](Section *section, const ConfigSetting &setting) {
		if (iniFile.Exists(section->name().c_str(), setting.iniKey_))
			setting.Get(section);
	});

	INFO_LOG(LOADER, "Loaded appended config '%s'.", appendedConfigFileName_.c_str());

	Save("Loaded appended config"); // Let's prevent reset
	return true;
}

void Config::SetAppendedConfigIni(const Path &path) {
	appendedConfigFileName_ = path;
}


void Config::Load(const char *iniFileName, const char *controllerIniFilename) {
	if (!bUpdatedInstanceCounter) {
		InitInstanceCounter();
		bUpdatedInstanceCounter = true;
	}

	UpdateIniLocation(iniFileName, controllerIniFilename);

	INFO_LOG(LOADER, "Loading config: %s", iniFilename_.c_str());
	bSaveSettings = true;

	bShowFrameProfiler = true;

	IniFile iniFile;
	if (!iniFile.Load(iniFilename_)) {
		ERROR_LOG(LOADER, "Failed to read '%s'. Setting config to default.", iniFilename_.c_str());
		// Continue anyway to initialize the config.
	}

	IterateSettings(iniFile, [](Section *section, const ConfigSetting &setting) {
		setting.Get(section);
	});

	iRunCount++;

	// This check is probably not really necessary here anyway, you can always
	// press Home or Browse if you're in a bad directory.
	if (!File::Exists(currentDirectory))
		currentDirectory = defaultCurrentDirectory;

	Section *log = iniFile.GetOrCreateSection(logSectionName);

	bool debugDefaults = false;
#ifdef _DEBUG
	debugDefaults = true;
#endif
	LogManager::GetInstance()->LoadConfig(log, debugDefaults);

	Section *recent = iniFile.GetOrCreateSection("Recent");
	recent->Get("MaxRecent", &iMaxRecent, 60);

	// Fix issue from switching from uint (hex in .ini) to int (dec)
	// -1 is okay, though. We'll just ignore recent stuff if it is.
	if (iMaxRecent == 0)
		iMaxRecent = 60;

	if (iMaxRecent > 0) {
		private_->ResetRecentIsosThread();
		std::lock_guard<std::mutex> guard(private_->recentIsosLock);
		recentIsos.clear();
		for (int i = 0; i < iMaxRecent; i++) {
			char keyName[64];
			std::string fileName;

			snprintf(keyName, sizeof(keyName), "FileName%d", i);
			if (recent->Get(keyName, &fileName, "") && !fileName.empty()) {
				recentIsos.push_back(fileName);
			}
		}
	}

	auto pinnedPaths = iniFile.GetOrCreateSection("PinnedPaths")->ToMap();
	vPinnedPaths.clear();
	for (auto it = pinnedPaths.begin(), end = pinnedPaths.end(); it != end; ++it) {
		// Unpin paths that are deleted automatically.
		const std::string &path = it->second;
		if (startsWith(path, "http://") || startsWith(path, "https://") || File::Exists(Path(path))) {
			vPinnedPaths.push_back(File::ResolvePath(path));
		}
	}

	// Default values for post process shaders
	bool postShadersInitialized = iniFile.HasSection("PostShaderList");
	Section *postShaderChain = iniFile.GetOrCreateSection("PostShaderList");
	Section *postShaderSetting = iniFile.GetOrCreateSection("PostShaderSetting");
	if (IsVREnabled() && !postShadersInitialized) {
		postShaderChain->Set("PostShader1", "ColorCorrection");
		postShaderSetting->Set("ColorCorrectionSettingCurrentValue1", 1.0f);
		postShaderSetting->Set("ColorCorrectionSettingCurrentValue2", 1.5f);
		postShaderSetting->Set("ColorCorrectionSettingCurrentValue3", 1.1f);
		postShaderSetting->Set("ColorCorrectionSettingCurrentValue4", 1.0f);
	}

	// Load post process shader values
	mPostShaderSetting.clear();
	for (const auto& it : postShaderSetting->ToMap()) {
		mPostShaderSetting[it.first] = std::stof(it.second);
	}

	// Load post process shader names
	vPostShaderNames.clear();
	for (const auto& it : postShaderChain->ToMap()) {
		if (it.second != "Off")
			vPostShaderNames.push_back(it.second);
	}

	// Check for an old dpad setting
	Section *control = iniFile.GetOrCreateSection("Control");
	float f;
	control->Get("DPadRadius", &f, 0.0f);
	if (f > 0.0f) {
		ResetControlLayout();
	}

	const char *gitVer = PPSSPP_GIT_VERSION;
	Version installed(gitVer);
	Version upgrade(upgradeVersion);
	const bool versionsValid = installed.IsValid() && upgrade.IsValid();

	// Do this regardless of iRunCount to prevent a silly bug where one might use an older
	// build of PPSSPP, receive an upgrade notice, then start a newer version, and still receive the upgrade notice,
	// even if said newer version is >= the upgrade found online.
	if ((dismissedVersion == upgradeVersion) || (versionsValid && (installed >= upgrade))) {
		upgradeMessage.clear();
	}

	// Check for new version on every 10 runs.
	// Sometimes the download may not be finished when the main screen shows (if the user dismisses the
	// splash screen quickly), but then we'll just show the notification next time instead, we store the
	// upgrade number in the ini.
	if (iRunCount % 10 == 0 && bCheckForNewVersion) {
		const char *versionUrl = "http://www.ppsspp.org/version.json";
		const char *acceptMime = "application/json, text/*; q=0.9, */*; q=0.8";
		auto dl = g_DownloadManager.StartDownloadWithCallback(versionUrl, Path(), &DownloadCompletedCallback, acceptMime);
		dl->SetHidden(true);
	}

	INFO_LOG(LOADER, "Loading controller config: %s", controllerIniFilename_.c_str());
	bSaveSettings = true;

	LoadStandardControllerIni();

	//so this is all the way down here to overwrite the controller settings
	//sadly it won't benefit from all the "version conversion" going on up-above
	//but these configs shouldn't contain older versions anyhow
	if (bGameSpecific) {
		loadGameConfig(gameId_, gameIdTitle_);
	}

	CleanRecent();

#if PPSSPP_PLATFORM(ANDROID)
	// The on path here is untested, since we don't expose it.
	g_Config.bVSync = false;
#endif

	PostLoadCleanup(false);

	INFO_LOG(LOADER, "Config loaded: '%s'", iniFilename_.c_str());
}

bool Config::Save(const char *saveReason) {
	if (!IsFirstInstance()) {
		// TODO: Should we allow saving config if started from a different directory?
		// How do we tell?
		WARN_LOG(LOADER, "Not saving config - secondary instances don't.");

		// Don't want to retry or something.
		return true;
	}

	if (!iniFilename_.empty() && g_Config.bSaveSettings) {
		saveGameConfig(gameId_, gameIdTitle_);

		PreSaveCleanup(false);

		CleanRecent();
		IniFile iniFile;
		if (!iniFile.Load(iniFilename_)) {
			ERROR_LOG(LOADER, "Error saving config - can't read ini '%s'", iniFilename_.c_str());
		}

		// Need to do this somewhere...
		bFirstRun = false;

		IterateSettings(iniFile, [&](Section *section, const ConfigSetting &setting) {
			if (!bGameSpecific || !setting.perGame_) {
				setting.Set(section);
			}
		});

		Section *recent = iniFile.GetOrCreateSection("Recent");
		recent->Set("MaxRecent", iMaxRecent);

		private_->ResetRecentIsosThread();
		for (int i = 0; i < iMaxRecent; i++) {
			char keyName[64];
			snprintf(keyName, sizeof(keyName), "FileName%d", i);
			std::lock_guard<std::mutex> guard(private_->recentIsosLock);
			if (i < (int)recentIsos.size()) {
				recent->Set(keyName, recentIsos[i]);
			} else {
				recent->Delete(keyName); // delete the nonexisting FileName
			}
		}

		Section *pinnedPaths = iniFile.GetOrCreateSection("PinnedPaths");
		pinnedPaths->Clear();
		for (size_t i = 0; i < vPinnedPaths.size(); ++i) {
			char keyName[64];
			snprintf(keyName, sizeof(keyName), "Path%d", (int)i);
			pinnedPaths->Set(keyName, vPinnedPaths[i]);
		}

		if (!bGameSpecific) {
			Section *postShaderSetting = iniFile.GetOrCreateSection("PostShaderSetting");
			postShaderSetting->Clear();
			for (auto it = mPostShaderSetting.begin(), end = mPostShaderSetting.end(); it != end; ++it) {
				postShaderSetting->Set(it->first.c_str(), it->second);
			}
			Section *postShaderChain = iniFile.GetOrCreateSection("PostShaderList");
			postShaderChain->Clear();
			for (size_t i = 0; i < vPostShaderNames.size(); ++i) {
				char keyName[64];
				snprintf(keyName, sizeof(keyName), "PostShader%d", (int)i+1);
				postShaderChain->Set(keyName, vPostShaderNames[i]);
			}
		}

		Section *control = iniFile.GetOrCreateSection("Control");
		control->Delete("DPadRadius");

		Section *log = iniFile.GetOrCreateSection(logSectionName);
		if (LogManager::GetInstance())
			LogManager::GetInstance()->SaveConfig(log);

		if (!iniFile.Save(iniFilename_)) {
			ERROR_LOG(LOADER, "Error saving config (%s)- can't write ini '%s'", saveReason, iniFilename_.c_str());
			return false;
		}
		INFO_LOG(LOADER, "Config saved (%s): '%s'", saveReason, iniFilename_.c_str());

		if (!bGameSpecific) //otherwise we already did this in saveGameConfig()
		{
			IniFile controllerIniFile;
			if (!controllerIniFile.Load(controllerIniFilename_)) {
				ERROR_LOG(LOADER, "Error saving controller config - can't read ini first '%s'", controllerIniFilename_.c_str());
			}
			KeyMap::SaveToIni(controllerIniFile);
			if (!controllerIniFile.Save(controllerIniFilename_)) {
				ERROR_LOG(LOADER, "Error saving config - can't write ini '%s'", controllerIniFilename_.c_str());
				return false;
			}
			INFO_LOG(LOADER, "Controller config saved: %s", controllerIniFilename_.c_str());
		}

		PostSaveCleanup(false);
	} else {
		INFO_LOG(LOADER, "Not saving config");
	}

	return true;
}

void Config::PostLoadCleanup(bool gameSpecific) {
	// Override ppsspp.ini JIT value to prevent crashing
	jitForcedOff = DefaultCpuCore() != (int)CPUCore::JIT && g_Config.iCpuCore == (int)CPUCore::JIT;
	if (jitForcedOff) {
		g_Config.iCpuCore = (int)CPUCore::IR_JIT;
	}

	// This caps the exponent 4 (so 16x.)
	if (iAnisotropyLevel > 4) {
		iAnisotropyLevel = 4;
	}

	// Set a default MAC, and correct if it's an old format.
	if (sMACAddress.length() != 17)
		sMACAddress = CreateRandMAC();

	if (g_Config.bAutoFrameSkip && g_Config.bSkipBufferEffects) {
		g_Config.bSkipBufferEffects = false;
	}

	// Automatically silence secondary instances. Could be an option I guess, but meh.
	if (PPSSPP_ID > 1) {
		g_Config.iGlobalVolume = 0;
	}

	// Automatically switch away from deprecated setting value.
	if (iTexScalingLevel <= 0) {
		iTexScalingLevel = 1;
	}
}

void Config::PreSaveCleanup(bool gameSpecific) {
	if (jitForcedOff) {
		// If we forced jit off and it's still set to IR, change it back to jit.
		if (g_Config.iCpuCore == (int)CPUCore::IR_JIT)
			g_Config.iCpuCore = (int)CPUCore::JIT;
	}
}

void Config::PostSaveCleanup(bool gameSpecific) {
	if (jitForcedOff) {
		// Force JIT off again just in case Config::Save() is called without exiting PPSSPP.
		if (g_Config.iCpuCore == (int)CPUCore::JIT)
			g_Config.iCpuCore = (int)CPUCore::IR_JIT;
	}
}

void Config::NotifyUpdatedCpuCore() {
	if (jitForcedOff && g_Config.iCpuCore == (int)CPUCore::IR_JIT) {
		// No longer forced off, the user set it to IR jit.
		jitForcedOff = false;
	}
}

// Use for debugging the version check without messing with the server
#if 0
#define PPSSPP_GIT_VERSION "v0.0.1-gaaaaaaaaa"
#endif

void Config::DownloadCompletedCallback(http::Download &download) {
	if (download.ResultCode() != 200) {
		ERROR_LOG(LOADER, "Failed to download %s: %d", download.url().c_str(), download.ResultCode());
		return;
	}
	std::string data;
	download.buffer().TakeAll(&data);
	if (data.empty()) {
		ERROR_LOG(LOADER, "Version check: Empty data from server!");
		return;
	}

	json::JsonReader reader(data.c_str(), data.size());
	const json::JsonGet root = reader.root();
	if (!root) {
		ERROR_LOG(LOADER, "Failed to parse json");
		return;
	}

	std::string version = root.getString("version", "");

	const char *gitVer = PPSSPP_GIT_VERSION;
	Version installed(gitVer);
	Version upgrade(version);
	Version dismissed(g_Config.dismissedVersion);

	if (!installed.IsValid()) {
		ERROR_LOG(LOADER, "Version check: Local version string invalid. Build problems? %s", PPSSPP_GIT_VERSION);
		return;
	}
	if (!upgrade.IsValid()) {
		ERROR_LOG(LOADER, "Version check: Invalid server version: %s", version.c_str());
		return;
	}

	if (installed >= upgrade) {
		INFO_LOG(LOADER, "Version check: Already up to date, erasing any upgrade message");
		g_Config.upgradeMessage.clear();
		g_Config.upgradeVersion = upgrade.ToString();
		g_Config.dismissedVersion.clear();
		return;
	}

	if (installed < upgrade && dismissed != upgrade) {
		g_Config.upgradeMessage = "New version of PPSSPP available!";
		g_Config.upgradeVersion = upgrade.ToString();
		g_Config.dismissedVersion.clear();
	}
}

void Config::DismissUpgrade() {
	g_Config.dismissedVersion = g_Config.upgradeVersion;
}

void Config::AddRecent(const std::string &file) {
	// Don't bother with this if the user disabled recents (it's -1).
	if (iMaxRecent <= 0)
		return;

	// We'll add it back below.  This makes sure it's at the front, and only once.
	RemoveRecent(file);

	private_->ResetRecentIsosThread();
	std::lock_guard<std::mutex> guard(private_->recentIsosLock);
	const std::string filename = File::ResolvePath(file);
	recentIsos.insert(recentIsos.begin(), filename);
	if ((int)recentIsos.size() > iMaxRecent)
		recentIsos.resize(iMaxRecent);
}

void Config::RemoveRecent(const std::string &file) {
	// Don't bother with this if the user disabled recents (it's -1).
	if (iMaxRecent <= 0)
		return;

	private_->ResetRecentIsosThread();
	std::lock_guard<std::mutex> guard(private_->recentIsosLock);
	const std::string filename = File::ResolvePath(file);
	for (auto iter = recentIsos.begin(); iter != recentIsos.end();) {
		const std::string recent = File::ResolvePath(*iter);
		if (filename == recent) {
			// Note that the increment-erase idiom doesn't work with vectors.
			iter = recentIsos.erase(iter);
		} else {
			iter++;
		}
	}
}

void Config::CleanRecent() {
	private_->SetRecentIsosThread([this] {
		SetCurrentThreadName("RecentISOs");

		AndroidJNIThreadContext jniContext;  // destructor detaches

		double startTime = time_now_d();

		std::lock_guard<std::mutex> guard(private_->recentIsosLock);
		std::vector<std::string> cleanedRecent;
		for (size_t i = 0; i < recentIsos.size(); i++) {
			bool exists = false;
			Path path = Path(recentIsos[i]);
			switch (path.Type()) {
			case PathType::CONTENT_URI:
			case PathType::NATIVE:
				exists = File::Exists(path);
				break;
			default:
				FileLoader *loader = ConstructFileLoader(path);
				exists = loader->ExistsFast();
				delete loader;
				break;
			}

			if (exists) {
				// Make sure we don't have any redundant items.
				auto duplicate = std::find(cleanedRecent.begin(), cleanedRecent.end(), recentIsos[i]);
				if (duplicate == cleanedRecent.end()) {
					cleanedRecent.push_back(recentIsos[i]);
				}
			}
		}

		double recentTime = time_now_d() - startTime;
		if (recentTime > 0.1) {
			INFO_LOG(SYSTEM, "CleanRecent took %0.2f", recentTime);
		}
		recentIsos = cleanedRecent;
	});
}

std::vector<std::string> Config::RecentIsos() const {
	std::lock_guard<std::mutex> guard(private_->recentIsosLock);
	return recentIsos;
}

bool Config::HasRecentIsos() const {
	std::lock_guard<std::mutex> guard(private_->recentIsosLock);
	return !recentIsos.empty();
}

void Config::ClearRecentIsos() {
	private_->ResetRecentIsosThread();
	std::lock_guard<std::mutex> guard(private_->recentIsosLock);
	recentIsos.clear();
}

void Config::SetSearchPath(const Path &searchPath) {
	searchPath_ = searchPath;
}

const Path Config::FindConfigFile(const std::string &baseFilename) {
	// Don't search for an absolute path.
	if (baseFilename.size() > 1 && baseFilename[0] == '/') {
		return Path(baseFilename);
	}
#ifdef _WIN32
	if (baseFilename.size() > 3 && baseFilename[1] == ':' && (baseFilename[2] == '/' || baseFilename[2] == '\\')) {
		return Path(baseFilename);
	}
#endif

	Path filename = searchPath_ / baseFilename;
	if (File::Exists(filename)) {
		return filename;
	}

	// Make sure at least the directory it's supposed to be in exists.
	Path path = filename.NavigateUp();
	// This check is just to avoid logging.
	if (!File::Exists(path)) {
		File::CreateFullPath(path);
	}
	return filename;
}

void Config::RestoreDefaults(RestoreSettingsBits whatToRestore) {
	if (bGameSpecific) {
		// TODO: This should be possible to do in a cleaner way.
		deleteGameConfig(gameId_);
		createGameConfig(gameId_);
		Load();
	} else {
		if (whatToRestore & RestoreSettingsBits::SETTINGS) {
			IterateSettings([](const ConfigSetting &setting) {
				setting.RestoreToDefault();
			});
		}

		if (whatToRestore & RestoreSettingsBits::CONTROLS) {
			KeyMap::RestoreDefault();
		}

		if (whatToRestore & RestoreSettingsBits::RECENT) {
			ClearRecentIsos();
			currentDirectory = defaultCurrentDirectory;
		}
	}
}

bool Config::hasGameConfig(const std::string &pGameId) {
	Path fullIniFilePath = getGameConfigFile(pGameId);
	return File::Exists(fullIniFilePath);
}

void Config::changeGameSpecific(const std::string &pGameId, const std::string &title) {
	if (!reload_)
		Save("changeGameSpecific");
	gameId_ = pGameId;
	gameIdTitle_ = title;
	bGameSpecific = !pGameId.empty();
}

bool Config::createGameConfig(const std::string &pGameId) {
	Path fullIniFilePath = getGameConfigFile(pGameId);

	if (hasGameConfig(pGameId)) {
		return false;
	}

	File::CreateEmptyFile(fullIniFilePath);
	return true;
}

bool Config::deleteGameConfig(const std::string& pGameId) {
	Path fullIniFilePath = Path(getGameConfigFile(pGameId));

	File::Delete(fullIniFilePath);
	return true;
}

Path Config::getGameConfigFile(const std::string &pGameId) {
	const char *ppssppIniFilename = IsVREnabled() ? "_ppssppvr.ini" : "_ppsspp.ini";
	std::string iniFileName = pGameId + ppssppIniFilename;
	Path iniFileNameFull = FindConfigFile(iniFileName);

	return iniFileNameFull;
}

bool Config::saveGameConfig(const std::string &pGameId, const std::string &title) {
	if (pGameId.empty()) {
		return false;
	}

	Path fullIniFilePath = getGameConfigFile(pGameId);

	IniFile iniFile;

	Section *top = iniFile.GetOrCreateSection("");
	top->AddComment(StringFromFormat("Game config for %s - %s", pGameId.c_str(), title.c_str()));

	PreSaveCleanup(true);

	IterateSettings(iniFile, [](Section *section, const ConfigSetting &setting) {
		if (setting.perGame_) {
			setting.Set(section);
		}
	});

	Section *postShaderSetting = iniFile.GetOrCreateSection("PostShaderSetting");
	postShaderSetting->Clear();
	for (auto it = mPostShaderSetting.begin(), end = mPostShaderSetting.end(); it != end; ++it) {
		postShaderSetting->Set(it->first.c_str(), it->second);
	}

	Section *postShaderChain = iniFile.GetOrCreateSection("PostShaderList");
	postShaderChain->Clear();
	for (size_t i = 0; i < vPostShaderNames.size(); ++i) {
		char keyName[64];
		snprintf(keyName, sizeof(keyName), "PostShader%d", (int)i+1);
		postShaderChain->Set(keyName, vPostShaderNames[i]);
	}

	KeyMap::SaveToIni(iniFile);
	iniFile.Save(fullIniFilePath);

	PostSaveCleanup(true);
	return true;
}

bool Config::loadGameConfig(const std::string &pGameId, const std::string &title) {
	Path iniFileNameFull = getGameConfigFile(pGameId);

	if (!hasGameConfig(pGameId)) {
		DEBUG_LOG(LOADER, "No game-specific settings found in %s. Using global defaults.", iniFileNameFull.c_str());
		return false;
	}

	changeGameSpecific(pGameId, title);
	IniFile iniFile;
	iniFile.Load(iniFileNameFull);

	auto postShaderSetting = iniFile.GetOrCreateSection("PostShaderSetting")->ToMap();
	mPostShaderSetting.clear();
	for (const auto &it : postShaderSetting) {
		float value = 0.0f;
		if (sscanf(it.second.c_str(), "%f", &value)) {
			mPostShaderSetting[it.first] = value;
		} else {
			WARN_LOG(LOADER, "Invalid float value string for param %s: '%s'", it.first.c_str(), it.second.c_str());
		}
	}

	auto postShaderChain = iniFile.GetOrCreateSection("PostShaderList")->ToMap();
	vPostShaderNames.clear();
	for (auto it : postShaderChain) {
		if (it.second != "Off")
			vPostShaderNames.push_back(it.second);
	}

	IterateSettings(iniFile, [](Section *section, const ConfigSetting &setting) {
		if (setting.perGame_) {
			setting.Get(section);
		}
	});

	KeyMap::LoadFromIni(iniFile);

	if (!appendedConfigFileName_.ToString().empty() &&
		std::find(appendedConfigUpdatedGames_.begin(), appendedConfigUpdatedGames_.end(), pGameId) == appendedConfigUpdatedGames_.end()) {

		LoadAppendedConfig();
		appendedConfigUpdatedGames_.push_back(pGameId);
	}

	PostLoadCleanup(true);
	return true;
}

void Config::unloadGameConfig() {
	if (bGameSpecific) {
		changeGameSpecific();

		IniFile iniFile;
		iniFile.Load(iniFilename_);

		// Reload game specific settings back to standard.
		IterateSettings(iniFile, [](Section *section, const ConfigSetting &setting) {
			if (setting.perGame_) {
				setting.Get(section);
			}
		});

		auto postShaderSetting = iniFile.GetOrCreateSection("PostShaderSetting")->ToMap();
		mPostShaderSetting.clear();
		for (auto it : postShaderSetting) {
			mPostShaderSetting[it.first] = std::stof(it.second);
		}

		auto postShaderChain = iniFile.GetOrCreateSection("PostShaderList")->ToMap();
		vPostShaderNames.clear();
		for (auto it : postShaderChain) {
			if (it.second != "Off")
				vPostShaderNames.push_back(it.second);
		}

		LoadStandardControllerIni();
		PostLoadCleanup(true);
	}
}

void Config::LoadStandardControllerIni() {
	IniFile controllerIniFile;
	if (!controllerIniFile.Load(controllerIniFilename_)) {
		ERROR_LOG(LOADER, "Failed to read %s. Setting controller config to default.", controllerIniFilename_.c_str());
		KeyMap::RestoreDefault();
	} else {
		// Continue anyway to initialize the config. It will just restore the defaults.
		KeyMap::LoadFromIni(controllerIniFile);
	}
}

void Config::ResetControlLayout() {
	auto reset = [](ConfigTouchPos &pos) {
		pos.x = defaultTouchPosShow.x;
		pos.y = defaultTouchPosShow.y;
		pos.scale = defaultTouchPosShow.scale;
	};
	reset(g_Config.touchActionButtonCenter);
	g_Config.fActionButtonSpacing = 1.0f;
	reset(g_Config.touchDpad);
	g_Config.fDpadSpacing = 1.0f;
	reset(g_Config.touchStartKey);
	reset(g_Config.touchSelectKey);
	reset(g_Config.touchFastForwardKey);
	reset(g_Config.touchLKey);
	reset(g_Config.touchRKey);
	reset(g_Config.touchAnalogStick);
	reset(g_Config.touchRightAnalogStick);
	reset(g_Config.touchCombo0);
	reset(g_Config.touchCombo1);
	reset(g_Config.touchCombo2);
	reset(g_Config.touchCombo3);
	reset(g_Config.touchCombo4);
	reset(g_Config.touchCombo5);
	reset(g_Config.touchCombo6);
	reset(g_Config.touchCombo7);
	reset(g_Config.touchCombo8);
	reset(g_Config.touchCombo9);
	g_Config.fLeftStickHeadScale = 1.0f;
	g_Config.fRightStickHeadScale = 1.0f;
}

void Config::GetReportingInfo(UrlEncoder &data) {
	for (size_t i = 0; i < ARRAY_SIZE(sections); ++i) {
		const std::string prefix = std::string("config.") + sections[i].section;
		for (size_t j = 0; j < sections[i].settingsCount; j++) {
			sections[i].settings[j].Report(data, prefix);
		}
	}
}

bool Config::IsPortrait() const {
	return (iInternalScreenRotation == ROTATION_LOCKED_VERTICAL || iInternalScreenRotation == ROTATION_LOCKED_VERTICAL180) && !bSkipBufferEffects;
}
