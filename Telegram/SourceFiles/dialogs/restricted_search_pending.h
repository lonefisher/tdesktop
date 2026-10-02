/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

namespace Dialogs {

// Tracks the merged-page lifecycle independently from native search requests.
class RestrictedSearchPending final {
public:
	void begin(std::uint64_t generation) {
		_generation = generation;
		_pending = true;
	}

	void cancel(std::uint64_t generation) {
		_generation = generation;
		_pending = false;
	}

	[[nodiscard]] bool finishPage(std::uint64_t generation) {
		if (generation != _generation) {
			return false;
		}
		_pending = false;
		return true;
	}

	[[nodiscard]] bool pending() const {
		return _pending;
	}

private:
	std::uint64_t _generation = 0;
	bool _pending = false;
};

} // namespace Dialogs
