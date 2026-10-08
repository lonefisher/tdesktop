#include "core/fishgram_data_snapshot.h"

#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>
#include <winioctl.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using Core::FishGramUpdates::SnapshotBeforeBaselineUpdate;

namespace {

bool TakeSnapshot(const QString &work, const QString &exe, QString *error) {
	const auto okay = SnapshotBeforeBaselineUpdate(work, exe, (quint64(7002009) << 32) | 9, error);
	if (!okay) std::cerr << error->toStdString() << '\n';
	return okay;
}

void Private(const QString &path) {
	HANDLE token = nullptr;
	assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<unsigned char> data(size);
	assert(GetTokenInformation(token, TokenUser, data.data(), size, &size));
	CloseHandle(token);
	LPWSTR sid = nullptr;
	assert(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid));
	const auto sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;%1)").arg(QString::fromWCharArray(sid));
	LocalFree(sid);
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr));
	assert(SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor));
	LocalFree(descriptor);
}

struct Fixture final {
	explicit Fixture(const QString &tempTemplate = {}) : temp(tempTemplate) {
		assert(temp.isValid());
		work = temp.path() + QStringLiteral("/portable-work");
		install = temp.path() + QStringLiteral("/install");
		assert(QDir().mkpath(work));
		assert(QDir().mkpath(install));
		Private(temp.path());
		exe = install + QStringLiteral("/Telegram.exe");
		Write(exe, "synthetic executable");
	}
	void Write(const QString &path, const QByteArray &bytes) {
		assert(QDir().mkpath(QFileInfo(path).absolutePath()));
		QFile file(path);
		assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		assert(file.write(bytes) == bytes.size());
	}
	QTemporaryDir temp;
	QString work, install, exe;
};

bool MakeJunction(const QString &linkPath, const QString &targetPath, DWORD *failure) {
	if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(linkPath.utf16()), nullptr)) { *failure = GetLastError(); return false; }
	const auto substitute = std::wstring(L"\\??\\") + QDir::toNativeSeparators(targetPath).toStdWString();
	const auto print = QDir::toNativeSeparators(targetPath).toStdWString();
	const auto stringsBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
	std::vector<unsigned char> bytes(16 + stringsBytes, 0);
	struct Header final {
		DWORD tag;
		WORD dataLength;
		WORD reserved;
		WORD substituteOffset;
		WORD substituteLength;
		WORD printOffset;
		WORD printLength;
	};
	const Header header = { IO_REPARSE_TAG_MOUNT_POINT, WORD(8 + stringsBytes), 0, 0,
		WORD(substitute.size() * sizeof(wchar_t)), WORD((substitute.size() + 1) * sizeof(wchar_t)),
		WORD(print.size() * sizeof(wchar_t)) };
	std::memcpy(bytes.data(), &header, sizeof(header));
	std::memcpy(bytes.data() + sizeof(header), substitute.data(), substitute.size() * sizeof(wchar_t));
	std::memcpy(bytes.data() + sizeof(header) + (substitute.size() + 1) * sizeof(wchar_t), print.data(), print.size() * sizeof(wchar_t));
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(linkPath.utf16()), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { *failure = GetLastError(); return false; }
	DWORD returned = 0;
	const auto okay = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(), DWORD(bytes.size()),
		nullptr, 0, &returned, nullptr) != FALSE;
	if (!okay) *failure = GetLastError();
	CloseHandle(handle);
	return okay;
}

QString SnapshotRoot(const QString &work) {
	const auto canonical = QDir(work).canonicalPath().toUtf8();
	const auto hash = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(16);
	return QFileInfo(work).absolutePath() + QStringLiteral("/.fishgram-snapshots-") + QString::fromLatin1(hash);
}

void TestFreshInstallSucceeds() {
	Fixture f;
	QString error;
	assert(TakeSnapshot(f.work, f.exe, &error));
	assert(!QFileInfo::exists(SnapshotRoot(f.work)));
}

