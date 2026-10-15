#include "info/global_media/global_media_query_cache.h"
#include "info/global_media/restricted_global_media_pagination.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using namespace Info::GlobalMedia::Pagination;

namespace {

struct QueryList {
	std::vector<int> list;
	std::vector<int> ids;
	int offsetPosition = 0;
	int offsetRate = 0;
	int fullCount = 0;
	int filteredCount = 0;
	int requestId = 0;
	std::uint64_t requestToken = 0;
	std::vector<std::function<void()>> requestWaiters;
	std::vector<int> requestCursors;
	bool loaded = false;
	bool failed = false;
};

void CheckFailedQueryPage() {
	using Info::GlobalMedia::CompleteQueryRequest;
	using Info::GlobalMedia::QueryPageCanLoad;
	using Info::GlobalMedia::RestartQueryCaches;
	using Info::GlobalMedia::RetryQueryRequest;
	auto lists = std::map<int, QueryList>();
	auto &first = lists[1];
	first.requestId = 17;
	first.requestCursors = { 0 };
	auto notifications = 0;
	first.requestWaiters.push_back([&] { ++notifications; });
	// RPC failure used to look identical to an exhausted empty page.
	assert(!CompleteQueryRequest(first, true, true));
	assert(!first.loaded && first.requestId == 0);
	assert(first.requestToken == 1); // stale completions no longer match
	assert(first.requestCursors.empty());
	assert(first.requestWaiters.size() == 1 && notifications == 0);
	assert(first.failed);
	auto failed = std::exchange(first.requestWaiters, {});
	failed.front()();
	assert(notifications == 1 && !QueryPageCanLoad(first));
	// A -> B -> A must retain a retryable first page, not a loaded empty list.
	const auto cancel = [](int) { assert(false); };
	assert(RestartQueryCaches(lists, false, cancel).empty());
	assert(lists.size() == 1 && !lists.at(1).loaded);
	assert(RetryQueryRequest(first));
	assert(QueryPageCanLoad(first));
	assert(!RetryQueryRequest(first));
	first.requestId = 18;
	first.requestCursors.push_back(0);
	first.requestWaiters.push_back([&] { ++notifications; });
	assert(!CompleteQueryRequest(first, true, true));
	failed = std::exchange(first.requestWaiters, {});
	failed.front()();
	assert(notifications == 2 && !QueryPageCanLoad(first));
	assert(RetryQueryRequest(first));
	first.requestId = 19;
	first.requestCursors.push_back(0);
	assert(CompleteQueryRequest(first, false, true));
	assert(first.loaded && !first.failed && !QueryPageCanLoad(first));
	assert(RestartQueryCaches(lists, false, cancel).empty());
	assert(lists.size() == 1 && lists.at(1).loaded);
	// Successful pages remain cached when a later page fails. Only the
	// failed page's cursor is rolled back, leaving the loop guard intact.
	auto &partial = lists[2];
	partial.list = { 9, 8 };
	partial.ids = { 9, 8 };
	partial.offsetPosition = 8;
	partial.offsetRate = 42;
	partial.fullCount = 6;
	partial.filteredCount = 1;
	partial.requestId = 18;
	partial.requestCursors = { 0, 8 };
	partial.requestWaiters.push_back([&] { ++notifications; });
	assert(!CompleteQueryRequest(partial, true, true));
	assert(!partial.loaded && partial.requestId == 0);
	assert(partial.requestToken == 1);
	assert((partial.requestCursors == std::vector<int>{ 0 }));
	assert(partial.requestWaiters.size() == 1 && notifications == 2);
	failed = std::exchange(partial.requestWaiters, {});
	failed.front()();
	assert(notifications == 3 && !QueryPageCanLoad(partial));
	assert(RestartQueryCaches(lists, false, cancel).empty());
	assert((partial.list == std::vector<int>{ 9, 8 }));
	assert((partial.ids == std::vector<int>{ 9, 8 }));
	assert(partial.offsetPosition == 8 && partial.offsetRate == 42);
	assert(partial.fullCount == 6 && partial.filteredCount == 1);
	// Retrying that same cursor can now complete; a genuine exhausted page
	// stays loaded and keeps its cursor and successful items on restart.
	partial.requestId = 19;
	assert(!RetryQueryRequest(partial));
	partial.requestId = 0;
	assert(RetryQueryRequest(partial));
	partial.requestId = 19;
	partial.requestCursors.push_back(8);
	partial.requestWaiters.push_back([&] { ++notifications; });
	assert(CompleteQueryRequest(partial, false, true));
	assert(partial.loaded && partial.requestId == 0);
	assert(partial.requestWaiters.size() == 1 && notifications == 3);
	assert((partial.requestCursors == std::vector<int>{ 0, 8 }));
	// Provider drains the successful page's waiters after merging its data.
	auto completed = std::exchange(partial.requestWaiters, {});
	completed.front()();
	assert(notifications == 4);
	assert(RestartQueryCaches(lists, false, cancel).empty());
	assert(partial.loaded && partial.list.size() == 2);
	auto empty = QueryList{};
	empty.requestCursors = { 0 };
	assert(CompleteQueryRequest(empty, false, true));
	assert(empty.loaded && empty.list.empty());
	auto more = QueryList{};
	more.requestId = 20;
	more.requestCursors = { 0 };
	assert(CompleteQueryRequest(more, false, false));
	assert(!more.loaded && more.requestId == 0);
	assert((more.requestCursors == std::vector<int>{ 0 }));
}

void CheckQueryCacheRestart() {
	using Info::GlobalMedia::RestartQueryCaches;
	auto lists = std::map<int, QueryList>();
	lists.emplace(1, QueryList{
		.list = { 9, 8 },
		.ids = { 9, 8 },
		.offsetPosition = 8,
		.offsetRate = 42,
		.fullCount = 6,
		.filteredCount = 1,
		.requestId = 17,
		.requestToken = 3,
		.requestCursors = { 0, 8 },
	});
	lists.emplace(2, QueryList{
		.list = { 7 },
		.ids = { 7 },
		.fullCount = 1,
		.requestCursors = { 0 },
		.loaded = true,
	});
	auto notified = 0;
	lists.at(1).requestWaiters.push_back([&] { ++notified; });
	lists.at(2).requestWaiters.push_back([&] { ++notified; });
	auto cancelled = std::vector<int>();
	const auto cancel = [&](int id) { cancelled.push_back(id); };
	const auto oldToken = lists.at(1).requestToken;
	auto waiters = RestartQueryCaches(lists, false, cancel);
	assert((cancelled == std::vector<int>{ 17 }));
	assert(notified == 0); // callbacks run only after restart state is reset
	assert(waiters.size() == 2);
	assert(lists.size() == 2); // ordinary query A -> B -> A retains both
	const auto &partial = lists.at(1);
	assert((partial.list == std::vector<int>{ 9, 8 }));
	assert((partial.ids == std::vector<int>{ 9, 8 }));
	assert(partial.offsetPosition == 8 && partial.offsetRate == 42);
	assert(partial.fullCount == 6 && partial.filteredCount == 1);
	assert(!partial.loaded && partial.requestId == 0);
	assert(partial.requestToken != oldToken); // reject cancelled completion
	assert((partial.requestCursors == std::vector<int>{ 0 }));
	assert(partial.requestWaiters.empty());
	assert(lists.at(2).loaded && lists.at(2).fullCount == 1);
	assert((lists.at(2).requestCursors == std::vector<int>{ 0 }));
	assert(lists.at(2).requestWaiters.empty());
	for (auto &callback : waiters) {
		callback();
	}
	assert(notified == 2);
	// A repeated restart must not remove a successfully consumed cursor,
	// cancel twice, or retain old waiters.
	assert(RestartQueryCaches(lists, false, cancel).empty());
	assert(cancelled.size() == 1);
	assert((lists.at(1).requestCursors == std::vector<int>{ 0 }));
	// Switching ordinary/restricted mode invalidates incompatible results,
	// including loaded results, and drains an in-flight request exactly once.
	lists.at(1).requestId = 18;
	lists.at(1).requestCursors.push_back(8);
	lists.at(1).requestWaiters.push_back([&] { ++notified; });
	waiters = RestartQueryCaches(lists, true, cancel);
	assert(lists.empty());
	assert((cancelled == std::vector<int>{ 17, 18 }));
	assert(waiters.size() == 1);
	waiters.front()();
	assert(notified == 3);
	assert(RestartQueryCaches(lists, true, cancel).empty());
}

struct MockPaginator {
	int total = 260;
	int pageSize = 100;
	int loaded = 0;
	bool pending = false;
	bool hasMore = true;
	int requests = 0;

