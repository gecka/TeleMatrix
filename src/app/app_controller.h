// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "account_domain.h"
#include "core/core_settings.h"
#include "protocol/protocol_types.h"
#include "theme/theme_manager.h"
#include "theme/system_theme_watcher.h"
#include "ui/input_submit_settings.h"
#include "window/notifications_manager.h"

#include <QObject>
#include <QPixmap>
#include <QRect>
#include <QSet>
#include <QTimer>
#include <QTranslator>

#include <memory>
#include <optional>

class QWidget;

namespace TeleMatrix {

class ProtocolBridge;
class AppMainWindow;
class AppMainWidget;
class UnreadStateStore;
class TrayIcon;

class IntroWidget;
class Account;

namespace Core {
class UpdateService;
} // namespace Core

/// Application orchestrator.
/// Owns the ProtocolBridge, Settings, and manages screen transitions.
class AppController : public QObject {
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    /// Show the main window. Call this from main() after QApplication is set up.
    void start();

    /// True if the user quit during start() (e.g. from the vault-unlock screen),
    /// so main() should skip app.exec() and terminate. start() runs a blocking
    /// loop before app.exec(), so the quit must be honored here or it is lost.
    [[nodiscard]] bool startupQuitRequested() const { return _startupQuitRequested; }

    /// Bring the existing window to the foreground (used when a second launch is
    /// attempted while single-instance is enforced).
    void bringToFront();

    /// Whether a secret backend is ready to receive the secrets a fresh
    /// login/registration is about to write.
    enum class SecretSetup {
        Ready,               ///< Keychain reachable, or the vault is unlocked.
        NeedsMasterPassword, ///< No usable store: send the user to the vault form.
        KeychainError,       ///< Keychain read failed on a platform that has one.
    };
    /// Check the secret backend before a fresh login/registration writes secrets.
    /// The caller acts on the result: the intro routes NeedsMasterPassword to the
    /// in-window create-password step and shows KeychainError inline (it is
    /// transient — a locked keychain or a denied access prompt — and retrying the
    /// submit is the remedy; moving the device onto a vault is not).
    [[nodiscard]] static SecretSetup checkSecretBackendForNewSession();

    /// The domain directory: parent of every account's data dir, and where the
    /// device-level secret vault file lives. Anchoring the vault here (rather than
    /// inside one account) is what lets a single unlock serve every account.
    [[nodiscard]] static QString domainDir();

    /// An account's own data directory (sqlite store, media cache, search index).
    /// `dirName` is the account's stable directory name, persisted in the account
    /// index, so allocation order never matters.
    [[nodiscard]] static QString accountDataDir(const QString &dirName);

    /// Device-level application settings, shared by every account.
    [[nodiscard]] Core::Settings &settings() { return _settings; }
    [[nodiscard]] const Core::Settings &settings() const { return _settings; }

    /// The active account's own settings (session identity, folders, recents).
    [[nodiscard]] Core::AccountSettings &accountSettings();
    [[nodiscard]] const Core::AccountSettings &accountSettings() const;

    /// Every signed-in account, and which one is showing.
    [[nodiscard]] AccountDomain &domain() { return _domain; }
    [[nodiscard]] const AccountDomain &domain() const { return _domain; }
    /// The account whose UI is showing. Never null while the app is running.
    [[nodiscard]] Account *activeAccount() const { return _domain.active(); }

    /// Logged-in user profile data (of the active account).
    [[nodiscard]] const QString &userId() const;
    [[nodiscard]] const QString &displayName() const;
    [[nodiscard]] const QString &avatarUrl() const;
    [[nodiscard]] ProtocolBridge *bridge() const;
    [[nodiscard]] Theme::ThemeManager *themeManager() const { return _themeManager.get(); }
    [[nodiscard]] UnreadStateStore *unreadStateStore() const;

    /// Account capabilities (e.g. m.3pid_changes), fetched in the background right
    /// after login/session-restore so pages built later (Settings is built lazily,
    /// only when opened) can render the correct email/phone state on first paint
    /// instead of the optimistic-until-loaded default.
    [[nodiscard]] const AccountSummary &cachedAccountSummary() const;
    [[nodiscard]] bool cachedAccountSummaryLoaded() const;

