/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/restricted_search_scope.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace Scope = Api::RestrictedSearchScope;

namespace {

void scopeFolderScenarios() {
	const auto both = Scope::scopeFolderIds(false, 42, 0, 1);
	assert((both == std::vector<int>{ 0, 1 }));
	assert((Scope::scopeFolderIds(true, 0, 0, 1)
		== std::vector<int>{ 0 }));
	assert((Scope::scopeFolderIds(true, 1, 0, 1)
		== std::vector<int>{ 1 }));
	assert(Scope::folderInScope(both, 0));
	assert(Scope::folderInScope(both, 1));
	assert(!Scope::folderInScope(both, 2));
}

void typePolicyMatchesOracle() {
	struct Example {
		Scope::PeerKind kind;
		bool user;
		bool group;
		bool broadcast;
	};
	const auto examples = std::vector<Example>{
		{ Scope::PeerKind::Other, false, false, false },
		{ Scope::PeerKind::User, true, false, false },
		{ Scope::PeerKind::Chat, false, true, false },
		{ Scope::PeerKind::Megagroup, false, true, false },
		{ Scope::PeerKind::Broadcast, false, false, true },
	};
	const auto filters = std::vector<Scope::TypeFilter>{
		{},
		{ .usersOnly = true },
		{ .groupsOnly = true },
		{ .broadcastsOnly = true },
		{ .usersOnly = true, .groupsOnly = true },
		{ .groupsOnly = true, .broadcastsOnly = true },
	};
	for (const auto &example : examples) {
		for (const auto &filter : filters) {
			const auto expected = (!filter.usersOnly || example.user)
				&& (!filter.groupsOnly || example.group)
				&& (!filter.broadcastsOnly || example.broadcast);
			assert(Scope::matchesType(example.kind, filter) == expected);
		}
	}
}

void mockCandidatePreparationMatchesOracle() {
	using Origin = Scope::Origin;
	using Kind = Scope::PeerKind;
	using State = Scope::RestrictedState;
	const auto candidates = std::vector<Scope::Candidate>{
		{ .origin = Origin::Dialog, .kind = Kind::Chat,
			.restricted = State::Restricted, .folderId = 0,
			.listedInDialogs = true }, // Main dialog.
		{ .origin = Origin::Dialog, .kind = Kind::Megagroup,
			.restricted = State::Unknown, .folderId = 1,
			.listedInDialogs = true }, // Archive; full peer still pending.
		{ .origin = Origin::Dialog, .kind = Kind::Chat,
			.folderId = 2, .listedInDialogs = true }, // Outside both folders.
		{ .origin = Origin::Dialog, .kind = Kind::Chat,
			.folderId = 0, .listedInDialogs = false }, // Cached, absent from dialog list.
		{ .origin = Origin::Dialog, .kind = Kind::Broadcast,
			.folderId = 0, .listedInDialogs = true,
			.channel = true, .haveLeft = true },
		{ .origin = Origin::Dialog, .kind = Kind::Broadcast,
			.folderId = 0, .listedInDialogs = true,
			.channel = true, .forbidden = true },
		{ .origin = Origin::Dialog, .kind = Kind::Megagroup,
			.folderId = 0, .listedInDialogs = true,
			.channel = true, .community = true,
			.haveLeft = true }, // Community rows retain their special rule.
		{ .origin = Origin::FoldedCommunityMember, .kind = Kind::Chat,
			.folderId = 0, .folderKnown = true, .joined = true },
		{ .origin = Origin::FoldedCommunityMember, .kind = Kind::Chat,
			.folderId = 1, .folderKnown = true, .joined = true },
		{ .origin = Origin::FoldedCommunityMember, .kind = Kind::Chat,
			.folderId = 2, .folderKnown = true, .joined = true },
		{ .origin = Origin::FoldedCommunityMember, .kind = Kind::Chat,
			.folderId = 0, .folderKnown = false, .joined = true },
		{ .origin = Origin::FoldedCommunityMember, .kind = Kind::Chat,
			.folderId = 0, .folderKnown = true, .joined = false },
		{ .origin = Origin::ExplicitCommunityLink, .kind = Kind::Broadcast,
			.canViewHistory = true },
		{ .origin = Origin::ExplicitCommunityLink, .kind = Kind::Broadcast,
			.canViewHistory = false },
	};
	const auto selection = Scope::selectCandidates(
		candidates,
		Scope::scopeFolderIds(false, 91, 0, 1),
		{});
	const auto expected = std::vector<std::size_t>{ 0, 1, 6, 7, 8, 12 };
	assert(selection.candidates == expected);
	assert(selection.needsFullPeer); // Unknown remains pending, not rejected.

	const auto groupsOnly = Scope::selectCandidates(
		candidates,
		Scope::scopeFolderIds(false, 91, 0, 1),
		{ .groupsOnly = true });
	assert((groupsOnly.candidates == std::vector<std::size_t>{ 0, 1, 6, 7, 8 }));
	assert(groupsOnly.needsFullPeer);
}

void preparationReadinessAndTimeout() {
	assert(Scope::needsFullPeer(Scope::RestrictedState::Unknown));
	assert(!Scope::needsFullPeer(Scope::RestrictedState::Restricted));
	assert(!Scope::searchable(Scope::RestrictedState::Unknown));
	assert(Scope::searchable(Scope::RestrictedState::Restricted));
	assert(!Scope::searchable(Scope::RestrictedState::Unrestricted));
	assert(Scope::preparation(false, false, false) == Scope::Preparation::Wait);
	assert(Scope::preparation(true, false, false) == Scope::Preparation::Complete);
	assert(Scope::preparation(false, true, false) == Scope::Preparation::Partial);
	assert(Scope::preparation(true, false, true) == Scope::Preparation::Partial);
}

} // namespace

int main() {
	scopeFolderScenarios();
	typePolicyMatchesOracle();
	mockCandidatePreparationMatchesOracle();
	preparationReadinessAndTimeout();
}