void TestSnapshotIsReadableByRecoveryTool() {
	Fixture f;
	f.Write(f.work + QStringLiteral("/tdata/map0"), "synthetic-account-data");
	QDir().mkpath(f.work + QStringLiteral("/tdata/cache/empty-nested"));
	f.Write(f.work + QStringLiteral("/tdata/account/sub/map0"), "nested synthetic data");
	f.Write(f.work + QStringLiteral("/tdata/account/empty"), QByteArray());
	QString error;
	assert(TakeSnapshot(f.work, f.exe, &error));
	const auto root = SnapshotRoot(f.work);
	QFile marker(root + QStringLiteral("/.fishgram-snapshots"));
	assert(marker.open(QIODevice::ReadOnly));
	assert(marker.readAll() == QByteArray("FishGram snapshots v1\n"));
	const auto entries = QDir(root).entryList({QStringLiteral("snapshot-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
	assert(entries.size() == 1);
	QFile manifest(root + QLatin1Char('/') + entries.front() + QStringLiteral("/manifest.json"));
	assert(manifest.open(QIODevice::ReadOnly));
	const auto doc = QJsonDocument::fromJson(manifest.readAll());
	assert(doc.isObject());
	const auto value = doc.object();
	assert(value.value(QStringLiteral("schema")).toInt() == 1);
	assert(value.value(QStringLiteral("version")).toString() == QStringLiteral("7.2.9-r9"));
	assert(value.value(QStringLiteral("createdUtc")).isString());
	const auto files = value.value(QStringLiteral("files")).toObject();
	assert(files.value(QStringLiteral("cache")).toObject().value(QStringLiteral("directory")).toBool());
	assert(files.value(QStringLiteral("cache/empty-nested")).toObject().value(QStringLiteral("directory")).toBool());
	const auto entry = files.value(QStringLiteral("map0")).toObject();
	assert(entry.value(QStringLiteral("size")).toInt() == 22);
	assert(entry.value(QStringLiteral("sha256")).toString().toLatin1() == QCryptographicHash::hash("synthetic-account-data", QCryptographicHash::Sha256).toHex());
	QFile copied(root + QLatin1Char('/') + entries.front() + QStringLiteral("/tdata/map0"));
	assert(copied.open(QIODevice::ReadOnly));
	assert(copied.readAll() == QByteArray("synthetic-account-data"));
	assert(files.value(QStringLiteral("account/sub/map0")).toObject().value(QStringLiteral("size")).toInt() == 21);
	assert(files.value(QStringLiteral("account/empty")).toObject().value(QStringLiteral("size")).toInt() == 0);
	assert(QFileInfo(root + QLatin1Char('/') + entries.front() + QStringLiteral("/tdata/cache/empty-nested")).isDir());
}

void TestGuardAllowsChildCreationAndWrite() {
	Fixture f;
	QFile guardProbe(f.work + QStringLiteral("/guard-probe"));
	assert(guardProbe.open(QIODevice::WriteOnly));
	assert(guardProbe.write("writable") == 8);
	guardProbe.close();
	assert(QFile::remove(guardProbe.fileName()));
	f.Write(f.work + QStringLiteral("/tdata/item"), "value");
	QString error;
	assert(TakeSnapshot(f.work, f.exe, &error));
}

void TestFailuresDoNotPublishPartialSnapshot() {
	Fixture f;
	f.Write(f.work + QStringLiteral("/tdata/item"), "value");
	QString error;
	assert(TakeSnapshot(f.work, f.exe, &error));
	f.Write(f.work + QStringLiteral("/.fishgram-data-restore.json"), "pending");
	assert(!TakeSnapshot(f.work, f.exe, &error));
	assert(!error.isEmpty());
	const auto existing = QDir(SnapshotRoot(f.work)).entryList({ QStringLiteral("snapshot-*") }, QDir::Dirs | QDir::NoDotAndDotDot);
	assert(existing.size() == 1);
	QFile retained(SnapshotRoot(f.work) + QLatin1Char('/') + existing.front() + QStringLiteral("/tdata/item"));
	assert(retained.open(QIODevice::ReadOnly));
	assert(retained.readAll() == QByteArray("value"));
}

void TestRetentionKeepsTwoSnapshots() {
	Fixture f;
	f.Write(f.work + QStringLiteral("/tdata/item"), "retained");
	QString error;
	assert(TakeSnapshot(f.work, f.exe, &error));
	assert(TakeSnapshot(f.work, f.exe, &error));
	assert(TakeSnapshot(f.work, f.exe, &error));
	const auto snapshots = QDir(SnapshotRoot(f.work)).entryList({ QStringLiteral("snapshot-*") }, QDir::Dirs | QDir::NoDotAndDotDot);
	assert(snapshots.size() == 2);
}

void TestJunctionIsRejectedWithoutReadingOutsideTarget() {
	Fixture f;
	const auto outside = f.temp.path() + QStringLiteral("/outside");
	QDir().mkpath(outside);
	f.Write(outside + QStringLiteral("/sentinel"), "synthetic outside data");
	f.Write(f.work + QStringLiteral("/tdata/map0"), "synthetic-account-data");
	const auto junction = f.work + QStringLiteral("/tdata/linked-directory");
	DWORD junctionError = 0;
	if (!MakeJunction(junction, outside, &junctionError)) std::cerr << "junction fixture WinError=" << junctionError << '\n';
	assert(junctionError == ERROR_SUCCESS);
	QString error;
	assert(!TakeSnapshot(f.work, f.exe, &error));
	assert(error.contains(QStringLiteral("reparse"), Qt::CaseInsensitive));
	QFile sentinel(outside + QStringLiteral("/sentinel"));
	assert(sentinel.open(QIODevice::ReadOnly));
	assert(sentinel.readAll() == QByteArray("synthetic outside data"));
	assert(!QFileInfo::exists(SnapshotRoot(f.work)));
	assert(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(junction.utf16())));
}

}

int main(int argc, char **argv) {
	if (argc == 3 && std::string(argv[1]) == "--emit-python-fixture") {
		Fixture f(QString::fromLocal8Bit(argv[2]) + QStringLiteral("/native-snapshot-XXXXXX"));
		if (!f.temp.isValid()) return 2;
		f.temp.setAutoRemove(false);
		f.Write(f.work + QStringLiteral("/tdata/account/map0"), "native synthetic account data");
		f.Write(f.work + QStringLiteral("/tdata/account/empty"), QByteArray());
		QDir().mkpath(f.work + QStringLiteral("/tdata/empty-directory"));
		QString error;
		if (!TakeSnapshot(f.work, f.exe, &error)) return 3;
		const auto root = SnapshotRoot(f.work);
		const auto names = QDir(root).entryList({ QStringLiteral("snapshot-*") }, QDir::Dirs | QDir::NoDotAndDotDot);
		if (names.size() != 1) return 4;
		QJsonObject output = {
			{ QStringLiteral("workDir"), f.work },
			{ QStringLiteral("executable"), f.exe },
			{ QStringLiteral("snapshotRoot"), root },
			{ QStringLiteral("snapshotName"), names.front() },
		};
		std::cout << QJsonDocument(output).toJson(QJsonDocument::Compact).constData() << '\n';
		return 0;
	}
	TestFreshInstallSucceeds();
	TestSnapshotIsReadableByRecoveryTool();
	TestGuardAllowsChildCreationAndWrite();
	TestFailuresDoNotPublishPartialSnapshot();
	TestRetentionKeepsTwoSnapshots();
	TestJunctionIsRejectedWithoutReadingOutsideTarget();
	std::cout << "FishGram data snapshot tests passed\n";
}
