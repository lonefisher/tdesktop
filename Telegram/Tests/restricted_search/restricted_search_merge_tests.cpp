/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/restricted_search_merge.h"
#include "api/restricted_search_schedule.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <set>
#include <utility>
#include <vector>

using RestrictedSearch::MergeEngine;
using RestrictedSearch::Position;
using RestrictedSearch::SourceError;
using Item = MergeEngine::Item;
using Options = MergeEngine::Options;
using Coverage = MergeEngine::Coverage;

namespace {

int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #cond); \
		++g_failures; \
	} \
} while (0)

Item makeItem(int64_t date, int64_t peer, int64_t msg, bool fwd = true) {
	auto item = Item();
	item.position = Position{ date, peer, msg };
	item.forwardable = fwd;
	return item;
}

struct Event {
	std::vector<Item> items;
	bool exhausted = false;
	bool fail = false;
	bool expectReject = false;
	SourceError error = SourceError::Network;
	int64_t retryAfterMs = 0;
};

Event page(std::vector<Item> items, bool exhausted = false) {
	auto event = Event();
	event.items = std::move(items);
	event.exhausted = exhausted;
	return event;
}

Event failure(SourceError error, int64_t retryAfterMs = 0) {
	auto event = Event();
	event.fail = true;
	event.error = error;
	event.retryAfterMs = retryAfterMs;
	return event;
}

struct Driven {
	std::vector<Position> emitted;
	std::vector<MergeEngine::Page> pages;
	std::vector<Item> delivered;
};

Driven drive(
		MergeEngine &engine,
		std::vector<std::deque<Event>> scripts,
		int pageSize) {
	auto result = Driven();
	for (auto guard = 0; !engine.finished() && guard < 200000; ++guard) {
		auto step = engine.step();
		if (step.pageReady) {
			auto page = engine.takePage();
			CHECK(int(page.items.size()) <= pageSize);
			CHECK(page.loadedCount
				== int(result.emitted.size() + page.items.size()));
			result.emitted.insert(
				result.emitted.end(),
				page.items.begin(),
				page.items.end());
			result.pages.push_back(std::move(page));
			continue;
		}
		CHECK(!step.fetch.empty());
		if (step.fetch.empty()) {
			break;
		}
		for (const auto src : step.fetch) {
			auto &script = scripts[size_t(src)];
			CHECK(!script.empty());
			if (script.empty()) {
				engine.fail(src, SourceError::Timeout);
				continue;
			}
			auto event = std::move(script.front());
			script.pop_front();
			engine.markFetching(src);
			if (event.fail) {
				engine.fail(src, event.error, event.retryAfterMs);
				continue;
			}
			const auto ok = engine.feed(
				src,
				event.items,
				event.exhausted);
			CHECK(ok == !event.expectReject);
			if (ok) {
				result.delivered.insert(
					result.delivered.end(),
					event.items.begin(),
					event.items.end());
			}
		}
	}
	CHECK(engine.finished());
	for (size_t i = 1; i < result.emitted.size(); ++i) {
		CHECK(result.emitted[i - 1] > result.emitted[i]);
	}
	return result;
}

std::vector<Position> expected(
		const std::vector<Item> &delivered,
		const Options &options) {
	auto sorted = delivered;
	std::sort(sorted.begin(), sorted.end(), [](const Item &a, const Item &b) {
		return b.position < a.position;
	});
	auto seen = std::set<std::pair<int64_t, int64_t>>();
	auto seenPeers = std::set<int64_t>();
	auto out = std::vector<Position>();
	for (const auto &item : sorted) {
		if (options.onlyForwardable && !item.forwardable) {
			continue;
		}
		const auto &p = item.position;
		if (!seen.emplace(p.peer, p.msg).second) {
			continue;
		}
		if (options.uniquePerPeer && !seenPeers.emplace(p.peer).second) {
			continue;
		}
		out.push_back(p);
	}
	return out;
}

void checkPages(
		const Driven &result,
		const Options &options,
		bool expectClean) {
	CHECK(!result.pages.empty());
	for (size_t i = 0; i + 1 < result.pages.size(); ++i) {
		CHECK(result.pages[i].hasMore);
	}
	if (result.pages.empty()) {
		return;
	}
	CHECK(!result.pages.back().hasMore);
	const auto want = expected(result.delivered, options);
	CHECK(result.emitted == want);
	if (expectClean) {
		CHECK(result.pages.back().exactTotal.has_value());
		CHECK(result.pages.back().exactTotal.value_or(-1) == int(want.size()));
		for (const auto &page : result.pages) {
			CHECK(page.coverage == Coverage::Complete);
			CHECK(page.error == SourceError::None);
		}
	} else {
		CHECK(!result.pages.back().exactTotal.has_value());
		CHECK(result.pages.back().coverage == Coverage::Partial);
	}
}

