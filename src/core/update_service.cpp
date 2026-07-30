// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "core/update_service.h"

#include "core/updater.h"

// cbindgen emits a plain C header with no `extern "C"` guard of its own, so the
// include site supplies it — same as protocol_bridge.cpp.
extern "C" {
#include "protocol_ffi.h"
}

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>
#include <QUrl>
#include <QSysInfo>

#include <mutex>
#include <thread>

namespace TeleMatrix {
namespace Core {

// Synchronisation guard shared with the FFI callback trampolines, mirroring
// ProtocolBridge's BridgeCallbackGuard — with one load-bearing difference.
//
// ProtocolBridge can hold a bare guard pointer in its userdata because
// tm_destroy() joins the tokio runtime, so no trampoline can possibly run after
// its destructor returns. The updater has no such join: its runtime is
// process-global and never shut down, so a check or download in flight can fire
// after this service is gone. Each pending callback therefore owns a *shared_ptr
// copy* of the guard inside its userdata. The guard outlives the service by
// exactly as long as callbacks remain outstanding, and a late one finds a live
// mutex with a null `service` and no-ops.
struct UpdateCallbackGuard {
    std::mutex mutex;
    UpdateService *service = nullptr;
};

namespace {

/// Where `latest.json` lives. Overridable at build time so a staging repo can be
/// pointed at without touching code.
#ifndef TELEMATRIX_UPDATE_MANIFEST_URL
#define TELEMATRIX_UPDATE_MANIFEST_URL \
    "https://github.com/gecka/telematrix/releases/latest/download/latest.json"
#endif

/// The beta channel's manifest. A separate URL is not a nicety: GitHub's
/// `/releases/latest/` excludes pre-releases by definition, so a beta's own
/// `latest.json` is unreachable through the stable URL no matter what the client
/// does. `channel-beta` is a rolling release CI re-points at the newest build of
/// *any* kind, so opting in also keeps delivering stable finals.
#ifndef TELEMATRIX_UPDATE_BETA_MANIFEST_URL
#define TELEMATRIX_UPDATE_BETA_MANIFEST_URL \
    "https://github.com/gecka/telematrix/releases/download/channel-beta/latest.json"
#endif

#ifndef TELEMATRIX_VERSION_STR
#define TELEMATRIX_VERSION_STR "0.0.0"
#endif

/// Userdata for one in-flight FFI call. The terminal callback owns it and is
/// guaranteed by Rust to fire exactly once (see UpdateCompletion in ffi.rs), so
/// deleting it there is safe even though the progress callback shares it.
struct UpdateCallbackData {
    std::shared_ptr<UpdateCallbackGuard> guard;
};

/// Run `fn(service)` only while the service is still alive.
template <typename F>
void withGuardedService(const std::shared_ptr<UpdateCallbackGuard> &guard, F &&fn) {
    if (!guard) {
        return;
    }
    std::lock_guard<std::mutex> lock(guard->mutex);
    if (guard->service) {
        fn(guard->service);
    }
}

[[nodiscard]] QString fromC(const char *value) {
    return value ? QString::fromUtf8(value) : QString();
}

/// Build the release-page URL from the compiled-in repository, ignoring the
/// manifest's own `page` field entirely.
///
/// The manifest is NOT signed — only the assets it points at are — so `page` is
/// fully attacker-controlled by anyone who can serve or tamper with the JSON.
/// And on a notify-only install (which is every install until a signing key is
/// compiled in) opening it is the *entire* update flow, so the signature scheme
/// never gets a say. Following the manifest's link would hand an attacker
/// "open this URL" at the exact moment the user has been told to expect a new
/// version and is primed to click.
///
/// Merely allowlisting the host is not enough: anyone can host a repository on
/// github.com, so `https://github.com/evil/telematrix/releases/tag/v9.9.9`
/// would sail through. Deriving the URL from TELEMATRIX_UPDATE_MANIFEST_URL
/// leaves the manifest with no influence at all — a staging build that
/// overrides that define still links to its own repo, which is what we want.
///
/// `version` is already semver-validated Rust-side (a manifest whose version
/// does not parse never reaches here), and is percent-encoded regardless.
[[nodiscard]] QString releasePageUrl(const QString &version) {
    const QUrl manifest(QStringLiteral(TELEMATRIX_UPDATE_MANIFEST_URL), QUrl::StrictMode);
    // .../<owner>/<repo>/releases/latest/download/latest.json
    const auto segments = manifest.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!manifest.isValid() || manifest.host().isEmpty() || segments.size() < 2) {
        return QString();
    }
    const auto base = QStringLiteral("https://%1/%2/%3/releases")
        .arg(manifest.host(), segments.at(0), segments.at(1));
    if (version.isEmpty()) {
        return base;
    }
    const auto encoded = QString::fromUtf8(
        QUrl::toPercentEncoding(version, QByteArrayLiteral(".-+")));
    return base + QStringLiteral("/tag/v") + encoded;
}

} // namespace

extern "C" {

static void updateCheckTrampoline(
        uint32_t status,
        const char *version,
        const char *page,
        const char *url,
        uint64_t size,
        const char *sha256,
        const char *minisig,
        const char *error,
        void *userdata) {
    auto *data = static_cast<UpdateCallbackData *>(userdata);
    auto guard = std::move(data->guard);
    delete data;

    // Convert everything before touching the lock: these pointers are only
    // borrowed for the duration of this call.
    const auto versionStr = fromC(version);
    const auto pageStr = fromC(page);
    const auto urlStr = fromC(url);
    const auto shaStr = fromC(sha256);
    const auto sigStr = fromC(minisig);
    const auto errorStr = fromC(error);
    const auto sizeValue = static_cast<quint64>(size);
    const auto statusValue = static_cast<uint>(status);

    withGuardedService(guard, [=](UpdateService *service) {
        QMetaObject::invokeMethod(service, [=] {
            service->onCheckResult(
                statusValue, versionStr, pageStr, urlStr, sizeValue, shaStr, sigStr, errorStr);
        }, Qt::QueuedConnection);
    });
}

static void updateProgressTrampoline(
        uint64_t received_bytes,
        uint64_t total_bytes,
        void *userdata) {
    // Non-terminal: must NOT free userdata.
    auto *data = static_cast<UpdateCallbackData *>(userdata);
    const auto guard = data->guard;
    const auto received = static_cast<quint64>(received_bytes);
    const auto total = static_cast<quint64>(total_bytes);
    withGuardedService(guard, [received, total](UpdateService *service) {
        QMetaObject::invokeMethod(service, [service, received, total] {
            Q_EMIT service->updateProgress(received, total);
        }, Qt::QueuedConnection);
    });
}

static void updateDownloadTrampoline(
        bool success,
        const char *local_path,
        const char *error,
        void *userdata) {
    auto *data = static_cast<UpdateCallbackData *>(userdata);
    auto guard = std::move(data->guard);
    delete data;

    const auto pathStr = fromC(local_path);
    const auto errorStr = fromC(error);
    withGuardedService(guard, [success, pathStr, errorStr](UpdateService *service) {
        QMetaObject::invokeMethod(service, [service, success, pathStr, errorStr] {
            service->onDownloadResult(success, pathStr, errorStr);
        }, Qt::QueuedConnection);
    });
}

} // extern "C"

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
    , _guard(std::make_shared<UpdateCallbackGuard>()) {
    _guard->service = this;
}

UpdateService::~UpdateService() {
    // Barrier: block until any trampoline already inside the guard has finished,
    // and make every later one observe a null service. Pending callbacks keep
    // their own shared_ptr, so the guard itself stays alive for them.
    std::lock_guard<std::mutex> lock(_guard->mutex);
    _guard->service = nullptr;
}

QString UpdateService::platformKey() {
#if defined(Q_OS_MACOS)
    // The macOS build is universal (arm64 + x86_64). Releases publish that one
    // asset under BOTH this key and "macos-aarch64", because clients built
    // before the universal switch ask for the latter and would otherwise stop
    // finding an update — silently, degrading to notify-only. See the manifest
    // generator in .github/workflows/release.yml.
    return QStringLiteral("macos-universal");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows-x86_64");
#elif defined(Q_OS_LINUX)
    // deb, rpm and AppImage are the SAME binary, so this one resolves at
    // runtime: only an AppImage run can self-update.
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) {
        return QStringLiteral("linux-appimage-x86_64");
    }
    return QString();
#else
    return QString();
#endif
}

