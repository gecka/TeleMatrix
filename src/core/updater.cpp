// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "core/updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstdio>  // ::rename
#include <cstring> // strerror
#include <fcntl.h> // ::open, O_RDONLY (directory fsync)
#include <unistd.h> // ::fsync, ::close
#endif

namespace TeleMatrix {
namespace Core {
namespace Updater {
namespace {

// Each helper is scoped to the platform that uses it — otherwise the two it
// doesn't apply to compile it as an unused static and warn.

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
[[nodiscard]] QString RandomSuffix() {
    return QString::number(QRandomGenerator::global()->generate(), 16);
}
#endif

#ifdef Q_OS_MACOS
/// Quote a path for /bin/sh. Single quotes disable every expansion, so only an
/// embedded single quote needs care — end the string, emit an escaped quote,
/// reopen. Paths come from our own cache dir, but a user's home directory can
/// contain anything and this is a shell command line.
[[nodiscard]] QString ShellQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

[[nodiscard]] QString UpdatesDir() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base).filePath(QStringLiteral("updates"));
}
#endif

#ifdef Q_OS_WIN
/// Quote for PowerShell single-quoted strings (double the quote to escape).
[[nodiscard]] QString PowerShellQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QStringLiteral("'"), QStringLiteral("''"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}
#endif

#ifdef Q_OS_MACOS
/// `.../TeleMatrix.app/Contents/MacOS/TeleMatrix` -> `.../TeleMatrix.app`.
[[nodiscard]] QString BundlePath() {
    QDir dir(QCoreApplication::applicationDirPath()); // Contents/MacOS
    if (!dir.cdUp() || !dir.cdUp()) {                 // Contents -> .app
        return QString();
    }
    const auto path = dir.absolutePath();
    return path.endsWith(QStringLiteral(".app")) ? path : QString();
}
#endif

#ifdef Q_OS_LINUX
/// The AppImage file we were launched from, or empty for an extracted run.
[[nodiscard]] QString AppImagePath() {
    const auto value = qEnvironmentVariable("APPIMAGE");
    if (value.isEmpty()) {
        return QString();
    }
    const QFileInfo info(value);
    if (!info.exists() || !info.isFile()) {
        return QString();
    }
    return info.canonicalFilePath();
}
#endif

} // namespace

bool CanSelfUpdate(QString *reason) {
    // Unused on Windows, where the NSIS installer can always update in place.
    [[maybe_unused]] const auto no = [reason](const QString &why) {
        if (reason) {
            *reason = why;
        }
        return false;
    };
    if (reason) {
        reason->clear();
    }

#if defined(Q_OS_MACOS)
    const auto bundle = BundlePath();
    if (bundle.isEmpty()) {
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater", "This build is not running from an app bundle."));
    }
    // App Translocation runs the bundle from a randomised read-only path; the
    // swap would silently do nothing useful.
    if (bundle.contains(QStringLiteral("/AppTranslocation/"))) {
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater",
            "Move TeleMatrix to your Applications folder to enable updates."));
    }
    if (bundle.startsWith(QStringLiteral("/Volumes/"))) {
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater",
            "TeleMatrix is running from a disk image. Copy it to your Applications "
            "folder to enable updates."));
    }
    // Replacing the bundle is a rename inside its parent, so that is what has to
    // be writable — a standard user under /Applications typically is not.
    const QFileInfo parent(QFileInfo(bundle).absolutePath());
    if (!parent.isWritable()) {
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater",
            "TeleMatrix cannot update itself because its folder is not writable."));
    }
    return true;

#elif defined(Q_OS_WIN)
    // The NSIS installer handles the replacement, elevating via UAC if needed.
    return true;

#elif defined(Q_OS_LINUX)
    const auto appImage = AppImagePath();
    if (appImage.isEmpty()) {
        // deb/rpm (or an extracted AppImage): a package manager owns these files.
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater",
            "This build is installed by your package manager, which handles updates."));
    }
    const QFileInfo dir(QFileInfo(appImage).absolutePath());
    if (!dir.isWritable()) {
        return no(QCoreApplication::translate(
            "TeleMatrix::Updater",
            "TeleMatrix cannot update itself because its folder is not writable."));
    }
    return true;