Options options(int pageSize, bool uniquePerPeer = false, bool forwardable = false) {
	auto result = Options();
	result.pageSize = pageSize;
	result.uniquePerPeer = uniquePerPeer;
	result.onlyForwardable = forwardable;
	return result;
}

void testBasicMerge() {
	auto engine = MergeEngine(options(3));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({ makeItem(100, 1, 1), makeItem(90, 2, 1) }),
		  page({ makeItem(80, 3, 1) }, true) },
		{ page({ makeItem(95, 4, 1), makeItem(85, 5, 1) }, true) },
	}, 3);
	checkPages(result, options(3), true);
	CHECK(result.emitted.size() == 5);
	CHECK((result.emitted.front() == Position{ 100, 1, 1 }));
	CHECK((result.emitted.back() == Position{ 80, 3, 1 }));
}

// Equal-date runs span several raw pages and arrive with unordered
// FullMsgIds; nothing of that date may be committed before the run
// is provably closed by every source.
void testEqualDateRunUnordered() {
	auto engine = MergeEngine(options(2));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({ makeItem(10, 5, 1), makeItem(10, 3, 2) }),
		  page({ makeItem(10, 9, 1), makeItem(10, 1, 7) }, true) },
		{ page({ makeItem(10, 2, 3), makeItem(8, 6, 1) }, true) },
	}, 2);
	checkPages(result, options(2), true);
	const auto want = std::vector<Position>{
		{ 10, 9, 1 }, { 10, 5, 1 }, { 10, 3, 2 },
		{ 10, 2, 3 }, { 10, 1, 7 }, { 8, 6, 1 },
	};
	CHECK(result.emitted == want);
	CHECK(result.pages.size() == 3);
}

// A source may legally repeat an item in a later raw page; the
// duplicate is dropped by the (peer, msg) dedup, not an error.
void testSameSourceDuplicate() {
	auto engine = MergeEngine(options(10));
	engine.addSource();
	auto result = drive(engine, {
		{ page({ makeItem(10, 1, 1), makeItem(9, 2, 1) }),
		  page({ makeItem(9, 2, 1), makeItem(8, 1, 2) }, true) },
	}, 10);
	checkPages(result, options(10), true);
	const auto want = std::vector<Position>{
		{ 10, 1, 1 }, { 9, 2, 1 }, { 8, 1, 2 },
	};
	CHECK(result.emitted == want);
}

void testUniquePerPeer() {
	auto engine = MergeEngine(options(10, true));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({ makeItem(10, 1, 1), makeItem(9, 1, 2), makeItem(8, 2, 1) },
			true) },
		{ page({ makeItem(9, 2, 2), makeItem(7, 3, 1) }, true) },
	}, 10);
	checkPages(result, options(10, true), true);
	const auto want = std::vector<Position>{
		{ 10, 1, 1 }, { 9, 2, 2 }, { 7, 3, 1 },
	};
	CHECK(result.emitted == want);
}

// Raw pages that carry no usable items (filtered out or empty) must
// not be mistaken for exhaustion; the source keeps being fetched.
void testFilteredEmptyPages() {
	auto engine = MergeEngine(options(2, false, true));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({}),
		  page({ makeItem(10, 1, 1, false), makeItem(9, 1, 2) }, true) },
		{ page({ makeItem(8, 2, 1, false) }, true) },
	}, 2);
	checkPages(result, options(2, false, true), true);
	const auto want = std::vector<Position>{ { 9, 1, 2 } };
	CHECK(result.emitted == want);
}

void testSourceFailure() {
	auto engine = MergeEngine(options(2));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({ makeItem(10, 1, 1) }), failure(SourceError::Network) },
		{ page({ makeItem(9, 2, 1) }, true) },
	}, 2);
	checkPages(result, options(2), false);
	CHECK(result.emitted.size() == 2);
	CHECK(result.pages.back().error == SourceError::Network);
}

