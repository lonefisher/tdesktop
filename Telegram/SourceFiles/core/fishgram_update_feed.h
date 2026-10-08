#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <optional>

namespace Core::FishGramUpdates {

struct Candidate {
	quint64 version = 0;
	quint64 size = 0;
	QString channel;
	QString url;
	QByteArray sha256;
};

struct Feed {
	std::optional<Candidate> candidate;
};

[[nodiscard]] std::optional<Feed> ParseFeed(
	const QByteArray &json,
	const QString &channel,
	quint64 running,
	QString *error = nullptr);

[[nodiscard]] bool MatchesDownload(
	const Candidate &candidate,
	const QByteArray &bytes);

} // namespace Core::FishGramUpdates