#else
    return no(QCoreApplication::translate(
        "TeleMatrix::Updater", "Automatic updates are not supported on this platform."));
#endif
}

ApplyResult Apply(const QString &localPath) {
    ApplyResult result;
    const auto fail = [&result](const QString &message) {
        result.ok = false;
        result.error = message;
        return result;
    };

    if (localPath.isEmpty() || !QFileInfo::exists(localPath)) {
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "The downloaded update is missing."));
    }
    QString reason;
    if (!CanSelfUpdate(&reason)) {
        return fail(reason);
    }

#if defined(Q_OS_MACOS)
    const auto bundle = BundlePath();
    const auto staging = QDir(UpdatesDir()).filePath(QStringLiteral("staged-") + RandomSuffix());
    if (!QDir().mkpath(staging)) {
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not prepare the update."));
    }

    // /usr/bin/tar, not a Rust/Qt tar: the bundle is full of framework symlinks
    // and Apple metadata that a naive extractor mangles, which breaks the code
    // signature seal.
    QProcess tar;
    tar.setWorkingDirectory(staging);
    tar.start(QStringLiteral("/usr/bin/tar"), { QStringLiteral("xzf"), localPath });
    if (!tar.waitForFinished(120000) || tar.exitStatus() != QProcess::NormalExit
        || tar.exitCode() != 0) {
        QDir(staging).removeRecursively();
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "The update archive could not be extracted."));
    }

    const auto apps = QDir(staging).entryList({ QStringLiteral("*.app") },
        QDir::Dirs | QDir::NoDotAndDotDot);
    if (apps.isEmpty()) {
        QDir(staging).removeRecursively();
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "The update archive did not contain an application."));
    }
    const auto newBundle = QDir(staging).filePath(apps.first());

    // Swap from a detached helper, because the bundle cannot be replaced while
    // this process is executing out of it. `mv` both ways — never a merge INTO
    // the existing bundle, since leftover files break the signature seal.
    //
    // Three failure modes the naive version gets wrong:
    //
    //  * If the cache and the install live on different volumes, BSD `mv`
    //    degrades to copy+unlink and can fail partway, leaving a PARTIAL
    //    directory at the target. Restoring with a plain `mv` would then move
    //    the old bundle *inside* it (TeleMatrix.app/TeleMatrix.app.old-xxxx) and
    //    leave no working app at all — so the restore clears the target first,
    //    and `ditto` is the cross-volume fallback.
    //  * `kill -0` waits forever if the PID is reused by another of the user's
    //    processes, so the wait is bounded and then proceeds anyway (this
    //    process is exiting regardless).
    //  * The rolled-back copy must survive a failed relaunch — deleting it
    //    unconditionally would discard the only working bundle.
    const auto aside = bundle + QStringLiteral(".old-") + RandomSuffix();
    const auto script = QStringLiteral(
        "waited=0\n"
        "while kill -0 %1 2>/dev/null && [ $waited -lt 600 ]; do\n"
        "  sleep 0.2; waited=$((waited+1))\n"
        "done\n"
        "if ! /bin/mv %2 %3 2>/dev/null; then exit 1; fi\n"
        "if ! /bin/mv %4 %2 2>/dev/null; then\n"
        "  /bin/rm -rf %2\n"
        "  if ! /usr/bin/ditto %4 %2; then\n"
        "    /bin/rm -rf %2\n"
        "    /bin/mv %3 %2\n"
        "    exit 1\n"
        "  fi\n"
        "fi\n"
        "if /usr/bin/open -n %2 --args --relaunched; then\n"
        "  /bin/rm -rf %3\n"
        "fi\n"
        "/bin/rm -rf %5\n")
        .arg(QString::number(QCoreApplication::applicationPid()),
             ShellQuote(bundle),
             ShellQuote(aside),
             ShellQuote(newBundle),
             ShellQuote(staging));

    if (!QProcess::startDetached(QStringLiteral("/bin/sh"),
            { QStringLiteral("-c"), script })) {
        QDir(staging).removeRecursively();
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not start the update helper."));
    }
    result.ok = true;
    return result;

