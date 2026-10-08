/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/update_checker.h"

#include "platform/platform_specific.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/base_platform_file_utilities.h"
#include "base/timer.h"
#include "base/bytes.h"
#include "base/unixtime.h"
#include "storage/localstorage.h"
#include "core/application.h"
#include "core/changelogs.h"
#include "core/click_handler_types.h"
#include "core/update_channel.h"
#include "core/update_keys.h"
#include "core/update_verify.h"
#include "core/fishgram_update_policy.h"
#include "core/fishgram_update_feed.h"
#include "core/fishgram_update_payload.h"
#ifdef Q_OS_WIN
#include "_other/fishgram_update_transaction.h"
#endif
#include <set>
#include <QtCore/QCryptographicHash>
#include "core/version.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "mainwindow.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "info/info_memento.h"
#include "info/info_controller.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "settings/sections/settings_advanced.h"
#include "settings/settings_intro.h"
#include "ui/layers/box_content.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSaveFile>

#include <ksandbox.h>

#if !defined Q_OS_WIN && !defined Q_OS_MAC
#include "base/platform/linux/base_linux_xdp_utilities.h"

#include <flatpakportal/flatpakportal.hpp>
#endif // !Q_OS_WIN && !Q_OS_MAC


#ifndef TDESKTOP_DISABLE_AUTOUPDATE
#if defined Q_OS_WIN && !defined TDESKTOP_USE_PACKAGED // use Lzma SDK for win
#include <LzmaLib.h>
#else // Q_OS_WIN && !TDESKTOP_USE_PACKAGED
#include <lzma.h>
#endif // else of Q_OS_WIN && !TDESKTOP_USE_PACKAGED
#endif // !TDESKTOP_DISABLE_AUTOUPDATE

#ifndef Q_OS_WIN
#include <unistd.h>
#endif // !Q_OS_WIN

namespace Core {
namespace {

constexpr auto kUpdaterTimeout = 10 * crl::time(1000);
constexpr auto kMaxResponseSize = 1024 * 1024;

// tdata/version marker for installed v2 canary packages, holding the full
// 64-bit (base << 32 | counter) version. 0x7FFFFFFF is the alpha marker.
constexpr auto kVersionFileFishGramMarker = quint32(0x7FFFFFFD);

#if !defined Q_OS_WIN && !defined Q_OS_MAC
constexpr auto kFlatpakPortalService = "org.freedesktop.portal.Flatpak";
constexpr auto kFlatpakPortalObjectPath = "/org/freedesktop/portal/Flatpak";
constexpr auto kFlatpakUpdated = "/app/.updated"_cs;
#endif // !Q_OS_WIN && !Q_OS_MAC

#ifdef TDESKTOP_DISABLE_AUTOUPDATE
bool UpdaterIsDisabled = true;
#else // TDESKTOP_DISABLE_AUTOUPDATE
bool UpdaterIsDisabled = false;
#endif // TDESKTOP_DISABLE_AUTOUPDATE

std::weak_ptr<Updater> UpdaterInstance;

using Progress = UpdateChecker::Progress;
using State = UpdateChecker::State;

#ifdef Q_OS_WIN
using VersionInt = DWORD;
#else // Q_OS_WIN
using VersionInt = int;
#endif // Q_OS_WIN

using Loader = MTP::AbstractDedicatedLoader;

#if !defined Q_OS_WIN && !defined Q_OS_MAC
using namespace gi::repository;
namespace GObject = gi::repository::GObject;
#endif // !Q_OS_WIN && !Q_OS_MAC

class Checker : public base::has_weak_ptr {
public:
	Checker(bool testing);

	virtual void start() = 0;

	virtual bool poll() const;

	rpl::producer<std::shared_ptr<Loader>> ready() const;
	rpl::producer<> failed() const;

	rpl::lifetime &lifetime();

	virtual ~Checker() = default;

protected:
	bool testing() const;
	void done(std::shared_ptr<Loader> result);
	void fail();

private:
	bool _testing = false;
	rpl::event_stream<std::shared_ptr<Loader>> _ready;
	rpl::event_stream<> _failed;

	rpl::lifetime _lifetime;

};

struct Implementation {
	std::unique_ptr<Checker> checker;
	std::shared_ptr<Loader> loader;
	bool failed = false;

};

class HttpChecker : public Checker {
public:
	HttpChecker(bool testing);

	void start() override;

	~HttpChecker();

private:
	void gotResponse();
	void gotFailure(QNetworkReply::NetworkError e);
	void clearSentRequest();
	bool handleResponse(const QByteArray &response);
    enum class Request { Manifest, ManifestSignature, Feed };
    void request(Request step);
    Request _request = Request::Manifest;
    QByteArray _manifestBytes;

	std::unique_ptr<QNetworkAccessManager> _manager;
	QNetworkReply *_reply = nullptr;

};

class HttpLoaderActor;

// All installed desktop instances use FishGram's HTTP source. MTP's abstract
// loader remains a transport helper, not an update discovery fallback.

class HttpLoader : public Loader {
public:
	HttpLoader(FishGramUpdates::Candidate candidate);
    const FishGramUpdates::Candidate &candidate() const { return _candidate; }

	~HttpLoader();

private:
	void startLoading() override;

	friend class HttpLoaderActor;

	QString _url;
    FishGramUpdates::Candidate _candidate;
	std::unique_ptr<QThread> _thread;
	HttpLoaderActor *_actor = nullptr;

};

class HttpLoaderActor : public QObject {
public:
	HttpLoaderActor(
		not_null<HttpLoader*> parent,
		not_null<QThread*> thread,
		const QString &url);

private:
	void start();
	void sendRequest();

	void gotMetaData();
	void partFinished(qint64 got, qint64 total);
	void partFailed(QNetworkReply::NetworkError e);

	not_null<HttpLoader*> _parent;
	QString _url;
	QNetworkAccessManager _manager;
	std::unique_ptr<QNetworkReply> _reply;

};

#if !defined Q_OS_WIN && !defined Q_OS_MAC
class FlatpakChecker : public Checker {
public:
	FlatpakChecker(bool testing);

	void start() override;

	bool poll() const override;

	~FlatpakChecker();

private:
	FlatpakPortal::Flatpak _interface;
	FlatpakPortal::FlatpakUpdateMonitor _monitor;
	QFileSystemWatcher _watcher;
	ulong _updateAvailableSignal = 0;

};

class FlatpakLoader : public Loader {
public:
	FlatpakLoader(FlatpakPortal::FlatpakUpdateMonitor monitor);

	~FlatpakLoader();

private:
	void startLoading() override;

