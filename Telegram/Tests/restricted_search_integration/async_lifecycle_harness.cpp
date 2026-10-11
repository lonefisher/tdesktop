#include "api/restricted_search_core.h"
#include <iostream>
#include <stdexcept>
namespace C = Api::RestrictedSearchCore;
namespace S = Api::RestrictedSearchScope;
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Clock final : C::Clock {
	int64_t time = 0;
	std::optional<int64_t> due;
	std::function<void()> callback;
	int64_t now() const override { return time; }
	void arm(int64_t delay, std::function<void()> cb) override { due = time + delay; callback = std::move(cb); }
	void cancel() override { due.reset(); callback = {}; }
	void advance(int64_t target) {
		check(target >= time, "clock reversed");
		int count = 0;
		while (due && *due <= target) {
			check(++count < 100000, "scheduler made no progress");
			time = *due; due.reset(); auto cb = std::move(callback); cb();
		}
		time = target;
	}
};
struct Candidates final : C::Candidates {
	C::Snapshot facts;
	std::function<void(C::Peer)> updated;
	std::vector<C::Peer> full;
	std::vector<int> dialogs;
	std::vector<std::pair<int, bool>> pinned;
	void begin(std::function<void(C::Peer)> cb) override { updated = std::move(cb); }
	C::Snapshot snapshot(const C::Query &) override { return facts; }
	void requestDialogs(int id) override { dialogs.push_back(id); }
	void requestPinned(int id, bool reload) override { pinned.emplace_back(id, reload); }
	void requestFull(C::Peer peer) override { full.push_back(peer); }
	void end() override { updated = {}; }
};
struct Transport final : C::Transport {
	struct Sent {
		C::Ticket ticket;
		C::Request request;
		std::function<void(C::Decode)> done;
		std::function<void(C::Error)> fail;
		bool cancelled = false;
	};
	std::vector<Sent> sent;
	int decoded = 0;
	void send(C::Ticket t, C::Request r, std::function<void(C::Decode)> done, std::function<void(C::Error)> fail) override {
		sent.push_back({ t, std::move(r), std::move(done), std::move(fail) });
	}
	void cancel(C::Ticket t) override { for (auto &s : sent) if (s.ticket == t) s.cancelled = true; }
	void reply(size_t i, C::Response r) {
		auto cb = sent.at(i).done;
		cb([this, r = std::move(r)] { ++decoded; return r; });
	}
	void error(size_t i, C::Error e = {}) { auto cb = sent.at(i).fail; cb(e); }
};
C::Candidate candidate(C::Peer peer, int folder = 0) {
	return { peer, { .origin = S::Origin::Dialog, .kind = S::PeerKind::Megagroup,
		.restricted = S::RestrictedState::Restricted, .folderId = folder, .listedInDialogs = true, .channel = true } };
}
C::Message message(int64_t date, C::Peer peer, int64_t id, bool forward = true) {
	return { peer, id, date, C::Engine::Item{ { date, int64_t(peer), id }, forward } };
}
C::Response end(std::vector<C::Message> rows = {}) { return { std::move(rows), true, false, {} }; }
struct Fixture {
	Clock clock; Candidates candidates; Transport transport;
	std::shared_ptr<C::Account> account = std::make_shared<C::Account>();
	C::Coordinator core{ transport, clock, candidates, account };
	std::vector<C::Page> pages;
	void start(C::Query q = {}) { core.start(std::move(q), [this](C::Page p) { pages.push_back(std::move(p)); }); flush(); }
	void flush() { clock.advance(clock.now()); }
};
void dispatchAndPage() {
	Fixture f; f.start();
	check(f.transport.sent.size() == 1, "production coordinator must dispatch official request");
	f.transport.reply(0, end({ message(90, 10, 1) })); f.flush();
	check(f.pages.size() == 1 && f.pages[0].items.size() == 1 && f.pages[0].exactTotal == 1, "response/page/count mismatch");
}
void scopes();
void preparation();
void lifecycle();
void pagination();
void protocolAndTimeout();
void sharedAccountFloodAndLimits();
void inputDelays();
void typeMatrix();
void preparationSignalsAndGeneration();
void manyCandidatesFloodRecovery();
void networkRetryAndDecodeReentrancy();
void synchronousCancellation();
int main() {
	try {
		for (const auto &[name, run] : std::vector<std::pair<const char*, void(*)()>>{
			{"dispatch/page",dispatchAndPage},{"scopes",scopes},{"preparation",preparation},
			{"lifecycle",lifecycle},{"pagination/cache",pagination},{"protocol/timeout",protocolAndTimeout},
			{"account/flood",sharedAccountFloodAndLimits},{"input delays",inputDelays},
			{"type matrix",typeMatrix},{"preparation signals/generation",preparationSignalsAndGeneration},
			{"106 candidates flood recovery",manyCandidatesFloodRecovery},{"network retry/decode cancellation",networkRetryAndDecodeReentrancy},
			{"synchronous dispatch cancellation",synchronousCancellation}}) {
			run(); std::cout << "PASS: " << name << '\n';
		}
		std::cout << "production core: 13 scenarios passed\n"; return 0;
	}
	catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
void scopes() {
	for (auto folder : { -1, 0, 1 }) {
		Fixture f; f.candidates.facts.candidates = { candidate(10), candidate(20, 1), candidate(30, 2) };
		auto broadcast = candidate(40); broadcast.facts.kind = S::PeerKind::Broadcast;
		auto cached = candidate(50); cached.facts.listedInDialogs = false;
		f.candidates.facts.candidates.push_back(broadcast); f.candidates.facts.candidates.push_back(cached);
		C::Query q; q.explicitFolder = folder >= 0; q.folderId = std::max(0, folder); q.type.groupsOnly = true;
		f.start(q); f.clock.advance(300);
		std::set<C::Peer> peers; for (const auto &r : f.transport.sent) if (r.request.peer) peers.insert(r.request.peer);
		const auto expected = folder < 0 ? std::set<C::Peer>{10,20} : folder == 0 ? std::set<C::Peer>{10} : std::set<C::Peer>{20};
		check(peers == expected, "Main/Archive/type/cached-only scope dispatch");
	}
	Fixture f;
	auto joined = candidate(10); joined.facts.origin = S::Origin::FoldedCommunityMember; joined.facts.joined = true;
	auto left = joined; left.peer = 20; left.facts.joined = false;
	auto unknownFolder = joined; unknownFolder.peer = 30; unknownFolder.facts.folderKnown = false;
	f.candidates.facts.candidates = {joined, left, unknownFolder}; f.start();
	check(f.transport.sent.size() == 2 && f.transport.sent[1].request.peer == 10, "folded community membership");
	Fixture g;
	auto link = candidate(60,2); link.facts.origin = S::Origin::ExplicitCommunityLink; link.facts.canViewHistory = true;
	auto denied = link; denied.peer = 70; denied.facts.canViewHistory = false;
	g.candidates.facts.candidates = {link,denied,candidate(80)};
	C::Query q; q.community = 90; g.start(q);
	check(g.candidates.full == std::vector<C::Peer>{90}, "explicit community must await full info");
	auto notify = g.candidates.updated; notify(90); g.clock.advance(25);
	check(g.transport.sent.size() == 2 && g.transport.sent[1].request.peer == 60, "explicit community/view-history scope");
}
void preparation() {
	Fixture f; auto unknown = candidate(10); unknown.facts.restricted = S::RestrictedState::Unknown;
	f.candidates.facts.candidates = {unknown}; f.start();
	check(f.candidates.full == std::vector<C::Peer>{10} && f.transport.sent.size() == 1, "Unknown must wait/request full");
	f.transport.reply(0,end()); f.candidates.facts.candidates[0].facts.restricted = S::RestrictedState::Restricted;
	auto notify = f.candidates.updated; check(bool(notify), "full info subscription"); notify(10); f.clock.advance(25);
	check(f.transport.sent.size() == 2, "full info must unblock supplement");
	f.transport.reply(1,end()); f.flush();
	check(f.pages.size() == 1 && f.pages[0].coverage == C::Engine::Coverage::Complete, "ready preparation coverage");
	Fixture t; t.candidates.facts.candidates = {unknown}; t.start(); t.transport.reply(0,end());
	t.clock.advance(9999); check(t.pages.empty(), "preparation timeout early"); t.clock.advance(10000);
	check(t.pages.size() == 1 && t.pages[0].error == C::Failure::Timeout && !t.pages[0].exactTotal, "Unknown partial deadline");
	Fixture pin; pin.account->pinnedFailures.insert(0); pin.account->pinnedVersions[0] = 1;
	pin.start(); pin.transport.reply(0,end());
	check(std::find(pin.candidates.pinned.begin(),pin.candidates.pinned.end(),std::pair<int,bool>{0,true}) != pin.candidates.pinned.end(), "pinned failure reload");
	pin.clock.advance(25); check(pin.pages.empty(), "pinned preparation must await version");
	pin.account->pinnedVersions[0]++; pin.clock.advance(50);
	check(pin.pages.size() == 1 && pin.pages[0].error == C::Failure::Network, "pinned repeated failure must be partial");
}
void lifecycle() {
	Fixture f; f.start(); f.core.cancel(); check(f.transport.sent[0].cancelled, "transport cancellation"); f.start();
	f.transport.reply(0,end({message(999,1,1)})); f.transport.error(0,{C::Failure::FloodWait,30000});
	check(f.transport.decoded == 0 && f.core.cooldownLeft() == 0 && f.pages.empty(), "late success/failure side effects");
	f.transport.reply(1,end({message(90,2,2)})); f.flush(); f.core.retry(); f.flush();
	check(f.transport.sent.size() == 3 && f.transport.sent[2].request.cursor.msg == 0, "fresh retry cursor");
	f.transport.reply(1,end({message(888,8,8)})); check(f.transport.decoded == 1, "stale retry response decoded");
	f.transport.reply(2,end({message(70,3,3)})); f.flush();
	check(f.pages.size() == 2 && f.pages[1].loadedCount == 1 && f.pages[1].items[0].peer == 3, "retry reused results");
	Clock clock; Candidates candidates; Transport transport;
	{ C::Coordinator core(transport,clock,candidates,std::make_shared<C::Account>());
			core.start({},[](C::Page){throw std::runtime_error("callback after destruction");}); clock.advance(0); }
	transport.reply(0,end()); transport.error(0);
	check(transport.decoded == 0 && transport.sent[0].cancelled, "destruction late callback safety");
	Fixture reentrant; reentrant.core.start({},[&](C::Page){reentrant.core.cancel();}); reentrant.flush();
	reentrant.transport.reply(0,end()); reentrant.flush(); check(!reentrant.core.active(), "callback reentrant cancel");
}
void pagination() {
	Fixture f; f.candidates.facts.candidates = {candidate(20)};
	C::Query q; q.merge.pageSize = 2; q.merge.onlyForwardable = true; q.text = "anonymous"; q.minDate = 1; q.maxDate = 100;
	f.start(q);
	check(f.transport.sent[1].request.query.text == q.text && f.transport.sent[1].request.limit == 50, "supplement query/limit");
	f.transport.reply(0, {{message(100,10,1),message(100,10,2,false)},false,false,1});
	f.transport.reply(1, {{message(100,20,1)},false,false,{}}); f.flush();
	check(f.pages.empty(), "equal-date boundary committed before closed");
	f.transport.reply(2, end({message(90,10,3),message(80,10,4)})); f.clock.advance(300);
	check(f.transport.sent.size() == 4 && f.transport.sent[3].request.cursor.msg == 1, "supplement raw cursor");
	f.transport.reply(3,end({message(90,20,2),message(90,20,3),message(80,20,4)})); f.flush();
	check(f.pages.size() == 1 && f.pages[0].items[0].peer == 20 && f.pages[0].items[1].peer == 10, "global tie order");
	auto requests = f.transport.sent.size(); f.core.requestMore(); f.flush();
	check(f.transport.sent.size() == requests && f.pages.size() == 2, "cached page must not refetch");
	check(f.pages[1].items[0].msg == 3 && f.pages[1].items[0].peer == 20, "cached tie order");
	f.core.requestMore(); f.flush(); f.core.requestMore(); f.flush();
	check(f.pages.back().exactTotal == 7 && f.pages.back().loadedCount == 7, "filtered final count");
	Fixture unique; C::Query u; u.merge.uniquePerPeer = true; unique.start(u);
	unique.transport.reply(0,end({message(100,1,1),message(90,1,1),message(80,1,2),message(70,2,1)})); unique.flush();
	check(unique.pages[0].items.size() == 2 && unique.pages[0].exactTotal == 2, "dedup/unique per peer");
	Fixture filtered; C::Query v; v.merge.onlyForwardable = true; filtered.start(v);
	filtered.transport.reply(0,{{message(100,1,1,false)},false,false,2}); filtered.flush();
	check(filtered.pages.empty() && filtered.transport.sent.size() == 2 && filtered.transport.sent[1].request.cursor.msg == 1, "filtered page must advance raw cursor");
	filtered.transport.reply(1,end({message(90,2,2)})); filtered.flush(); check(filtered.pages[0].items.size() == 1,"filtered continuation");
}
void protocolAndTimeout() {
	Fixture f; f.start(); f.transport.reply(0,{{message(100,1,1)},false,false,1}); f.flush();
	f.transport.reply(1,{{message(90,1,1)},false,false,1}); f.flush();
	check(f.pages.size() == 1 && f.pages[0].error == C::Failure::Protocol, "stalled cursor must be partial");
	Fixture order; order.start(); order.transport.reply(0,{{message(90,1,1),message(100,1,2)},true,false,{}}); order.flush();
	check(order.pages[0].error == C::Failure::Protocol, "nonmonotonic raw dates");
	Fixture timeout; timeout.candidates.facts.candidates = {candidate(10)}; timeout.start();
	timeout.transport.reply(0,end()); timeout.clock.advance(9999); check(timeout.pages.empty(),"request deadline early"); timeout.clock.advance(10000);
	check(timeout.pages.size() == 1 && timeout.pages[0].error == C::Failure::Timeout && timeout.account->running() == 0,"deadline/slot release");
	timeout.transport.reply(1,end()); check(timeout.transport.decoded == 1,"timed out success decoded");
}
void sharedAccountFloodAndLimits() {
	Clock aclock,bclock; Candidates acandidates,bcandidates; Transport at,bt;
	auto account = std::make_shared<C::Account>(); C::Coordinator a(at,aclock,acandidates,account), b(bt,bclock,bcandidates,account);
	for (C::Peer p = 1; p <= 10; ++p) { acandidates.facts.candidates.push_back(candidate(p)); bcandidates.facts.candidates.push_back(candidate(p+20)); }
	a.start({},[](C::Page){}); b.start({},[](C::Page){}); aclock.advance(0); bclock.advance(0);
	check(at.sent.size() == 2 && bt.sent.size() == 1 && account->running() == 1,"shared send interval");
	at.error(1,{C::Failure::FloodWait,5000}); aclock.advance(0);
	check(account->running() == 0 && b.cooldownLeft() == 5000,"flood shared cooldown/release");
	auto before = at.sent.size(); aclock.advance(4999); bclock.advance(4999);
	check(at.sent.size() == before && bt.sent.size() == 1,"cooldown leaked dispatch");
	aclock.advance(5000); bclock.advance(5000);
	check(at.sent.size() == before+1 && at.sent.back().request.peer == 1,"same source retry after flood");
	aclock.advance(6500); bclock.advance(6500); check(account->running() == 6,"account cap 6");
	auto total = at.sent.size()+bt.sent.size(); aclock.advance(6800); bclock.advance(6800);
	check(at.sent.size()+bt.sent.size() == total,"cap exceeded"); a.cancel(); b.cancel(); check(account->running() == 0,"cancel all slots");
	Fixture repeated; repeated.candidates.facts.candidates = {candidate(10)}; repeated.start(); repeated.transport.reply(0,end());
	for (auto attempt = 0; attempt < 4; ++attempt) {
		repeated.transport.error(size_t(attempt+1),{C::Failure::FloodWait,1000}); repeated.flush();
		if (attempt < 3) repeated.clock.advance((attempt+1)*1000);
	}
	check(repeated.pages.size() == 1 && repeated.pages[0].error == C::Failure::FloodWait && repeated.pages[0].cooldownLeft == 1000,"bounded flood retries");
}
void inputDelays() {
	Fixture f; f.candidates.facts.candidates = {candidate(10)}; C::Query q; q.officialDelay = 350; q.supplementDelay = 900;
	f.start(q); f.clock.advance(349); check(f.transport.sent.empty(),"official delay early"); f.clock.advance(350);
	check(f.transport.sent.size() == 1 && f.transport.sent[0].request.peer == 0,"official delay"); f.clock.advance(899);
	check(f.transport.sent.size() == 1,"supplement delay early"); f.clock.advance(900); check(f.transport.sent.size() == 2,"supplement delay");
}
void typeMatrix() {
	for (const auto type : {0,1,2,3}) {
		Fixture f; auto user = candidate(10); user.facts.kind = S::PeerKind::User;
		auto chat = candidate(20); chat.facts.kind = S::PeerKind::Chat;
		auto mega = candidate(30); auto broadcast = candidate(40); broadcast.facts.kind = S::PeerKind::Broadcast;
		auto left = candidate(50); left.facts.haveLeft = true;
		auto forbidden = candidate(60); forbidden.facts.forbidden = true;
		auto unrestricted = candidate(70); unrestricted.facts.restricted = S::RestrictedState::Unrestricted;
		f.candidates.facts.candidates = {user,chat,mega,broadcast,left,forbidden,unrestricted};
		C::Query q; q.type.usersOnly = type == 1; q.type.groupsOnly = type == 2; q.type.broadcastsOnly = type == 3;
		f.start(q); f.clock.advance(1000); std::set<C::Peer> actual;
		for (const auto &r : f.transport.sent) if (r.request.peer) actual.insert(r.request.peer);
		const auto expected = type == 0 ? std::set<C::Peer>{10,20,30,40} : type == 1 ? std::set<C::Peer>{10}
			: type == 2 ? std::set<C::Peer>{20,30} : std::set<C::Peer>{40};
		check(actual == expected,"full type/restricted/left/forbidden matrix");
	}
}
void preparationSignalsAndGeneration() {
	Fixture f; f.candidates.facts.ready = false; f.candidates.facts.foldersLoaded[0] = false;
	f.candidates.facts.fullPeers = {100}; f.candidates.facts.candidates = {candidate(10)}; f.start();
	check(f.candidates.dialogs.size() >= 3 && f.candidates.full == std::vector<C::Peer>{100},"folder/community preparation effects");
	auto stale = f.candidates.updated; f.core.retry(); f.flush();
	stale(100); f.candidates.facts.ready = true; f.candidates.facts.foldersLoaded[0] = true; f.clock.advance(25);
	check(f.transport.sent.size() == 2,"stale full-info callback must not unblock replacement");
	auto current = f.candidates.updated; current(100); f.clock.advance(50);
	check(f.transport.sent.size() == 3 && f.transport.sent.back().request.peer == 10,"new full-info callback unblocks");
	Fixture unknown; unknown.candidates.facts.ready = false; unknown.candidates.facts.fullPeers = {200};
	unknown.candidates.facts.candidates = {candidate(10)}; unknown.start(); unknown.transport.reply(0,end()); unknown.clock.advance(10000);
	check(unknown.transport.sent.size() == 2,"known restricted source retained despite unknown community info");
	unknown.transport.reply(1,end({message(90,10,1)})); unknown.flush();
	check(unknown.pages.size() == 1 && unknown.pages[0].coverage == C::Engine::Coverage::Partial,"missing community info partial");
	Fixture success; success.account->pinnedFailures.insert(0); success.account->pinnedVersions[0]=1;
	success.start(); success.transport.reply(0,end()); success.account->pinnedVersions[0]++;
	success.account->pinnedFailures.erase(0); success.clock.advance(25);
	check(success.pages.size() == 1 && success.pages[0].coverage == C::Engine::Coverage::Complete,"pinned recovery complete");
}
void manyCandidatesFloodRecovery() {
	Fixture f; constexpr auto count = 106;
	for (C::Peer peer = 1; peer <= count; ++peer) f.candidates.facts.candidates.push_back(candidate(peer));
	C::Query q; q.merge.pageSize = 200; f.start(q); f.transport.reply(0,end());
	f.transport.error(1,{C::Failure::FloodWait,5000}); f.flush();
	check(f.account->running()==0,"flood slot release"); f.clock.advance(5000);
	std::set<C::Peer> completed; size_t handled = 2;
	while (completed.size() < count) {
		while (handled < f.transport.sent.size()) {
			const auto index = handled++; const auto peer = f.transport.sent[index].request.peer;
			check(completed.insert(peer).second,"source unexpectedly repeated after success");
			f.transport.reply(index,end({message(1000-int64_t(peer),peer,1)})); f.flush();
		}
		if (completed.size() < count) f.clock.advance(f.clock.now()+300);
		check(f.clock.now()<50000,"unsent candidate lost after flood");
	}
	check(f.pages.size()==1 && f.pages[0].items.size()==count && f.pages[0].exactTotal==count
		&& f.pages[0].coverage==C::Engine::Coverage::Complete,"all unsent sources must recover after flood");
	check(f.account->running()==0,"recovery slot leak");
}
void networkRetryAndDecodeReentrancy() {
	Fixture f; f.start(); f.transport.error(0); f.flush();
	check(f.pages.size()==1 && f.pages[0].error==C::Failure::Network && !f.pages[0].exactTotal,"network partial");
	f.core.retry(); f.flush(); f.transport.error(0,{C::Failure::FloodWait,5000});
	check(f.core.cooldownLeft()==0,"old failure mutates replacement account");
	f.transport.reply(1,end({message(100,1,1)})); f.flush(); check(f.pages[1].exactTotal==1,"network retry fresh final");
	Fixture r; r.start(); auto callback=r.transport.sent[0].done;
	callback([&] { r.core.cancel(); return end({message(100,1,1)}); }); r.flush();
	check(r.pages.empty() && !r.core.active(),"cancel during data decode must not commit");
	Fixture flood; flood.start(); flood.transport.error(0,{C::Failure::FloodWait,5000}); flood.flush();
	auto requests=flood.transport.sent.size(); flood.core.retry(); flood.flush();
	check(flood.transport.sent.size()==requests,"retry must respect cooldown");
}
void synchronousCancellation() {
	struct CancellingTransport final : C::Transport {
		C::Coordinator *core = nullptr;
		void send(C::Ticket, C::Request, std::function<void(C::Decode)>, std::function<void(C::Error)>) override { core->cancel(); }
		void cancel(C::Ticket) override {}
	} transport;
	Clock clock; Candidates candidates; candidates.facts.candidates = {candidate(10)};
	C::Coordinator core(transport,clock,candidates,std::make_shared<C::Account>()); transport.core = &core;
	core.start({},[](C::Page){throw std::runtime_error("page after synchronous cancel");});
	clock.advance(0); check(!core.active() && !clock.due,"dispatch reentrancy must not leave a timer");
}
