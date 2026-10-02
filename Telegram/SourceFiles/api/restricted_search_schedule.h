/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>

namespace RestrictedSearch {

class AccountSchedule {
public:
	static constexpr int64_t kSendInterval = 300;

	bool acquire(int64_t now) {
		if (now < cooldown || now < _nextSend || _running >= 6) return false;
		++_running;
		_nextSend = now + kSendInterval;
		return true;
	}
	void release() {
		if (_running > 0) --_running;
	}
	int running() const { return _running; }
	int64_t cooldownLeft(int64_t now) const {
		return std::max<int64_t>(0, cooldown - now);
	}
	void flood(int64_t now, int64_t duration) {
		cooldown = std::max(cooldown, now + duration);
	}
	int64_t cooldown = 0;
private:
	int _running = 0;
	int64_t _nextSend = 0;
};

struct Deadline {
	void start(int64_t now, int64_t duration) { due = now + duration; }
	bool expired(int64_t now) const { return due && now >= due; }
	int64_t due = 0;
};

inline bool InputDelayElapsed(int64_t started, int64_t delay, int64_t now) {
	return now >= started + delay;
}

} // namespace RestrictedSearch
