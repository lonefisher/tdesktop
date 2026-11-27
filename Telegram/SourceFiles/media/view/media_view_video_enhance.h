/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Media::View {

void FillVideoEnhanceMenu(
	not_null<Ui::PopupMenu*> menu,
	quintptr windowId);

} // namespace Media::View
