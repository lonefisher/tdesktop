#include "core/fishgram_update_payload.h"
#include <LzmaLib.h>
#include <QtCore/QDataStream>
#include <QtCore/QStringList>
#include <cassert>
#include <cstring>
#include <iostream>

Core::Updates::VerifiedUpdate Package(QByteArray raw) {
	Core::Updates::VerifiedUpdate result;
	result.envelope.version = (quint64(7002009) << 32) | 9;
	result.envelope.target = { Core::Updates::Os::Windows, Core::Updates::Arch::X64 };
	QByteArray compressed(raw.size() * 2 + 4096, char(0));
	SizeT length = SizeT(compressed.size());
	unsigned char props[LZMA_PROPS_SIZE] = {};
	SizeT propsSize = LZMA_PROPS_SIZE;
	assert(LzmaCompress(reinterpret_cast<Byte*>(compressed.data()), &length,
		reinterpret_cast<const Byte*>(raw.constData()), SizeT(raw.size()),
		props, &propsSize, 5, 1 << 20, -1, -1, -1, -1, 1) == SZ_OK);
	compressed.resize(int(length));
	result.envelope.payload = QByteArray(reinterpret_cast<const char*>(props), LZMA_PROPS_SIZE);
	const auto size = qint32(raw.size());
	result.envelope.payload.append(reinterpret_cast<const char*>(&size), sizeof(size));
	result.envelope.payload.append(compressed);
	return result;
}

QByteArray Files(const QStringList &names, quint32 base = 7002009) {
	QByteArray raw;
	QDataStream stream(&raw, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << base << quint32(names.size());
	for (const auto &name : names) {
		const auto bytes = QByteArray("program:") + name.toUtf8();
		stream << name << quint32(bytes.size()) << bytes;
	}
	return raw;
}

int main() {
	using Core::FishGramUpdates::DecodeVerifiedPayload;
	const auto decoded = DecodeVerifiedPayload(Package(Files({"Telegram.exe", "Updater.exe", "codec.dll"})));
	assert(decoded && decoded->files.size() == 3);
	assert(decoded->files.at("Telegram.exe") == QByteArray("program:Telegram.exe"));
	for (const auto &names : {QStringList{"Telegram.exe"},
		QStringList{"Telegram.exe", "Updater.exe", "../evil.dll"},
		QStringList{"Telegram.exe", "Updater.exe", "telegram.EXE"},
		QStringList{"Telegram.exe", "Updater.exe", "tdata/key_data"}}) {
		assert(!DecodeVerifiedPayload(Package(Files(names))));
	}
	assert(!DecodeVerifiedPayload(Package(Files({"Telegram.exe", "Updater.exe"}, 7002010))));
	assert(!DecodeVerifiedPayload(Package(Files({"Telegram.exe", "Updater.exe"}) + "trailing")));
	auto bad = Package(Files({"Telegram.exe", "Updater.exe"}));
	bad.envelope.payload.chop(1);
	assert(!DecodeVerifiedPayload(bad));
	bad = Package(Files({"Telegram.exe", "Updater.exe"}));
	bad.envelope.target.arch = Core::Updates::Arch::Arm;
	assert(!DecodeVerifiedPayload(bad));
	std::cout << "Verified Windows payload tests passed.\n";
}