	FlatpakPortal::FlatpakUpdateMonitor _monitor;
	ulong _progressSignal = 0;

};
#endif // !Q_OS_WIN && !Q_OS_MAC

std::shared_ptr<Updater> GetUpdaterInstance() {
	if (const auto result = UpdaterInstance.lock()) {
		return result;
	}
	const auto result = std::make_shared<Updater>();
	UpdaterInstance = result;
	return result;
}

QString UpdatesFolder() {
	return cWorkingDir() + u"tupdates"_q;
}

void ClearAll() {
	base::Platform::DeleteDirectory(UpdatesFolder());
}

QString FindUpdateFile() {
	QDir updates(UpdatesFolder());
	if (!updates.exists()) {
		return QString();
	}
	const auto list = updates.entryInfoList(QDir::Files);
	for (const auto &info : list) {
		static const auto RegExp = QRegularExpression(
            "^fishgram-update-win-x64-[1-9][0-9]*-r[1-9][0-9]*(-beta)?$");
        if (RegExp.match(info.fileName()).hasMatch()) {
			return info.absoluteFilePath();
		}
	}
	return QString();
}

[[nodiscard]] std::optional<Updates::Manifest> HeldManifest() {
	auto error = QString();
	auto result = Updates::ParseVerifiedManifest(
		Updates::EmbeddedManifest(),
		Updates::EmbeddedManifestSignature(),
		Updates::RootPublicKeyPem(),
		&error);
	if (!result) {
		LOG(("Update Error: Bad embedded manifest: %1").arg(error));
	}
	auto manifest = QByteArray();
	auto signature = QByteArray();
	if (Local::readUpdateManifest(&manifest, &signature)) {
		auto persisted = Updates::ParseVerifiedManifest(
			manifest,
			signature,
			Updates::RootPublicKeyPem());
		if (persisted && (!result || persisted->version > result->version)) {
			result = std::move(persisted);
		}
	}
	return result;
}

void AdoptManifest(const Updates::Manifest &manifest) {
	const auto held = HeldManifest();
	if (!held || manifest.version > held->version) {
		LOG(("Update Info: Adopting manifest version %1."
			).arg(manifest.version));
		Local::writeUpdateManifest(manifest.bytes, manifest.signature);
	}
}

QString ExtractFilename(const QString &url) {
	const auto expression = QRegularExpression(u"/([^/\\?]+)(\\?|$)"_q);
	if (const auto match = expression.match(url); match.hasMatch()) {
		return match.captured(1).replace(
			QRegularExpression(u"[^a-zA-Z0-9_\\-]"_q),
			QString());
	}
	return QString();
}

#ifndef TDESKTOP_DISABLE_AUTOUPDATE

// The data must point to the exact v1 post-signature layout, which is also
// the v2 payload layout: [lzma props on Windows,] original size, compressed
// bytes. Callers only pass authenticated bytes here.
[[nodiscard]] std::optional<QByteArray> DecompressUpdatePayload(
		const char *data,
		int32 size) {
#if defined Q_OS_WIN && !defined TDESKTOP_USE_PACKAGED // use Lzma SDK for win
	const int32 hPropsLen = LZMA_PROPS_SIZE;
#else // Q_OS_WIN && !TDESKTOP_USE_PACKAGED
	const int32 hPropsLen = 0;
#endif // Q_OS_WIN && !TDESKTOP_USE_PACKAGED
	const int32 hOriginalSizeLen = sizeof(int32);
	const int32 hSize = hPropsLen + hOriginalSizeLen;
	const int32 compressedLen = size - hSize;
	if (compressedLen <= 0) {
		LOG(("Update Error: bad compressed size: %1").arg(size));
		return std::nullopt;
	}

	QByteArray uncompressed;

	int32 uncompressedLen;
	memcpy(&uncompressedLen, data + hPropsLen, hOriginalSizeLen);
	if (uncompressedLen <= 0 || uncompressedLen > 1024 * 1024 * 1024) {
		LOG(("Update Error: bad uncompressed size: %1").arg(uncompressedLen));
		return std::nullopt;
	}
	uncompressed.resize(uncompressedLen);

	size_t resultLen = uncompressed.size();
#if defined Q_OS_WIN && !defined TDESKTOP_USE_PACKAGED // use Lzma SDK for win
	SizeT srcLen = compressedLen;
	int uncompressRes = LzmaUncompress((uchar*)uncompressed.data(), &resultLen, (const uchar*)(data + hSize), &srcLen, (const uchar*)data, LZMA_PROPS_SIZE);
	if (uncompressRes != SZ_OK) {
		LOG(("Update Error: could not uncompress lzma, code: %1").arg(uncompressRes));
		return std::nullopt;
	}
#else // Q_OS_WIN && !TDESKTOP_USE_PACKAGED
	lzma_stream stream = LZMA_STREAM_INIT;

	lzma_ret ret = lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED);
	if (ret != LZMA_OK) {
		const char *msg;
		switch (ret) {
		case LZMA_MEM_ERROR: msg = "Memory allocation failed"; break;
		case LZMA_OPTIONS_ERROR: msg = "Specified preset is not supported"; break;
		case LZMA_UNSUPPORTED_CHECK: msg = "Specified integrity check is not supported"; break;
		default: msg = "Unknown error, possibly a bug"; break;
		}
		LOG(("Error initializing the decoder: %1 (error code %2)").arg(msg).arg(ret));
		return std::nullopt;
	}

	stream.avail_in = compressedLen;
	stream.next_in = (uint8_t*)(data + hSize);
	stream.avail_out = resultLen;
	stream.next_out = (uint8_t*)uncompressed.data();

	lzma_ret res = lzma_code(&stream, LZMA_FINISH);
	if (stream.avail_in) {
		LOG(("Error in decompression, %1 bytes left in _in of %2 whole.").arg(stream.avail_in).arg(compressedLen));
		return std::nullopt;
	} else if (stream.avail_out) {
		LOG(("Error in decompression, %1 bytes free left in _out of %2 whole.").arg(stream.avail_out).arg(resultLen));
		return std::nullopt;
	}
	lzma_end(&stream);
	if (res != LZMA_OK && res != LZMA_STREAM_END) {
		const char *msg;
		switch (res) {
		case LZMA_MEM_ERROR: msg = "Memory allocation failed"; break;
		case LZMA_FORMAT_ERROR: msg = "The input data is not in the .xz format"; break;
		case LZMA_OPTIONS_ERROR: msg = "Unsupported compression options"; break;
		case LZMA_DATA_ERROR: msg = "Compressed file is corrupt"; break;
		case LZMA_BUF_ERROR: msg = "Compressed data is truncated or otherwise corrupt"; break;
		default: msg = "Unknown error, possibly a bug"; break;
		}
		LOG(("Error in decompression: %1 (error code %2)").arg(msg).arg(res));
		return std::nullopt;
	}
#endif // Q_OS_WIN && !TDESKTOP_USE_PACKAGED