// Only the date is required to stay monotonic; an item dated after
// the previous tail fails the source as Protocol, items fed before
// the violation stay committed.
void testProtocolViolation() {
	auto engine = MergeEngine(options(10));
	engine.addSource();
	engine.addSource();
	auto bad = page({ makeItem(11, 1, 2), makeItem(9, 1, 3) });
	bad.expectReject = true;
	auto result = drive(engine, {
		{ page({ makeItem(10, 1, 1) }), bad },
		{ page({ makeItem(9, 2, 1) }, true) },
	}, 10);
	checkPages(result, options(10), false);
	const auto want = std::vector<Position>{
		{ 10, 1, 1 }, { 9, 2, 1 },
	};
	CHECK(result.emitted == want);
	CHECK(result.pages.back().error == SourceError::Protocol);
}

void testFloodWait() {
	auto engine = MergeEngine(options(2));
	engine.addSource();
	engine.addSource();
	engine.addSource();
	// FloodWait lands on the first extra fetch, before the other
	// source's Network failure, so it stays the reported error.
	auto result = drive(engine, {
		{ page({ makeItem(10, 1, 1) }),
		  failure(SourceError::FloodWait, 5000) },
		{ page({ makeItem(8, 3, 1) }),
		  failure(SourceError::Network) },
		{ page({ makeItem(9, 2, 1) }, true) },
	}, 2);
	checkPages(result, options(2), false);
	CHECK(result.pages.back().error == SourceError::FloodWait);
	CHECK(result.pages.back().retryAfterMs == 5000);
}

void testFloodResumeSameSource() {
	auto engine = MergeEngine(options(10));
	const auto source = engine.addSource();
	auto delivered = std::vector<Item>();
	auto account = RestrictedSearch::AccountSchedule();
	auto now = int64_t(0);
	for (auto attempt = 0; attempt != 3; ++attempt) {
		const auto step = engine.step();
		CHECK(!step.pageReady);
		CHECK(step.fetch == std::vector<int>{ source });
		while (!account.acquire(now)) {
			now += RestrictedSearch::AccountSchedule::kSendInterval;
		}
		engine.markFetching(source);
		account.flood(now, 1000);
		account.release();
		CHECK(engine.deferFetching(source, account.cooldownLeft(now)));
		CHECK(!engine.terminal(source));
		CHECK(engine.lastFed(source) == Position());
		CHECK(!account.acquire(account.cooldown - 1));
		now = account.cooldown;
		CHECK(account.cooldownLeft(now) == 0);
		CHECK(engine.needsFetch(source));
	}
	const auto resumed = engine.step();
	CHECK(resumed.fetch == std::vector<int>{ source });
	CHECK(account.acquire(now));
	engine.markFetching(source);
	const auto pageItems = std::vector<Item>{
		makeItem(10, 7, 1), makeItem(9, 7, 2),
	};
	delivered.insert(delivered.end(), pageItems.begin(), pageItems.end());
	CHECK(engine.feed(source, pageItems, true));
	account.release();
	CHECK(engine.finished() == false);
	const auto ready = engine.step();
	CHECK(ready.pageReady);
	const auto result = engine.takePage();
	CHECK(result.items == expected(delivered, options(10)));
	CHECK(result.coverage == Coverage::Complete);
	CHECK(result.exactTotal.value_or(-1) == 2);
	CHECK(result.error == SourceError::None);
}

void testFloodRetryExhaustionIsPartial() {
	auto engine = MergeEngine(options(10));
	const auto source = engine.addSource();
	auto account = RestrictedSearch::AccountSchedule();
	auto now = int64_t(0);
	for (auto attempt = 0; attempt != 4; ++attempt) {
		const auto step = engine.step();
		CHECK(!step.pageReady);
		CHECK(step.fetch == std::vector<int>{ source });
		while (!account.acquire(now)) {
			now += RestrictedSearch::AccountSchedule::kSendInterval;
		}
		engine.markFetching(source);
		account.flood(now, 500);
		account.release();
		const auto deferred = engine.deferFetching(
			source,
			account.cooldownLeft(now));
		CHECK(deferred == (attempt < 3));
		if (attempt < 3) {
			CHECK(!account.acquire(account.cooldown - 1));
			now = account.cooldown;
			CHECK(account.cooldownLeft(now) == 0);
			CHECK(!engine.terminal(source));
			CHECK(engine.needsFetch(source));
		}
	}
	CHECK(engine.terminal(source));
	const auto ready = engine.step();
	CHECK(ready.pageReady);
	const auto result = engine.takePage();
	CHECK(result.coverage == Coverage::Partial);
	CHECK(result.error == SourceError::FloodWait);
	CHECK(!result.exactTotal);
	CHECK(!result.hasMore);
}

