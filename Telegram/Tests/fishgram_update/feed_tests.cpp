#include "core/fishgram_update_feed.h"
#include "core/fishgram_update_policy.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <cassert>

int main() {
	using namespace Core::FishGramUpdates;
	const auto bytes = QByteArray("signed test package");
	auto update = QJsonObject{
		{ "version", QString::number(MakeVersion(7002009, 9)) },
		{ "size", QString::number(bytes.size()) },
		{ "channel", "stable" },
		{ "url", "https://github.com/lonefisher/fishgram/releases/download/v7.2.9-r9/fishgram-update-win-x64-7002009-r9" },
		{ "sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) },
	};
	auto root = QJsonObject{ {"schema", 1}, {"channel", "stable"}, {"platform", "windows-x64"}, {"update", update} };
	const auto parse = [&] { return ParseFeed(QJsonDocument(root).toJson(), "stable", MakeVersion(7002009, 8)); };
	const auto good = parse();
	assert(good && good->candidate);
	assert(good->candidate->channel == "stable");
	assert(good->candidate->version == MakeVersion(7002009, 9));
	assert(MatchesDownload(*good->candidate, bytes));
	assert(!MatchesDownload(*good->candidate, bytes + "corrupt"));
	// Beta clients discover through the beta index even after opting out of
	// beta packages. The index channel and signed package channel are distinct.
	auto betaIndex = root;
	betaIndex["channel"] = "beta";
	betaIndex["update"] = update;
	const auto betaIndexParse = [&] {
		return ParseFeed(QJsonDocument(betaIndex).toJson(), "beta", MakeVersion(7002009, 8));
	};
	const auto stableFromBeta = betaIndexParse();
	assert(stableFromBeta && stableFromBeta->candidate);
	assert(stableFromBeta->candidate->channel == "stable");
	assert(stableFromBeta->candidate->url.endsWith("fishgram-update-win-x64-7002009-r9"));
	assert(MatchesDownload(*stableFromBeta->candidate, bytes));
	const auto staleFromBeta = ParseFeed(
		QJsonDocument(betaIndex).toJson(), "beta", MakeVersion(7002009, 9));
	assert(staleFromBeta && !staleFromBeta->candidate);
	auto betaPackage = update;
	betaPackage["channel"] = "beta";
	betaPackage["url"] = "https://github.com/lonefisher/fishgram/releases/download/v7.2.9-r9/fishgram-update-win-x64-7002009-r9-beta";
	betaIndex["update"] = betaPackage;
	const auto betaFromBeta = betaIndexParse();
	assert(betaFromBeta && betaFromBeta->candidate);
	assert(betaFromBeta->candidate->channel == "beta");
	assert(betaFromBeta->candidate->url.endsWith("-beta"));
	root["update"] = betaPackage;
	assert(!parse()); // Stable indexes cannot advertise beta packages.
	root["update"] = update;
	auto legacyBetaIndex = betaIndex;
	auto legacyEntry = update;
	legacyEntry.remove("channel");
	legacyEntry["url"] = "https://github.com/lonefisher/fishgram/releases/download/v7.2.9-r9/fishgram-update-win-x64-7002009-r9-beta";
	legacyBetaIndex["update"] = legacyEntry;
	const auto legacyBeta = ParseFeed(
		QJsonDocument(legacyBetaIndex).toJson(), "beta", MakeVersion(7002009, 8));
	assert(legacyBeta && legacyBeta->candidate);
	assert(legacyBeta->candidate->channel == "beta");
	assert(legacyBeta->candidate->url.endsWith("-beta"));
	auto canaryPackage = update;
	canaryPackage["channel"] = "canary-public";
	betaIndex["update"] = canaryPackage;
	assert(!betaIndexParse());
	betaIndex["update"] = update;
	for (const auto bad : { "https://github.com/telegramdesktop/tdesktop/releases/download/v7.2.9/tupdate7002009", "http://github.com/lonefisher/fishgram/releases/download/v9/file", "https://github.com.evil.test/lonefisher/fishgram/releases/download/v9/file", "https://github.com/lonefisher/fishgram/releases/download/v9/../file", "https://github.com/lonefisher/fishgram/releases/download/v9/fishgram-update-win-x64-7002009-r8" }) {
		auto changed = update; changed["url"] = bad; root["update"] = changed;
		assert(!parse());
	}
	for (const auto field : { "version", "size" }) {
		auto changed = update; changed[field] = 30073400000000001.0; root["update"] = changed;
		assert(!parse());
	}
	root["update"] = update; root["platform"] = "windows-arm64"; assert(!parse());
	root["platform"] = "windows-x64"; root["channel"] = "beta"; assert(!parse());
	root["channel"] = "stable"; root["update"] = QJsonValue::Null;
	assert(parse() && !parse()->candidate);
	root.remove("update"); assert(!parse());
}