	return uncompressed;
}

[[nodiscard]] bool ExtractUpdateFiles(
		QDataStream &stream,
		quint32 filesCount,
		const QString &tempDirPath) {
    if (filesCount < 2 || filesCount > 128) return false;
    std::set<QString> names;
	for (uint32 i = 0; i < filesCount; ++i) {
		QString relativeName;
		quint32 fileSize;
		QByteArray fileInnerData;
		bool executable = false;

		stream >> relativeName >> fileSize >> fileInnerData;
#ifndef Q_OS_WIN
		stream >> executable;
#endif // !Q_OS_WIN
		if (stream.status() != QDataStream::Ok) {
			LOG(("Update Error: cant read file from downloaded stream, status: %1").arg(stream.status()));
			return false;
		}
		if (fileSize != quint32(fileInnerData.size())) {
			LOG(("Update Error: bad file size %1 not matching data size %2").arg(fileSize).arg(fileInnerData.size()));
			return false;
		}

        const auto encoded = relativeName.toUtf8();
        if (!FishGramUpdates::IsSafePayloadName(std::string_view(encoded.constData(), std::size_t(encoded.size())))
            || !names.insert(relativeName.toLower()).second) {
            LOG(("FishGram Update Error: unsafe or duplicate payload name."));
            return false;
        }
		QFile f(tempDirPath + '/' + relativeName);
		if (!QDir().mkpath(QFileInfo(f).absolutePath())) {
			LOG(("Update Error: cant mkpath for file '%1'").arg(tempDirPath + '/' + relativeName));
			return false;
		}
		if (!f.open(QIODevice::WriteOnly)) {
			LOG(("Update Error: cant open file '%1' for writing").arg(tempDirPath + '/' + relativeName));
			return false;
		}
		auto writtenBytes = f.write(fileInnerData);
		if (writtenBytes != fileSize) {
			f.close();
			LOG(("Update Error: cant write file '%1', desiredSize: %2, write result: %3").arg(tempDirPath + '/' + relativeName).arg(fileSize).arg(writtenBytes));
			return false;
		}
		f.close();
		if (executable) {
			QFileDevice::Permissions p = f.permissions();
			p |= QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther;
			f.setPermissions(p);
		}
	}
	return names.contains(QStringLiteral("telegram.exe")) && names.contains(QStringLiteral("updater.exe"))
        && stream.atEnd();
}

[[nodiscard]] bool WriteUpdateVersionFile(
        QDir &,
        const QString &tempDirPath,
        quint64 fullVersion,
        Updates::Channel channel) {
    const auto marker = VersionInt(kVersionFileFishGramMarker);
    const auto channelValue = VersionInt(channel);
    QSaveFile ready(tempDirPath + u"/ready"_q);
    return ready.open(QIODevice::WriteOnly)
        && ready.write(reinterpret_cast<const char*>(&marker), sizeof(marker)) == sizeof(marker)
        && ready.write(reinterpret_cast<const char*>(&fullVersion), sizeof(fullVersion)) == sizeof(fullVersion)
        && ready.write(reinterpret_cast<const char*>(&channelValue), sizeof(channelValue)) == sizeof(channelValue)
        && ready.commit();
}

[[nodiscard]] bool UnpackUpdateV2(
		const QString &filepath,
		const QByteArray &content) {
	// The expected target follows the feed key, not the build: an x64
	// build under Rosetta asks for armac and must accept that package.
	const auto target = Updates::TargetFromPlatformKey(
		Platform::AutoUpdateKey().toLatin1());
	if (!target) {
		LOG(("Update Error: No v2 target for platform key '%1'."
			).arg(Platform::AutoUpdateKey()));
		return false;
	}

	// The full verification happens before any decompression, so no
	// unauthenticated bytes ever reach the LZMA or QDataStream parsers.
	auto error = QString();
	const auto verified = Updates::VerifyUpdate(
		content,
		BuildUpdateChannel,
		AppBetaVersion || cInstallBetaVersion(),
		*target,
		RunningUpdateVersion(),
		HeldManifest(),
		Updates::RootPublicKeyPem(),
		base::unixtime::now(),
		&error);
	if (!verified) {
		LOG(("Update Error: v2 update rejected: %1").arg(error));
		return false;
	}
	if (verified->adoptManifest) {
		crl::on_main([manifest = verified->manifest] {
			AdoptManifest(manifest);
		});
	}

	const auto tempDirPath = cWorkingDir() + u"tupdates/temp"_q;
	const auto readyFilePath = cWorkingDir() + u"tupdates/temp/ready"_q;
#ifdef Q_OS_WIN
	std::wstring work;
	if (!FishGramUpdates::WindowsTransaction::Details::CanonicalDirectory(
		QDir::toNativeSeparators(cWorkingDir()).toStdWString(), &work)
		|| !FishGramUpdates::WindowsTransaction::Details::EnsureDirectory(
			FishGramUpdates::WindowsTransaction::Details::Join(work, L"tupdates"))) return false;
	const auto staged = FishGramUpdates::WindowsTransaction::Details::Join(work, L"tupdates\\temp");
	if (!FishGramUpdates::WindowsTransaction::Details::IsAbsent(staged)
		&& !FishGramUpdates::WindowsTransaction::Details::IsSafeDirectory(staged)) return false;
#endif
	base::Platform::DeleteDirectory(tempDirPath);

	QDir tempDir(tempDirPath);
	if (tempDir.exists() || QFile(readyFilePath).exists()) {
		LOG(("Update Error: cant clear tupdates/temp dir!"));
		return false;
	}

	const auto &payload = verified->envelope.payload;
	const auto uncompressed = DecompressUpdatePayload(
		payload.constData(),
		payload.size());
	if (!uncompressed) {
		return false;
	}

	tempDir.mkdir(tempDir.absolutePath());

#ifdef Q_OS_WIN
	// Installation re-verifies this exact v2 package under the installed root.
	// Persist the latest held key authorization outside the untrusted workdir.
	namespace Transaction = FishGramUpdates::WindowsTransaction;
	std::wstring install, meta, versions;
	if (!Transaction::Details::CanonicalDirectory(QDir::toNativeSeparators(cExeDir()).toStdWString(), &install)
		|| !Transaction::Details::PrepareMetadata(install, &meta, &versions)) return false;
	Transaction::Details::InstallLock installLock;
	Transaction::Result lockFailure = Transaction::Result::IoError;
	if (!Transaction::Details::AcquireLock(meta, &installLock, &lockFailure)) return false;
	const auto trustPath = QString::fromStdWString(Transaction::Details::Join(meta, L"held-trust"));
	QFile priorTrust(trustPath);
	if (priorTrust.exists()) {
		std::uint64_t trustSize = 0;
		if (!Transaction::Details::FileSize(trustPath.toStdWString(), &trustSize)) return false;
		if (!priorTrust.open(QIODevice::ReadOnly) || priorTrust.size() > 1024 * 1024) return false;
		const auto prior = FishGramUpdates::ReadVerifiedTrustRecord(priorTrust.readAll(), Updates::RootPublicKeyPem());
		if (!prior || prior->version > verified->manifest.version
			|| (prior->version == verified->manifest.version && prior->bytes != verified->manifest.bytes)) return false;
		priorTrust.close();
	}
	QSaveFile trustFile(trustPath);
	const auto trustRecord = FishGramUpdates::EncodeTrustRecord(verified->manifest);
	if (!trustFile.open(QIODevice::WriteOnly) || trustFile.write(trustRecord) != trustRecord.size() || !trustFile.commit()) return false;
	QSaveFile packageFile(cWorkingDir() + u"tupdates/package.v2"_q);
	if (!packageFile.open(QIODevice::WriteOnly) || packageFile.write(content) != content.size() || !packageFile.commit()) return false;
#endif

	{
		QDataStream stream(*uncompressed);
		stream.setVersion(QDataStream::Qt_5_1);

		quint32 version;
		stream >> version;
		if (stream.status() != QDataStream::Ok
			|| version != Updates::UpdateVersionBase(
				verified->envelope.version)) {
			LOG(("Update Error: v2 inner version does not match envelope."));
			return false;
		}

		quint32 filesCount;
		stream >> filesCount;
		if (stream.status() != QDataStream::Ok || !filesCount) {
			LOG(("Update Error: cant read v2 files count."));
			return false;
		}
		if (!ExtractUpdateFiles(stream, filesCount, tempDirPath)
			|| !WriteUpdateVersionFile(
				tempDir,
				tempDirPath,
				verified->envelope.version,
                verified->envelope.channel)) {
			return false;
		}
	}

	QFile(filepath).remove();

	return true;
}

#endif // !TDESKTOP_DISABLE_AUTOUPDATE

bool UnpackUpdate(const QString &filepath, const FishGramUpdates::Candidate &candidate) {
#ifndef TDESKTOP_DISABLE_AUTOUPDATE
    QFile input(filepath);
    if (filepath.isEmpty() || !input.open(QIODevice::ReadOnly)
        || input.size() != qint64(candidate.size) || input.size() > Loader::kMaxFileSize) {
        LOG(("FishGram Update Error: package size or download file is invalid."));
        return false;
    }
    const auto content = input.readAll();
    input.close();
    if (!FishGramUpdates::MatchesDownload(candidate, content) || !Updates::IsV2UpdateFile(content)) {
        LOG(("FishGram Update Error: package hash or v2 format is invalid."));
        return false;
    }
    const auto envelope = Updates::ParseEnvelope(content);
    if (!envelope || envelope->version != candidate.version
        || Updates::ChannelName(envelope->channel) != candidate.channel.toLatin1()) {
        LOG(("FishGram Update Error: signed channel or full version does not match discovery."));
        return false;
    }
    return UnpackUpdateV2(filepath, content);
#else
    return false;
#endif
}

Checker::Checker(bool testing) : _testing(testing) {
}

rpl::producer<std::shared_ptr<Loader>> Checker::ready() const {
	return _ready.events();
}

rpl::producer<> Checker::failed() const {
	return _failed.events();
}

bool Checker::poll() const {
	return true;
}

bool Checker::testing() const {
	return _testing;
}

void Checker::done(std::shared_ptr<Loader> result) {
	_ready.fire(std::move(result));
}

void Checker::fail() {
	_failed.fire({});
}

rpl::lifetime &Checker::lifetime() {
	return _lifetime;
}

HttpChecker::HttpChecker(bool testing) : Checker(testing) {
}

void HttpChecker::start() {
    request(Request::Manifest);
}

void HttpChecker::request(Request step) {
    _request = step;
    const auto suffix = (step == Request::Manifest)
        ? QStringLiteral("keys/manifest.min.json")
        : (step == Request::ManifestSignature)
        ? QStringLiteral("keys/manifest.sig")
        : QStringLiteral("channels/%1/windows-x64.json").arg(
            (BuildUpdateChannel == Updates::Channel::Beta || cInstallBetaVersion()) ? "beta" : "stable");
    const auto url = QUrl(QStringLiteral("https://lonefisher.github.io/fishgram/") + suffix);
    auto networkRequest = QNetworkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    _manager = std::make_unique<QNetworkAccessManager>();
    _reply = _manager->get(networkRequest);
    _reply->connect(_reply, &QNetworkReply::finished, [=] { gotResponse(); });
    _reply->connect(_reply, &QNetworkReply::errorOccurred, [=](auto e) { gotFailure(e); });
}

void HttpChecker::gotResponse() {
    if (!_reply) return;
    if (_reply->error() != QNetworkReply::NoError
        || _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        gotFailure(QNetworkReply::UnknownContentError);
        return;
    }
    const auto response = _reply->readAll();
    clearSentRequest();
    if (response.size() >= kMaxResponseSize) {
        fail();
        return;
    }
    if (_request == Request::Manifest) {
        _manifestBytes = response;
        request(Request::ManifestSignature);
    } else if (_request == Request::ManifestSignature) {
        const auto manifest = Updates::ParseVerifiedManifest(
            _manifestBytes, response, Updates::RootPublicKeyPem());
        const auto held = HeldManifest();
        if (!manifest || (held && (manifest->version < held->version
            || (manifest->version == held->version && manifest->bytes != held->bytes)))) {
            LOG(("FishGram Update Error: key manifest verification or monotonic version failed."));
            fail();
            return;
        }
        AdoptManifest(*manifest);
        request(Request::Feed);
    } else if (!handleResponse(response)) {
        fail();
    }
}

bool HttpChecker::handleResponse(const QByteArray &response) {
    auto error = QString();
    const auto channel = (BuildUpdateChannel == Updates::Channel::Beta || cInstallBetaVersion())
        ? QStringLiteral("beta") : QStringLiteral("stable");
    const auto feed = FishGramUpdates::ParseFeed(response, channel, RunningUpdateVersion(), &error);
    if (!feed) {
        LOG(("FishGram Update Error: %1").arg(error));
        return false;
    }
    done(feed->candidate ? std::make_shared<HttpLoader>(*feed->candidate) : nullptr);
    return true;
}

void HttpChecker::clearSentRequest() {
	const auto reply = base::take(_reply);
	if (!reply) {
		return;
	}
	reply->disconnect(reply, &QNetworkReply::finished, nullptr, nullptr);
	reply->disconnect(reply, &QNetworkReply::errorOccurred, nullptr, nullptr);
	reply->abort();
	reply->deleteLater();
	_manager = nullptr;
}

HttpChecker::~HttpChecker() {
	clearSentRequest();
}

void HttpChecker::gotFailure(QNetworkReply::NetworkError e) {
	LOG(("Update Error: "
		"could not get current version %1").arg(e));
	if (const auto reply = base::take(_reply)) {
		reply->deleteLater();
	}

	fail();
}

HttpLoader::HttpLoader(FishGramUpdates::Candidate candidate)
: Loader(UpdatesFolder() + '/' + ExtractFilename(candidate.url), kChunkSize)
, _url(candidate.url)
, _candidate(std::move(candidate)) {
}

void HttpLoader::startLoading() {
	LOG(("Update Info: Loading using HTTP from '%1'.").arg(_url));

	_thread = std::make_unique<QThread>();
	_actor = new HttpLoaderActor(this, _thread.get(), _url);
	_thread->start();
}

HttpLoader::~HttpLoader() {
	if (const auto thread = base::take(_thread)) {
		if (const auto actor = base::take(_actor)) {
			QObject::connect(
				thread.get(),
				&QThread::finished,
				actor,
				&QObject::deleteLater);
		}
		thread->quit();
		thread->wait();
	}
}

HttpLoaderActor::HttpLoaderActor(
		not_null<HttpLoader*> parent,
		not_null<QThread*> thread,
		const QString &url)
: _parent(parent) {
	_url = url;
	moveToThread(thread);
	_manager.moveToThread(thread);

	connect(thread, &QThread::started, this, [=] { start(); });
}

void HttpLoaderActor::start() {
	sendRequest();
}

void HttpLoaderActor::sendRequest() {
	auto request = QNetworkRequest(_url);
	const auto rangeHeaderValue = "bytes="
		+ QByteArray::number(_parent->alreadySize())
		+ "-";
	request.setRawHeader("Range", rangeHeaderValue);
	request.setAttribute(
		QNetworkRequest::HttpPipeliningAllowedAttribute,
		true);
	_reply.reset(_manager.get(request));
	connect(
		_reply.get(),
		&QNetworkReply::downloadProgress,
		this,
		&HttpLoaderActor::partFinished);
	connect(
		_reply.get(),
		&QNetworkReply::errorOccurred,
		this,
		&HttpLoaderActor::partFailed);
	connect(
		_reply.get(),
		&QNetworkReply::metaDataChanged,
		this,
		&HttpLoaderActor::gotMetaData);
}

void HttpLoaderActor::gotMetaData() {
	const auto pairs = _reply->rawHeaderPairs();
	for (const auto &pair : pairs) {
		if (QString::fromUtf8(pair.first).toLower() == "content-range") {
			const auto m = QRegularExpression(u"/(\\d+)([^\\d]|$)"_q).match(QString::fromUtf8(pair.second));
			if (m.hasMatch()) {
				_parent->writeChunk({}, m.captured(1).toLongLong());
			}
		}
	}
}

void HttpLoaderActor::partFinished(qint64 got, qint64 total) {
	if (!_reply) return;

	const auto statusCode = _reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute);
	if (statusCode.isValid()) {
		const auto status = statusCode.toInt();
		if (status != 200 && status != 206 && status != 416) {
			LOG(("Update Error: "
				"Bad HTTP status received in partFinished(): %1"
				).arg(status));
			_parent->threadSafeFailed();
			return;
		}
	}

