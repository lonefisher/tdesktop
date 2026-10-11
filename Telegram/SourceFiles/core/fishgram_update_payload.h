#pragma once

#include "core/update_verify.h"
#include <map>

namespace Core::FishGramUpdates {

struct VerifiedPayload {
	quint64 version = 0;
	Updates::Channel channel = Updates::Channel::Stable;
	std::map<QString, QByteArray> files;
};

// The caller must obtain this input from VerifyUpdate, never ParseEnvelope.
// Authentication precedes decompression and all inner payload parsing.
[[nodiscard]] std::optional<VerifiedPayload> DecodeVerifiedPayload(
	const Updates::VerifiedUpdate &verified, QString *error = nullptr);

[[nodiscard]] QByteArray EncodeTrustRecord(const Updates::Manifest &manifest);
[[nodiscard]] std::optional<Updates::Manifest> ReadVerifiedTrustRecord(
	const QByteArray &record, const QByteArray &root, QString *error = nullptr);

} // namespace Core::FishGramUpdates
