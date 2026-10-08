#include "core/update_verify.h"
#include "core/fishgram_update_policy.h"
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
	const auto verified = VerifyUpdate(Read(QString::fromUtf8(argv[1])),
		*channel, std::strcmp(argv[5], "beta") == 0,
		{ Os::Windows, Arch::X64 }, *running, held, root,
		QDateTime::currentSecsSinceEpoch());
	if (!verified) return 1;
	const auto &payload = verified->envelope.payload;
	if (payload.size() < LZMA_PROPS_SIZE + 4) return 1;
	qint32 size = 0;
	std::memcpy(&size, payload.constData() + LZMA_PROPS_SIZE, 4);
	if (size <= 0 || size > 1024 * 1024) return 1;
	auto result = QByteArray(size, char(0));
	SizeT outputSize = SizeT(size);
	SizeT inputSize = SizeT(payload.size() - LZMA_PROPS_SIZE - 4);
	if (LzmaUncompress(reinterpret_cast<Byte *>(result.data()), &outputSize,
		reinterpret_cast<const Byte *>(payload.constData() + LZMA_PROPS_SIZE + 4),
		&inputSize, reinterpret_cast<const Byte *>(payload.constData()),
		LZMA_PROPS_SIZE) != SZ_OK || outputSize != SizeT(size)
		|| inputSize != SizeT(payload.size() - LZMA_PROPS_SIZE - 4)) return 1;
	QBuffer buffer(&result);
	buffer.open(QIODevice::ReadOnly);
	QDataStream stream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	quint32 base = 0, count = 0;
	stream >> base >> count;
	if (base != UpdateVersionBase(verified->envelope.version) || count != 2) return 1;
	std::set<QString> names;
	for (quint32 i = 0; i != count; ++i) {
		QString name;
		quint32 length = 0;
		QByteArray bytes;
		stream >> name >> length >> bytes;
		if (!Core::FishGramUpdates::IsSafePayloadName(name.toStdString())
			|| !names.insert(name.toLower()).second
			|| length != quint32(bytes.size())
			|| bytes != QByteArray("test-program:") + name.toUtf8()) return 1;
	}
	return stream.status() == QDataStream::Ok && buffer.atEnd()
		&& names.contains("telegram.exe") && names.contains("updater.exe") ? 0 : 1;
}