	DEBUG_LOG(("Update Info: part %1 of %2").arg(got).arg(total));

	const auto data = _reply->readAll();
	_parent->writeChunk(bytes::make_span(data), total);
}

void HttpLoaderActor::partFailed(QNetworkReply::NetworkError e) {
	if (!_reply) return;

	const auto statusCode = _reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute);
	_reply.release()->deleteLater();
	if (statusCode.isValid()) {
		const auto status = statusCode.toInt();
		if (status == 416) { // Requested range not satisfiable
			_parent->writeChunk({}, _parent->alreadySize());
			return;
		}
	}
	LOG(("Update Error: failed to download part after %1, error %2"
		).arg(_parent->alreadySize()
		).arg(e));
	_parent->threadSafeFailed();
}

#if !defined Q_OS_WIN && !defined Q_OS_MAC
FlatpakChecker::FlatpakChecker(bool testing)
: Checker(testing)
, _watcher({u"/app"_q}) {
	FlatpakPortal::FlatpakProxy::new_for_bus(
			Gio::BusType::SESSION_,
			Gio::DBusProxyFlags::NONE_,
			kFlatpakPortalService,
			kFlatpakPortalObjectPath,
			crl::guard(this, [=](GObject::Object, Gio::AsyncResult res) {
		auto result = FlatpakPortal::FlatpakProxy::new_for_bus_finish(res);
		if (!result) {
			Gio::DBusErrorNS_::strip_remote_error(result.error());
			LOG(("Update Error: %1").arg(result.error().message_().c_str()));
			return;
		}

		_interface = *result;
		_interface.call_create_update_monitor(
				GLib::Variant::new_array(
					GLib::VariantType::new_("{sv}"),
					{}),
				[=](GObject::Object, Gio::AsyncResult res) {
			const auto result = _interface.call_create_update_monitor_finish(
				res);

			if (!result) {
				Gio::DBusErrorNS_::strip_remote_error(result.error());
				LOG(("Update Error: %1").arg(
					result.error().message_().c_str()));
				fail();
				return;
			}

			FlatpakPortal::FlatpakUpdateMonitorProxy::new_for_bus(
					Gio::BusType::SESSION_,
					Gio::DBusProxyFlags::NONE_,
					kFlatpakPortalService,
					std::get<1>(*result),
					crl::guard(this, [=](GObject::Object, Gio::AsyncResult res) {
				using FlatpakPortal::FlatpakUpdateMonitorProxy;
				auto result = FlatpakUpdateMonitorProxy::new_for_bus_finish(
					res);

				if (!result) {
					Gio::DBusErrorNS_::strip_remote_error(result.error());
					LOG(("Update Error: %1").arg(
						result.error().message_().c_str()));
					fail();
					return;
				}

				_monitor = *result;
				_updateAvailableSignal
					= _monitor.signal_update_available().connect([=](
							FlatpakPortal::FlatpakUpdateMonitor,
							GLib::Variant updateInfo) {
						done(std::make_shared<FlatpakLoader>(_monitor));
					});
			}));
		});
	}));

	QObject::connect(
		&_watcher,
		&QFileSystemWatcher::directoryChanged,
		[=](const QString &path) {
			start();
		});
}

