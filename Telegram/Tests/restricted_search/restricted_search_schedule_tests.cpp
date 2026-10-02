#include "api/restricted_search_schedule.h"
#include "api/restricted_search_merge.h"
#include <cassert>
#include <iostream>
#include <algorithm>
#include <vector>

using namespace RestrictedSearch;

struct FakeTransport {
	AccountSchedule &account;
	int64_t now = 0;
	struct Request { Deadline deadline; bool active = true; };
	std::vector<Request> requests;
	bool send() {
		if (!account.acquire(now)) return false;
		Request request;
		request.deadline.start(now, 10000);
		requests.push_back(request);
		return true;
	}
	void cancel() {
		for (auto &request : requests) if (request.active) {
			request.active = false;
			account.release();
		}
	}
	void expire() {
		for (auto &request : requests) if (request.active && request.deadline.expired(now)) {
			request.active = false;
			account.release();
		}
	}
};

int main() {
	AccountSchedule account;
	FakeTransport main{account}, media{account};
	main.now = 350;
	assert(main.send());
	media.now = 649;
	assert(!media.send());
	main.cancel();
	media.now = 650;
	assert(media.send());
	main.now = 950;
	assert(main.send());
	media.now = 1250;
	assert(media.send());
	main.now = 1550;
	assert(main.send());
	media.now = 1850;
	assert(media.send());
	main.now = 2150;
	assert(main.send());
	assert(account.running() == 6);
	media.now = 2449;
	assert(!main.send() && !media.send());
	main.cancel();
	main.cancel();
	assert(account.running() == 3);
	media.now = 11849;
	media.expire();
	assert(account.running() == 1);
	media.now = 11850;
	media.expire();
	assert(account.running() == 0);
	account.flood(11850, 30000);
	main.now = 11851;
	assert(!main.send());
	account.flood(13000, 1000);
	assert(account.cooldown == 41850);
	assert(account.cooldownLeft(13000) == 28850);
	assert(account.cooldownLeft(41849) == 1);
	assert(account.cooldownLeft(41850) == 0);
	media.now = 41849;
	assert(!media.send());
	media.now = 41850;
	assert(media.send());
	media.cancel();
	assert(InputDelayElapsed(0, 350, 350));
	assert(!InputDelayElapsed(0, 900, 350));
	assert(InputDelayElapsed(0, 900, 900));
	assert(InputDelayElapsed(0, 350, 1200));
	assert(InputDelayElapsed(5000, 0, 5000));
	Deadline queued;
	assert(!queued.expired(20000));
	queued.start(20000, 10000);
	assert(!queued.expired(29999));
	assert(queued.expired(30000));

	// A FloodWait closes only the request that received it. Unsent
	// candidates remain live and are dispatched after the shared cooldown.
	using Engine = MergeEngine;
	using Item = Engine::Item;
	auto floodAccount = AccountSchedule();
	auto engine = Engine(Engine::Options{ .pageSize = 200 });
	constexpr auto kCandidates = 106;
	for (auto i = 0; i != kCandidates; ++i) engine.addSource();
	auto now = int64_t(0);
	std::vector<int> initiallySent;
	const auto initialStep = engine.step();
	assert(int(initialStep.fetch.size()) == kCandidates);
	for (const auto source : initialStep.fetch) {
		if (initiallySent.size() == 6) break;
		while (now < floodAccount.cooldown || !floodAccount.acquire(now)) {
			now += AccountSchedule::kSendInterval;
			assert(now < 10000);
		}
		engine.markFetching(source);
		initiallySent.push_back(source);
		now += AccountSchedule::kSendInterval;
	}
	assert(initiallySent.size() == 6);
	const auto flooded = initiallySent[2];
	now += 100;
	floodAccount.flood(now, 5000);
	engine.fail(flooded, SourceError::FloodWait, floodAccount.cooldownLeft(now));
	floodAccount.release();
	for (const auto source : initiallySent) {
		if (source == flooded) continue;
		assert(engine.feed(source, { Item{ Position{ 1000 - source, source + 1, 1 } } }, true));
		floodAccount.release();
	}
	assert(floodAccount.running() == 0);
	for (auto source = 0; source != kCandidates; ++source) {
		if (source == flooded || source < 6) assert(engine.terminal(source));
		else assert(!engine.terminal(source));
	}
	auto step = engine.step();
	assert(!step.pageReady);
	assert(step.fetch.size() >= size_t(kCandidates - 6));
	const auto cooldownEnd = floodAccount.cooldown;
	assert(floodAccount.cooldownLeft(cooldownEnd - 1) == 1);
	assert(!floodAccount.acquire(cooldownEnd - 1));

	now = cooldownEnd;
	assert(floodAccount.cooldownLeft(now) == 0);
	while (!engine.finished()) {
		step = engine.step();
		if (step.pageReady) {
			const auto page = engine.takePage();
			assert(page.coverage == Engine::Coverage::Partial);
			assert(page.error == SourceError::FloodWait);
			assert(page.retryAfterMs == 5000);
			assert(page.items.size() == size_t(kCandidates - 1));
			assert(!page.exactTotal);
			break;
		}
		assert(!step.fetch.empty());
		auto sent = 0;
		for (const auto source : step.fetch) {
			if (engine.terminal(source)) continue;
			while (!floodAccount.acquire(now)) now += AccountSchedule::kSendInterval;
			engine.markFetching(source);
			++sent;
			assert(floodAccount.running() <= 6);
			assert(engine.feed(source, { Item{ Position{ 1000 - source, source + 1, 1 } } }, true));
			floodAccount.release();
			now += AccountSchedule::kSendInterval;
		}
		assert(sent > 0);
	}
	std::cout << "restricted_search_schedule: all tests passed\n";
}
