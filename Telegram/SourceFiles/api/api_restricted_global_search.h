/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_messages.h"
#include "mtproto/sender.h"
#include <crl/crl_time.h>

class ChannelData;

namespace Main {
class Session;
}

namespace Api {
class RestrictedGlobalSearchCoordinator final {
public:
	// Session/MTP adapters delegate lifecycle and paging to RestrictedSearchCore.
	// Its deterministic transport tests run the same production coordinator core.
	struct Query {
		MTPmessages_SearchGlobal request;
		MTPmessages_SearchGlobal::Flags flags;
		int folderId = 0;
		ChannelData *community = nullptr;
		QString text;
		MTPMessagesFilter filter;
		int minDate = 0;
		int maxDate = 0;
		int rawPageSize = 50;
		int pageSize = 50;
		bool uniquePerPeer = false;
		bool onlyForwardable = false;
		crl::time officialDelay = 0;
		crl::time supplementDelay = 0;

		friend inline bool operator==(
			const Query &,
			const Query &) = default;
	};
	enum class Coverage : uchar {
		Complete,
		Partial,
	};

	enum class ErrorKind : uchar {
		None,
		Network,
		FloodWait,
		Timeout,
		Protocol,
	};

	struct Page {
		std::vector<Data::MessagePosition> messageIds;
		bool hasMore = false;
		Coverage coverage = Coverage::Complete;
		int loadedCount = 0;
		std::optional<int> exactTotal;
		ErrorKind error = ErrorKind::None;
		crl::time retryAfter = 0;
	};

	explicit RestrictedGlobalSearchCoordinator(
		not_null<Main::Session*> session);
	RestrictedGlobalSearchCoordinator(
		const RestrictedGlobalSearchCoordinator &other) = delete;
	RestrictedGlobalSearchCoordinator &operator=(
		const RestrictedGlobalSearchCoordinator &other) = delete;
	~RestrictedGlobalSearchCoordinator();
	void start(Query query, Fn<void(Page)> done);
	void requestMore();
	void cancel();
	void retry();
	[[nodiscard]] bool active() const;
	[[nodiscard]] crl::time cooldownLeft() const;

private:
	class Impl;
	const std::unique_ptr<Impl> _impl;

};

}