void FlatpakChecker::start() {
	if (QFileInfo::exists(kFlatpakUpdated.utf16())) {
		done(std::make_shared<FlatpakLoader>(_monitor));
	}
}

bool FlatpakChecker::poll() const {
	return false;
}

FlatpakChecker::~FlatpakChecker() {
	if (_monitor) {
		_monitor.disconnect(_updateAvailableSignal);
		_monitor.call_close(nullptr);
	}
}

FlatpakLoader::FlatpakLoader(FlatpakPortal::FlatpakUpdateMonitor monitor)
: Loader({}, kChunkSize)
, _monitor(monitor) {
	if (!_monitor) {
		return;
	}

	_progressSignal = _monitor.signal_progress().connect([=](
			FlatpakPortal::FlatpakUpdateMonitor,
			GLib::Variant info) {
		auto dict = GLib::VariantDict::new_(info);
		switch (dict.lookup_value("status").get_uint32()) {
		case 0: {
			const auto n_ops = dict.lookup_value("n_ops").get_uint32();
			const auto op = dict.lookup_value("op").get_uint32();
			const auto progress = dict.lookup_value("progress").get_uint32();
			threadSafeProgress({
				int64(
					std::round((op + (progress / 100.)) / n_ops * 104857600)),
				104857600,
				true,
			});
		} break;
		case 1:
		case 2: threadSafeReady(); break;
		case 3: {
			LOG(("Update Error: %1").arg(
				dict.lookup_value("error_message").get_string(
					nullptr).c_str()));
			threadSafeFailed();
		} break;
		}
	});
}