UpdateService::ApplyMode UpdateService::applyMode() const {
    if (!tm_update_signing_configured() || platformKey().isEmpty()
        || !Updater::CanSelfUpdate(nullptr)) {
        return ApplyMode::NotifyOnly;
    }
    return ApplyMode::OneClick;
}

QString UpdateService::notifyOnlyReason() const {
    // Most actionable first: a platform reason is usually something the user can
    // fix ("move it to Applications"), the other two are build facts they can't.
    QString reason;
    if (!Updater::CanSelfUpdate(&reason)) {
        return reason;
    }
    if (platformKey().isEmpty()) {
        return tr("This installation has no automatic update packages.");
    }
    if (!tm_update_signing_configured()) {
        return tr("This build has no update signing key, so updates have to be "
                  "installed manually.");
    }
    return QString();
}

void UpdateService::check(bool userInitiated) {
    if (_checking || _downloading) {
        return;
    }
    _checking = true;
    _userInitiatedCheck = userInitiated;
    Q_EMIT checkStarted();

    const auto current = QStringLiteral(TELEMATRIX_VERSION_STR).toUtf8();
    const auto url = (_betaChannel
        ? QStringLiteral(TELEMATRIX_UPDATE_BETA_MANIFEST_URL)
        : QStringLiteral(TELEMATRIX_UPDATE_MANIFEST_URL)).toUtf8();
    // An unsupported platform still checks — it just reports notify-only. Send a
    // key that cannot match so no asset is ever resolved for it.
    const auto key = platformKey().toUtf8();

    auto *data = new UpdateCallbackData{ _guard };
    tm_update_check(
        current.constData(),
        url.constData(),
        key.constData(),
        updateCheckTrampoline,
        static_cast<void *>(data));
}

