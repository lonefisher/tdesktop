#include "info/global_media/restricted_global_media_pagination.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <vector>

using namespace Info::GlobalMedia::Pagination;

namespace {

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
