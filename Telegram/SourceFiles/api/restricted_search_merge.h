/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

// Deterministic merge core for restricted global search.
// Pure C++17, no Qt/MTP dependencies. Each raw feed only has to
// deliver items in non-increasing date order; inside one date the
// order of FullMsgId is unspecified and may even repeat an item.
namespace RestrictedSearch {

// WHY: position order must mirror Data::MessagePosition exactly —
// descending (date, peer, msg), matching FullMsgId's (peer, msg).
struct Position {
	int64_t date = 0;
	int64_t peer = 0;
	int64_t msg = 0;

	friend bool operator<(const Position &a, const Position &b) {
		return std::tie(a.date, a.peer, a.msg)
			< std::tie(b.date, b.peer, b.msg);
	}
	friend bool operator==(const Position &a, const Position &b) {
		return !(a < b) && !(b < a);
	}
	friend bool operator!=(const Position &a, const Position &b) {
		return !(a == b);
	}
	friend bool operator>(const Position &a, const Position &b) {
		return b < a;
	}
	friend bool operator>=(const Position &a, const Position &b) {
		return !(a < b);
	}
	friend bool operator<=(const Position &a, const Position &b) {
		return !(b < a);
	}
};

enum class SourceError : uint8_t {
	None,
	Network,
	FloodWait,
	Timeout,
	Protocol, // stalled cursor or non-monotonic order
};

class MergeEngine final {
public:
	struct Options {
		int pageSize = 50;
		bool uniquePerPeer = false;
		bool onlyForwardable = false;
	};

	enum class Coverage : uint8_t {
		Complete,
		Partial,
	};

	struct Item {
		Position position;
		bool forwardable = true;

		friend bool operator==(
			const Item &,
			const Item &) = default;
	};

	struct Page {
		std::vector<Position> items;
		bool hasMore = false;
		Coverage coverage = Coverage::Complete;
		int loadedCount = 0;
		std::optional<int> exactTotal;
		SourceError error = SourceError::None;
		int64_t retryAfterMs = 0;
	};

	struct Step {
		std::vector<int> fetch;
		bool pageReady = false;
	};

	explicit MergeEngine(Options options);

	// Each source is an independent strictly-descending raw stream;
	// ids are assigned in creation order, starting from 0.
	int addSource();

	// Appends one raw page of already-filtered visible items.
	// 'exhausted' means the raw cursor provably ended. Only the
	// date has to stay monotonic; an item dated after the previous
	// tail fails the source as Protocol.
	[[nodiscard]] bool feed(
		int source,
		std::vector<Item> items,
		bool exhausted);

	// An explicit failure ends the source: its tail stays unknown,
	// so the round degrades to Partial instead of claiming exhaustion.
	void fail(int source, SourceError error, int64_t retryAfterMs = 0);
	// A FloodWait releases the in-flight fetch without closing the raw
	// stream. The same cursor can be fetched again after the account cooldown.
	// The fourth consecutive FloodWait on one page closes the source as partial.
	[[nodiscard]] bool deferFetching(
		int source,
		int64_t retryAfterMs = 0);

	void markFetching(int source);

	[[nodiscard]] bool needsFetch(int source) const;
	[[nodiscard]] bool terminal(int source) const;
	[[nodiscard]] Position lastFed(int source) const;

	// Moves every provably closed item into the pending batch,
	// then reports either that a page can be committed or which
	// live sources can still deliver items inside the next page
	// and therefore have to be fetched first.
	Step step();

	// Requires step().pageReady. Commits a prefix that is final and
	// can never be reordered by later feeds.
	Page takePage();

	[[nodiscard]] bool finished() const;
	[[nodiscard]] int committed() const;

private:
	struct Source {
		std::deque<Item> buffer;
		Position lastFed;
		bool hasLastFed = false;
		bool exhausted = false;
		bool failed = false;
		bool fetching = false;
		SourceError error = SourceError::None;
		int64_t retryAfterMs = 0;
		int floodRetries = 0;