    /// Whether the homeserver can verify email addresses, probed in the background
    /// at session-ready. Empty when the server settled nothing (assume it works).
    [[nodiscard]] std::optional<bool> emailVerificationSupported() const;

    /// Schedule a delayed settings save (coalescing timer).
    void saveSettingsDelayed();

    // Persist the current recent-emoji list to the server (account data). Called
    // when the user picks an emoji; the list itself lives in Settings in-memory.
    void pushRecentEmoji();

    /// Immediately save settings to disk.
    bool saveSettings();

    /// Recalculate the dock badge from current unread state (summed across every
    /// account, since background accounts keep receiving messages).
    void refreshNotificationsBadge();

    /// Show `index`'s account: rebuilds the logged-in UI against its bridge while
    /// every other account stays connected in the background. `slideDirection`
    /// steers the switch animation (+1 exits left, -1 exits right, 0 = derive
    /// from list order).
    void activateAccount(int index, int slideDirection = 0);
    /// Same, addressed the way the Ctrl+Shift+N shortcuts are (1-based).
    void activateAccountByOrdinal(int ordinal);
    /// Show the account `delta` steps from the active one, wrapping around
    /// (Ctrl+Shift+] = +1, Ctrl+Shift+[ = -1).
    void activateAdjacentAccount(int delta);

    /// Apply the app-global "search in encrypted rooms" toggle to every account's
    /// bridge, so background accounts run the enable/disable side-effects too — not
    /// just the visible one. See code-review-2026-07-19 MA-5.
    void setE2eeSearchEnabledAllAccounts(bool enabled);

    /// Sign in with an additional account: allocates one, makes it active, and
    /// shows the login form against its own fresh bridge. The account being left
    /// keeps syncing in the background throughout.
    void showAddAccountIntro();

    /// Restart the application (save settings, relaunch, quit).
    void restartApplication();

    /// Restart into a specific binary instead of the running one. Needed by the
    /// AppImage updater: applicationFilePath() inside an AppImage points at the
    /// OLD version's inner mount path, so re-execing it would relaunch the
    /// version that was just replaced.
    void restartIntoPath(const QString &path);

    /// Forwarded to the main widget, which owns the connection pill.
    void setConnectingBottomSkip(int skip);

    /// App-global auto-updater. Owned here rather than per-account: it has to
    /// work before login and outlives account switches.
    [[nodiscard]] Core::UpdateService *updateService() const { return _updateService.get(); }

    /// Re-arm (or stand down) automatic update checks after a policy change.
    void notifyUpdatePolicyChanged();

    /// Switch the application language. Saves the new language ID and restarts.
    void setLanguage(const QString &langCode);

    /// Notify that the chat-background doodle setting changed (repaints backgrounds).
    void notifyBackgroundDoodlesChanged(bool enabled);

    /// Notify that the large emoji setting changed (triggers timeline relayout).
    void notifyLargeEmojiChanged(bool enabled);

    /// Notify that the message hover reply/reaction button settings changed.
    void notifyReplyButtonOnMessagesChanged(bool enabled);
    void notifyReactionButtonOnMessagesChanged(bool enabled);

    /// Notify that the "hide system messages in public rooms" setting changed.
    void notifyHideSystemMessagesInPublicRoomsChanged(bool enabled);

    /// Notify that "Include muted chats in folders counters" changed (rebuilds folder badges).
    void notifyIncludeMutedInFoldersChanged();

    /// Ask the main widget to open the theme picker (and close the settings layer).
    void requestThemeSelector();

    // The theme picker applies a theme as you click it, but the choice only
    // survives if you press Save. While previewing, the themeChanged handler
    // leaves settings.json alone; ending a preview without saving puts the
    // theme that was in effect when the panel opened back.
    void beginThemePreview();
    void saveThemePreview();  // keep what is applied: persist it, stop previewing
    void endThemePreview();   // discard: restore the theme the preview started from

Q_SIGNALS:
    /// The active account's identity changed — it finished restoring, signed in,
    /// or the app switched to a different account. Anything showing the account
    /// (the main menu's cover, for one) has to repaint: at startup the menu can
    /// be opened before the session has restored, and would otherwise sit on
    /// "unknown user" until it was closed and opened again.
    void activeAccountProfileChanged();