	bool check(bool preloadTop, bool preloadBottom) {
		if (!ShouldRequestOlderPage(
				preloadTop,
				preloadBottom,
				hasMore,
				pending)) {
			return false;
		}
		pending = true;
		++requests;
		return true;
	}

	void completePage() {
		assert(pending);
		loaded = std::min(total, loaded + pageSize);
		hasMore = loaded < total;
		pending = false;
	}
};

} // namespace

int main() {
	CheckFailedQueryPage();
	CheckQueryCacheRestart();
	// The first 100 items cover the viewport; neither top-edge checks nor
	// repeated layout checks should eagerly consume the remaining pages.
	auto paginator = MockPaginator{};
	paginator.loaded = 100;
	assert(!paginator.check(true, false));
	assert(!paginator.check(true, false));
	assert(paginator.requests == 0);

	// A short viewport keeps bottom preload asserted until enough items arrive.
	assert(paginator.check(false, true));
	assert(!paginator.check(false, true)); // one in-flight page only
	paginator.completePage();
	assert(paginator.loaded == 200);
	assert(paginator.check(false, true));
	paginator.completePage();
	assert(paginator.loaded == 260);
	assert(!paginator.hasMore);
	assert(!paginator.check(false, true)); // exhausted
	assert(!paginator.check(true, true)); // top remains inert after exhaustion
	assert(paginator.requests == 2);

	// After the viewport is filled, no more pages load until the user reaches
	// the bottom preload region.
	auto scrolled = MockPaginator{};
	scrolled.loaded = 100;
	assert(!scrolled.check(false, false));
	assert(!scrolled.check(true, false));
	assert(scrolled.requests == 0);
	assert(scrolled.check(false, true));

	// Verify skipped counts for both a moving slice window and the newest-first
	// cumulative restricted-search slice, including an unknown total.
	const auto middle = MakeSkippedCounts(80, 40, 260, true);
	assert(middle.after == 80);
	assert(middle.before == 140);
	const auto firstPage = MakeSkippedCounts(0, 100, 260, true);
	assert(firstPage.after == 0);
	assert(firstPage.before == 160);
	const auto unknownFirstPage = MakeSkippedCounts(0, 100, std::nullopt, true);
	assert(unknownFirstPage.after == 0);
	assert(unknownFirstPage.before > 0);
	const auto exhausted = MakeSkippedCounts(0, 260, 260, false);
	assert(exhausted.after == 0 && exhausted.before == 0);

	std::cout << "restricted_global_media_pagination: all tests passed\n";
	return 0;
}
