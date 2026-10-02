/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <vector>

// Pure candidate and preparation policy for restricted global search.
// Runtime objects such as PeerData and CommunityInfo stay with the caller;
// this header decides only from authoritative facts supplied by that caller.
namespace Api::RestrictedSearchScope {

enum class PeerKind {
	Other,
	User,
	Chat,
	Megagroup,
	Broadcast,
};

struct TypeFilter {
	bool usersOnly = false;
	bool groupsOnly = false;
	bool broadcastsOnly = false;
};

enum class Origin {
	Dialog,
	FoldedCommunityMember,
	ExplicitCommunityLink,
};

enum class RestrictedState {
	Unknown,
	Restricted,
	Unrestricted,
};

struct Candidate {
	Origin origin = Origin::Dialog;
	PeerKind kind = PeerKind::Other;
	RestrictedState restricted = RestrictedState::Unknown;
	int folderId = 0;
	bool listedInDialogs = false;
	bool folderKnown = true;
	bool joined = false;
	bool canViewHistory = false;
	bool channel = false;
	bool community = false;
	bool haveLeft = false;
	bool forbidden = false;
};

enum class Preparation {
	Wait,
	Complete,
	Partial,
};

inline std::vector<int> scopeFolderIds(
		bool hasExplicitFolder,
		int requestedFolderId,
		int mainFolderId,
		int archiveFolderId) {
	return hasExplicitFolder
		? std::vector<int>{ requestedFolderId }
		: std::vector<int>{ mainFolderId, archiveFolderId };
}

inline bool folderInScope(
		const std::vector<int> &folders,
		int folderId) {
	return std::find(folders.begin(), folders.end(), folderId)
		!= folders.end();
}

inline bool matchesType(PeerKind kind, TypeFilter filter) {
	if (filter.usersOnly && kind != PeerKind::User) return false;
	if (filter.groupsOnly
		&& kind != PeerKind::Chat
		&& kind != PeerKind::Megagroup) return false;
	if (filter.broadcastsOnly && kind != PeerKind::Broadcast) return false;
	return true;
}

inline bool originEligible(
		const Candidate &candidate,
		const std::vector<int> &scopeFolders) {
	switch (candidate.origin) {
	case Origin::Dialog:
		if (!candidate.listedInDialogs
			|| !folderInScope(scopeFolders, candidate.folderId)) {
			return false;
		}
		return !candidate.channel
			|| candidate.community
			|| (!candidate.haveLeft && !candidate.forbidden);
	case Origin::FoldedCommunityMember:
		return candidate.folderKnown
			&& folderInScope(scopeFolders, candidate.folderId)
			&& candidate.joined;
	case Origin::ExplicitCommunityLink:
		return candidate.canViewHistory;
	}
	return false;
}

inline bool candidateAllowed(
		const Candidate &candidate,
		const std::vector<int> &scopeFolders,
		TypeFilter typeFilter) {
	return matchesType(candidate.kind, typeFilter)
		&& originEligible(candidate, scopeFolders);
}

struct Selection {
	std::vector<std::size_t> candidates;
	bool needsFullPeer = false;
};

inline Selection selectCandidates(
		const std::vector<Candidate> &candidates,
		const std::vector<int> &scopeFolders,
		TypeFilter typeFilter) {
	auto result = Selection();
	for (auto i = std::size_t(0); i != candidates.size(); ++i) {
		const auto &candidate = candidates[i];
		if (!candidateAllowed(candidate, scopeFolders, typeFilter)) continue;
		result.candidates.push_back(i);
		result.needsFullPeer = result.needsFullPeer
			|| (candidate.restricted == RestrictedState::Unknown);
	}
	return result;
}

inline bool needsFullPeer(RestrictedState state) {
	return state == RestrictedState::Unknown;
}

inline bool searchable(RestrictedState state) {
	return state == RestrictedState::Restricted;
}

inline Preparation preparation(
		bool ready,
		bool deadlineExpired,
		bool hasError) {
	if (!ready && !deadlineExpired) return Preparation::Wait;
	return (!ready || hasError)
		? Preparation::Partial
		: Preparation::Complete;
}

} // namespace Api::RestrictedSearchScope