void FlatpakLoader::startLoading() {
	if (QFileInfo::exists(kFlatpakUpdated.utf16())) {
		threadSafeReady();
	}

	if (!_monitor) {
		return;
	}

	_monitor.call_update(
		base::Platform::XDP::ParentWindowID(),
		GLib::Variant::new_array(
			GLib::VariantType::new_("{sv}"),
			{}),
		crl::guard(this, [=](GObject::Object, Gio::AsyncResult res) {
			const auto result = _monitor.call_close_finish(res);
			if (!result) {
				Gio::DBusErrorNS_::strip_remote_error(result.error());
				LOG(("Update Error: %1").arg(
					result.error().message_().c_str()));
				threadSafeFailed();
			}
		}));
}

FlatpakLoader::~FlatpakLoader() {
	if (_monitor) {
		_monitor.disconnect(_progressSignal);
	}
}
#endif // !Q_OS_WIN && !Q_OS_MAC

} // namespace

bool UpdaterDisabled() {
	return UpdaterIsDisabled;
}

void SetUpdaterDisabledAtStartup() {
	Expects(UpdaterInstance.lock() == nullptr);

	UpdaterIsDisabled = true;
}

class Updater : public base::has_weak_ptr {
public:
	Updater();

	rpl::producer<> checking() const;
	rpl::producer<> isLatest() const;
	rpl::producer<Progress> progress() const;
	rpl::producer<> failed() const;
	rpl::producer<> ready() const;

	void start(bool forceWait);
	void stop();
	void test();

	State state() const;
	int already() const;
	int size() const;
	bool percent() const;

	void setMtproto(base::weak_ptr<Main::Session> session);

	~Updater();

private:
	enum class Action {
		Waiting,
		Checking,
		Loading,
		Unpacking,
		Ready,
	};
	void check();
	void startImplementation(
		not_null<Implementation*> which,
		std::unique_ptr<Checker> checker);
	bool tryLoaders();
	void handleTimeout();
	void checkerDone(
		not_null<Implementation*> which,
		std::shared_ptr<Loader> loader);
	void checkerFail(not_null<Implementation*> which);

	void finalize(QString filepath);
	void unpackDone(bool ready);
	void handleChecking();
	void handleProgress();
	void handleLatest();
	void handleFailed();
	void handleReady();
	void scheduleNext();

	bool _testing = false;
	Action _action = Action::Waiting;
	base::Timer _timer;
	base::Timer _retryTimer;
	rpl::event_stream<> _checking;
	rpl::event_stream<> _isLatest;
	rpl::event_stream<Progress> _progress;
	rpl::event_stream<> _failed;
	rpl::event_stream<> _ready;
	Implementation _httpImplementation;
	Implementation _mtpImplementation;
	Implementation _flatpakImplementation;
	std::shared_ptr<Loader> _activeLoader;
	bool _usingMtprotoLoader = (cAlphaVersion() != 0);
	base::weak_ptr<Main::Session> _session;

	rpl::lifetime _lifetime;

};

Updater::Updater()
: _timer([=] { check(); })
, _retryTimer([=] { handleTimeout(); }) {
	checking() | rpl::on_next([=] {
		handleChecking();
	}, _lifetime);
	progress() | rpl::on_next([=] {
		handleProgress();
	}, _lifetime);
	failed() | rpl::on_next([=] {
		handleFailed();
	}, _lifetime);
	ready() | rpl::on_next([=] {
		handleReady();
	}, _lifetime);
	isLatest() | rpl::on_next([=] {
		handleLatest();
	}, _lifetime);
}

rpl::producer<> Updater::checking() const {
	return _checking.events();
}

rpl::producer<> Updater::isLatest() const {
	return _isLatest.events();
}

auto Updater::progress() const
-> rpl::producer<Progress> {
	return _progress.events();
}

rpl::producer<> Updater::failed() const {
	return _failed.events();
}

rpl::producer<> Updater::ready() const {
	return _ready.events();
}

void Updater::check() {
	start(false);
}

void Updater::handleReady() {
	stop();
	_action = Action::Ready;
	if (!Quitting()) {
		cSetLastUpdateCheck(base::unixtime::now());
		Local::writeSettings();
	}
}

void Updater::handleFailed() {
	scheduleNext();
}

void Updater::handleLatest() {
	if (const auto update = FindUpdateFile(); !update.isEmpty()) {
		QFile(update).remove();
	}
	scheduleNext();
}

void Updater::handleChecking() {
	_action = Action::Checking;
	_retryTimer.callOnce(kUpdaterTimeout);
}

void Updater::handleProgress() {
	_retryTimer.callOnce(kUpdaterTimeout);
}

void Updater::scheduleNext() {
	stop();
	if (!Quitting()) {
		cSetLastUpdateCheck(base::unixtime::now());
		Local::writeSettings();
		start(true);
	}
}

auto Updater::state() const -> State {
	if (_action == Action::Ready) {
		return State::Ready;
	} else if (_action == Action::Loading) {
		return State::Download;
	}
	return State::None;
}

int Updater::size() const {
	return _activeLoader ? _activeLoader->totalSize() : 0;
}

int Updater::already() const {
	return _activeLoader ? _activeLoader->alreadySize() : 0;
}

bool Updater::percent() const {
	return _activeLoader ? _activeLoader->preferPercent() : 0;
}

void Updater::stop() {
	_httpImplementation = Implementation();
	_mtpImplementation = Implementation();
	_flatpakImplementation = Implementation{
		std::move(_flatpakImplementation.checker)
	};
	_activeLoader = nullptr;
	_action = Action::Waiting;
}

