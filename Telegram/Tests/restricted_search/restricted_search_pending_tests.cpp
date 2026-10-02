#include "dialogs/restricted_search_pending.h"

#include <cassert>

int main() {
	auto pending = Dialogs::RestrictedSearchPending();

	// Finishing native search does not touch this independent state.
	pending.begin(1);
	assert(pending.pending());
	assert(pending.finishPage(1));
	assert(!pending.pending());

	// Cancelling a query invalidates late pages, including after a new query.
	pending.begin(2);
	pending.cancel(3);
	pending.begin(4);
	assert(!pending.finishPage(2));
	assert(pending.pending());
	assert(pending.finishPage(4));
	assert(!pending.pending());

	// Retry keeps the same generation pending until its replacement page lands.
	pending.begin(5);
	pending.begin(5);
	assert(pending.pending());
	assert(pending.finishPage(5));
	assert(!pending.pending());
}
