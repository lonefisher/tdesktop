#include "core/update_verify.h"
#include "core/fishgram_update_policy.h"
#include "core/fishgram_update_payload.h"
#include <LzmaLib.h>
#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <set>
#include <cstring>

namespace {
QByteArray Read(const QString &path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

// Exercise the actual signature implementation on bytes produced by Packer,
// then read the Windows payload back to prove the program bytes were preserved.
int main(int argc, char **argv) {
	using namespace Core::Updates;
	if (argc != 6) return 2;
	const auto trust = QString::fromUtf8(argv[2]);
	const auto root = Read(trust + "/root-public.pem");
	const auto held = ParseVerifiedManifest(Read(trust + "/manifest.min.json"),
		Read(trust + "/manifest.sig"), root);
	const auto running = Core::FishGramUpdates::ParseVersion(argv[3]);
	const auto channel = ChannelFromName(argv[4]);
	if (!held || !running || !channel) return 2;
	const auto record = Core::FishGramUpdates::EncodeTrustRecord(*held);
	const auto recorded = Core::FishGramUpdates::ReadVerifiedTrustRecord(record, root);
	if (!recorded || recorded->bytes != held->bytes) return 2;
	if (Core::FishGramUpdates::ReadVerifiedTrustRecord(record + "trailing", root)) return 2;
	const auto verified = VerifyUpdate(Read(QString::fromUtf8(argv[1])),
		*channel, std::strcmp(argv[5], "beta") == 0,
		{ Os::Windows, Arch::X64 }, *running, held, root,
		QDateTime::currentSecsSinceEpoch());
	if (!verified) return 1;
	const auto decoded = Core::FishGramUpdates::DecodeVerifiedPayload(*verified);
	if (!decoded || decoded->files.size() != 2) return 1;
	for (const auto &[name, bytes] : decoded->files) {
		if (bytes != QByteArray("test-program:") + name.toUtf8()) return 1;
	}
	return 0;
}
