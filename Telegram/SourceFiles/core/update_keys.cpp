/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/update_keys.h"

#include "update_keys_data.h"

namespace Core::Updates {

QByteArray RootPublicKeyPem() {
#ifndef FISHGRAM_UPDATE_TRUST_CONFIGURED
	return {};
#else
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kRootPublicKeyPem),
		sizeof(details::kRootPublicKeyPem));
#endif
}

QByteArray EmbeddedManifest() {
#ifndef FISHGRAM_UPDATE_TRUST_CONFIGURED
	return {};
#else
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kEmbeddedManifest),
		sizeof(details::kEmbeddedManifest));
#endif
}

QByteArray EmbeddedManifestSignature() {
#ifndef FISHGRAM_UPDATE_TRUST_CONFIGURED
	return {};
#else
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kEmbeddedManifestSig),
		sizeof(details::kEmbeddedManifestSig));
#endif
}

} // namespace Core::Updates