    /// Emitted when the theme picker should open.
    void themeSelectorRequested();

    /// Emitted when the send-submit mode changes (Enter vs Cmd+Enter).
    void sendSubmitWayChanged(TeleMatrix::InputSubmitSettings setting);

    /// Emitted when the chat-background doodle setting changes.
    void backgroundDoodlesChanged(bool enabled);

    /// Emitted when the large emoji setting changes.
    void largeEmojiChanged(bool enabled);

    /// Emitted when the message hover reply/reaction button settings change.
    void replyButtonOnMessagesChanged(bool enabled);
    void reactionButtonOnMessagesChanged(bool enabled);

    /// Emitted when the "hide system messages in public rooms" setting changes.
    void hideSystemMessagesInPublicRoomsChanged(bool enabled);

    /// Emitted when "Include muted chats in folders counters" changes.
    void includeMutedInFoldersChanged();

private:
    /// Data-directory name of the active account (empty when there is none).
    [[nodiscard]] QString activeDirName() const;
    /// Whether ANY account is signed in — the question that decides whether the
    /// secret store must be unlocked at startup, since one vault serves them all.
    [[nodiscard]] bool hasAnySession() const;
    /// Whether any signed-in account keeps its secrets in the file vault.
    [[nodiscard]] bool anyAccountUsesVault() const;
    /// Which account holds `roomId`, or -1. Used to route a notification click
    /// when the account it came from can no longer be named directly.
    [[nodiscard]] int accountIndexForRoom(const QString &roomId) const;
    // Bridge for a notification action, routed to the account the toast came from
    // (falling back to a room scan then the active account). See MA-3.
    [[nodiscard]] ProtocolBridge *bridgeForNotification(
        const QString &accountDirName, const QString &roomId) const;

    /// Bring up every signed-in account that isn't the active one, staggered so
    /// their cold starts don't contend with the sync the user is waiting on.
    void startBackgroundAccounts();
    /// Remove non-active accounts whose settings claim a session but whose
    /// secrets are provably gone — the "ghost" entries that can never restore.
    /// Runs at startup once every unlock flow has resolved; never acts when the
    /// secret backend is unreachable (absence proves nothing there).
    void sweepUndeadAccounts();
    /// Create `index`'s bridge and restore its session without any UI: used for
    /// background accounts, which must never interrupt the account on screen.
    void startAccountSession(int index);
    // Feed an account's unread store from its own bridge so its unread reaches the
    // aggregate badge even while it is a background account. See MA-4.
    void wireUnreadBadgeFeed(Account *account);
    /// Seed the bridge's Saved Messages id from this account's settings and keep
    /// the two in sync, so the first paint already has the name and userpic.
    void wireSavedMessagesCache(Account *account);
    /// Drop an account that was only ever a half-finished "add account" (no
    /// session), so nothing nameless is left in the list or on disk.
    void discardAccount(int index);
    /// Sign a non-active account out of this device: secrets, entry and data dir
    /// all go. Unlike discardAccount this also removes entries that still claim
    /// a session — used to put down a dead (unrestorable) entry on request.
    void removeAccountEntry(int index);
    /// Slide a snapshot of the account being left off the window, revealing the
    /// one that replaced it. No-op without a usable snapshot.
    void playAccountSwitchSlide(
        const QPixmap &outgoing, const QRect &geometry, int direction);

