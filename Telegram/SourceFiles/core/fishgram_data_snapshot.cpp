#include "core/fishgram_data_snapshot.h"

#include "_other/fishgram_update_transaction.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QUuid>

#include <Windows.h>
#include <Aclapi.h>
#undef small

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace Core::FishGramUpdates {
namespace {

using namespace WindowsTransaction;
using namespace WindowsTransaction::Details;
constexpr quint64 kMaxManifest = 8 * 1024 * 1024;
constexpr int kMaxEntries = 50000;
constexpr quint64 kReserveBytes = 1024 * 1024;
constexpr auto kRestoreJournal = ".fishgram-data-restore.json";
constexpr auto kMarkerName = ".fishgram-snapshots";
constexpr char kMarkerBytes[] = "FishGram snapshots v1\n";

struct Entry final {
	QString relative;
	QString source;
	quint64 size = 0;
	QByteArray hash;
	bool directory = false;
};

struct WinHandle final {
	HANDLE value = INVALID_HANDLE_VALUE;
	WinHandle() = default;
	WinHandle(const WinHandle&) = delete;
	WinHandle &operator=(const WinHandle&) = delete;
	WinHandle(WinHandle &&other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
	WinHandle &operator=(WinHandle &&other) noexcept {
		if (this != &other) {
			if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
			value = std::exchange(other.value, INVALID_HANDLE_VALUE);
		}
		return *this;
	}
	~WinHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool Fail(QString *error, const QString &message);

bool LockFile(const QString &path, WinHandle *lock, QString *error) {
	lock->value = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (lock->value == INVALID_HANDLE_VALUE) return Fail(error, QStringLiteral("Cannot open snapshot lock."));
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(lock->value, &size)) return Fail(error, QStringLiteral("Cannot inspect snapshot lock."));
	if (!size.QuadPart) {
		DWORD written = 0;
		const char zero = '0';
		if (!WriteFile(lock->value, &zero, 1, &written, nullptr) || written != 1 || !FlushFileBuffers(lock->value)) {
			return Fail(error, QStringLiteral("Cannot initialize snapshot lock."));
		}
	}
	OVERLAPPED overlapped = {};
	if (!LockFileEx(lock->value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
		return Fail(error, QStringLiteral("Another snapshot or restore operation is active."));
	}
	return true;
}

bool Fail(QString *error, const QString &message) {
	if (error) *error = message;
	return false;
}

bool SafePath(const QString &path, bool directory) {
	const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
	return attributes != INVALID_FILE_ATTRIBUTES
		&& bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory
		&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool Trusted(const QString &path) {
	return HasTrustedPermissions(path.toStdWString());
}

bool VersionString(quint64 encoded, QString *result) {
	const auto base = quint32(encoded >> 32);
	const auto revision = quint32(encoded);
	if (!base || !revision) return false;
	const auto major = base / 1000000;
	const auto minor = (base / 1000) % 1000;
	const auto patch = base % 1000;
	*result = QStringLiteral("%1.%2.%3-r%4").arg(major).arg(minor).arg(patch).arg(revision);
	return true;
}

bool HashFile(const QString &path, QByteArray *hash, quint64 *size) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;
	QCryptographicHash digest(QCryptographicHash::Sha256);
	quint64 total = 0;
	while (!file.atEnd()) {
		const auto block = file.read(1024 * 1024);
		if (block.isEmpty() && file.error() != QFile::NoError) return false;
		total += quint64(block.size());
		digest.addData(block);
	}
	*hash = digest.result().toHex();
	*size = total;
	return true;
}

bool Enumerate(const QString &root, std::vector<Entry> *entries, quint64 *bytes, QString *error, const QString &prefix = {}) {
	const auto rootNative = root.toStdWString();
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(rootNative, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return Fail(error, QStringLiteral("Cannot enumerate tdata."));
	bool okay = true;
	do {
		const auto name = QString::fromWCharArray(data.cFileName);
		if (name == QLatin1Char('.') || name == QStringLiteral("..")) continue;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			okay = Fail(error, QStringLiteral("tdata contains a reparse point."));
			break;
		}
		const auto source = QDir(root).filePath(name);
		const auto relative = prefix.isEmpty() ? name : (prefix + QLatin1Char('/') + name);
		const bool directory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
		if (name == QLatin1Char('.') || name == QStringLiteral("..")
			|| name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
			|| name.contains(QLatin1Char(':')) || name.endsWith(QLatin1Char('.'))
			|| name.endsWith(QLatin1Char(' '))
			|| QRegularExpression(QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$"), QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) {
			okay = Fail(error, QStringLiteral("tdata contains an unsafe name."));
			break;
		}
		if (directory) {
			if (!SafePath(source, true)) { okay = Fail(error, QStringLiteral("Unsafe tdata directory.")); break; }
			entries->push_back({ relative, source, 0, {}, true });
			if (!Enumerate(source, entries, bytes, error, relative)) { okay = false; break; }
		} else {
			if (!SafePath(source, false)) { okay = Fail(error, QStringLiteral("Unsafe tdata file.")); break; }
			QByteArray hash;
			quint64 size = 0;
		if (!HashFile(source, &hash, &size) || size > (std::numeric_limits<quint64>::max)() - *bytes) {
				okay = Fail(error, QStringLiteral("Cannot read tdata file."));
				break;
			}
			*bytes += size;
			entries->push_back({ relative, source, size, hash, false });
		}
		if (int(entries->size()) > kMaxEntries) { okay = Fail(error, QStringLiteral("tdata has too many entries.")); break; }
	} while (FindNextFileW(find, &data));
	const auto lastError = GetLastError();
	FindClose(find);
	if (okay && lastError != ERROR_NO_MORE_FILES) return Fail(error, QStringLiteral("Cannot finish enumerating tdata."));
	return okay;
}

bool HoldFiles(const std::vector<Entry> &entries, std::vector<WinHandle> *handles, QString *error) {
	handles->reserve(entries.size());
	for (const auto &entry : entries) {
		if (entry.directory) continue;
		WinHandle held;
		held.value = CreateFileW(reinterpret_cast<LPCWSTR>(entry.source.utf16()), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (held.value == INVALID_HANDLE_VALUE) return Fail(error, QStringLiteral("A tdata file is busy or cannot be held safely."));
		BY_HANDLE_FILE_INFORMATION info = {};
		if (!GetFileInformationByHandle(held.value, &info)
			|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
			return Fail(error, QStringLiteral("A tdata file changed type during snapshot preparation."));
		}
		handles->push_back(std::move(held));
	}
	return true;
}

bool VerifyPrivateTree(const QString &root) {
	if (!SafePath(root, true) || !Trusted(root)) return false;
	QDir directory(root);
	for (const auto &name : directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
		const auto path = directory.filePath(name);
		if (!SafePath(path, QFileInfo(path).isDir()) || !Trusted(path)) return false;
		if (QFileInfo(path).isDir() && !VerifyPrivateTree(path)) return false;
	}
	return true;
}

bool WritePrivateFile(const QString &path, const QByteArray &bytes, QString *error) {
	QSaveFile file(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
		return Fail(error, QStringLiteral("Cannot write snapshot metadata."));
	}
	return Trusted(path);
}

bool CopyFile(const Entry &entry, const QString &target, QString *error) {
	const auto source = CreateFileW(reinterpret_cast<LPCWSTR>(entry.source.utf16()), GENERIC_READ,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (source == INVALID_HANDLE_VALUE) return Fail(error, QStringLiteral("Cannot reopen a held tdata file."));
	const auto output = CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()), GENERIC_WRITE, 0,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (output == INVALID_HANDLE_VALUE) { CloseHandle(source); return Fail(error, QStringLiteral("Cannot create snapshot file.")); }
	QCryptographicHash digest(QCryptographicHash::Sha256);
	quint64 total = 0;
	std::vector<char> buffer(1024 * 1024);
	bool okay = true;
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(source, buffer.data(), DWORD(buffer.size()), &read, nullptr)) { okay = false; break; }
		if (!read) break;
		DWORD written = 0;
		if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read) { okay = false; break; }
		digest.addData(buffer.data(), int(read));
		total += read;
	}
	okay = okay && FlushFileBuffers(output) && total == entry.size && digest.result().toHex() == entry.hash;
	CloseHandle(source);
	CloseHandle(output);
	if (!okay) return Fail(error, QStringLiteral("Snapshot copy hash or size verification failed."));
	if (!Trusted(target)) return Fail(error, QStringLiteral("Snapshot file permissions did not verify."));
	return true;
}

bool SameInventory(const QString &root, const std::vector<Entry> &entries, QString *error) {
	std::vector<Entry> current;
	quint64 bytes = 0;
	if (!Enumerate(root, &current, &bytes, error) || current.size() != entries.size()) return false;
	std::sort(current.begin(), current.end(), [](const Entry &a, const Entry &b) { return a.relative < b.relative; });
	for (size_t i = 0; i != entries.size(); ++i) {
		if (current[i].relative != entries[i].relative || current[i].directory != entries[i].directory
			|| current[i].size != entries[i].size || current[i].hash != entries[i].hash) return false;
	}
	return true;
}

QJsonObject Manifest(const std::vector<Entry> &entries, const QString &version) {
	QJsonObject files;
	for (const auto &entry : entries) {
		files.insert(entry.relative, entry.directory
			? QJsonObject{ { QStringLiteral("directory"), true } }
			: QJsonObject{ { QStringLiteral("size"), qint64(entry.size) }, { QStringLiteral("sha256"), QString::fromLatin1(entry.hash) } });
	}
	return {
		{ QStringLiteral("schema"), 1 },
		{ QStringLiteral("version"), version },
		{ QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("files"), files },
	};
}

} // namespace

bool SnapshotBeforeBaselineUpdate(
		const QString &workDir,
		const QString &installedExecutable,
		quint64 runningVersion,
		QString *error) {
	if (error) error->clear();
	QString version;
	if (!VersionString(runningVersion, &version)) return Fail(error, QStringLiteral("Invalid running version."));
	const auto inputWork = QDir::cleanPath(QFileInfo(workDir).absoluteFilePath());
	const auto inputExecutable = QDir::cleanPath(QFileInfo(installedExecutable).absoluteFilePath());
	const auto hasParentTraversal = [](const QString &value) {
		return QDir::fromNativeSeparators(value).split(QLatin1Char('/')).contains(QStringLiteral(".."));
	};
	if (hasParentTraversal(workDir) || hasParentTraversal(installedExecutable)) {
		return Fail(error, QStringLiteral("Parent traversal is not permitted in snapshot paths."));
	}
	std::wstring canonicalWork;
	if (!CanonicalDirectory(inputWork.toStdWString(), &canonicalWork)) {
		return Fail(error, QStringLiteral("Invalid portable work or installation path."));
	}
	const auto canonicalWorkQt = QDir::fromNativeSeparators(QString::fromStdWString(canonicalWork));
	const auto inputInstall = QFileInfo(inputExecutable).absolutePath();
	std::wstring canonicalInstall;
	if (!CanonicalDirectory(inputInstall.toStdWString(), &canonicalInstall)) return Fail(error, QStringLiteral("Invalid install directory or reparse point."));
	const auto executableName = QFileInfo(inputExecutable).fileName();
	const auto executableNative = Join(canonicalInstall, executableName.toStdWString());
	const auto executable = QString::fromStdWString(executableNative);
	if (!SafePath(executable, false)) return Fail(error, QStringLiteral("Installed executable is missing or unsafe."));
	DirectoryGuard workGuard, installGuard;
	if (!HoldDirectoryPath(canonicalWork, &workGuard)) return Fail(error, QStringLiteral("Cannot hold portable work path safely."));
	if (!HoldDirectoryPath(canonicalInstall, &installGuard)) return Fail(error, QStringLiteral("Cannot hold installation path safely."));
	if (TargetProcessRunning(executableNative)) return Fail(error, QStringLiteral("The installed client is still running."));
	const auto data = QDir(canonicalWorkQt).filePath(QStringLiteral("tdata"));
	const auto journal = QDir(canonicalWorkQt).filePath(QLatin1String(kRestoreJournal));
	if (!IsAbsent(journal.toStdWString())) return Fail(error, QStringLiteral("An interrupted data restore must be resolved first."));
	if (IsAbsent(data.toStdWString())) return true;
	if (!SafePath(data, true)) return Fail(error, QStringLiteral("tdata is not a safe directory."));
	DirectoryGuard dataGuard;
	if (!HoldDirectoryPath(data.toStdWString(), &dataGuard)) return Fail(error, QStringLiteral("Cannot hold tdata directory safely."));
	std::vector<Entry> entries;
	quint64 totalBytes = 0;
	if (!Enumerate(data, &entries, &totalBytes, error)) return false;
	std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.relative < b.relative; });
	std::vector<std::unique_ptr<DirectoryGuard>> sourceDirectoryGuards;
	for (const auto &entry : entries) {
		if (!entry.directory) continue;
		sourceDirectoryGuards.push_back(std::make_unique<DirectoryGuard>());
		if (!HoldDirectoryPath(entry.source.toStdWString(), sourceDirectoryGuards.back().get())) return Fail(error, QStringLiteral("Cannot hold nested tdata directory safely."));
	}
	std::vector<WinHandle> held;
	if (!HoldFiles(entries, &held, error)) return false;
	if (TargetProcessRunning(executable.toStdWString())) return Fail(error, QStringLiteral("The installed client started during snapshot preparation."));
	if (!SameInventory(data, entries, error)) return Fail(error, QStringLiteral("tdata changed during snapshot preparation."));
	QJsonObject manifest = Manifest(entries, version);
	auto manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
	if (manifestBytes.size() > kMaxManifest) return Fail(error, QStringLiteral("Snapshot manifest exceeds 8 MiB."));
	const auto digest = QCryptographicHash::hash(canonicalWorkQt.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
	const auto root = QFileInfo(canonicalWorkQt).absolutePath() + QStringLiteral("/.fishgram-snapshots-") + QString::fromLatin1(digest);
	const auto storage = QStorageInfo(QFileInfo::exists(root) ? root : QFileInfo(root).absolutePath());
	if (!storage.isValid() || storage.bytesAvailable() < qint64(totalBytes + kReserveBytes + quint64(manifestBytes.size()))) {
		return Fail(error, QStringLiteral("Insufficient free space for a complete snapshot."));
	}
	if (!EnsurePrivateDirectory(root.toStdWString())) return Fail(error, QStringLiteral("Cannot create or validate the private snapshot root ACL."));
	if (!HoldDirectoryPath(root.toStdWString(), &workGuard)) return Fail(error, QStringLiteral("Cannot hold snapshot storage path safely."));
	WinHandle rootLock, workLock;
	if (!LockFile(QDir(root).filePath(QStringLiteral(".snapshot.lock")), &rootLock, error)
		|| !LockFile(QDir(canonicalWorkQt).filePath(QStringLiteral(".fishgram-data.lock")), &workLock, error)) return false;
	const auto markerPath = QDir(root).filePath(QLatin1String(kMarkerName));
	const auto rootEntries = QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
	const auto snapshotPattern = QRegularExpression(QStringLiteral("^snapshot-[0-9]{8}T[0-9]{12}Z-[a-f0-9]{32}$"));
	const auto partialPattern = QRegularExpression(QStringLiteral("^\\.partial-[a-f0-9]{32}$"));
	for (const auto &name : rootEntries) {
		if (name == QLatin1String(kMarkerName) || name == QStringLiteral(".snapshot.lock")) continue;
		if (!snapshotPattern.match(name).hasMatch() && !partialPattern.match(name).hasMatch()) return Fail(error, QStringLiteral("Snapshot root contains an unrelated entry."));
	}
	if (!IsAbsent(markerPath.toStdWString())) {
		if (!SafePath(markerPath, false) || !Trusted(markerPath)) return Fail(error, QStringLiteral("Snapshot marker is unsafe or not private."));
		QFile marker(markerPath);
		if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != QByteArray(kMarkerBytes, int(sizeof(kMarkerBytes) - 1))) return Fail(error, QStringLiteral("Invalid snapshot marker."));
	} else if (!WritePrivateFile(markerPath, QByteArray(kMarkerBytes, int(sizeof(kMarkerBytes) - 1)), error)) return false;
	if (!VerifyPrivateTree(root)) return Fail(error, QStringLiteral("Snapshot root contains unsafe or non-private entries."));
	const auto stageName = QStringLiteral(".partial-") + QUuid::createUuid().toString(QUuid::Id128);
	const auto stage = QDir(root).filePath(stageName);
	if (!QDir().mkdir(stage) || !Trusted(stage)) return Fail(error, QStringLiteral("Cannot create private snapshot staging directory."));
	bool published = false;
	auto cleanup = qScopeGuard([&] { if (!published) QDir(stage).removeRecursively(); });
	const auto targetTdata = QDir(stage).filePath(QStringLiteral("tdata"));
	if (!QDir().mkdir(targetTdata) || !Trusted(targetTdata)) return Fail(error, QStringLiteral("Cannot create private snapshot data directory."));
	for (const auto &entry : entries) {
		const auto target = QDir(targetTdata).filePath(entry.relative);
		if (entry.directory) {
			if (!QDir().mkpath(target) || !Trusted(target)) return Fail(error, QStringLiteral("Cannot create private snapshot subdirectory."));
		} else if (!CopyFile(entry, target, error)) return false;
	}
	if (!SameInventory(data, entries, error) || !SameInventory(targetTdata, entries, error)
		|| TargetProcessRunning(executable.toStdWString())) return Fail(error, QStringLiteral("Snapshot verification failed or the client restarted."));
	if (!WritePrivateFile(QDir(stage).filePath(QStringLiteral("manifest.json")), manifestBytes, error)
		|| !VerifyPrivateTree(stage)) return Fail(error, QStringLiteral("Snapshot contents or permissions did not verify."));
	const auto stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz"))
		+ QStringLiteral("000Z-") + QUuid::createUuid().toString(QUuid::Id128);
	const auto finalName = QStringLiteral("snapshot-") + stamp;
	const auto finalPath = QDir(root).filePath(finalName);
	if (!MoveFileExW(reinterpret_cast<LPCWSTR>(stage.utf16()), reinterpret_cast<LPCWSTR>(finalPath.utf16()), MOVEFILE_WRITE_THROUGH)) {
		return Fail(error, QStringLiteral("Cannot atomically publish snapshot."));
	}
	published = true;
	QDir(root).setSorting(QDir::Name | QDir::Reversed);
	const auto refreshed = QDir(root).entryList({ QStringLiteral("snapshot-*") }, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
	for (int i = 2; i < refreshed.size(); ++i) {
		const auto expired = QDir(root).filePath(refreshed[i]);
		if (snapshotPattern.match(refreshed[i]).hasMatch() && VerifyPrivateTree(expired)) QDir(expired).removeRecursively();
	}
	return true;
}

} // namespace Core::FishGramUpdates
