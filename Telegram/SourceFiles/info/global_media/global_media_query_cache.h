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
		// These callbacks push the slice and request more recursively. Drop
		// them on failure: the next viewer request retries the same cursor.
		list.requestWaiters.clear();
		return false;
	}
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
			// The final cursor belongs to the cancelled, unconsumed page.
			// Keep completed cursors so the loop guard still works.
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
