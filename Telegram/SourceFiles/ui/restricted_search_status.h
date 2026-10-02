/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_restricted_global_search.h"
#include "lang/lang_keys.h"

namespace Ui {

inline QString RestrictedSearchFailureText(
		Api::RestrictedGlobalSearchCoordinator::ErrorKind error) {
	using Error = Api::RestrictedGlobalSearchCoordinator::ErrorKind;
	const auto reason = [&] {
		switch (error) {
		case Error::Network:
			return tr::lng_restricted_search_network(tr::now);
		case Error::FloodWait:
			return tr::lng_restricted_search_flood(tr::now);
		case Error::Timeout:
			return tr::lng_restricted_search_timeout(tr::now);
		case Error::Protocol:
			return tr::lng_restricted_search_protocol(tr::now);
		case Error::None:
			return QString();
		}
		return QString();
	}();
	return tr::lng_settings_restricted_search_incomplete(tr::now)
		+ (reason.isEmpty() ? QString() : u" · "_q + reason)
		+ u" · "_q
		+ tr::lng_settings_restricted_search_retry(tr::now);
}

} // namespace Ui