void UpdateService::onCheckResult(
        uint status,
        const QString &version,
        const QString &page,
        const QString &url,
        quint64 size,
        const QString &sha256,
        const QString &minisig,
        const QString &error) {
    // `page` is intentionally unused: the manifest is unsigned, so its link is
    // attacker-controlled. The release URL is derived from the compiled-in repo
    // instead — see releasePageUrl(). The field is kept in the manifest (and in
    // this signature) because it is useful to a human reading latest.json.
    Q_UNUSED(page)
    _checking = false;
    const auto userInitiated = _userInitiatedCheck;
    _userInitiatedCheck = false;

    switch (status) {
    case 0:
        _availableVersion.clear();
        _assetUrl.clear();
        Q_EMIT updateUpToDate();
        return;
    case 1:
        // A newer version invalidates anything already downloaded: applying the
        // old payload while the UI advertises the new version would install
        // something the user did not agree to.
        if (_availableVersion != version) {
            _readyPath.clear();
        }
        _availableVersion = version;
        // Deliberately derived, not taken from `page` — see releasePageUrl().
        _releasePage = releasePageUrl(version);
        _assetUrl = url;
        _assetSize = size;
        _assetSha256 = sha256;
        _assetMinisig = minisig;
        Q_EMIT updateAvailable(version);
        return;
    default:
        // Automatic checks stay silent about the *reason* — a flaky network must
        // never produce an error the user has to dismiss. They must not stay
        // silent about the *state*, though: checkStarted() already repainted the
        // row as "Checking…", so without a completion signal an open About page
        // would sit there indefinitely.
        if (userInitiated) {
            Q_EMIT updateError(error.isEmpty()
                ? tr("Could not check for updates.")
                : error);
        } else {
            Q_EMIT checkFinished();
        }
        return;
    }
}

void UpdateService::download() {
    if (_downloading || _availableVersion.isEmpty() || _assetUrl.isEmpty()) {
        return;
    }
    if (applyMode() != ApplyMode::OneClick) {
        return;
    }
    _downloading = true;
    _cancelRequested = false;
    // Synchronous, before the FFI call: otherwise the UI still reads "Download"
    // until the first progress tick, and a second click in that window would hit
    // the cancel branch instead.
    Q_EMIT downloadStarted();

    const auto url = _assetUrl.toUtf8();
    const auto sha = _assetSha256.toUtf8();
    const auto sig = _assetMinisig.toUtf8();
    const auto expected = _availableVersion.toUtf8();
    const auto current = QStringLiteral(TELEMATRIX_VERSION_STR).toUtf8();
    const auto cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toUtf8();

    auto *data = new UpdateCallbackData{ _guard };
    tm_update_download(
        url.constData(),
        _assetSize,
        sha.constData(),
        sig.constData(),
        expected.constData(),
        current.constData(),
        cacheDir.constData(),
        updateProgressTrampoline,
        updateDownloadTrampoline,
        static_cast<void *>(data));
}