void Updater::start(bool forceWait) {
	if (cExeName().isEmpty()) {
		return;
	}

	_timer.cancel();
	if (!cAutoUpdate() || _action != Action::Waiting) {
		return;
	}

	_retryTimer.cancel();
	const auto constDelay = cAlphaVersion() ? 600 : UpdateDelayConstPart;
	const auto randDelay = cAlphaVersion() ? 300 : UpdateDelayRandPart;
	const auto updateInSecs = cLastUpdateCheck()
		+ constDelay
		+ int(rand() % randDelay)
		- base::unixtime::now();
	auto sendRequest = (updateInSecs <= 0)
		|| (updateInSecs > constDelay + randDelay);
	if (!sendRequest && !forceWait) {
		if (!FindUpdateFile().isEmpty()) {
			sendRequest = true;
		}
	}
	if (cManyInstance() && !Logs::DebugEnabled()) {
		// Only main instance is updating.
		return;
	}

	if (KSandbox::isFlatpak()) {
#if !defined Q_OS_WIN && !defined Q_OS_MAC
		if (!_flatpakImplementation.checker) {
			startImplementation(
				&_flatpakImplementation,
				std::make_unique<FlatpakChecker>(_testing));
		}
#endif // !Q_OS_WIN && !Q_OS_MAC
	} else if (sendRequest) {
        startImplementation(&_httpImplementation, std::make_unique<HttpChecker>(_testing));
        _mtpImplementation = Implementation{};
        _mtpImplementation.failed = true;

		_checking.fire({});
	} else {
		_timer.callOnce((updateInSecs + 5) * crl::time(1000));
	}
}

void Updater::startImplementation(
		not_null<Implementation*> which,
		std::unique_ptr<Checker> checker) {
	if (!checker) {
		class EmptyChecker : public Checker {
		public:
			EmptyChecker() : Checker(false) {
			}

			void start() override {
				crl::on_main(this, [=] { fail(); });
			}

		};
		checker = std::make_unique<EmptyChecker>();
	}

	checker->ready(
	) | rpl::on_next([=](std::shared_ptr<Loader> &&loader) {
		checkerDone(which, std::move(loader));
	}, checker->lifetime());
	checker->failed(
	) | rpl::on_next([=] {
		checkerFail(which);
	}, checker->lifetime());

	*which = Implementation{ std::move(checker) };

	crl::on_main(which->checker.get(), [=] {
		which->checker->start();
	});
}

void Updater::checkerDone(
		not_null<Implementation*> which,
		std::shared_ptr<Loader> loader) {
	if (which->checker->poll()) which->checker = nullptr;
	which->loader = std::move(loader);

	tryLoaders();
}

void Updater::checkerFail(not_null<Implementation*> which) {
	which->checker = nullptr;
	which->failed = true;

	tryLoaders();
}

void Updater::test() {
	_testing = true;
	cSetLastUpdateCheck(0);
	start(false);
}

void Updater::setMtproto(base::weak_ptr<Main::Session> session) {
	_session = session;
}

void Updater::handleTimeout() {
	if (_action == Action::Checking) {
		const auto reset = [&](Implementation &which) {
			if (base::take(which.checker)) {
				which.failed = true;
			}
		};
		reset(_httpImplementation);
		reset(_mtpImplementation);
		if (!tryLoaders()) {
			cSetLastUpdateCheck(0);
			_timer.callOnce(kUpdaterTimeout);
		}
	} else if (_action == Action::Loading) {
		_failed.fire({});
	}
}

bool Updater::tryLoaders() {
	if (_httpImplementation.checker || _mtpImplementation.checker) {
		// Some checkers didn't finish yet.
		return true;
	}
	_retryTimer.cancel();

	const auto tryOne = [&](Implementation &which) {
		_activeLoader = std::move(which.loader);
		if (const auto loader = _activeLoader.get()) {
			_action = Action::Loading;

			loader->progress(
			) | rpl::start_to_stream(_progress, loader->lifetime());
			loader->ready(
			) | rpl::on_next([=](QString &&filepath) {
				finalize(std::move(filepath));
			}, loader->lifetime());
			loader->failed(
			) | rpl::on_next([=] {
				_failed.fire({});
			}, loader->lifetime());

			_retryTimer.callOnce(kUpdaterTimeout);
			loader->wipeFolder();
			loader->start();
		} else {
			_isLatest.fire({});
		}
	};
	if (KSandbox::isFlatpak()) {
		if (_flatpakImplementation.failed) {
			_failed.fire({});
			return false;
		} else {
			tryOne(_flatpakImplementation);
		}
	} else if (_mtpImplementation.failed && _httpImplementation.failed) {
		_failed.fire({});
		return false;
	} else if (!_mtpImplementation.loader) {
		tryOne(_httpImplementation);
	} else if (!_httpImplementation.loader) {
		tryOne(_mtpImplementation);
	} else {
		tryOne(_usingMtprotoLoader
			? _mtpImplementation
			: _httpImplementation);
		_usingMtprotoLoader = !_usingMtprotoLoader;
	}
	return true;
}

void Updater::finalize(QString filepath) {
	if (_action != Action::Loading) {
		return;
	}
	_retryTimer.cancel();
    const auto candidate = static_cast<HttpLoader*>(_activeLoader.get())->candidate();
	_activeLoader = nullptr;
	_action = Action::Unpacking;
	crl::async([=] {
		const auto ready = UnpackUpdate(filepath, candidate);
		crl::on_main([=] {
			GetUpdaterInstance()->unpackDone(ready);
		});
	});
}

void Updater::unpackDone(bool ready) {
	if (ready) {
		_ready.fire({});
	} else {
		ClearAll();
		_failed.fire({});
	}
}

Updater::~Updater() {
	stop();
}

UpdateChecker::UpdateChecker()
: _updater(GetUpdaterInstance()) {
	if (IsAppLaunched() && Core::App().domain().started()) {
		if (const auto session = Core::App().activeAccount().maybeSession()) {
			_updater->setMtproto(session);
		}
	}
}

rpl::producer<> UpdateChecker::checking() const {
	return _updater->checking();
}

rpl::producer<> UpdateChecker::isLatest() const {
	return _updater->isLatest();
}

auto UpdateChecker::progress() const
-> rpl::producer<Progress> {
	return _updater->progress();
}

rpl::producer<> UpdateChecker::failed() const {
	return _updater->failed();
}

rpl::producer<> UpdateChecker::ready() const {
	return _updater->ready();
}

void UpdateChecker::start(bool forceWait) {
	_updater->start(forceWait);
}

void UpdateChecker::test() {
	_updater->test();
}

void UpdateChecker::setMtproto(base::weak_ptr<Main::Session> session) {
	_updater->setMtproto(session);
}

void UpdateChecker::stop() {
	_updater->stop();
}

auto UpdateChecker::state() const
-> State {
	return _updater->state();
}

int UpdateChecker::already() const {
	return _updater->already();
}

int UpdateChecker::size() const {
	return _updater->size();
}

bool UpdateChecker::percent() const {
	return _updater->percent();
}

