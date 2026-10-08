/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once
#include "api/restricted_search_merge.h"
#include "api/restricted_search_schedule.h"
#include "api/restricted_search_scope.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace Api::RestrictedSearchCore {
using Peer = uint64_t;
using Ticket = uint64_t;
using Failure = RestrictedSearch::SourceError;
using Engine = RestrictedSearch::MergeEngine;
namespace Scope = RestrictedSearchScope;
struct Account : RestrictedSearch::AccountSchedule {
	std::set<int> pinnedFailures;
	std::map<int, int> pinnedVersions;
};
struct Query {
	bool explicitFolder = false;
	int folderId = 0;
	int archiveFolderId = 1;
	Peer community = 0;
	Scope::TypeFilter type;
	std::string text;
	int minDate = 0;
	int maxDate = 0;
	int rawPageSize = 50;
	Engine::Options merge;
	int64_t officialDelay = 0;
	int64_t supplementDelay = 0;
};
struct Candidate { Peer peer = 0; Scope::Candidate facts; };
struct Snapshot {
	bool ready = true;
	std::vector<Candidate> candidates;
	std::vector<Peer> fullPeers;
	std::map<int, bool> foldersLoaded;
};
struct Cursor { int rate = 0; Peer peer = 0; int64_t msg = 0; };
struct Request { Query query; Peer peer = 0; Cursor cursor; int limit = 50; };
struct Message {
	Peer peer = 0;
	int64_t id = 0;
	int64_t date = 0;
	std::optional<Engine::Item> item;
};
struct Response {
	std::vector<Message> messages;
	bool exhausted = false;
	bool protocolError = false;
	std::optional<int> nextRate;
};
struct Error { Failure kind = Failure::Network; int64_t floodWait = 0; };
struct Page : Engine::Page { int64_t cooldownLeft = 0; };
struct Events {
	std::function<void(int, bool, int64_t)> prepared;
	std::function<void(int, int64_t)> flood;
	std::function<void(int, bool, Failure, int64_t)> page;
};
// Decoding may write the data model: invoke only after accepting the ticket.
using Decode = std::function<Response()>;
class Transport {
public:
	virtual ~Transport() = default;
	virtual void send(Ticket, Request, std::function<void(Decode)>, std::function<void(Error)>) = 0;
	virtual void cancel(Ticket) = 0;
};
class Clock {
public:
	virtual ~Clock() = default;
	virtual int64_t now() const = 0;
	virtual void arm(int64_t delay, std::function<void()>) = 0;
	virtual void cancel() = 0;
};
class Candidates {
public:
	virtual ~Candidates() = default;
	virtual void begin(std::function<void(Peer)> fullUpdated) = 0;
	virtual Snapshot snapshot(const Query &) = 0;
	virtual void requestDialogs(int folder) = 0;
	virtual void requestPinned(int folder, bool reload) = 0;
	virtual void requestFull(Peer) = 0;
	virtual void end() = 0;
};
class Coordinator final {
public:
	static constexpr int64_t kDeadline = 10000;
	Coordinator(Transport &transport, Clock &clock, Candidates &candidates,
			std::shared_ptr<Account> account, Events events = {})
	: _transport(transport), _clock(clock), _candidates(candidates), _account(std::move(account)), _events(std::move(events)) {
	}
	Coordinator(const Coordinator &) = delete;
	Coordinator &operator=(const Coordinator &) = delete;
	~Coordinator() { cancel(); }
	void start(Query query, std::function<void(Page)> done) {
		cancel();
		_query = std::move(query);
		_done = std::move(done);
		_token = std::make_shared<int>(0);
		_started = _clock.now();
		_wanted = true;
		_prepared = false;
		_prepError = Failure::None;
		_pinnedRetries.clear();
		_fullAsked.clear();
		_fullPending.clear();
		auto options = _query.merge;
		options.pageSize = std::max(1, options.pageSize);
		_engine = std::make_unique<Engine>(options);
		_sources.emplace_back();
		_engine->addSource();
		const auto weak = std::weak_ptr<int>(_token);
		_candidates.begin([this, weak](Peer peer) {
			if (weak.expired()) return;
			_fullPending.erase(peer);
		});
		if (!_query.community) {
			for (const auto folder : { 0, _query.archiveFolderId }) {
				_candidates.requestDialogs(folder);
				_candidates.requestPinned(folder, false);
				if (_account->pinnedFailures.contains(folder)) {
					_pinnedRetries[folder] = _account->pinnedVersions[folder];
					_candidates.requestPinned(folder, true);
				}
			}
		}
		arm(0);
	}
	void requestMore() {
		if (!_token || _engine->finished()) return;
		_wanted = true;
		arm(0);
	}
	void cancel() {
		_token.reset();
		_clock.cancel();
		_candidates.end();
		for (auto &source : _sources) release(source, true);
		_sources.clear();
		_engine.reset();
		_wanted = false;
	}
	void retry() {
		if (!_token || cooldownLeft()) return;
		auto query = _query;
		auto done = _done;
		start(std::move(query), std::move(done));
	}
	bool active() const { return bool(_token); }
	int64_t cooldownLeft() const { return _account->cooldownLeft(_clock.now()); }
private:
	struct Source {
		Peer peer = 0;
		Ticket request = 0;
		RestrictedSearch::Deadline deadline;
		bool slot = false;
		Cursor cursor;
		int64_t lastDate = 0;
		std::set<std::tuple<int, Peer, int64_t>> cursors;
	};
	void arm(int64_t delay) {
		const auto weak = std::weak_ptr<int>(_token);
		_clock.arm(delay, [this, weak] { if (!weak.expired()) tick(); });
	}
	void askFull(Peer peer) {
		if (!_fullAsked.insert(peer).second) return;
		_fullPending.insert(peer);
		_candidates.requestFull(peer);
	}
	void prepare() {
		auto snapshot = _candidates.snapshot(_query);
		auto ready = snapshot.ready;
		const auto folders = Scope::scopeFolderIds(
			_query.explicitFolder, _query.folderId, 0, _query.archiveFolderId);
		if (!_query.community) {
			for (const auto folder : { 0, _query.archiveFolderId }) {
				const auto retry = _pinnedRetries.find(folder);
				if (retry != _pinnedRetries.end()
					&& retry->second == _account->pinnedVersions[folder]) ready = false;
				else if (_account->pinnedFailures.contains(folder)) _prepError = Failure::Network;
				const auto loaded = snapshot.foldersLoaded.find(folder);
				if (loaded != snapshot.foldersLoaded.end() && !loaded->second) {
					ready = false;
					_candidates.requestDialogs(folder);
				}
			}
		} else {
			askFull(_query.community);
		}
		for (const auto peer : snapshot.fullPeers) askFull(peer);
		auto peers = std::map<Peer, Scope::RestrictedState>();
		for (const auto &candidate : snapshot.candidates) {
			const auto &facts = candidate.facts;
			if (bool(_query.community) != (facts.origin == Scope::Origin::ExplicitCommunityLink)) continue;
			if (!Scope::candidateAllowed(facts, folders, _query.type)) continue;
			peers[candidate.peer] = facts.restricted;
		}
		for (const auto &[peer, restricted] : peers) {
			if (Scope::needsFullPeer(restricted)) { askFull(peer); ready = false; }
		}
		if (!_fullPending.empty()) ready = false;
		const auto status = Scope::preparation(ready,
			_clock.now() >= _started + kDeadline, _prepError != Failure::None);
		if (status == Scope::Preparation::Wait) return;
		for (const auto &[peer, restricted] : peers) {
			if (!Scope::searchable(restricted)) continue;
			_sources.emplace_back();
			_sources.back().peer = peer;
			_engine->addSource();
		}
		if (status == Scope::Preparation::Partial) {
			_sources.emplace_back();
			_engine->fail(_engine->addSource(),
				_prepError != Failure::None ? _prepError : Failure::Timeout);
		}
		_prepared = true;
		if (_events.prepared) _events.prepared(int(_sources.size()),
			status == Scope::Preparation::Partial, _clock.now() - _started);
		_candidates.end();
	}
	void release(Source &source, bool cancelRequest) {
		const auto ticket = std::exchange(source.request, Ticket(0));
		source.deadline.due = 0;
		if (source.slot) { _account->release(); source.slot = false; }
		if (ticket && cancelRequest) _transport.cancel(ticket);
	}
	void fail(int index, Failure failure) {
		release(_sources[index], true);
		_engine->fail(index, failure);
	}
	void received(int index, Response response) {
		auto &source = _sources[index];
		release(source, false);
		auto exhausted = response.exhausted || response.messages.empty();
		auto protocol = response.protocolError;
		auto next = source.cursor;
		auto items = std::vector<Engine::Item>();
		for (const auto &message : response.messages) {
			next.peer = message.peer;
			next.msg = message.id;
			if (!message.peer || !message.id) protocol = true;
			if (!message.date) continue;
			if (source.lastDate && message.date > source.lastDate) protocol = true;
			source.lastDate = message.date;
			if (message.item) items.push_back(*message.item);
		}
		if (!index && response.nextRate) next.rate = *response.nextRate;
		const auto cursor = std::make_tuple(index ? 0 : next.rate,
			index ? Peer(0) : next.peer, next.msg);
		const auto previous = std::make_tuple(index ? 0 : source.cursor.rate,
			index ? Peer(0) : source.cursor.peer, source.cursor.msg);
		if (!exhausted && (cursor == previous || !source.cursors.insert(cursor).second)) protocol = true;
		source.cursor = next;
		if (protocol) _engine->fail(index, Failure::Protocol);
		else (void)_engine->feed(index, std::move(items), exhausted);
		arm(0);
	}
	void failed(int index, Error error) {
		if (error.kind == Failure::FloodWait) {
			_account->flood(_clock.now(), std::max<int64_t>(1000, error.floodWait));
			const auto wait = cooldownLeft();
			if (_events.flood) _events.flood(index, wait);
			release(_sources[index], false);
			(void)_engine->deferFetching(index, wait);
		} else {
			fail(index, error.kind);
		}
		arm(0);
	}
	void dispatch(int index) {
		auto &source = _sources[index];
		if (source.request || _engine->terminal(index)) return;
		const auto now = _clock.now();
		if (!RestrictedSearch::InputDelayElapsed(_started,
			index ? _query.supplementDelay : _query.officialDelay, now) || cooldownLeft()) return;
		if (index) {
			if (!_account->acquire(now)) return;
			source.slot = true;
		}
		_engine->markFetching(index);
		source.deadline.start(now, kDeadline);
		const auto ticket = source.request = ++_nextTicket;
		const auto weak = std::weak_ptr<int>(_token);
		_transport.send(ticket, Request{ _query, source.peer, source.cursor, index ? 50 : _query.rawPageSize },
			[this, weak, index, ticket](Decode decode) {
				if (weak.expired() || _sources[index].request != ticket) return;
				auto response = decode();
				// Data-model notifications during decoding can cancel/restart search.
				if (weak.expired() || _sources[index].request != ticket) return;
				received(index, std::move(response));
			}, [this, weak, index, ticket](Error error) {
				if (weak.expired() || _sources[index].request != ticket) return;
				failed(index, error);
			});
	}
	void tick() {
		if (!_token) return;
		const auto weak = std::weak_ptr<int>(_token);
		if (!_prepared && _wanted) prepare();
		if (weak.expired()) return;
		for (auto i = 0; i < int(_sources.size()); ++i) {
			if (_sources[i].request && _sources[i].deadline.expired(_clock.now())) fail(i, Failure::Timeout);
		}
		if (!_wanted) {
			if (std::any_of(_sources.begin(), _sources.end(), [](const Source &s) { return bool(s.request); })) arm(25);
			return;
		}
		if (!_prepared) {
			if (!_sources[0].cursor.msg && _engine->needsFetch(0)) dispatch(0);
			if (weak.expired()) return;
			arm(25);
			return;
		}
		const auto step = _engine->step();
		if (step.pageReady) {
			auto page = Page();
			static_cast<Engine::Page&>(page) = _engine->takePage();
			page.cooldownLeft = cooldownLeft();
			_wanted = false;
			arm(25);
			if (_events.page) _events.page(page.loadedCount, page.hasMore, page.error, _clock.now() - _started);
			auto callback = _done;
			callback(std::move(page));
			return;
		}
		for (const auto index : step.fetch) {
			dispatch(index);
			if (weak.expired()) return;
		}
		arm(25);
	}
	Transport &_transport;
	Clock &_clock;
	Candidates &_candidates;
	std::shared_ptr<Account> _account;
	Events _events;
	std::shared_ptr<int> _token;
	Query _query;
	std::function<void(Page)> _done;
	std::unique_ptr<Engine> _engine;
	std::vector<Source> _sources;
	std::set<Peer> _fullAsked, _fullPending;
	std::map<int, int> _pinnedRetries;
	Ticket _nextTicket = 0;
	int64_t _started = 0;
	bool _wanted = false, _prepared = false;
	Failure _prepError = Failure::None;
};
} // namespace Api::RestrictedSearchCore
