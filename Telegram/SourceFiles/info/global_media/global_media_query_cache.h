/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <iterator>
#include <utility>

namespace Info::GlobalMedia {

template <typename List>
[[nodiscard]] bool QueryPageCanLoad(const List &list) {
	return !list.loaded && !list.failed;
}

template <typename List>
[[nodiscard]] bool RetryQueryRequest(List &list) {
	if (!list.failed || list.requestId) {
		return false;
	}
	list.failed = false;
	return true;
}

template <typename List>
[[nodiscard]] bool CompleteQueryRequest(
		List &list,
		bool failed,
		bool exhausted) {
	list.requestId = 0;
	if (failed) {
		++list.requestToken;
		if (!list.requestCursors.empty()) {
			list.requestCursors.pop_back();
		}
		list.loaded = false;
		list.failed = true;
		// Keep waiters so empty failures publish a retry action without a query change.
		return false;
	}
	list.failed = false;
	list.loaded = exhausted;
	return true;
}

template <typename Lists, typename CancelRequest>
[[nodiscard]] auto RestartQueryCaches(
		Lists &lists,
		bool modeChanged,
		CancelRequest cancelRequest) {
	auto waiters = decltype(lists.begin()->second.requestWaiters)();
	for (auto &entry : lists) {
		auto &list = entry.second;
		if (list.requestId) {
			// Keep consumed cursors so cancellation cannot defeat pagination loop detection.
			++list.requestToken;
			if (!list.requestCursors.empty()) {
				list.requestCursors.pop_back();
			}
			cancelRequest(std::exchange(list.requestId, 0));
		}
		auto pending = std::exchange(list.requestWaiters, {});
		waiters.insert(
			waiters.end(),
			std::make_move_iterator(pending.begin()),
			std::make_move_iterator(pending.end()));
	}
	if (modeChanged) {
		lists.clear();
	}
	return waiters;
}

} // namespace Info::GlobalMedia