#elif defined(Q_OS_WIN)
    // Wait for this PID, run the installer silently, then relaunch. All three
    // steps must live in the helper: quitting takes seconds (up to six account
    // runtimes), NSIS silently fails to delete an in-use exe, and `/S` skips the
    // installer's own finish-page relaunch.
    const auto exeDir = QCoreApplication::applicationDirPath();
    const auto exe = QDir(exeDir).filePath(QStringLiteral("TeleMatrix.exe"));
    const auto command = QStringLiteral(
        "Wait-Process -Id %1 -ErrorAction SilentlyContinue; "
        "Start-Process -FilePath %2 -ArgumentList '/S' -Wait; "
        "Start-Process -FilePath %3 -ArgumentList '--relaunched'")
        .arg(QString::number(QCoreApplication::applicationPid()),
             PowerShellQuote(localPath),
             PowerShellQuote(exe));

    // Absolute path, not a bare "powershell": PATH/CWD lookup would let a
    // planted powershell.exe in a user-writable directory run at exactly the
    // moment we are about to trigger a UAC elevation.
    auto shell = qEnvironmentVariable("SystemRoot");
    if (shell.isEmpty()) {
        shell = QStringLiteral("C:/Windows");
    }
    shell += QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    if (!QProcess::startDetached(QDir::toNativeSeparators(shell),
            { QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"),
              QStringLiteral("Hidden"), QStringLiteral("-Command"), command })) {
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not start the update helper."));
    }
    result.ok = true;
    return result;

#elif defined(Q_OS_LINUX)
    // Swapping an AppImage in place is safe while it runs: the FUSE mount pins
    // the old inode, so this process (and any other profile running the same
    // file) keeps working off the version it started with.
    const auto appImage = AppImagePath();
    const QFileInfo info(appImage);
    const auto dir = info.absolutePath();
    // Same directory => same filesystem => rename(2) cannot fail with EXDEV.
    const auto staged = QDir(dir).filePath(
        QStringLiteral(".TeleMatrix.new-") + RandomSuffix());

    QFile::remove(staged);
    if (!QFile::copy(localPath, staged)) {
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not write the new version next to the current one."));
    }
    if (!QFile::setPermissions(staged,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
        QFile::remove(staged);
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not make the new version executable."));
    }
    // Flush the copy to disk BEFORE it takes over the real name. ext4/btrfs
    // delayed allocation means the rename can otherwise be durable while the
    // bytes are not, and a crash here would leave a truncated binary at
    // $APPIMAGE with the old one already gone.
    {
        QFile stagedFile(staged);
        if (!stagedFile.open(QIODevice::ReadOnly)
            || ::fsync(stagedFile.handle()) != 0) {
            const auto reason = QString::fromLocal8Bit(strerror(errno));
            QFile::remove(staged);
            return fail(QCoreApplication::translate(
                "TeleMatrix::Updater", "Could not flush the new version to disk: %1")
                .arg(reason));
        }
    }
    // POSIX rename(2), not QFile::rename: Qt refuses to overwrite an existing
    // destination, which would force a remove-then-rename — and if the rename
    // then failed, the user would be left with NO binary at all. rename(2)
    // replaces atomically, so the path either points at the old version or the
    // new one, never at nothing.
    if (::rename(QFile::encodeName(staged).constData(),
                 QFile::encodeName(appImage).constData()) != 0) {
        const auto reason = QString::fromLocal8Bit(strerror(errno));
        QFile::remove(staged);
        return fail(QCoreApplication::translate(
            "TeleMatrix::Updater", "Could not replace the current version: %1")
            .arg(reason));
    }
    // And fsync the directory so the rename itself survives a crash. Best
    // effort: the swap has already succeeded, so a failure here is not worth
    // aborting an otherwise-good update over.
    if (const auto dirFd = ::open(QFile::encodeName(dir).constData(), O_RDONLY); dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    result.ok = true;
    result.relaunchPath = appImage;
    return result;

#else
    return fail(QCoreApplication::translate(
        "TeleMatrix::Updater", "Automatic updates are not supported on this platform."));
#endif
}

} // namespace Updater
} // namespace Core
} // namespace TeleMatrix