    void applyLanguageAndLocale(const QString &langCode);
    void persistSession(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    /// Same, for an account that isn't the active one — the "add account" popup
    /// signs in against its own account while another stays on screen.
    void persistSessionFor(
        Account *account,
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    void showIntro(bool startOnLogin = false);
    /// `restoreWindowGeometry` re-applies the saved window position; pass false
    /// when the window is already where the user wants it and only the contents
    /// are being rebuilt (an account switch).
    void showMain(bool restoreWindowGeometry = true);
    void onLoginSuccess(const QString &userId);
    /// Why a sign-out is happening. `Remote` skips the "are you sure?" question
    /// — the homeserver has already decided — and explains instead.
    enum class SignOutReason {
        UserRequested,
        Remote,
    };
    /// Sign out the active account. Kept parameterless so it stays usable as a
    /// pointer-to-member slot for `logoutRequested`; `signOut` is the body.
    void handleLogout();
    void signOut(SignOutReason reason);
    /// Connect this account's bridge to the remote-sign-out signal. Call after
    /// every bridge creation, for background accounts too: a dead token has to
    /// be acted on whether or not that account is the one on screen.
    void wireSessionInvalidation(Account *account);
    /// The homeserver rejected `dirName`'s access token. Routes to the
    /// interactive or the in-place sign-out (see `RouteForcedSignOut`).
    void handleSessionInvalidated(const QString &dirName, bool softLogout);
    /// Sign out an account that is not the one on screen, without disturbing the
    /// account that is: notify, tear its bridge down, and drop its slot.
    void forceSignOutBackgroundAccount(int index);
    /// Watch the bridge sync state for the unrecoverable local-store error
    /// and inform the user (call after every bridge (re)creation).
    void watchBridgeStoreErrors();
    /// Kick off the background account-capabilities fetch (call once the session
    /// is ready: after a successful restore or a fresh login/registration).
    void prefetchAccountCapabilities();
    void showStoreErrorDialog();
    void startUnauthorisedCleanup();
    // Blocking master-password unlock at startup (vault backend). Returns true when
    // unlocked; false means the user chose "reset" (session already cleared).
    bool unlockSecretStoreBlocking();
    // Wipe an insecure/incomplete saved session and its local encrypted stores.
    void dropInsecureSession();

    // Copy the ThemeManager's current theme, variant and auto-night state into
    // settings and schedule a write.
    void persistCurrentTheme();

    // What the theme picker has to put back when it is closed without saving.
    struct ThemeSnapshot {
        QString id;
        int mode = 0;
        bool autoNight = true;
    };
    ThemeSnapshot _themeSnapshot;

    Core::Settings _settings;
    bool _themePreviewing = false;
    QTimer _saveSettingsTimer;
    QTranslator *_translator = nullptr;

    AccountDomain _domain;
    std::unique_ptr<Theme::ThemeManager> _themeManager;
    std::unique_ptr<Theme::SystemThemeWatcher> _systemThemeWatcher;
    std::unique_ptr<Core::UpdateService> _updateService;
    // Periodic update poll; stopped entirely when the policy is Off, so that
    // setting really does mean zero automatic network calls.
    QTimer _updateCheckTimer;
    // Deferred first check. A member (not an anonymous singleShot) so toggling
    // the policy off actually cancels it — otherwise each Off->On flip would
    // queue another uncancellable check.
    QTimer _updateStartupTimer;
    AppMainWindow *_window = nullptr;
    IntroWidget *_intro = nullptr;
    AppMainWidget *_mainWidget = nullptr;
    std::unique_ptr<Notifications::System> _notifications;
    std::unique_ptr<TrayIcon> _tray;
    // Last unread total written to the tray, to skip redundant OS writes (MA-8).
    int _lastTrayUnread = -1;

    bool _storeErrorDialogShown = false;
    bool _secretStoreNeedsUnlock = false;
    // Set when the user quits from the startup vault-unlock screen, so start()
    // aborts instead of proceeding to the main shell.
    bool _startupQuitRequested = false;
    // Guards against re-entering the switch while the UI subtree is being rebuilt.
    bool _switchingAccount = false;
    // Accounts (by dir name) whose forced sign-out is already underway. Every
    // in-flight request against a dead token rejects, so the signal repeats;
    // restarting the teardown would double-wipe and race its own bridge
    // shutdown. Entries are never removed: the account leaves with the teardown.
    QSet<QString> _forcedSignOutInFlight;
};

} // namespace TeleMatrix