//QString winapiErrorWrap() {
//	WCHAR errMsg[2048];
//	DWORD errorCode = GetLastError();
//	LPTSTR errorText = NULL, errorTextDefault = L"(Unknown error)";
//	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&errorText, 0, 0);
//	if (!errorText) {
//		errorText = errorTextDefault;
//	}
//	StringCbPrintf(errMsg, sizeof(errMsg), L"Error code: %d, error message: %s", errorCode, errorText);
//	if (errorText != errorTextDefault) {
//		LocalFree(errorText);
//	}
//	return QString::fromWCharArray(errMsg);
//}

bool checkReadyUpdate() {
	QString readyFilePath = cWorkingDir() + u"tupdates/temp/ready"_q, readyPath = cWorkingDir() + u"tupdates/temp"_q;
	if (!QFile(readyFilePath).exists() || cExeName().isEmpty()) {
		if (QDir(cWorkingDir() + u"tupdates/ready"_q).exists() || QDir(cWorkingDir() + u"tupdates/temp"_q).exists()) {
			ClearAll();
		}
		return false;
	}

	// check ready version
	const auto versionPath = readyFilePath;
	{
		QFile fVersion(versionPath);
		if (!fVersion.open(QIODevice::ReadOnly)) {
			LOG(("Update Error: cant read version file '%1'").arg(versionPath));
			ClearAll();
			return false;
		}
		auto versionNum = VersionInt();
		if (fVersion.read((char*)&versionNum, sizeof(VersionInt)) != sizeof(VersionInt)) {
			LOG(("Update Error: cant read version from file '%1'").arg(versionPath));
			ClearAll();
			return false;
		}
        quint64 fullVersion = 0;
        VersionInt channelValue = 0;
        if (versionNum != kVersionFileFishGramMarker
            || fVersion.read(reinterpret_cast<char*>(&fullVersion), sizeof(fullVersion)) != sizeof(fullVersion)
            || fVersion.read(reinterpret_cast<char*>(&channelValue), sizeof(channelValue)) != sizeof(channelValue)
            || !fVersion.atEnd() || channelValue > 1 || fullVersion <= RunningUpdateVersion()
            || !Updates::ChannelPolicyAllows(BuildUpdateChannel, cInstallBetaVersion(), Updates::Channel(channelValue), fullVersion, RunningUpdateVersion())) {
            ClearAll();
            return false;
        }
		fVersion.close();
	}

#ifdef Q_OS_WIN
	QString curUpdater = (cExeDir() + u"Updater.exe"_q);
	QFileInfo updater(cWorkingDir() + u"tupdates/temp/Updater.exe"_q);
#elif defined Q_OS_MAC // Q_OS_WIN
	QString curUpdater = (cExeDir() + cExeName() + u"/Contents/Frameworks/Updater"_q);
	QFileInfo updater(cWorkingDir() + u"tupdates/temp/Telegram.app/Contents/Frameworks/Updater"_q);
#else // Q_OS_MAC
	QString curUpdater = (cExeDir() + u"Updater"_q);
	QFileInfo updater(cWorkingDir() + u"tupdates/temp/Updater"_q);
#endif // else for Q_OS_WIN || Q_OS_MAC
	if (!updater.exists()) {
		QFileInfo current(curUpdater);
		if (!current.exists()) {
			ClearAll();
			return false;
		}
		if (!QFile(current.absoluteFilePath()).copy(updater.absoluteFilePath())) {
			ClearAll();
			return false;
		}
	}
#ifdef Q_OS_WIN
    // Only check payload presence here. The launcher runs a copy of the
    // installed Updater, which re-verifies the retained package before writing.
    if (!updater.isFile() || updater.isSymLink()) return false;
#elif defined Q_OS_MAC // Q_OS_WIN
	QDir().mkpath(QFileInfo(curUpdater).absolutePath());
	DEBUG_LOG(("Update Info: moving %1 to %2...").arg(updater.absoluteFilePath()).arg(curUpdater));
	if (!objc_moveFile(updater.absoluteFilePath(), curUpdater)) {
		ClearAll();
		return false;
	}
#else // Q_OS_MAC
	// if the files in the directory are owned by user, while the directory is not,
	// update will still fail since it's not possible to remove files
	if (QFile::exists(curUpdater)
		&& unlink(QFile::encodeName(curUpdater).constData())) {
		if (errno == EACCES) {
			DEBUG_LOG(("Update Info: "
				"could not unlink current Updater, access denied."));
			cSetWriteProtected(true);
			return true;
		} else {
			DEBUG_LOG(("Update Error: could not unlink current Updater."));
			ClearAll();
			return false;
		}
	}
	if (!linuxMoveFile(QFile::encodeName(updater.absoluteFilePath()).constData(), QFile::encodeName(curUpdater).constData())) {
		if (errno == EACCES) {
			DEBUG_LOG(("Update Info: "
				"could not copy new Updater, access denied."));
			cSetWriteProtected(true);
			return true;
		} else {
			DEBUG_LOG(("Update Error: could not copy new Updater."));
			ClearAll();
			return false;
		}
	}
#endif // else for Q_OS_WIN || Q_OS_MAC

#ifdef Q_OS_MAC
	base::Platform::RemoveQuarantine(QFileInfo(curUpdater).absolutePath());
	base::Platform::RemoveQuarantine(updater.absolutePath());
#endif // Q_OS_MAC

	return true;
}

void UpdateApplication() {
	if (UpdaterDisabled()) {
		const auto url = [&] {
#ifdef OS_WIN_STORE
			return "https://www.microsoft.com/en-us/store/p/telegram-desktop/9nztwsqntd0s";
#elif defined OS_MAC_STORE // OS_WIN_STORE
			return "https://itunes.apple.com/ae/app/telegram-desktop/id946399090";
#else // OS_WIN_STORE || OS_MAC_STORE
			if (KSandbox::isFlatpak()) {
				return "https://flathub.org/apps/details/org.telegram.desktop";
			} else if (KSandbox::isSnap()) {
				return "https://snapcraft.io/telegram-desktop";
			}
			return "https://desktop.telegram.org";
#endif // OS_WIN_STORE || OS_MAC_STORE
		}();
		UrlClickHandler::Open(url);
	} else {
		cSetAutoUpdate(true);
		const auto window = Core::IsAppLaunched()
			? Core::App().activePrimaryWindow()
			: nullptr;
		if (window) {
			if (const auto controller = window->sessionController()) {
				controller->showSection(
					std::make_shared<Info::Memento>(
						Info::Settings::Tag{ controller->session().user() },
						::Settings::AdvancedId()),
					Window::SectionShow());
			} else {
				window->widget()->showSpecialLayer(
					Box<::Settings::LayerWidget>(window),
					anim::type::normal);
			}
			window->widget()->showFromTray();
		}
		cSetLastUpdateCheck(0);
		Core::UpdateChecker().start();
	}
}

QString countAlphaVersionSignature(uint64) {
	return {};
}

} // namespace Core