		[[nodiscard]] bool alive() const {
			return !exhausted && !failed;
		}
	};

	void collect();
	void offer(Item item);
	[[nodiscard]] int64_t openFrontier() const;
	[[nodiscard]] int64_t pageBoundary() const;
	[[nodiscard]] bool allTerminal() const;

	Options _options;
	std::vector<Source> _sources;
	std::deque<Item> _batch;
	std::set<std::pair<int64_t, int64_t>> _seen;
	std::set<int64_t> _seenPeers;
	int _committed = 0;
	SourceError _firstError = SourceError::None;
	int64_t _maxRetryAfterMs = 0;
	bool _finalEmitted = false;

};

inline MergeEngine::MergeEngine(Options options)
: _options(options) {
}

inline int MergeEngine::addSource() {
	_sources.push_back(Source());
	return int(_sources.size()) - 1;
}

inline bool MergeEngine::feed(
		int source,
		std::vector<Item> items,
		bool exhausted) {
	auto &s = _sources[size_t(source)];
	if (!s.alive()) {
		return true; // late response for a closed source
	}
	s.fetching = false;
	for (const auto &item : items) {
		if (s.hasLastFed && item.position.date > s.lastFed.date) {
			s.failed = true;
			s.error = SourceError::Protocol;
			if (_firstError == SourceError::None) {
				_firstError = SourceError::Protocol;
			}
			return false;
		}
		s.lastFed = item.position;
		s.hasLastFed = true;
		s.buffer.push_back(item);
	}
	if (exhausted) {
		s.exhausted = true;
	}
	s.floodRetries = 0;
	return true;
}

inline void MergeEngine::fail(
		int source,
		SourceError error,
		int64_t retryAfterMs) {
	auto &s = _sources[size_t(source)];
	if (!s.alive()) {
		return;
	}
	s.failed = true;
	s.fetching = false;
	s.error = error;
	s.retryAfterMs = retryAfterMs;
	if (_firstError == SourceError::None) {
		_firstError = error;
	}
	if (retryAfterMs > _maxRetryAfterMs) {
		_maxRetryAfterMs = retryAfterMs;
	}
}

inline bool MergeEngine::deferFetching(
		int source,
		int64_t retryAfterMs) {
	auto &s = _sources[size_t(source)];
	if (!s.alive()) {
		return false;
	}
	s.fetching = false;
	s.retryAfterMs = retryAfterMs;
	if (++s.floodRetries <= 3) {
		return true;
	}
	fail(source, SourceError::FloodWait, retryAfterMs);
	return false;
}

inline void MergeEngine::markFetching(int source) {
	_sources[size_t(source)].fetching = true;
}

inline bool MergeEngine::needsFetch(int source) const {
	const auto &s = _sources[size_t(source)];
	return s.alive()
		&& !s.fetching
		&& (!s.hasLastFed || s.lastFed.date >= pageBoundary());
}

inline bool MergeEngine::terminal(int source) const {
	return !_sources[size_t(source)].alive();
}

inline auto MergeEngine::lastFed(int source) const -> Position {
	const auto &s = _sources[size_t(source)];
	return s.hasLastFed ? s.lastFed : Position();
}

inline bool MergeEngine::allTerminal() const {
	for (const auto &s : _sources) {
		if (s.alive()) {
			return false;
		}
	}
	return true;
}

inline void MergeEngine::offer(Item item) {
	const auto &p = item.position;
	if (_options.onlyForwardable && !item.forwardable) {
		return;
	}
	if (!_seen.emplace(p.peer, p.msg).second) {
		return;
	}
	if (_options.uniquePerPeer && !_seenPeers.emplace(p.peer).second) {
		return;
	}
	_batch.push_back(item);
}

// WHY: a source only guarantees non-increasing dates, so its
// buffer keeps a descending prefix. While any live source can
// still deliver an item dated D or later, no buffered item dated
// D or earlier may be committed — the equal-date run could be
// continued by a later raw page. The open frontier is therefore
// the largest lastFed date among live sources.
inline int64_t MergeEngine::openFrontier() const {
	auto result = std::numeric_limits<int64_t>::min();
	for (const auto &s : _sources) {
		if (!s.alive()) {
			continue;
		}
		if (!s.hasLastFed) {
			return std::numeric_limits<int64_t>::max();
		}
		if (s.lastFed.date > result) {
			result = s.lastFed.date;
		}
	}
	return result;
}

inline void MergeEngine::collect() {
	const auto frontier = openFrontier();
	if (frontier == std::numeric_limits<int64_t>::max()) {
		return;
	}
	auto pool = std::vector<Item>();
	for (auto &s : _sources) {
		while (!s.buffer.empty()
			&& s.buffer.front().position.date > frontier) {
			pool.push_back(s.buffer.front());
			s.buffer.pop_front();
		}
	}
	std::sort(pool.begin(), pool.end(), [](const Item &a, const Item &b) {
		return b.position < a.position;
	});
	for (auto &item : pool) {
		offer(std::move(item));
	}
}

// WHY: to commit a full page the frontier must drop below the
// date of the last needed candidate; every live source that can
// still produce items at or above that boundary must be fetched.
inline int64_t MergeEngine::pageBoundary() const {
	const auto needed = _options.pageSize - int(_batch.size());
	if (needed <= 0) {
		return std::numeric_limits<int64_t>::min();
	}
	auto dates = std::vector<int64_t>();
	for (const auto &s : _sources) {
		for (const auto &item : s.buffer) {
			dates.push_back(item.position.date);
		}
	}
	if (int(dates.size()) < needed) {
		return std::numeric_limits<int64_t>::min();
	}
	std::sort(dates.begin(), dates.end(), std::greater<int64_t>());
	return dates[size_t(needed) - 1];
}

inline auto MergeEngine::step() -> Step {
	collect();
	auto result = Step();
	if (_finalEmitted) {
		return result;
	}
	if (int(_batch.size()) >= _options.pageSize || allTerminal()) {
		result.pageReady = true;
		return result;
	}
	const auto boundary = pageBoundary();
	for (auto i = 0; i < int(_sources.size()); ++i) {
		const auto &s = _sources[size_t(i)];
		if (s.alive()
			&& !s.fetching
			&& (!s.hasLastFed || s.lastFed.date >= boundary)) {
			result.fetch.push_back(i);
		}
	}
	return result;
}

inline auto MergeEngine::takePage() -> Page {
	auto page = Page();
	const auto count = int(_batch.size()) > _options.pageSize
		? _options.pageSize
		: int(_batch.size());
	page.items.reserve(size_t(count));
	for (auto i = 0; i < count; ++i) {
		page.items.push_back(_batch.front().position);
		_batch.pop_front();
	}
	_committed += count;
	page.loadedCount = _committed;
	page.coverage = (_firstError == SourceError::None)
		? Coverage::Complete
		: Coverage::Partial;
	page.error = _firstError;
	page.retryAfterMs = _maxRetryAfterMs;
	auto allEmpty = _batch.empty();
	if (allEmpty) {
		for (const auto &s : _sources) {
			if (!s.buffer.empty()) {
				allEmpty = false;
				break;
			}
		}
	}
	const auto done = allTerminal() && allEmpty;
	page.hasMore = !done;
	if (done) {
		_finalEmitted = true;
		if (_firstError == SourceError::None) {
			page.exactTotal = _committed;
		}
	}
	return page;
}

inline bool MergeEngine::finished() const {
	return _finalEmitted;
}

inline int MergeEngine::committed() const {
	return _committed;
}

} // namespace RestrictedSearch
