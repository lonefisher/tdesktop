/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_restricted_global_search.h"
#include "api/restricted_search_schedule.h"
#include "api/restricted_search_pinned_state.h"
#include "api/restricted_search_scope.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_community.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "api/restricted_search_merge.h"
#include <map>
#include <set>

namespace Api {
namespace {
using Engine = RestrictedSearch::MergeEngine;
using Failure = RestrictedSearch::SourceError;
constexpr auto kDeadline = crl::time(10000);
struct AccountState : RestrictedSearch::AccountSchedule {
	std::set<int> pinnedFailures;
	std::map<int, int> pinnedVersions;
};
std::map<Main::Session*, std::shared_ptr<AccountState>> Accounts;
std::shared_ptr<AccountState> Account(not_null<Main::Session*> session) {
	const auto found = Accounts.find(session);
	if (found != Accounts.end()) return found->second;
	auto result = std::make_shared<AccountState>();
	Accounts.emplace(session, result);
	session->lifetime().add([session] { Accounts.erase(session); });
	return result;
}
} // namespace

void RestrictedSearchPinnedDialogsResult(
		not_null<Main::Session*> session,
		int folderId,
		bool success) {
	const auto account = Account(session);
	++account->pinnedVersions[folderId];
	if (success) account->pinnedFailures.erase(folderId);
	else account->pinnedFailures.emplace(folderId);
}

class RestrictedGlobalSearchCoordinator::Impl final {
public:
	explicit Impl(not_null<Main::Session*> session)
	: _session(session)
	, _sender(&session->mtp())
	, _account(Account(session))
	, _timer([this] { tick(); }) {
	}
	~Impl() { cancel(); }
	void start(Query query, Fn<void(Page)> done) {
		cancel();
		_query = std::move(query);
		_done = std::move(done);
		_token = std::make_shared<int>(0);
		_started = crl::now();
		_wanted = true;
		_prepared = false;
		_prepFailed = false;
		_prepError = Failure::None;
		_pinnedRetries.clear();
		_fullAsked.clear();
		_fullPending.clear();
		_engine = std::make_unique<Engine>(Engine::Options{
			.pageSize = std::max(1, _query.pageSize),
			.uniquePerPeer = _query.uniquePerPeer,
			.onlyForwardable = _query.onlyForwardable,
		});
		_sources.emplace_back();
		_engine->addSource();
		_session->changes().peerUpdates(Data::PeerUpdate::Flag::FullInfo
		) | rpl::on_next([this](const Data::PeerUpdate &update) {
			_fullPending.erase(update.peer);
		}, _lifetime);
		if (!_query.community) {
			const auto scanFolders = std::vector<Data::Folder*>{ nullptr, _session->data().folder(Data::Folder::kId).get() };
			for (const auto folder : scanFolders) {
				_session->api().requestDialogs(folder);
				_session->api().requestPinnedDialogs(folder);
				const auto id = folder ? folder->id() : 0;
				if (_account->pinnedFailures.contains(id)) {
				    _pinnedRetries[id] = _account->pinnedVersions[id];
				    _session->api().reloadPinnedDialogs(folder);
				}
			}
		}
		_timer.callOnce(0);
	}
	void cancel() {
		_token.reset();
		_timer.cancel();
		_lifetime.destroy();
		for (auto &source : _sources) release(source, true);
		_sources.clear();
		_engine.reset();
		_wanted = false;
	}
	void more() {
		if (!_token || _engine->finished()) return;
		_wanted = true;
		_timer.callOnce(0);
	}
	void retry() {
		if (!_token || cooldownLeft()) return;
		auto query = _query;
            auto callback = _done;
		start(std::move(query), std::move(callback));
	}
	bool active() const { return bool(_token); }
	crl::time cooldownLeft() const {
		return _account->cooldownLeft(crl::now());
	}
private:
	struct Source {
		PeerData *peer = nullptr;
		mtpRequestId request = 0;
		RestrictedSearch::Deadline deadline;
		bool slot = false;
		int rate = 0;
		PeerId offsetPeer;
		MsgId offsetId;
		TimeId lastDate = 0;
		std::set<std::tuple<int, uint64, int64>> cursors;
	};
	std::vector<Data::Folder*> scopeFolders() {
		using Flag = MTPmessages_SearchGlobal::Flag;
		const auto ids = RestrictedSearchScope::scopeFolderIds(
			bool(_query.flags & Flag::f_folder_id),
			_query.folderId,
			0,
			Data::Folder::kId);
		auto result = std::vector<Data::Folder*>();
		result.reserve(ids.size());
		for (const auto id : ids) {
			result.push_back(id
				? _session->data().folder(id).get()
				: nullptr);
		}
		return result;
	}
	void askFull(not_null<PeerData*> peer) {
		if (!_fullAsked.emplace(peer).second) return;
		_fullPending.emplace(peer);
		_session->api().requestFullPeer(peer);
	}
	bool matchesType(not_null<PeerData*> peer) const {
		using Flag = MTPmessages_SearchGlobal::Flag;
		const auto channel = peer->asChannel();
		const auto kind = peer->isUser()
			? RestrictedSearchScope::PeerKind::User
			: peer->isChat()
			? RestrictedSearchScope::PeerKind::Chat
			: channel && channel->isMegagroup()
			? RestrictedSearchScope::PeerKind::Megagroup
			: channel && channel->isBroadcast()
			? RestrictedSearchScope::PeerKind::Broadcast
			: RestrictedSearchScope::PeerKind::Other;
		return RestrictedSearchScope::matchesType(
			kind,
			{
				.usersOnly = bool(_query.flags & Flag::f_users_only),
				.groupsOnly = bool(_query.flags & Flag::f_groups_only),
				.broadcastsOnly = bool(_query.flags & Flag::f_broadcasts_only),
			});
	}
	std::set<PeerData*> candidates(bool &ready) {
		namespace Scope = RestrictedSearchScope;
		auto peers = std::set<PeerData*>();
		const auto folders = scopeFolders();
		auto folderIds = std::vector<int>();
		folderIds.reserve(folders.size());
		for (const auto folder : folders) {
			folderIds.push_back(folder ? folder->id() : 0);
		}
		auto add = [&](not_null<PeerData*> peer, Scope::Candidate candidate) {
			if (matchesType(peer) && Scope::originEligible(candidate, folderIds)) {
				peers.emplace(peer);
			}
		};
		if (_query.community) {
			askFull(_query.community);
			if (const auto info = _query.community->communityInfo()) {
				for (const auto &linked : info->linkedPeers()) {
					add(linked.peer, {
						.origin = Scope::Origin::ExplicitCommunityLink,
						.canViewHistory = linked.canViewHistory,
					});
				}
			} else {
				ready = false;
			}
		} else {
			const auto scanFolders = std::vector<Data::Folder*>{ nullptr, _session->data().folder(Data::Folder::kId).get() };
			for (const auto folder : scanFolders) {
				const auto folderId = folder ? folder->id() : 0;
				const auto retry = _pinnedRetries.find(folderId);
				if (retry != _pinnedRetries.end() && retry->second == _account->pinnedVersions[folderId]) ready = false;
				else if (_account->pinnedFailures.contains(folderId)) _prepError = Failure::Network;
				if (!_session->data().chatsListLoaded(folder)) { ready = false; _session->api().requestDialogs(folder); }
				for (const auto row : *_session->data().chatsList(folder)->indexed()) {
					const auto history = row->key().history();
					if (!history) continue;
					const auto peer = history->peer;
					const auto channel = peer->asChannel();
					add(peer, {
						.origin = Scope::Origin::Dialog,
						.folderId = folder ? folder->id() : 0,
						.listedInDialogs = true,
						.channel = bool(channel),
						.community = channel && channel->isCommunity(),
						.haveLeft = channel && channel->haveLeft(),
						.forbidden = channel && channel->isForbidden(),
					});
					if (channel && channel->isCommunity()) {
						askFull(channel);
						if (const auto info = channel->communityInfo()) {
							for (const auto member : info->histories()) {
								if (!member->folderKnown()) { ready = false; continue; }
								add(member->peer, {
									.origin = Scope::Origin::FoldedCommunityMember,
									.folderId = member->folder()
										? member->folder()->id()
										: 0,
									.folderKnown = true,
									.joined = Data::CommunityChatJoined(member),
								});
							}
						} else ready = false;
					}
				}
			}
		}
		for (const auto peer : peers) {
			const auto state = peer->restrictedState();
			const auto restricted = (state == PeerData::RestrictedState::Unknown)
				? Scope::RestrictedState::Unknown
				: (state == PeerData::RestrictedState::Restricted)
				? Scope::RestrictedState::Restricted
				: Scope::RestrictedState::Unrestricted;
			if (Scope::needsFullPeer(restricted)) {
				askFull(peer);
				ready = false;
			}
		}
		if (!_fullPending.empty()) ready = false;
		return peers;
	}
	void prepare() {
		auto ready = true;
		const auto peers = candidates(ready);
		const auto status = RestrictedSearchScope::preparation(
			ready,
			crl::now() >= _started + kDeadline,
			_prepError != Failure::None);
		if (status == RestrictedSearchScope::Preparation::Wait) return;
		_prepFailed = (status == RestrictedSearchScope::Preparation::Partial);
		for (const auto peer : peers) {
			const auto state = peer->restrictedState();
			const auto restricted = (state == PeerData::RestrictedState::Unknown)
				? RestrictedSearchScope::RestrictedState::Unknown
				: (state == PeerData::RestrictedState::Restricted)
				? RestrictedSearchScope::RestrictedState::Restricted
				: RestrictedSearchScope::RestrictedState::Unrestricted;
			if (!RestrictedSearchScope::searchable(restricted)) continue;
			_sources.emplace_back();
			_sources.back().peer = peer;
			_engine->addSource();
		}
		if (_prepFailed) {
			_sources.emplace_back();
			_engine->fail(_engine->addSource(), _prepError != Failure::None ? _prepError : Failure::Timeout);
		}
		_prepared = true;
        LOG(("RestrictedSearch: prepared sources=%1 partial=%2 elapsed_ms=%3").arg(_sources.size()).arg(_prepFailed).arg(crl::now() - _started));
		_lifetime.destroy();
	}
	void release(Source &source, bool cancelRequest) {
		if (source.request && cancelRequest) _sender.request(source.request).cancel();
		source.request = 0;
		source.deadline.due = 0;
		if (source.slot) { _account->release(); source.slot = false; }
	}
	void fail(int index, Failure failure, crl::time wait = 0) {
		release(_sources[index], true);
		_engine->fail(index, failure, wait);
	}
	void received(int index, const MTPmessages_Messages &result) {
		auto &source = _sources[index];
		release(source, false);
		auto items = std::vector<Engine::Item>();
		auto exhausted = false;
		auto protocol = false;
		auto nextRate = source.rate;
		auto nextPeer = source.offsetPeer;
		auto nextId = source.offsetId;
		auto consume = [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			if (source.peer) source.peer->processTopics(data.vtopics());
			const auto &raw = data.vmessages().v;
			if (raw.empty()) exhausted = true;
			for (const auto &message : raw) {
				const auto peerId = PeerFromMessage(message);
				const auto id = IdFromMessage(message);
				const auto date = DateFromMessage(message);
				nextPeer = peerId;
				nextId = id;
				if (!peerId || !id) protocol = true;
				if (!date) continue;
				if (source.lastDate && date > source.lastDate) protocol = true;
				source.lastDate = date;
				if (const auto item = _session->data().addNewMessage(
						message, MessageFlags(), NewMessageType::Existing)) {
					const auto position = item->position();
					items.push_back({
						{position.date, int64(position.fullId.peer.value), position.fullId.msg.bare},
						item->allowsForward(),
					});
				}
			}
		};
		result.match([&](const MTPDmessages_messages &data) {
			consume(data);
			exhausted = true;
		}, [&](const MTPDmessages_messagesSlice &data) {
			consume(data);
			if (!index) {
				if (const auto rate = data.vnext_rate()) nextRate = rate->v;
				else exhausted = true;
			}
		}, [&](const MTPDmessages_channelMessages &data) {
			consume(data);
			if (source.peer) {
				if (const auto channel = source.peer->asChannel()) channel->ptsReceived(data.vpts().v);
			}
		}, [&](const MTPDmessages_messagesNotModified &) {
			protocol = true;
		});
		const auto cursor = std::make_tuple(index ? 0 : nextRate,
			index ? uint64(0) : nextPeer.value, nextId.bare);
		const auto previous = std::make_tuple(index ? 0 : source.rate,
			index ? uint64(0) : source.offsetPeer.value, source.offsetId.bare);
		if (!exhausted && (cursor == previous || !source.cursors.emplace(cursor).second)) protocol = true;
		source.rate = nextRate;
		source.offsetPeer = nextPeer;
		source.offsetId = nextId;
		if (protocol) _engine->fail(index, Failure::Protocol);
		else (void)_engine->feed(index, std::move(items), exhausted);
		_timer.callOnce(0);
	}
	void failed(int index, const MTP::Error &error) {
		if (MTP::IsFloodError(error)) {
			const auto seconds = std::max(1, error.type().section('_', -1).toInt());
			_account->flood(crl::now(), crl::time(seconds) * 1000);
			const auto wait = cooldownLeft();
			LOG(("RestrictedSearch: flood source=%1 retry_ms=%2")
				.arg(index)
				.arg(wait));
			release(_sources[index], false);
			(void)_engine->deferFetching(index, wait);
		} else {
			fail(index, Failure::Network);
		}
		_timer.callOnce(0);
	}
	void dispatch(int index) {
		auto &source = _sources[index];
		if (source.request || _engine->terminal(index)) return;
		const auto now = crl::now();
		if (!RestrictedSearch::InputDelayElapsed(_started,
			index ? _query.supplementDelay : _query.officialDelay, now)) return;
		if (cooldownLeft()) return;
		if (index) {
			if (!_account->acquire(now)) return;
			source.slot = true;
		}
		_engine->markFetching(index);
		source.deadline.start(now, kDeadline);
		const auto weak = std::weak_ptr<int>(_token);
		auto done = [this, weak, index](const MTPmessages_Messages &result, mtpRequestId id) {
			if (weak.expired() || _sources[index].request != id) return;
			received(index, result);
		};
		auto error = [this, weak, index](const MTP::Error &error, mtpRequestId id) {
			if (weak.expired() || _sources[index].request != id) return;
			failed(index, error);
		};
		if (!index) {
			const auto offset = source.offsetPeer
				? _session->data().peer(source.offsetPeer)->input()
				: MTP_inputPeerEmpty();
			source.request = _sender.request(MTPmessages_SearchGlobal(
				MTP_flags(_query.flags), MTP_int(_query.folderId),
				_query.community ? _query.community->inputChannel() : MTPInputChannel(),
				MTP_string(_query.text), _query.filter,
				MTP_int(_query.minDate), MTP_int(_query.maxDate),
				MTP_int(source.rate), offset, MTP_int(source.offsetId),
				MTP_int(_query.rawPageSize)
			)).done(std::move(done)).fail(std::move(error)).handleAllErrors().send();
		} else {
			source.request = _sender.request(MTPmessages_Search(
				MTP_flags(MTPmessages_Search::Flags()), source.peer->input(),
				MTP_string(_query.text), MTPInputPeer(), MTPInputPeer(),
				MTPVector<MTPReaction>(), MTP_int(0), _query.filter,
				MTP_int(_query.minDate), MTP_int(_query.maxDate),
				MTP_int(source.offsetId), MTP_int(0), MTP_int(50),
				MTP_int(0), MTP_int(0), MTP_long(0)
			)).done(std::move(done)).fail(std::move(error)).handleAllErrors().send();
		}
	}
	void tick() {
		if (!_token) return;
		if (!_prepared && _wanted) prepare();
		for (auto i = 0; i < int(_sources.size()); ++i) {
			if (_sources[i].request && _sources[i].deadline.expired(crl::now())) fail(i, Failure::Timeout);

		}
		if (!_wanted) {
			if (std::any_of(_sources.begin(), _sources.end(), [](const Source &s) { return bool(s.request); })) _timer.callOnce(25);
			return;
		}
		if (!_prepared) {
			if (!_sources[0].offsetId && _engine->needsFetch(0)) dispatch(0);
			_timer.callOnce(25);
			return;
		}
		const auto step = _engine->step();
		if (step.pageReady) {
			auto merged = _engine->takePage();
			auto page = Page();
			for (const auto &position : merged.items) {
				page.messageIds.push_back({ FullMsgId(PeerId(PeerIdHelper(uint64(position.peer))), MsgId(position.msg)), TimeId(position.date) });
			}
			page.hasMore = merged.hasMore;
			page.coverage = merged.coverage == Engine::Coverage::Partial ? Coverage::Partial : Coverage::Complete;
			page.loadedCount = merged.loadedCount;
			page.exactTotal = merged.exactTotal;
			page.error = ErrorKind(merged.error);
			page.retryAfter = cooldownLeft();
			_wanted = false;
			_timer.callOnce(25);
			LOG(("RestrictedSearch: page loaded=%1 has_more=%2 error=%3 elapsed_ms=%4").arg(page.loadedCount).arg(page.hasMore).arg(int(page.error)).arg(crl::now() - _started));
            auto callback = _done;
			callback(std::move(page));
			return;
		}
		for (const auto index : step.fetch) dispatch(index);
		_timer.callOnce(25);
	}
	const not_null<Main::Session*> _session;
	MTP::Sender _sender;
	std::shared_ptr<AccountState> _account;
	base::Timer _timer;
	rpl::lifetime _lifetime;
	std::shared_ptr<int> _token;
	Query _query;
	Fn<void(Page)> _done;
	std::unique_ptr<Engine> _engine;
	std::vector<Source> _sources;
	std::set<PeerData*> _fullAsked;
	std::set<PeerData*> _fullPending;
	crl::time _started = 0;
	bool _wanted = false;
	bool _prepared = false;
	bool _prepFailed = false;
	Failure _prepError = Failure::None;
	std::map<int, int> _pinnedRetries;
};

RestrictedGlobalSearchCoordinator::RestrictedGlobalSearchCoordinator(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
}
RestrictedGlobalSearchCoordinator::~RestrictedGlobalSearchCoordinator() = default;
void RestrictedGlobalSearchCoordinator::start(Query query, Fn<void(Page)> done) { _impl->start(std::move(query), std::move(done)); }
void RestrictedGlobalSearchCoordinator::requestMore() { _impl->more(); }
void RestrictedGlobalSearchCoordinator::cancel() { _impl->cancel(); }
void RestrictedGlobalSearchCoordinator::retry() { _impl->retry(); }
bool RestrictedGlobalSearchCoordinator::active() const { return _impl->active(); }
crl::time RestrictedGlobalSearchCoordinator::cooldownLeft() const { return _impl->cooldownLeft(); }

} // namespace Api
