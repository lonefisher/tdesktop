/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <optional>

namespace Info::GlobalMedia::Pagination {

struct SkippedCounts {
	int after = 0;
	int before = 0;
};

// Slices are ordered newest first: "after" is above the slice and "before"
// is below it. When the total is unknown, one means older items remain.
[[nodiscard]] inline SkippedCounts MakeSkippedCounts(
		int firstIndex,
		int itemCount,
		std::optional<int> exactTotal,
		bool hasMoreOlder) {
	const auto first = std::max(0, firstIndex);
	const auto count = std::max(0, itemCount);
	const auto after = first;
	const auto before = exactTotal
		? std::max(0, *exactTotal - first - count)
		: (hasMoreOlder ? 1 : 0);
	return { after, before };
}

// Restricted global search advances from newest to older messages. The top
// edge never requests another page; short viewports and bottom scrolling do.
[[nodiscard]] inline bool ShouldRequestOlderPage(
		bool preloadTop,
		bool preloadBottom,
		bool hasMore,
		bool pagePending) {
	(void)preloadTop;
	return preloadBottom && hasMore && !pagePending;
}

} // namespace Info::GlobalMedia::Pagination
