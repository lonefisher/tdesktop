/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_restricted_global_search.h"
#include "api/restricted_search_core.h"
#include "api/restricted_search_pinned_state.h"

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

namespace Api {
namespace {
namespace Core = RestrictedSearchCore;
namespace Scope = RestrictedSearchScope;
std::map<Main::Session*, std::shared_ptr<Core::Account>> Accounts;
std::shared_ptr<Core::Account> Account(not_null<Main::Session*> session) {
	const auto found = Accounts.find(session);
	if (found != Accounts.end()) return found->second;
	auto result = std::make_shared<Core::Account>();
	Accounts.emplace(session, result);
	session->lifetime().add([session] { Accounts.erase(session); });
	return result;
}
Core::Query CoreQuery(const RestrictedGlobalSearchCoordinator::Query &query) {
	using Flag = MTPmessages_SearchGlobal::Flag;
	return {
		.explicitFolder = bool(query.flags & Flag::f_folder_id),
		.folderId = query.folderId,
		.archiveFolderId = Data::Folder::kId,
		.community = query.community ? query.community->id.value : uint64(0),
		.type = {
			.usersOnly = bool(query.flags & Flag::f_users_only),
			.groupsOnly = bool(query.flags & Flag::f_groups_only),
			.broadcastsOnly = bool(query.flags & Flag::f_broadcasts_only),
		},
		.text = query.text.toUtf8().toStdString(),
		.minDate = query.minDate,
		.maxDate = query.maxDate,
		.rawPageSize = query.rawPageSize,
		.merge = {
			.pageSize = query.pageSize,
			.uniquePerPeer = query.uniquePerPeer,
			.onlyForwardable = query.onlyForwardable,
		},
		.officialDelay = query.officialDelay,
		.supplementDelay = query.supplementDelay,
	};
}
Scope::PeerKind PeerKind(not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	return peer->isUser() ? Scope::PeerKind::User
		: peer->isChat() ? Scope::PeerKind::Chat
		: channel && channel->isMegagroup() ? Scope::PeerKind::Megagroup
		: channel && channel->isBroadcast() ? Scope::PeerKind::Broadcast
		: Scope::PeerKind::Other;
}
Scope::RestrictedState RestrictedState(not_null<PeerData*> peer) {
	const auto state = peer->restrictedState();
	return state == PeerData::RestrictedState::Unknown ? Scope::RestrictedState::Unknown
		: state == PeerData::RestrictedState::Restricted ? Scope::RestrictedState::Restricted
		: Scope::RestrictedState::Unrestricted;
}
} // namespace

void RestrictedSearchPinnedDialogsResult(not_null<Main::Session*> session, int folderId, bool success) {
	const auto account = Account(session);
	++account->pinnedVersions[folderId];
	if (success) account->pinnedFailures.erase(folderId);
	else account->pinnedFailures.emplace(folderId);
}

// Only external effects live here. Both the client and deterministic tests use
// Core::Coordinator for preparation, generation, requests, paging and retry.
class RestrictedGlobalSearchCoordinator::Impl final
	: private Core::Transport
	, private Core::Clock
	, private Core::Candidates {
public:
	explicit Impl(not_null<Main::Session*> session)
	: _session(session)
	, _sender(&session->mtp())
	, _timer([this] { auto callback = std::move(_wake); if (callback) callback(); })
	, _core(*this, *this, *this, Account(session), {
		.prepared = [](int sources, bool partial, int64_t elapsed) {
			LOG(("RestrictedSearch: prepared sources=%1 partial=%2 elapsed_ms=%3").arg(sources).arg(partial).arg(elapsed));
		},
		.flood = [](int source, int64_t wait) {
			LOG(("RestrictedSearch: flood source=%1 retry_ms=%2").arg(source).arg(wait));
		},
		.page = [](int loaded, bool more, Core::Failure error, int64_t elapsed) {
			LOG(("RestrictedSearch: page loaded=%1 has_more=%2 error=%3 elapsed_ms=%4").arg(loaded).arg(more).arg(int(error)).arg(elapsed));
		},
	}) {
	}
	~Impl() { _core.cancel(); _alive.reset(); }
	void start(Query query, Fn<void(Page)> done) {
		_query = std::move(query);
		_core.start(CoreQuery(_query), [done = std::move(done)](Core::Page source) mutable {
			auto page = Page();
			for (const auto &position : source.items) {
				page.messageIds.push_back({
					FullMsgId(PeerId(PeerIdHelper(uint64(position.peer))), MsgId(position.msg)), TimeId(position.date) });
			}
			page.hasMore = source.hasMore;
			page.coverage = source.coverage == Core::Engine::Coverage::Partial ? Coverage::Partial : Coverage::Complete;
			page.loadedCount = source.loadedCount;
			page.exactTotal = source.exactTotal;
			page.error = ErrorKind(source.error);
			page.retryAfter = source.cooldownLeft;
			done(std::move(page));
		});
	}
	void more() { _core.requestMore(); }
	void retry() { _core.retry(); }
	void cancelSearch() { _core.cancel(); }
	bool active() const { return _core.active(); }
	crl::time cooldownLeft() const { return _core.cooldownLeft(); }
private:
	not_null<PeerData*> peer(Core::Peer id) const {
		return _session->data().peer(PeerId(PeerIdHelper(id)));
	}
	Data::Folder *folder(int id) const {
		return id ? _session->data().folder(id).get() : nullptr;
	}
	// Clock/scheduler adapter.
	int64_t now() const override { return crl::now(); }
	void arm(int64_t delay, std::function<void()> callback) override {
		_wake = std::move(callback);
		_timer.callOnce(delay);
	}
	void cancel() override { _timer.cancel(); _wake = {}; }

	// Candidate/data adapter: report raw authoritative facts; core filters them.
	void begin(std::function<void(Core::Peer)> updated) override {
		_session->changes().peerUpdates(Data::PeerUpdate::Flag::FullInfo)
			| rpl::on_next([updated = std::move(updated)](const Data::PeerUpdate &update) {
				updated(update.peer->id.value);
			}, _lifetime);
	}
	void end() override { _lifetime.destroy(); }
	void requestDialogs(int id) override { _session->api().requestDialogs(folder(id)); }
	void requestPinned(int id, bool reload) override {
		if (reload) _session->api().reloadPinnedDialogs(folder(id));
		else _session->api().requestPinnedDialogs(folder(id));
	}
	void requestFull(Core::Peer id) override { _session->api().requestFullPeer(peer(id)); }
	Core::Snapshot snapshot(const Core::Query &query) override {
		auto result = Core::Snapshot();
		auto add = [&](not_null<PeerData*> p, Scope::Candidate facts) {
			facts.kind = PeerKind(p);
			facts.restricted = RestrictedState(p);
			result.candidates.push_back({ p->id.value, facts });
		};
		if (query.community) {
			const auto community = peer(query.community)->asChannel();
			if (const auto info = community->communityInfo()) {
				for (const auto &linked : info->linkedPeers()) {
					add(linked.peer, { .origin = Scope::Origin::ExplicitCommunityLink, .canViewHistory = linked.canViewHistory });
				}
			} else result.ready = false;
		} else {
			for (const auto id : { 0, query.archiveFolderId }) {
				const auto f = folder(id);
				result.foldersLoaded[id] = _session->data().chatsListLoaded(f);
				for (const auto row : *_session->data().chatsList(f)->indexed()) {
					const auto history = row->key().history();
					if (!history) continue;
					const auto p = history->peer;
					const auto channel = p->asChannel();
					add(p, {
						.origin = Scope::Origin::Dialog,
						.folderId = id,
						.listedInDialogs = true,
						.channel = bool(channel),
						.community = channel && channel->isCommunity(),
						.haveLeft = channel && channel->haveLeft(),
						.forbidden = channel && channel->isForbidden(),
					});
					if (channel && channel->isCommunity()) {
						result.fullPeers.push_back(channel->id.value);
						if (const auto info = channel->communityInfo()) {
							for (const auto member : info->histories()) {
								if (!member->folderKnown()) { result.ready = false; continue; }
								add(member->peer, {
									.origin = Scope::Origin::FoldedCommunityMember,
									.folderId = member->folder() ? member->folder()->id() : 0,
									.folderKnown = true,
									.joined = Data::CommunityChatJoined(member),
								});
							}
						} else result.ready = false;
					}
				}
			}
		}
		return result;
	}
	// MTP decoder/data sink. Only called by the core after accepting a callback.
	Core::Response decode(Core::Peer sourcePeer, const MTPmessages_Messages &result) {
		auto response = Core::Response();
		const auto source = sourcePeer ? peer(sourcePeer).get() : nullptr;
		auto consume = [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			if (source) source->processTopics(data.vtopics());
			for (const auto &message : data.vmessages().v) {
				const auto id = IdFromMessage(message);
				const auto date = DateFromMessage(message);
				auto row = Core::Message{ PeerFromMessage(message).value, id.bare, date, {} };
				if (date) {
					if (const auto item = _session->data().addNewMessage(message, MessageFlags(), NewMessageType::Existing)) {
						const auto position = item->position();
						row.item = Core::Engine::Item{
							{ position.date, int64(position.fullId.peer.value), position.fullId.msg.bare }, item->allowsForward() };
					}
				}
				response.messages.push_back(std::move(row));
			}
		};
		result.match([&](const MTPDmessages_messages &data) {
			consume(data); response.exhausted = true;
		}, [&](const MTPDmessages_messagesSlice &data) {
			consume(data);
			if (!source) {
				if (const auto rate = data.vnext_rate()) response.nextRate = rate->v;
				else response.exhausted = true;
			}
		}, [&](const MTPDmessages_channelMessages &data) {
			consume(data);
			if (source) if (const auto channel = source->asChannel()) channel->ptsReceived(data.vpts().v);
		}, [&](const MTPDmessages_messagesNotModified &) {
			response.protocolError = true;
		});
		return response;
	}
	void send(Core::Ticket ticket, Core::Request request,
			std::function<void(Core::Decode)> completed, std::function<void(Core::Error)> failed) override {
		const auto weak = std::weak_ptr<int>(_alive);
		const auto sourcePeer = request.peer;
		auto done = [this, weak, ticket, sourcePeer, completed = std::move(completed)](
				const MTPmessages_Messages &result, mtpRequestId) {
			if (weak.expired()) return;
			_requests.erase(ticket);
			// Decode is synchronous within completed; the MTP reference stays alive.
			completed([this, sourcePeer, &result] { return decode(sourcePeer, result); });
		};
		auto fail = [this, weak, ticket, failed = std::move(failed)](const MTP::Error &error, mtpRequestId) {
			if (weak.expired()) return;
			_requests.erase(ticket);
			if (MTP::IsFloodError(error)) {
				const auto seconds = std::max(1, error.type().section('_', -1).toInt());
				failed({ Core::Failure::FloodWait, crl::time(seconds) * 1000 });
			} else failed({ Core::Failure::Network, 0 });
		};
		const auto text = MTP_string(QString::fromUtf8(request.query.text.data(), int(request.query.text.size())));
		if (!sourcePeer) {
			const auto offset = request.cursor.peer ? peer(request.cursor.peer)->input() : MTP_inputPeerEmpty();
			_requests[ticket] = _sender.request(MTPmessages_SearchGlobal(
				MTP_flags(_query.flags), MTP_int(request.query.folderId),
				_query.community ? _query.community->inputChannel() : MTPInputChannel(),
				text, _query.filter, MTP_int(request.query.minDate), MTP_int(request.query.maxDate),
				MTP_int(request.cursor.rate), offset, MTP_int(int(request.cursor.msg)), MTP_int(request.limit)
			)).done(std::move(done)).fail(std::move(fail)).handleAllErrors().send();
		} else {
			_requests[ticket] = _sender.request(MTPmessages_Search(
				MTP_flags(MTPmessages_Search::Flags()), peer(sourcePeer)->input(), text,
				MTPInputPeer(), MTPInputPeer(), MTPVector<MTPReaction>(), MTP_int(0), _query.filter,
				MTP_int(request.query.minDate), MTP_int(request.query.maxDate), MTP_int(int(request.cursor.msg)),
				MTP_int(0), MTP_int(request.limit), MTP_int(0), MTP_int(0), MTP_long(0)
			)).done(std::move(done)).fail(std::move(fail)).handleAllErrors().send();
		}
	}
	void cancel(Core::Ticket ticket) override {
		const auto found = _requests.find(ticket);
		if (found == _requests.end()) return;
		const auto id = found->second;
		_requests.erase(found);
		_sender.request(id).cancel();
	}
	const not_null<Main::Session*> _session;
	MTP::Sender _sender;
	base::Timer _timer;
	std::function<void()> _wake;
	rpl::lifetime _lifetime;
	std::shared_ptr<int> _alive = std::make_shared<int>(0);
	std::map<Core::Ticket, mtpRequestId> _requests;
	Query _query;
	Core::Coordinator _core;
};

RestrictedGlobalSearchCoordinator::RestrictedGlobalSearchCoordinator(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
}
RestrictedGlobalSearchCoordinator::~RestrictedGlobalSearchCoordinator() = default;
void RestrictedGlobalSearchCoordinator::start(Query query, Fn<void(Page)> done) { _impl->start(std::move(query), std::move(done)); }
void RestrictedGlobalSearchCoordinator::requestMore() { _impl->more(); }
void RestrictedGlobalSearchCoordinator::cancel() { _impl->cancelSearch(); }
void RestrictedGlobalSearchCoordinator::retry() { _impl->retry(); }
bool RestrictedGlobalSearchCoordinator::active() const { return _impl->active(); }
crl::time RestrictedGlobalSearchCoordinator::cooldownLeft() const { return _impl->cooldownLeft(); }
} // namespace Api
