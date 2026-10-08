#include "core/fishgram_update_feed.h"
#include "core/fishgram_update_policy.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Core::FishGramUpdates {

std::optional<Feed> ParseFeed(
		const QByteArray &json,
		const QString &indexChannel,
		quint64 running,
		QString *error) {
	const auto fail = [&](const char *reason) -> std::optional<Feed> {
		if (error) *error = QString::fromLatin1(reason);
		return std::nullopt;
	};
	if (json.size() > 1024 * 1024
		|| (indexChannel != "stable" && indexChannel != "beta")) {
		return fail("Invalid update channel or oversized index.");
	}
	QJsonParseError parsed;
	const auto document = QJsonDocument::fromJson(json, &parsed);
	if (parsed.error != QJsonParseError::NoError || !document.isObject()) {
		return fail("Invalid update index JSON.");
	}
	const auto root = document.object();
	if (root.value("schema") != QJsonValue(1)
		|| root.value("channel") != indexChannel
		|| root.value("platform") != "windows-x64"
		|| !root.contains("update")) {
		return fail("Update index schema, channel or platform mismatch.");
	}
	if (root.value("update").isNull()) return Feed{};
	if (!root.value("update").isObject()) return fail("Invalid update entry.");
	const auto entry = root.value("update").toObject();
	// Older indexes implicitly put packages on their own index channel. New
	// beta indexes may carry a stable package to let beta users migrate when
	// their beta setting is turned off, so take an explicit package channel
	// when present and preserve the old interpretation otherwise.
	const auto packageChannelValue = entry.value("channel");
	const auto packageChannel = packageChannelValue.isUndefined()
		? indexChannel
		: packageChannelValue.toString();
	if ((packageChannel != "stable" && packageChannel != "beta")
		|| (indexChannel == "stable" && packageChannel != "stable")) {
		return fail("Update package channel is not allowed in this index.");
	}
	const auto decimal = [&](const char *field) -> std::optional<std::uint64_t> {
		const auto value = entry.value(QLatin1String(field));
		if (!value.isString()) return std::nullopt;
		const auto bytes = value.toString().toUtf8();
		return ParseVersion(std::string_view(bytes.constData(), std::size_t(bytes.size())));
	};
	const auto version = decimal("version");
	const auto size = decimal("size");
	if (!version || (*version >> 32) == 0 || quint32(*version) == 0
		|| !size || *size > 256 * 1024 * 1024) {
		return fail("Invalid full update version or package size.");
	}
	const auto hash = entry.value("sha256");
	if (!hash.isString() || !QRegularExpression("^[a-f0-9]{64}$").match(hash.toString()).hasMatch()) {
		return fail("Invalid update SHA256.");
	}
	const auto link = entry.value("url");
	if (!link.isString()) return fail("Missing update URL.");
	const auto url = QUrl(link.toString(), QUrl::StrictMode);
	const auto expected = QString("fishgram-update-win-x64-%1-r%2%3")
		.arg(*version >> 32).arg(quint32(*version))
		.arg(packageChannel == "beta" ? "-beta" : "");
	const auto path = url.path(QUrl::FullyEncoded);
	const auto prefix = QString("/lonefisher/fishgram/releases/download/");
	const auto suffix = path.mid(prefix.size()).split('/');
	if (!url.isValid() || url.scheme() != "https" || url.host() != "github.com"
		|| !url.userName().isEmpty() || !url.password().isEmpty()
		|| url.port(-1) != -1 || url.hasQuery() || url.hasFragment()
		|| !path.startsWith(prefix) || suffix.size() != 2
		|| !QRegularExpression("^[A-Za-z0-9][A-Za-z0-9._-]*$").match(suffix[0]).hasMatch()
		|| suffix[1] != expected) {
		return fail("Update URL is not a FishGram release package.");
	}
	if (*version <= running) return Feed{};
	return Feed{ Candidate{ *version, *size, packageChannel, url.toString(), hash.toString().toLatin1() } };
}

bool MatchesDownload(const Candidate &candidate, const QByteArray &bytes) {
	return quint64(bytes.size()) == candidate.size
		&& QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == candidate.sha256;
}

} // namespace Core::FishGramUpdates
