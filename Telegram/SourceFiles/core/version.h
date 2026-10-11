/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/const_string.h"

#define TDESKTOP_REQUESTED_ALPHA_VERSION (0ULL)

#ifdef TDESKTOP_ALLOW_CLOSED_ALPHA
#define TDESKTOP_ALPHA_VERSION TDESKTOP_REQUESTED_ALPHA_VERSION
#else // TDESKTOP_ALLOW_CLOSED_ALPHA
#define TDESKTOP_ALPHA_VERSION (0ULL)
#endif // TDESKTOP_ALLOW_CLOSED_ALPHA

// used in Updater.cpp and Setup.iss for Windows
constexpr auto AppId = "{78A74EE4-924F-4D24-B9F5-39E257CDE0F4}"_cs;
constexpr auto AppNameOld = "FishGram"_cs;
constexpr auto AppName = "FishGram"_cs;
constexpr auto AppFile = "Telegram"_cs;
constexpr auto AppVersion = 7002009;
#define FISHGRAM_STRINGIFY_IMPL(value) #value
#define FISHGRAM_STRINGIFY(value) FISHGRAM_STRINGIFY_IMPL(value)
constexpr auto AppVersionStr = "7.2.9-r" FISHGRAM_STRINGIFY(FISHGRAM_REVISION);
constexpr auto AppBetaVersion = false;
constexpr auto AppAlphaVersion = TDESKTOP_ALPHA_VERSION;
