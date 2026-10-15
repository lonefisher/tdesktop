/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>

namespace Info::GlobalMedia {

template <typename EmptyWidget, typename Query>
void UpdateQueryEmptyState(
		EmptyWidget &empty,
		const Query &query,
		bool retryable) {
	empty.setSearchQuery(query);
	if (!retryable) {
		empty.setStatus(Query(), [] {});
		empty.setLoadedCount(std::nullopt);
	}
}

} // namespace Info::GlobalMedia