void testEmptyRound() {
	auto engine = MergeEngine(options(5));
	engine.addSource();
	engine.addSource();
	auto result = drive(engine, {
		{ page({}, true) },
		{ page({}, true) },
	}, 5);
	CHECK(result.pages.size() == 1);
	CHECK(result.pages.back().items.empty());
	CHECK(!result.pages.back().hasMore);
	CHECK(result.pages.back().coverage == Coverage::Complete);
	CHECK(result.pages.back().exactTotal.value_or(-1) == 0);
}

// Deterministic randomized oracle check: shuffled equal-date runs,
// duplicates, cross-source (peer, msg) collisions, random filters
// and random failures, all compared against the sorted union.
void testRandomized() {
	auto rng = std::mt19937(20261002u);
	auto coin = [&rng](int denominator) {
		return std::uniform_int_distribution<int>(0, denominator - 1)(rng)
			== 0;
	};
	for (auto iteration = 0; iteration < 400; ++iteration) {
		auto opts = options(
			1 + int(std::uniform_int_distribution<int>(0, 6)(rng)),
			coin(3),
			coin(3));
		auto engine = MergeEngine(opts);
		const auto count = 1
			+ int(std::uniform_int_distribution<int>(0, 4)(rng));
		auto scripts = std::vector<std::deque<Event>>();
		for (auto s = 0; s < count; ++s) {
			engine.addSource();
			const auto n = int(std::uniform_int_distribution<int>(0, 30)(rng));
			auto stream = std::vector<Item>();
			auto date = int64_t(50)
				+ std::uniform_int_distribution<int>(0, 60)(rng);
			for (auto i = 0; i < n; ++i) {
				date -= std::uniform_int_distribution<int>(0, 3)(rng);
				stream.push_back(makeItem(
					date,
					std::uniform_int_distribution<int>(1, 12)(rng),
					std::uniform_int_distribution<int>(1, 12)(rng),
					!coin(5)));
			}
			// Shuffle equal-date runs to model unordered raw pages.
			auto begin = stream.begin();
			while (begin != stream.end()) {
				auto end = begin;
				while (end != stream.end()
					&& end->position.date == begin->position.date) {
					++end;
				}
				std::shuffle(begin, end, rng);
				begin = end;
			}
			auto script = std::deque<Event>();
			auto rest = std::deque<Item>(stream.begin(), stream.end());
			while (!rest.empty()) {
				const auto take = 1
					+ int(std::uniform_int_distribution<int>(0, 7)(rng));
				auto chunk = std::vector<Item>();
				for (auto i = 0; i < take && !rest.empty(); ++i) {
					chunk.push_back(rest.front());
					rest.pop_front();
				}
				// Sometimes repeat the last item of a page as the
				// first of the next page: a legal duplicate.
				if (!rest.empty() && coin(6)) {
					rest.push_front(chunk.back());
				}
				script.push_back(page(std::move(chunk)));
			}
			if (coin(7)) {
				if (coin(2)) {
					script.push_back(failure(
						SourceError::FloodWait,
						std::uniform_int_distribution<int>(1, 9)(rng)
							* 1000));
				} else {
					script.push_back(failure(SourceError::Network));
				}
			} else if (!script.empty()) {
				script.back().exhausted = true;
			} else {
				script.push_back(page({}, true));
			}
			scripts.push_back(std::move(script));
		}
		auto clean = true;
		for (const auto &script : scripts) {
			for (const auto &event : script) {
				if (event.fail) {
					clean = false;
				}
			}
		}
		auto result = drive(engine, std::move(scripts), opts.pageSize);
		checkPages(result, opts, clean);
	}
}

} // namespace

int main() {
	testBasicMerge();
	testEqualDateRunUnordered();
	testSameSourceDuplicate();
	testUniquePerPeer();
	testFilteredEmptyPages();
	testSourceFailure();
	testProtocolViolation();
	testFloodWait();
	testFloodResumeSameSource();
	testFloodRetryExhaustionIsPartial();
	testEmptyRound();
	testRandomized();
	if (g_failures == 0) {
		std::printf("restricted_search_merge: all tests passed\n");
		return 0;
	}
	std::fprintf(stderr, "%d check(s) failed\n", g_failures);
	return 1;
}