void UpdateService::downloadAndApply() {
    // Arm first, then download() — but only keep it armed if download() actually
    // started, or a later auto-download would inherit the flag and self-apply.
    _applyWhenReady = true;
    download();
    if (!_downloading) {
        _applyWhenReady = false;
    }
}

void UpdateService::cancelDownload() {
    if (_downloading) {
        _cancelRequested = true;
        tm_update_cancel();
    }
}

void UpdateService::onDownloadResult(
        bool success,
        const QString &localPath,
        const QString &error) {
    _downloading = false;
    const auto applyNow = _applyWhenReady;
    _applyWhenReady = false;
    if (success) {
        _cancelRequested = false;
        _readyPath = localPath;
        Q_EMIT updateReady(localPath);
        if (applyNow) {
            // Same event-loop turn as updateReady, so the ready bar never gets a
            // frame of its own — the UI goes straight from progress to
            // "Updating…".
            applyAndRestart();
        }
        return;
    }
    _readyPath.clear();
    if (_cancelRequested) {
        // A cancelled download still reports failure across the FFI (that is how
        // the userdata gets freed). Reporting it as an error would show the user
        // their own deliberate action as something that went wrong.
        _cancelRequested = false;
        Q_EMIT downloadCancelled();
        return;
    }
    Q_EMIT updateError(error.isEmpty() ? tr("The update could not be downloaded.") : error);
}

void UpdateService::applyAndRestart() {
    if (_readyPath.isEmpty() || _applying) {
        return;
    }
    _applying = true;
    Q_EMIT applyStarted();

    // Everything the worker touches is copied by value: it must not read a
    // member while the main thread is still free to run.
    const auto readyPath = _readyPath;
    const auto path = _readyPath.toUtf8();
    const auto sha = _assetSha256.toUtf8();
    const auto sig = _assetMinisig.toUtf8();
    const auto expected = _availableVersion.toUtf8();
    const auto current = QStringLiteral(TELEMATRIX_VERSION_STR).toUtf8();
    // Same guard the FFI trampolines use: a shared_ptr copy keeps it alive past
    // this service, so a worker that outlives us (Cmd+Q mid-apply) finds a live
    // mutex and a null service instead of a dangling pointer.
    auto guard = _guard;

    std::thread([guard, readyPath, path, sha, sig, expected, current] {
        // Re-verify before handing the file to the platform. It has been sitting
        // at a predictable path since the download — hours, under auto-download
        // — and on Windows what runs next is an installer launched through a UAC
        // prompt. One streaming pass over a few hundred MB, which is most of why
        // this is off the main thread at all.
        auto ok = false;
        QString relaunchPath;
        QString error;
        char *verifyError = nullptr;
        const auto verified = tm_update_verify_file(
            path.constData(),
            sha.constData(),
            sig.constData(),
            expected.constData(),
            current.constData(),
            &verifyError);
        if (!verified) {
            error = verifyError
                ? QString::fromUtf8(verifyError)
                : tr("The downloaded update failed its final check.");
            if (verifyError) {
                tm_free_string(verifyError);
            }
        } else {
            // Safe off the main thread: Apply only touches QDir/QFileInfo and a
            // QProcess it creates and finishes here.
            const auto result = Updater::Apply(readyPath);
            ok = result.ok;
            relaunchPath = result.relaunchPath;
            if (!ok) {
                error = result.error.isEmpty()
                    ? tr("The update could not be installed.")
                    : result.error;
            }
        }

        withGuardedService(guard, [ok, relaunchPath, error](UpdateService *service) {
            QMetaObject::invokeMethod(service, [service, ok, relaunchPath, error] {
                service->onApplyResult(ok, relaunchPath, error);
            }, Qt::QueuedConnection);
        });
    }).detach();
}

void UpdateService::onApplyResult(
        bool success,
        const QString &relaunchPath,
        const QString &error) {
    _applying = false;
    // Either way the payload is spent: a verified one has been moved into place,
    // and one that failed must never be retried — the user has to be able to
    // start a clean download.
    _readyPath.clear();
    if (!success) {
        Q_EMIT updateError(error.isEmpty()
            ? tr("The update could not be installed.")
            : error);
        return;
    }
    _pendingRelaunchPath = relaunchPath;
    Q_EMIT applyReady(relaunchPath);
}

} // namespace Core
} // namespace TeleMatrix
