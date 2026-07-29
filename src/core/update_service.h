// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace TeleMatrix {
namespace Core {

struct UpdateCallbackGuard;

/// App-global auto-update client: checks GitHub Releases for a newer version and
/// downloads the platform asset, which Rust verifies (sha256 + minisign, with the
/// version bound into the signed trusted comment) before this ever sees a path.
///
/// Deliberately not per-account — the updater must work before login, and with up
/// to six accounts there is no canonical `Handle` to hang it off. It therefore
/// talks to the handle-free `tm_update_*` FFI.
class UpdateService final : public QObject {
    Q_OBJECT

public:
    /// Persisted 0..2; the order is the order shown in Settings.
    enum class Policy {
        Off = 0,           // never check automatically
        CheckAndNotify = 1, // auto-check, user starts the download
        AutoDownload = 2,   // auto-check + auto-download, user applies
    };

    /// What the app can do with an available update on this install.
    enum class ApplyMode {
        OneClick,   // download + "Update & Restart"
        NotifyOnly, // no self-update path — open the release page instead
    };

    explicit UpdateService(QObject *parent = nullptr);
    ~UpdateService() override;

    /// Manifest key for this build (`macos-universal`, `windows-x86_64`,
    /// `linux-appimage-x86_64`, ...). Empty when the platform has no updater
    /// asset at all, which forces notify-only.
    [[nodiscard]] static QString platformKey();

    /// Whether a verified one-click update is possible here. Answers false for
    /// deb/rpm, an unwritable install location, a translocated or DMG-mounted
    /// bundle, and builds with no signing key compiled in.
    [[nodiscard]] ApplyMode applyMode() const;
    /// Human-readable reason `applyMode()` is NotifyOnly; empty when one-click.
    [[nodiscard]] QString notifyOnlyReason() const;

    [[nodiscard]] bool checking() const { return _checking; }
    [[nodiscard]] bool downloading() const { return _downloading; }
    /// Set once a check found something newer; survives until the next check.
    [[nodiscard]] QString availableVersion() const { return _availableVersion; }
    [[nodiscard]] QString releasePage() const { return _releasePage; }
    /// Verified local file, set on updateReady. Empty until then.
    [[nodiscard]] QString readyPath() const { return _readyPath; }

    /// Follow the beta channel (pre-releases included) instead of stable. Read
    /// at the start of each check, so toggling it takes effect on the next one
    /// rather than disturbing a check already in flight.
    [[nodiscard]] bool betaChannel() const { return _betaChannel; }
    void setBetaChannel(bool beta) { _betaChannel = beta; }

    /// Start a manifest check. Silent (log-only) failures when `userInitiated` is
    /// false, so the 4-hourly poll never interrupts anyone.
    void check(bool userInitiated);
    /// Download the asset the last check found. No-op unless one is available.
    void download();
    void cancelDownload();

    /// Stage the downloaded update. On success the caller must quit promptly —
    /// on macOS/Windows a helper is already waiting on this PID. Returns false
    /// if nothing is ready or the platform step failed.
    bool applyAndRestart();

    /// After a successful applyAndRestart(): the binary the caller should
    /// relaunch itself (AppImage). Empty means a helper owns the relaunch and
    /// the caller only has to quit.
    [[nodiscard]] QString pendingRelaunchPath() const { return _pendingRelaunchPath; }

    // Called by the FFI trampolines once marshalled onto the main thread. Public
    // only because those trampolines are free functions; not part of the API.
    void onCheckResult(
        uint status,
        const QString &version,
        const QString &page,
        const QString &url,
        quint64 size,
        const QString &sha256,
        const QString &minisig,
        const QString &error);
    void onDownloadResult(bool success, const QString &localPath, const QString &error);

Q_SIGNALS:
    void checkStarted();
    /// A check ended without a result to report (an automatic one that failed).
    /// Carries no reason — it exists so the UI can leave the "Checking…" state.
    void checkFinished();
    /// Emitted synchronously from download(), so the UI reflects "downloading"
    /// before the first byte arrives rather than after the first progress tick.
    void downloadStarted();
    void updateAvailable(const QString &version);
    void updateUpToDate();
    void updateProgress(quint64 received, quint64 total);
    void updateReady(const QString &localPath);
    /// The user cancelled — distinct from updateError so a deliberate action is
    /// never presented as a failure.
    void downloadCancelled();
    /// Only emitted for user-initiated checks and for download failures — an
    /// automatic check that fails stays silent.
    void updateError(const QString &message);

private:
    // Guard shared with in-flight FFI callbacks. Unlike ProtocolBridge there is
    // no tm_destroy to join the updater runtime at shutdown, so each pending
    // callback holds its own shared_ptr copy: the guard outlives this object and
    // a late callback finds a live mutex and a null service. See the .cpp.
    std::shared_ptr<UpdateCallbackGuard> _guard;

    bool _checking = false;
    bool _downloading = false;
    bool _userInitiatedCheck = false;
    bool _betaChannel = false;
    // Set by cancelDownload() so the terminal failure that follows is reported
    // as a cancellation rather than an error.
    bool _cancelRequested = false;

    QString _availableVersion;
    QString _releasePage;
    QString _readyPath;

    // Asset for the available version; empty url means notify-only.
    QString _assetUrl;
    quint64 _assetSize = 0;
    QString _assetSha256;
    QString _assetMinisig;

    QString _pendingRelaunchPath;
};

} // namespace Core
} // namespace TeleMatrix
