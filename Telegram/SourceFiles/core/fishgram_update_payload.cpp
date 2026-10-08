#include "core/fishgram_update_payload.h"
#include "core/fishgram_update_policy.h"
#include <LzmaLib.h>
#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <set>
#include <cstring>

namespace Core::FishGramUpdates {

QByteArray EncodeTrustRecord(const Updates::Manifest &manifest) {
	QByteArray result;
	QDataStream stream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << manifest.bytes << manifest.signature;
	return result;
}

std::optional<Updates::Manifest> ReadVerifiedTrustRecord(
		const QByteArray &record, const QByteArray &root, QString *error) {
	if (record.isEmpty() || record.size() > 1024 * 1024) return std::nullopt;
	QDataStream stream(record);
	stream.setVersion(QDataStream::Qt_5_1);
	QByteArray json, signature;
	stream >> json >> signature;
	if (stream.status() != QDataStream::Ok || !stream.atEnd() || signature.size() != 64) return std::nullopt;
	return Updates::ParseVerifiedManifest(json, signature, root, error);
}

std::optional<VerifiedPayload> DecodeVerifiedPayload(
		const Updates::VerifiedUpdate &verified, QString *error) {
	const auto fail = [&](const char *reason) -> std::optional<VerifiedPayload> {
		if (error) *error = QString::fromLatin1(reason);
		return std::nullopt;
	};
	const auto &envelope = verified.envelope;
	if (!(envelope.target == Updates::Target{ Updates::Os::Windows, Updates::Arch::X64 })
		|| !envelope.version || !Updates::UpdateVersionCounter(envelope.version)
		|| (envelope.channel != Updates::Channel::Stable && envelope.channel != Updates::Channel::Beta)) {
		return fail("Unsupported Windows payload target, version or channel.");
	}
	const auto &payload = envelope.payload;
	if (payload.size() <= LZMA_PROPS_SIZE + 4 || payload.size() > int(Updates::kMaxPayloadSize)) {
		return fail("Invalid compressed payload size.");
	}
	qint32 size = 0;
	std::memcpy(&size, payload.constData() + LZMA_PROPS_SIZE, sizeof(size));
	if (size <= 0 || size > 1024 * 1024 * 1024) return fail("Invalid expanded payload size.");
	QByteArray raw(size, char(0));
	SizeT outputSize = SizeT(size);
	const auto expectedInput = SizeT(payload.size() - LZMA_PROPS_SIZE - 4);
	SizeT inputSize = expectedInput;
	if (LzmaUncompress(reinterpret_cast<Byte*>(raw.data()), &outputSize,
		reinterpret_cast<const Byte*>(payload.constData() + LZMA_PROPS_SIZE + 4),
		&inputSize, reinterpret_cast<const Byte*>(payload.constData()), LZMA_PROPS_SIZE) != SZ_OK
		|| outputSize != SizeT(size) || inputSize != expectedInput) {
		return fail("Payload decompression failed.");
	}
	QBuffer buffer(&raw);
	if (!buffer.open(QIODevice::ReadOnly)) return fail("Payload cannot be read.");
	QDataStream stream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	quint32 base = 0, count = 0;
	stream >> base >> count;
	if (stream.status() != QDataStream::Ok || base != Updates::UpdateVersionBase(envelope.version)
		|| count < 2 || count > 128) return fail("Invalid payload version or file count.");
	VerifiedPayload result{ envelope.version, envelope.channel, {} };
	std::set<QString> names;
	for (quint32 i = 0; i < count; ++i) {
		QString name;
		quint32 length = 0;
		QByteArray bytes;
		stream >> name >> length >> bytes;
		if (stream.status() != QDataStream::Ok || bytes.isEmpty()
			|| length != quint32(bytes.size()) || !IsSafePayloadName(name.toStdString())
			|| !names.insert(name.toLower()).second) return fail("Invalid payload file.");
		result.files.emplace(std::move(name), std::move(bytes));
	}
	if (!buffer.atEnd() || !names.contains("telegram.exe") || !names.contains("updater.exe")) {
		return fail("Incomplete payload or trailing data.");
	}
	return result;
}

} // namespace Core::FishGramUpdates
