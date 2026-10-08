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
		{ "url", "https://github.com/lonefisher/fishgram/releases/download/v7.2.9-r9/fishgram-update-win-x64-7002009-r9" },
		{ "sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) },
	};
	auto root = QJsonObject{ {"schema", 1}, {"channel", "stable"}, {"platform", "windows-x64"}, {"update", update} };
	const auto parse = [&] { return ParseFeed(QJsonDocument(root).toJson(), "stable", MakeVersion(7002009, 8)); };
	const auto good = parse();
	assert(good && good->candidate);
	assert(MatchesDownload(*good->candidate, bytes));
	assert(!MatchesDownload(*good->candidate, bytes + "corrupt"));
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
