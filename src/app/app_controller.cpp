// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app_controller.h"
#include "account.h"
#include "app_main_window.h"
#include "app_main_widget.h"
#include "tray_icon.h"
#include "unread_state_store.h"

#include "../dialogs/dialogs_intro_box.h"
#include "../intro/intro_colors.h"
#include "../intro/intro_widget.h"
#include "../intro/intro_vault_unlock.h"
#include "../core/localization.h"
#include "../core/update_service.h"
#include "../protocol/media_cache.h"
#include "../protocol/protocol_bridge.h"
#include "../settings/sessions/session_loading_overlay.h"
#include "../storage/storage_local.h"
#include "../ui/style/runtime_font.h"
#include "../ui/style/icon_provider.h"
#include "../ui/style/runtime_scale.h"
#include "../ui/emoji_config.h"
#include "../ui/emoji_sprites.h"
#include "../styles/style_constants.h"
#include "../history/history_confirm_dialog.h"
#include "../history/history_emoji_picker.h"
#include "../history/history_input.h"
#include "../history/history_message.h"
#include "../history/history_widget.h"
#ifdef Q_OS_MAC
#include "../window/notifications_manager_mac.h"
#elif defined(Q_OS_WIN)
#include "../window/notifications_manager_win.h"
#elif defined(Q_OS_LINUX)
#include "../window/notifications_manager_linux.h"
#endif

#include <thread>
#include <vector>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QPainter>
#include <QVariantAnimation>
#include <QWidget>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QVBoxLayout>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace TeleMatrix {

namespace {
constexpr int kSaveSettingsDelay = 1000; // 1 second coalescing delay.

// Update polling. The startup check is deferred so it never competes with the
// first sync; the recheck interval only matters for sessions left running for
// days.
constexpr int kUpdateCheckStartupDelay = 15 * 1000;       // 15 seconds
constexpr int kUpdateCheckInterval = 4 * 60 * 60 * 1000;  // 4 hours

// Gap between background accounts' bring-ups. Each opens its own SQLite stores,
// so starting them together would contend with the active account's first sync —
// the one the user is actually waiting on.
constexpr int kBackgroundAccountStartStagger = 2000;

// Convert between the bridge's (emoji, count) pairs and Settings' RecentEmoji.
QVector<Core::RecentEmoji> ToRecentEmoji(
        const QVector<QPair<QString, int>> &pairs) {
    QVector<Core::RecentEmoji> out;
    out.reserve(pairs.size());
    for (const auto &pair : pairs) {
        out.push_back({ pair.first,
            static_cast<uint16_t>(qBound(1, pair.second, 0xFFFF)) });
    }
    return out;
}

QVector<QPair<QString, int>> FromRecentEmoji(
        const QVector<Core::RecentEmoji> &items) {
    QVector<QPair<QString, int>> out;
    out.reserve(items.size());
    for (const auto &item : items) {
        out.push_back({ item.emoji, int(item.rating) });
    }
    return out;
}

QString SessionSecretKey(
        const QString &kind,
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId) {
    return QStringLiteral("v1|%1|%2|%3|%4").arg(
        kind,
        homeserver,
        userId,
        deviceId);
}

QString SessionSecretKey(const QString &kind, const Core::AccountSettings &settings) {
    return SessionSecretKey(
        kind,
        settings.sessionHomeserver(),
        settings.sessionUserId(),
        settings.sessionDeviceId());
}

// The account switch, played as one frame: the account being left slides off
// while the one arriving trails in behind it, with a shadow cast along the
// moving edge.
//
// Both sides are cached pixmaps (as tdesktop's own slide animations do) rather
// than live widgets. That is what makes the paired motion possible at all — the
// outgoing widget is destroyed the moment the new one takes the window, and the
// incoming one is the layout's central widget, which would fight being moved.
class AccountSwitchSlide final : public QWidget {
public:
    /// `direction` +1: the leaving screen exits to the left (next account);
    /// -1: it exits to the right (previous account).
    AccountSwitchSlide(
        QWidget *parent, QPixmap outgoing, QPixmap incoming, int direction)
    : QWidget(parent)
    , _outgoing(std::move(outgoing))
    , _incoming(std::move(incoming))
    , _direction(direction < 0 ? -1 : 1) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setProgress(qreal progress) {
        _progress = progress;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const auto full = width();

        // Cover-reveal: the arriving account sits still underneath while the
        // leaving screen slides away whole. A scrim lifting off the revealed
        // screen gives the two layers depth.
        p.drawPixmap(0, 0, _incoming);
        auto scrim = QColor(0, 0, 0);
        scrim.setAlphaF(kIncomingDim * (1.0 - _progress));
        p.fillRect(rect(), scrim);

        const auto outgoingX = qRound(_direction * -(full * _progress));
        // Shadow the moving edge casts on the revealed screen; it travels out
        // with the edge, so it never needs its own fade.
        const auto edge = (_direction > 0) ? (outgoingX + full) : outgoingX;
        if (edge > 0 && edge < full) {
            QLinearGradient gradient(
                edge, 0, edge + _direction * kShadowWidth, 0);
            auto shadow = QColor(0, 0, 0);
            shadow.setAlphaF(kShadowAlpha);
            gradient.setColorAt(0.0, shadow);
            shadow.setAlphaF(0.0);
            gradient.setColorAt(1.0, shadow);
            const auto shadowRect = (_direction > 0)
                ? QRectF(edge, 0, kShadowWidth, height())
                : QRectF(edge - kShadowWidth, 0, kShadowWidth, height());
            p.fillRect(shadowRect, gradient);
        }

        p.drawPixmap(outgoingX, 0, _outgoing);
    }

private:
    static constexpr qreal kIncomingDim = 0.18;
    static constexpr qreal kShadowAlpha = 0.28;
    static constexpr int kShadowWidth = 32;

    QPixmap _outgoing;
    QPixmap _incoming;
    int _direction = 1;
    qreal _progress = 0.0;
};

// Key for one account's local-cache passphrase. Namespaced by data-directory name
// (mirrors Rust's SessionStorageService::local_secret_key), because these stores
// are opened before any session exists to name them — so every account encrypts
// its own local caches with its own passphrase.
QString LocalSecretKey(const QString &dirName, const QString &kind) {
    return QStringLiteral("v1|local_cache|%1|%2").arg(dirName, kind);
}

void RemoveDatabaseWithSidecars(const QString &dataDir, const QString &name) {
    const auto path = dataDir + QStringLiteral("/") + name;
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
}

// Cold-start cleanup for an insecure/incomplete leftover session (runs once,
// before any UI). The interactive logout path instead renames these stores
// aside in Rust (SessionLifecycleService::move_stores_to_trash) and deletes
// them off the critical path; any trash left here is reclaimed by the Rust
// startup sweep on the next tm_create.
void ClearLocalProtocolStorage(const QString &dataDir) {
    QDir(dataDir + QStringLiteral("/store")).removeRecursively();
    QDir(dataDir + QStringLiteral("/media_cache")).removeRecursively();
    RemoveDatabaseWithSidecars(dataDir, QStringLiteral("search_index.db"));
    RemoveDatabaseWithSidecars(dataDir, QStringLiteral("preview_cache.db"));
    RemoveDatabaseWithSidecars(dataDir, QStringLiteral("app_cache.db"));
}

void ScrubLegacyPlaintextCacheFiles(const QString &dataDir) {
    const auto appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    for (const auto &name : {
        QStringLiteral("rooms_cache.json"),
        QStringLiteral("folders_cache.json"),
        QStringLiteral("path_cache.json"),
    }) {
        QFile::remove(appDataDir + QLatin1Char('/') + name);
    }
    QFile::remove(dataDir + QStringLiteral("/server_versions.json"));
}

// What the keychain holds for a saved session, distinguished finely enough for
// callers to act safely: `Incomplete` (some-but-not-all secrets present) is the
// only state that on its own proves the session unusable. `AllAbsent` is
// ambiguous — a keychain can report an item it won't hand over as simply not
// being there — and only means "genuinely gone" when the same run proved the
// backend readable via some other account's complete set.
enum class SecretsState {
    NoSession,  // settings carry no session at all
    Complete,   // every secret present
    Incomplete, // some present, some missing: genuinely half-written, unusable
    AllAbsent,  // none present: unreadable blob OR a fully wiped session
    ReadFailed, // the keychain refused a read: the answer is unknown
};

SecretsState ClassifySessionSecrets(
        const Core::AccountSettings &settings,
        const QString &dirName) {
    if (!settings.hasSession()) {
        return SecretsState::NoSession;
    }
    const QString keys[] = {
        SessionSecretKey(QStringLiteral("session_access_token"), settings),
        SessionSecretKey(QStringLiteral("sdk_store_passphrase"), settings),
        SessionSecretKey(QStringLiteral("search_passphrase"), settings),
        LocalSecretKey(dirName, QStringLiteral("app_cache_passphrase")),
        LocalSecretKey(dirName, QStringLiteral("preview_cache_passphrase")),
        LocalSecretKey(dirName, QStringLiteral("media_cache_passphrase")),
    };
    int present = 0;
    int missing = 0;
    for (const auto &key : keys) {
        bool failed = false;
        const auto value = ProtocolBridge::keychainLoad(key, &failed);
        if (failed) {
            return SecretsState::ReadFailed;
        }
        if (value.isEmpty()) {
            ++missing;
        } else {
            ++present;
        }
    }
    if (missing == 0) {
        return SecretsState::Complete;
    }
    if (present == 0) {
        return SecretsState::AllAbsent;
    }
    qWarning() << "Saved session is missing" << missing << "of" << (missing + present)
        << "secrets";
    return SecretsState::Incomplete;
}

// False means the session is missing a secret it cannot run without. `readFailed`
// is set when the keychain refused a read instead: the answer is then unknown, not
// "missing", and the caller must not act on it.
bool HasSecureSessionSecrets(
        const Core::AccountSettings &settings,
        const QString &dirName,
        bool *readFailed = nullptr) {
    if (readFailed) {
        *readFailed = false;
    }
    switch (ClassifySessionSecrets(settings, dirName)) {
    case SecretsState::Complete:
        return true;
    case SecretsState::AllAbsent:
        // Not one of the six came back, yet the settings say there is a session. All
        // six are written together into a single keychain blob, so "none of them"
        // cannot mean a half-provisioned session — it means we are not seeing that
        // blob. A keychain can report an item it won't hand over as simply not being
        // there (the read is answered, so nothing reports a failure), and treating
        // that as "the session is incomplete" is what wiped working sessions after a
        // rebuild, when the OS re-challenges access. Report it as unreadable.
        qWarning() << "Session secrets all absent while a session is saved;"
            << "treating the keychain as unreadable rather than wiping it";
        [[fallthrough]];
    case SecretsState::ReadFailed:
        if (readFailed) {
            *readFailed = true;
        }
        return false;
    case SecretsState::Incomplete:
    case SecretsState::NoSession:
        return false;
    }
    return false;
}
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
    , _domain(this)
{
    // Initialize storage and load the device settings plus the account list.
    Local::start();
    AccountIndex index;
    Local::readSettings(_settings, index);
    _domain.restore(index);
    // Drop accounts that were added but never signed into (an "add account" the
    // user closed) and any signed-out residue. Unconditional: with nothing
    // signed in at all, two leftover empty entries would otherwise both live in
    // the switcher forever as ghost accounts; the login target is recreated by
    // the empty-domain branch right below.
    bool prunedAny = false;
    for (int i = _domain.count() - 1; i >= 0; --i) {
        if (!_domain.account(i)->settings().hasSession()) {
            QDir(_domain.account(i)->dataDir()).removeRecursively();
            _domain.remove(i);
            prunedAny = true;
        }
    }
    // A fresh install has no accounts yet; the app always needs one to sign in
    // with, so give it an empty one rather than special-casing "no account"
    // through the whole intro flow.
    if (_domain.isEmpty()) {
        _domain.add(std::make_unique<Account>(_domain.allocateDirName()));
    }
    // Persist the prune, or the on-disk index keeps the ghost entries until
    // some unrelated save happens to run.
    if (prunedAny) {
        saveSettings();
    }

    // Data directory for persistent storage (sqlite store, media cache).
    const QString dataDir = _domain.active()->dataDir();
    QDir().mkpath(dataDir);
    ScrubLegacyPlaintextCacheFiles(dataDir);

	// Configure the secret backend before any secret access (this gate included).
	// With a saved session, honor the sticky marker recorded at login so live
	// detection can't flip backends between runs and trigger a wipe. For a fresh
	// start, use the device-level preference chosen at first run. (0 = keychain,
	// 1 = vault.)
	//
	// The backend is chosen once per device (the first-run chooser), so any signed-in
	// account answers for all of them: unlocking is what the whole domain shares.
	const bool wantVault = anyAccountUsesVault()
		|| (!hasAnySession()
			&& _settings.preferredSecretBackend() == QStringLiteral("vault"));
	// Anchored at the domain dir, not this account's: the vault is device-level, so
	// one unlock serves every account.
	ProtocolBridge::secretStoreInit(domainDir(), wantVault ? 1 : 0);

	if (hasAnySession()) {
		// 0 KeychainReady,1 KeychainUnavailable,2 VaultLocked,3 VaultUnlocked,4 VaultAbsent.
		// Crucially, do NOT wipe when the backend is merely unreachable or the
		// vault is locked — that was the data-loss bug. Defer to start(), which
		// prompts to unlock (vault) or reports the keychain as unavailable.
		const int storeState = ProtocolBridge::secretStoreState();
		if (storeState == 1 || storeState == 2) {
			_secretStoreNeedsUnlock = true;
		} else if (storeState == 4) {
			dropInsecureSession();
		} else {
			// A refused read is not a missing secret. Only a secret that is genuinely
			// absent means the session can't run; an unreadable keychain says nothing
			// either way, and dropping on it destroys a working session.
			bool readFailed = false;
			if (!HasSecureSessionSecrets(accountSettings(), activeDirName(), &readFailed)
					&& !readFailed) {
				dropInsecureSession();
			}
		}
	}
	if (accountSettings().hadLegacySessionAccessToken()) {
		qWarning() << "Scrubbing legacy plaintext Matrix access token from settings";
		accountSettings().clearLegacySessionAccessTokenMarker();
		saveSettings();
	}

	_domain.active()->setUnreadStateStore(std::make_unique<UnreadStateStore>(this));
	_domain.active()->setBridge(std::make_unique<ProtocolBridge>(dataDir));
	wireUnreadBadgeFeed(_domain.active());
	wireSavedMessagesCache(_domain.active());

    _translator = new QTranslator(this);
    applyLanguageAndLocale(_settings.languageId());

    // Apply custom font family before any UI is created.
    if (!_settings.customFontFamily().isEmpty()) {
        Style::SetCustomFont(_settings.customFontFamily());
        const auto effective = Style::EffectiveFontFamily();
        if (!effective.isEmpty()) {
            auto font = qApp->font();
            font.setFamily(effective);
            qApp->setFont(font);
        }
    }

    // Apply scale settings before any UI is created.
    Style::SetDevicePixelRatio(qRound(qApp->devicePixelRatio()));
    const auto configScale = _settings.configScale();
    if (configScale != Style::kScaleAuto) {
        Style::SetScale(Style::CheckScale(configScale));
    } else {
        // macOS Retina default is 110%, non-Retina is 100%.
        const auto defaultScale = (Style::DevicePixelRatio() >= 2) ? 110 : 100;
        Style::SetScale(defaultScale);
    }

    // Build the emoji sprite index. Cheap — it decodes no atlas pages, those load on
    // first use. After the DPR/scale calls above because emoji_sprites.cpp caches its
    // scaled pixmaps in device pixels.
    Ui::Emoji::Init();
    if (!TeleMatrix::Emoji::Available()) {
        // Not fatal: every draw path falls back to text emoji, which is what the app
        // did before the sprite port. Worth a line in the log, though, because on a
        // host with no colour emoji font that fallback renders nothing at all.
        qWarning() << "Emoji: sprite atlases unavailable, falling back to text emoji.";
    }

    // Apply runtime scaling to all pixel constants.
    st::initPxValues();
    HistoryMessage::initMessagePxValues();
    HistoryWidget::initTopBarPxValues();
    HistoryInput::initInputPxValues();
    HistoryEmojiPicker::initEmojiPanelPxValues();
    Style::IconProvider::clearCache();

    // Initialize theme manager BEFORE creating any windows.
    _themeManager = std::make_unique<Theme::ThemeManager>(this);
    _systemThemeWatcher = std::make_unique<Theme::SystemThemeWatcher>(this);

    // Feed initial system dark state.
    _themeManager->setSystemDarkState(_systemThemeWatcher->isDark());

    // Apply persisted theme before first paint.
    const auto themeMode = static_cast<Theme::ThemeMode>(
        std::clamp(_settings.themeMode(), 0, 2));
    _themeManager->initializeFromSettings(
        _settings.themeId(), themeMode, _settings.systemDarkModeEnabled());

    // Watch for system dark mode changes.
    connect(_systemThemeWatcher.get(), &Theme::SystemThemeWatcher::systemDarkModeChanged,
            this, [this](bool isDark) {
        _themeManager->setSystemDarkState(isDark);
    });

    // Persist theme changes -- unless the theme picker is previewing, in which
    // case the choice only reaches disk when the user presses Save.
    connect(_themeManager.get(), &Theme::ThemeManager::themeChanged,
            this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
        if (_themePreviewing) {
            return;
        }
        persistCurrentTheme();
    });

    // Coalescing save timer: multiple rapid changes → single write.
    _saveSettingsTimer.setSingleShot(true);
    connect(&_saveSettingsTimer, &QTimer::timeout,
            this, &AppController::saveSettings);

    // Auto-updater. Created before any window and independent of accounts — it
    // has to work while the user is still sitting on the login screen.
    _updateService = std::make_unique<Core::UpdateService>(this);
    _updateService->setBetaChannel(_settings.installBetaVersions());
    connect(_updateService.get(), &Core::UpdateService::updateAvailable,
            this, [this](const QString &) {
        // Auto-download only in that policy; the apply step stays user-initiated
        // in every mode.
        if (_settings.updatePolicy() == static_cast<int>(Core::UpdateService::Policy::AutoDownload)
            && _updateService->applyMode() == Core::UpdateService::ApplyMode::OneClick) {
            _updateService->download();
        }
    });
    // Staging is done and a helper is already waiting on this PID, so go down
    // now. Hiding the window first matters: quitting still blocks the main
    // thread while up to six Rust runtimes drain, and a visible window frozen
    // for that beat reads as a hang rather than as the app closing.
    connect(_updateService.get(), &Core::UpdateService::applyReady,
            this, [this](const QString &relaunchPath) {
        if (_window) {
            _window->hide();
        }
        restartIntoPath(relaunchPath);
    });
    _updateCheckTimer.setInterval(kUpdateCheckInterval);
    connect(&_updateCheckTimer, &QTimer::timeout, this, [this] {
        _updateService->check(false);
    });
    _updateStartupTimer.setSingleShot(true);
    _updateStartupTimer.setInterval(kUpdateCheckStartupDelay);
    connect(&_updateStartupTimer, &QTimer::timeout, this, [this] {
        _updateService->check(false);
    });
    notifyUpdatePolicyChanged();

    // Save on application quit (covers Cmd+Q, SIGTERM, etc.).
    connect(qApp, &QApplication::aboutToQuit,
            this, [this] {
        if (_window) {
            _window->savePositionToSettings();
        }
        saveSettings();
        // Room and folder snapshots are persisted by Rust update paths.
        // Drain every account's Rust runtime CONCURRENTLY: each tm_destroy always
        // burns its full ~1.5s cap (sync tasks never idle), so draining 6 in series
        // froze quit for ~9s. Kick them all, then join; afterwards each bridge's
        // handle is null, so its destructor no-ops. Safe to parallelise: drainForQuit
        // nulls each bridge's callback guard first, and every media trampoline only
        // posts to the main thread via a queued invokeMethod — no worker thread ever
        // touches the shared MediaCache. (A prior revert blamed a MediaCache QHash race
        // here; the real quit crash was the MA-9 dedup guard, fixed separately.)
        // See code-review-2026-07-19 PERF-2.
        std::vector<std::thread> drains;
        for (int i = 0; i < _domain.count(); ++i) {
            if (const auto account = _domain.account(i); account && account->bridge()) {
                if (auto t = account->bridge()->drainForQuit(); t.joinable()) {
                    drains.push_back(std::move(t));
                }
            }
        }
        for (auto &t : drains) {
            t.join();
        }
    });
}

AppController::~AppController() = default;

// The active account always exists while the app runs (the constructor makes an
// empty one on a fresh install), but these still tolerate its absence so a
// half-built controller can't crash the UI.
namespace {
const QString kNoString;
const AccountSummary kNoAccountSummary;
Core::AccountSettings &NoAccountSettings() {
    static Core::AccountSettings instance;
    return instance;
}
} // namespace

Core::AccountSettings &AppController::accountSettings() {
    const auto account = _domain.active();
    return account ? account->settings() : NoAccountSettings();
}

const Core::AccountSettings &AppController::accountSettings() const {
    const auto account = _domain.active();
    return account ? account->settings() : NoAccountSettings();
}

const QString &AppController::userId() const {
    const auto account = _domain.active();
    return account ? account->userId() : kNoString;
}

const QString &AppController::displayName() const {
    const auto account = _domain.active();
    return account ? account->displayName() : kNoString;
}

const QString &AppController::avatarUrl() const {
    const auto account = _domain.active();
    return account ? account->avatarUrl() : kNoString;
}

ProtocolBridge *AppController::bridge() const {
    const auto account = _domain.active();
    return account ? account->bridge() : nullptr;
}

UnreadStateStore *AppController::unreadStateStore() const {
    const auto account = _domain.active();
    return account ? account->unreadStateStore() : nullptr;
}

const AccountSummary &AppController::cachedAccountSummary() const {
    const auto account = _domain.active();
    return account ? account->cachedAccountSummary() : kNoAccountSummary;
}

bool AppController::cachedAccountSummaryLoaded() const {
    const auto account = _domain.active();
    return account && account->cachedAccountSummaryLoaded();
}

std::optional<bool> AppController::emailVerificationSupported() const {
    const auto account = _domain.active();
    return account ? account->emailVerificationSupported() : std::nullopt;
}

QString AppController::activeDirName() const {
    const auto account = _domain.active();
    return account ? account->dirName() : QString();
}

int AppController::accountIndexForRoom(const QString &roomId) const {
    if (roomId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < _domain.count(); ++i) {
        const auto bridge = _domain.account(i)->bridge();
        if (!bridge) {
            continue;
        }
        for (const auto &room : bridge->cachedRooms()) {
            if (room.roomId == roomId) {
                return i;
            }
        }
    }
    return -1;
}

ProtocolBridge *AppController::bridgeForNotification(const QString &accountDirName,
                                                    const QString &roomId) const {
    // Prefer the account the toast recorded; fall back to a room scan, then the
    // active account. See MA-3.
    int index = _domain.indexOfDirName(accountDirName);
    if (index < 0) {
        index = accountIndexForRoom(roomId);
    }
    const auto account = _domain.account(index);
    return account ? account->bridge() : bridge();
}

void AppController::wireUnreadBadgeFeed(Account *account) {
    if (!account || !account->bridge() || !account->unreadStateStore()) {
        return;
    }
    auto *bridge = account->bridge();
    auto *store = account->unreadStateStore();
    // Keep this account's unread store fed from its own bridge, independent of the
    // active widget tree, so its unread reaches the aggregate badge even while it is
    // a background account (only the active account's store is fed by AppMainWidget).
    // applyRoomListSnapshot is an idempotent replace, so feeding the active account
    // here too is harmless. Store is the connection context, so it auto-disconnects
    // if the store outlives the bridge. See code-review-2026-07-19 MA-4.
    connect(bridge, &ProtocolBridge::roomListChanged, store, [bridge, store] {
        store->applyRoomListSnapshot(bridge->cachedRooms());
    });
    store->applyRoomListSnapshot(bridge->cachedRooms());
}

void AppController::wireSavedMessagesCache(Account *account) {
    if (!account || !account->bridge()) {
        return;
    }
    auto *bridge = account->bridge();
    // Seed BEFORE anything reads the room list: the marker lives in account data
    // and only lands after a round-trip, but getRoomsBlockingForStartupOnly()
    // paints the cached rooms long before that. Without this the saved room shows
    // its raw server name and avatar for the first second of every launch.
    bridge->seedSavedMessagesRoomId(account->settings().savedMessagesRoomId());
    // Per-account, not accountSettings(): a background account's bridge can
    // resolve its marker while another account is active, and the id must land in
    // its own settings.
    connect(bridge, &ProtocolBridge::savedMessagesRoomChanged, this,
        [this, account](const QString &roomId) {
            if (account->settings().savedMessagesRoomId() == roomId) {
                return;
            }
            account->settings().setSavedMessagesRoomId(roomId);
            saveSettingsDelayed();
        });
}

void AppController::setE2eeSearchEnabledAllAccounts(bool enabled) {
    for (int i = 0; i < _domain.count(); ++i) {
        if (const auto account = _domain.account(i); account && account->bridge()) {
            account->bridge()->setE2eeSearchEnabled(enabled);
        }
    }
}

bool AppController::hasAnySession() const {
    for (int i = 0; i < _domain.count(); ++i) {
        if (_domain.account(i)->settings().hasSession()) {
            return true;
        }
    }
    return false;
}

bool AppController::anyAccountUsesVault() const {
    for (int i = 0; i < _domain.count(); ++i) {
        const auto &settings = _domain.account(i)->settings();
        if (settings.hasSession()
                && settings.sessionSecretBackend() == QStringLiteral("vault")) {
            return true;
        }
    }
    return false;
}

void AppController::applyLanguageAndLocale(const QString &langCode) {
    if (_translator) {
        qApp->removeTranslator(_translator);
    }

    const auto resolvedId = Core::resolveLanguageId(langCode);
    QLocale::setDefault(QLocale(resolvedId));
    if (resolvedId == QStringLiteral("en")) {
        return;
    }

    const auto path = QStringLiteral(":/translations/telematrix_%1.qm").arg(resolvedId);
    if (_translator && _translator->load(path)) {
        qApp->installTranslator(_translator);
        return;
    }

    qWarning() << "Failed to load translation:" << path;
    QLocale::setDefault(QLocale(QStringLiteral("en")));
}

void AppController::persistSession(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl) {
    persistSessionFor(_domain.active(), userId, displayName, avatarUrl);
}

void AppController::persistSessionFor(
        Account *account,
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl) {
    if (!account || !account->bridge()) {
        return;
    }
    account->setProfile(userId, displayName, avatarUrl);
    if (account == _domain.active()) {
        Q_EMIT activeAccountProfileChanged();
    }

    const auto session = account->bridge()->getSessionInfoBlockingForPersistence();
    if (session.homeserver.isEmpty()
        || session.userId.isEmpty()
        || session.deviceId.isEmpty()
        || session.accessToken.isEmpty()) {
        qWarning() << "Cannot persist Matrix session: incomplete session info"
            << "homeserver" << !session.homeserver.isEmpty()
            << "user" << !session.userId.isEmpty()
            << "device" << !session.deviceId.isEmpty()
            << "token" << !session.accessToken.isEmpty();
        account->settings().clear();
        saveSettings();
        return;
    }

    const auto accessTokenKey = SessionSecretKey(
        QStringLiteral("session_access_token"),
        session.homeserver,
        session.userId,
        session.deviceId);
    const auto storeKey = SessionSecretKey(
        QStringLiteral("sdk_store_passphrase"),
        session.homeserver,
        session.userId,
        session.deviceId);
    const auto searchKey = SessionSecretKey(
        QStringLiteral("search_passphrase"),
        session.homeserver,
        session.userId,
        session.deviceId);
    // Only 0 (KeychainReady) and 3 (VaultUnlocked) are reachable+readable. Treat
    // missing secrets as a real failure only then — never clear the session just
    // because the backend was unreachable.
    const int storeState = ProtocolBridge::secretStoreState();
    const bool storeReachable = (storeState == 0 || storeState == 3);
    bool readFailed = false;
    const auto secretMissing = [&readFailed](const QString &key) {
        bool failed = false;
        const auto value = ProtocolBridge::keychainLoad(key, &failed);
        readFailed = readFailed || failed;
        return value.isEmpty();
    };
    const bool anySecretMissing = storeReachable
        && (secretMissing(accessTokenKey)
            || secretMissing(storeKey)
            || secretMissing(searchKey)
            || secretMissing(LocalSecretKey(account->dirName(), QStringLiteral("app_cache_passphrase")))
            || secretMissing(LocalSecretKey(account->dirName(), QStringLiteral("preview_cache_passphrase")))
            || secretMissing(LocalSecretKey(account->dirName(), QStringLiteral("media_cache_passphrase"))));
    if (readFailed) {
        // Unreadable is not missing: these secrets were just written, so keep the
        // session rather than wiping what we merely failed to read back.
        qWarning() << "Keychain refused a read while persisting the session;"
            << "keeping it rather than wiping secrets we couldn't verify";
    } else if (anySecretMissing) {
        qWarning() << "Cannot persist Matrix session: secure secrets missing";
        // Only this account's secrets: a sibling's must survive a failure here.
        account->deleteSecrets();
        account->settings().clear();
        saveSettings();
        return;
    }

    account->persistSession(
        session.homeserver,
        session.userId,
        session.deviceId,
        // Sticky backend marker, honored at next startup. Vault states are 2/3.
        (storeState == 2 || storeState == 3)
            ? QStringLiteral("vault") : QStringLiteral("keychain"),
        displayName,
        avatarUrl);

    // One entry per Matrix account: signing in as a user who already has
    // another (stale/ghost) entry replaces that entry, or the switcher shows
    // the same identity twice and the cycle shortcuts land on a dead twin.
    bool removedDisplayed = false;
    for (int i = _domain.count() - 1; i >= 0; --i) {
        const auto other = _domain.account(i);
        if (!other || other == account
            || other->settings().sessionUserId() != session.userId) {
            continue;
        }
        qWarning() << "Removing duplicate account entry for" << session.userId
            << "in dir" << other->dirName();
        // The duplicate can be the account currently ON SCREEN (the "add
        // account" popup signed into the same user the main window shows).
        // Removing it destroys the bridge the live UI is built on, so tear
        // that UI down first and rebuild onto the fresh entry below.
        if (other == _domain.active() && _mainWidget) {
            removedDisplayed = true;
            _notifications.reset();
            _mainWidget->disconnect(this);
            _mainWidget = nullptr;
            MediaCache::resetPendingRequests();
        }
        if (_notifications) {
            _notifications->detachAccount(other->dirName());
        }
        const auto dir = other->dataDir();
        // Its keychain keys are namespaced by dir, so this cannot touch the
        // freshly persisted entry's secrets.
        other->deleteSecrets();
        _domain.remove(i);
        QDir(dir).removeRecursively();
    }
    if (removedDisplayed) {
        const auto index = _domain.indexOfDirName(account->dirName());
        if (index >= 0) {
            _domain.activate(index);
        }
        // Same window, new tenant: keep the user's geometry.
        showMain(/*restoreWindowGeometry=*/false);
    }

    saveSettings();

    // A fresh login is the first moment the secret backend is proven readable
    // AND holds a complete set — the exact condition under which a sibling whose
    // secrets are absent is a genuine ghost, not a transiently-unreadable blob.
    // Secrets share one keychain bundle, so a re-signed rebuild that forces a
    // re-login rewrites that bundle with only THIS account; the other accounts'
    // secrets vanish from it, leaving undead entries the startup sweep couldn't
    // touch (nothing classified Complete yet at launch, so nothing proved the
    // backend readable). Reap them now instead of stranding the user with a ghost
    // until the next restart. Deferred so it runs after showMain() has settled,
    // off the login→main transition; sweepUndeadAccounts re-resolves by dirName,
    // so a shifting domain in between is safe.
    QTimer::singleShot(0, this, [this] { sweepUndeadAccounts(); });
}

void AppController::start() {
    _window = new AppMainWindow(this);
    _window->restoreWindowState();

    // Settings menu (⌘,). Connect ONCE here: both _window and AppController live
    // for the whole process, so re-connecting per login (as showMain() used to)
    // accumulates duplicate slots — and because showSettings() toggles, an even
    // number of duplicates makes ⌘, open-then-close (a no-op) after odd
    // sign-out/sign-in cycles. The live _mainWidget guard keeps it safe pre-login.
    connect(_window, &AppMainWindow::settingsRequested,
            this, [this] {
        if (_mainWidget) {
            _mainWidget->showSettings();
        }
    });

    connect(_window, &AppMainWindow::exploreRoomsRequested,
            this, [this] {
        if (_mainWidget) {
            _mainWidget->openExploreRooms();
        }
    });

    connect(_window, &AppMainWindow::searchRequested,
            this, [this] {
        if (_mainWidget) {
            _mainWidget->focusSearch();
        }
    });

    connect(this, &AppController::themeSelectorRequested, this, [this] {
        if (_mainWidget) {
            _mainWidget->showThemeSelector();
        }
    });

    // Capture user profile data on successful login.
    connect(bridge(), &ProtocolBridge::loginResult,
            this, [this](bool success, const QString &userId, const QString &displayName, const QString &avatarUrl) {
        if (success) {
            persistSession(userId, displayName, avatarUrl);
        }
    });
    watchBridgeStoreErrors();

    // Recent emojis are server-synced (io.element.recent_emoji); apply startup +
    // cross-device updates to the in-memory list the picker reads.
    connect(bridge(), &ProtocolBridge::recentEmojiChanged, this,
        [this](const QVector<QPair<QString, int>> &pairs) {
            // Local edits win for the session — don't let a lagging server echo
            // (our own write, or pre-pick state) revert a just-picked ordering.
            // It reconciles again from the server on the next launch.
            const auto account = _domain.active();
            if (!account || account->recentEmojiTouchedLocally()) {
                return;
            }
            account->settings().setRecentEmoji(ToRecentEmoji(pairs));
        });

    if (_secretStoreNeedsUnlock) {
        _window->show();
        const int storeState = ProtocolBridge::secretStoreState();
        if (storeState == 2 /*VaultLocked*/) {
            // Prompt for the master password (or the user resets, which clears the
            // session and falls through to the login screen below).
            unlockSecretStoreBlocking();
            if (_startupQuitRequested) {
                return; // User quit from the unlock screen; let the app terminate.
            }
        } else if (storeState == 0 || storeState == 3) {
            // The backend recovered between the constructor gate and here (e.g. the
            // keyring daemon finished starting). Apply the completeness check the
            // gate deferred, instead of the quit-only dialog below. A refused read
            // says nothing about completeness, so it must not drop the session —
            // the token read below owns that case, and offers a retry.
            bool readFailed = false;
            if (!HasSecureSessionSecrets(accountSettings(), activeDirName(), &readFailed) && !readFailed) {
                dropInsecureSession();
            }
        } else {
            // Keychain unreachable: never wipe. Ask the user to restore their
            // keyring and reopen; quitting preserves the saved session on disk.
            HistoryConfirmDialog dialog(
                _window,
                tr("Secure storage unavailable"),
                tr("TeleMatrix can't reach your system keyring, so it can't open "
                   "your saved session. Your data is safe — make sure your keyring "
                   "and D-Bus are running, then reopen TeleMatrix."),
                tr("Quit"),
                QString(),
                HistoryConfirmDialog::Normal,
                0,
                -1,
                /*showCancel=*/false);
            dialog.exec();
            // Same as the unlock path: start() runs before app.exec(), so signal
            // the quit to main() rather than QCoreApplication::quit() (which the
            // upcoming app.exec() would reset away).
            _startupQuitRequested = true;
            return;
        }
    }

    if (accountSettings().hasSession()) {
        // Show the main shell immediately for authorized users, but keep
        // the locally cached room list stable until initial sync is ready.
        showMain();
        const auto cachedRooms = bridge()->getRoomsBlockingForStartupOnly();
        if (!cachedRooms.isEmpty() && _mainWidget) {
            _mainWidget->applyCachedRooms(cachedRooms);
        }
        bridge()->getCustomFoldersBlockingForStartupOnly();

        // Hydrate recent emojis from the local app_cache.db mirror for instant
        // display; the server reconciles via recentEmojiChanged once sync is up.
        const auto recentEmoji = bridge()->recentEmojiForStartup();
        if (!recentEmoji.isEmpty()) {
            accountSettings().setRecentEmoji(ToRecentEmoji(recentEmoji));
        }

        // A refused read is not an absent session. macOS pins a keychain item's ACL to
        // the designated requirement of the binary that *created* it. A Developer ID
        // requirement is stable, so rebuilds, re-signs and shipped updates all keep
        // access; an ad-hoc requirement is a bare cdhash, so an ad-hoc build locks
        // every other build — including the released app — out of the item with
        // errSecAuthFailed. The reachability probe above can pass while this very read
        // is refused. Wiping on that destroys a working session, so offer a retry and
        // let the user quit with everything intact instead.
        QString accessToken;
        for (;;) {
            bool readFailed = false;
            accessToken = ProtocolBridge::keychainLoad(
                SessionSecretKey(QStringLiteral("session_access_token"), accountSettings()),
                &readFailed);
            if (!readFailed && accessToken.isEmpty()) {
                // An empty answer is ambiguous — the keychain may be withholding the
                // whole blob rather than genuinely holding nothing, and it can say so
                // without reporting an error at all. Ask whether ANY of the session's
                // secrets are visible before believing this one is gone.
                HasSecureSessionSecrets(accountSettings(), activeDirName(), &readFailed);
            }
            if (!readFailed) {
                break;
            }
            _window->show();
            HistoryConfirmDialog dialog(
                _window,
                tr("Can't read your saved sign-in"),
                tr("TeleMatrix couldn't read your saved sign-in from the system "
                   "keychain, so it can't open your session. Nothing has been "
                   "deleted — your data is safe. Allow TeleMatrix to access the "
                   "keychain, then try again."),
                tr("Try again"),
                tr("Sign out"));
            const auto choice = dialog.exec();
            if (choice == HistoryConfirmDialog::Dismissed) {
                // Esc or a click outside. Signing out from here erases the passphrases
                // that decrypt this account's local databases, and dismissing a dialog
                // is how a user says "leave things alone" — so it quits instead.
                _startupQuitRequested = true;
                return;
            }
            if (choice != HistoryConfirmDialog::Accepted) {
                // Pressed "Sign out". Spell out the damage before doing it: the reason
                // the read failed is usually recoverable (another build of TeleMatrix
                // owns the keychain item), while this is not.
                HistoryConfirmDialog confirm(
                    _window,
                    tr("Sign out and erase local data?"),
                    tr("This deletes the keys that decrypt the copy of your messages "
                       "stored on this device. They cannot be recovered, and everything "
                       "will be downloaded again after you sign in."),
                    tr("Sign out"),
                    tr("Back"),
                    HistoryConfirmDialog::Attention);
                if (confirm.exec() != HistoryConfirmDialog::Accepted) {
                    continue;
                }
                // Falling through clears the session and shows the login form; the
                // secrets are only destroyed here because they asked for it twice.
                accessToken.clear();
                break;
            }
            // Otherwise the retry would just re-serve the cached (empty) bundle.
            ProtocolBridge::keychainForgetCache();
        }
        if (accessToken.isEmpty()) {
            // Only this account's secrets, not the shared bundle: a sibling account
            // that stays signed in must keep working. deleteSecrets() needs the live
            // session identity, so it runs before clear(). The wholesale wipe is only
            // right when nothing is signed in anywhere. See code-review-2026-07-19 MA-2.
            if (const auto account = _domain.active()) {
                account->deleteSecrets();
                account->clear();
            } else {
                accountSettings().clear();
            }
            if (!hasAnySession()) {
                ProtocolBridge::keychainClearAll();
            }
            saveSettings();
            _notifications.reset();
            _mainWidget = nullptr;
            showIntro();
            _window->show();
            return;
        }

        // Restore session async — updates profile and starts sync.
        connect(bridge(), &ProtocolBridge::sessionRestored,
                this, [this, dirName = activeDirName()](bool success, const QString &userId, const QString &displayName, const QString &avatarUrl, const QString &error) {
            // Resolve the account that finished restoring by the name captured when
            // this was wired: the user may have switched accounts during the up-to-15s
            // restore, so _domain.active() is no longer guaranteed to be this one.
            // Filing the result against the wrong account would corrupt its identity
            // or (on the sign-out path) delete a still-signed-in account's secrets.
            // See code-review-2026-07-19 MA-6.
            const auto account = _domain.account(_domain.indexOfDirName(dirName));
            if (!account) {
                return;
            }
            if (success) {
                account->setProfile(userId, displayName, avatarUrl);
                account->setState(Account::State::Ready);
                Q_EMIT activeAccountProfileChanged();
                if (dirName == activeDirName()) {
                    prefetchAccountCapabilities();
                }
                return;
            }
            account->setState(Account::State::RestoreFailed);
            // The interactive retry/sign-out flow below drives the on-screen UI, so
            // it only applies while this account is still the active one. If the user
            // switched away during the restore, recording RestoreFailed is enough
            // (matching the background-account path). Also defer to a forced
            // corruption logout if one is already underway.
            if (dirName != activeDirName() || _storeErrorDialogShown) {
                return;
            }
            // A failed restore is NOT proof the token died. It also fires on a
            // timeout, an unreachable homeserver, or a startup slow enough to trip
            // the 15s cap — and the app can't tell those apart, because nothing
            // here ever sees an M_UNKNOWN_TOKEN. Wiping on that destroyed working
            // sessions, so ask instead: retrying is free, and signing out is the
            // user's call to make.
            if (_window) {
                _window->show();
            }
            HistoryConfirmDialog dialog(
                _window,
                tr("Couldn't open your session"),
                tr("TeleMatrix couldn't restore your session. The homeserver may be "
                   "unreachable, or startup may have taken too long. Nothing has "
                   "been deleted — your data is safe.\n\n%1").arg(error),
                tr("Try again"),
                tr("Sign out"));
            if (dialog.exec() == HistoryConfirmDialog::Accepted) {
                bool readFailed = false;
                const auto token = ProtocolBridge::keychainLoad(
                    SessionSecretKey(QStringLiteral("session_access_token"), accountSettings()),
                    &readFailed);
                if (!readFailed && !token.isEmpty()) {
                    bridge()->restoreSession(
                        accountSettings().sessionHomeserver(),
                        accountSettings().sessionUserId(),
                        accountSettings().sessionDeviceId(),
                        token);
                    return;
                }
            }
            // The user chose to sign out (or the token is genuinely gone): only now
            // is it right to clear the session and its secrets. `account` is the
            // still-active restoring account resolved above.
            account->deleteSecrets();
            account->clear();
            saveSettings();
            _notifications.reset();
            _mainWidget = nullptr;
            showIntro(true);
            if (_intro) {
                _intro->setInnerFocus();
            }
        });

        // Apply the persisted E2EE-search setting before sync starts, so the
        // backend gates indexing correctly from session start (default on).
        bridge()->setE2eeSearchEnabled(_settings.searchEncryptedRooms());
        bridge()->restoreSession(
            accountSettings().sessionHomeserver(),
            accountSettings().sessionUserId(),
            accountSettings().sessionDeviceId(),
            accessToken);
    } else {
        // No saved session: show login, and clear any data left over from a
        // previous (possibly crashed/corrupted) start before the forms are usable.
        showIntro();
        startUnauthorisedCleanup();
    }

    sweepUndeadAccounts();
    startBackgroundAccounts();

    _window->show();
}

void AppController::sweepUndeadAccounts() {
    // Undead entries — settings that claim a session whose secrets are gone —
    // are what haunt the switcher as ghost accounts: hasSession() shields them
    // from the session-less prune, their restore fails forever, and switching
    // to one used to drop the app on the welcome screen. Put them down at
    // startup (before their background bring-up would spin up a dead bridge) and
    // again after each fresh login — a login is often the first point the backend
    // is proven readable, which is what turns a "maybe unreadable" sibling into a
    // provable ghost (see the caller in persistSessionFor).
    //
    // Runs after every unlock flow has resolved, so a locked vault or a
    // still-starting keyring never masquerades as missing secrets.
    const int storeState = ProtocolBridge::secretStoreState();
    if (storeState != 0 && storeState != 3) {
        // Backend unreachable or locked: absence proves nothing. Never wipe.
        return;
    }

    struct Classified {
        QString dirName;
        SecretsState state;
    };
    std::vector<Classified> classified;
    bool anyComplete = false;
    for (int i = 0; i < _domain.count(); ++i) {
        const auto account = _domain.account(i);
        if (!account->settings().hasSession()) {
            continue;
        }
        const auto state = ClassifySessionSecrets(
            account->settings(), account->dirName());
        anyComplete = anyComplete || (state == SecretsState::Complete);
        classified.push_back({ account->dirName(), state });
    }

    bool changed = false;
    for (const auto &entry : classified) {
        // Incomplete is proof on its own (a half-written set can never run).
        // AllAbsent is proof only when another account's complete set showed
        // the backend readable this run — otherwise it may be an unreadable
        // blob, and wiping on that destroyed working sessions before.
        const bool undead = (entry.state == SecretsState::Incomplete)
            || (entry.state == SecretsState::AllAbsent && anyComplete);
        if (!undead) {
            continue;
        }
        const auto index = _domain.indexOfDirName(entry.dirName);
        const auto account = _domain.account(index);
        if (!account) {
            continue;
        }
        if (index == _domain.activeIndex()) {
            // The active account's own startup path handles its failure
            // interactively (retry / sign-out dialog); pulling it out from
            // under that flow here would fight it.
            continue;
        }
        qWarning() << "[accounts] Dropping ghost account" << entry.dirName
            << "(" << account->settings().sessionUserId() << "):"
            << "its session secrets are gone";
        if (_notifications) {
            _notifications->detachAccount(account->dirName());
        }
        account->deleteSecrets();
        const auto dataDir = account->dataDir();
        _domain.remove(index);
        // Without its store passphrases the data is undecryptable junk.
        QDir(dataDir).removeRecursively();
        changed = true;
    }
    if (changed) {
        saveSettings();
        // Drop the ghost from any account list that is already on screen (the
        // switcher rebuilds on this signal). Harmless at startup, where the UI
        // has not been built yet, and needed when the sweep runs post-login.
        Q_EMIT activeAccountProfileChanged();
    }
}

void AppController::startBackgroundAccounts() {
    // Every account stays connected, not just the one on screen — that is what
    // keeps their notifications arriving and their unread counts live.
    //
    // Staggered rather than all at once: each bring-up opens its own SQLite
    // stores, and firing every account's cold start simultaneously makes them
    // contend with the active account's first sync, which is the one the user is
    // actually waiting on.
    int delayMs = 0;
    for (int i = 0; i < _domain.count(); ++i) {
        if (i == _domain.activeIndex()) {
            continue;
        }
        const auto account = _domain.account(i);
        if (!account->settings().hasSession() || account->bridge()) {
            continue;
        }
        delayMs += kBackgroundAccountStartStagger;
        QTimer::singleShot(delayMs, this, [this, dirName = account->dirName()] {
            // Resolve by name, not index: the list may have shifted while we waited.
            const auto index = _domain.indexOfDirName(dirName);
            if (index >= 0) {
                startAccountSession(index);
            }
        });
    }
}

void AppController::startAccountSession(int index) {
    const auto account = _domain.account(index);
    if (!account || account->bridge() || !account->settings().hasSession()) {
        return;
    }

    const auto dataDir = account->dataDir();
    QDir().mkpath(dataDir);
    account->setUnreadStateStore(std::make_unique<UnreadStateStore>(this));
    account->setBridge(std::make_unique<ProtocolBridge>(dataDir));
    wireUnreadBadgeFeed(account);
    wireSavedMessagesCache(account);
    account->setState(Account::State::Restoring);

    // No interactive retry here, unlike the active account's path: a background
    // account must never put a dialog in front of the account being used. A
    // failure is recorded and reported if the user switches to it.
    bool readFailed = false;
    const auto token = ProtocolBridge::keychainLoad(
        SessionSecretKey(QStringLiteral("session_access_token"), account->settings()),
        &readFailed);
    if (token.isEmpty()) {
        qWarning() << "Background account" << account->dirName()
            << "has no readable access token; leaving it signed out for now";
        account->setState(Account::State::RestoreFailed);
        return;
    }

    const auto bridge = account->bridge();
    // Background accounts come up AFTER showMain() has built the notification
    // system, so they have to attach themselves — otherwise this account would
    // sync silently: no desktop notifications, and nothing to route a click back
    // to when one is clicked.
    if (_notifications) {
        _notifications->attachAccount(account->dirName(), bridge);
    }
    connect(bridge, &ProtocolBridge::sessionRestored, this,
            [this, dirName = account->dirName()](
                bool success,
                const QString &userId,
                const QString &displayName,
                const QString &avatarUrl,
                const QString &error) {
        const auto index = _domain.indexOfDirName(dirName);
        const auto account = _domain.account(index);
        if (!account) {
            return;
        }
        if (success) {
            account->setProfile(userId, displayName, avatarUrl);
            account->setState(Account::State::Ready);
            // Its row in the switcher shows this identity even while it is not
            // the account on screen.
            Q_EMIT activeAccountProfileChanged();
        } else {
            qWarning() << "Background account" << dirName << "failed to restore:" << error;
            account->setState(Account::State::RestoreFailed);
        }
    });
    connect(account->unreadStateStore(), &UnreadStateStore::totalUnreadChanged,
            this, [this](int) { refreshNotificationsBadge(); });

    bridge->setE2eeSearchEnabled(_settings.searchEncryptedRooms());
    bridge->restoreSession(
        account->settings().sessionHomeserver(),
        account->settings().sessionUserId(),
        account->settings().sessionDeviceId(),
        token);
}

void AppController::activateAccount(int index, int slideDirection) {
    const auto target = _domain.account(index);
    if (!target || index == _domain.activeIndex() || _switchingAccount) {
        return;
    }
    // A target with no session cannot be presented: it is either the
    // half-finished "add account" (its popup owns its lifecycle) or signed-out
    // residue. Switching the whole app onto it is what used to land on the
    // welcome screen, clobber the window geometry (showIntro centres at the
    // default size) and persist a ghost as the active account.
    if (!target->settings().hasSession()) {
        return;
    }
    _switchingAccount = true;

    // Slide direction: explicit from the cycling shortcuts (so wraparound
    // keeps its perceived direction), by list order for direct jumps.
    const auto direction = (slideDirection != 0)
        ? slideDirection
        : ((index > _domain.activeIndex()) ? 1 : -1);

    // Leaving a half-finished "add account" behind: it has nothing to come back
    // to, so drop it instead of parking an empty account in the list. Indices
    // shift when it goes, so re-resolve the target by name.
    const auto targetDirName = target->dirName();
    if (_domain.count() > 1) {
        discardAccount(_domain.activeIndex());
    }
    index = _domain.indexOfDirName(targetDirName);
    if (index < 0) {
        _switchingAccount = false;
        return;
    }

    // Bring the target up BEFORE any teardown, so a target that cannot come up
    // leaves the account on screen untouched.
    //
    // A failed earlier bring-up (e.g. a transient keychain read right after
    // launch) left a dead unauthenticated bridge behind. Drop it so the switch
    // retries the whole restore instead of presenting the dead twin.
    if (target->bridge()
        && target->state() == Account::State::RestoreFailed) {
        target->setBridge(nullptr);
    }
    // Bring the account up if it never started (its bring-up was still queued
    // behind the stagger, or the retry above dropped the dead bridge).
    if (!target->bridge()) {
        startAccountSession(index);
    }
    // Still dead after the retry: its access token is unreadable or gone.
    // Presenting it would only show the welcome screen over the whole app —
    // the ghost-account failure. Keep the current account on screen and let
    // the user decide what happens to the dead entry.
    if (!target->bridge()
        || target->state() == Account::State::RestoreFailed) {
        HistoryConfirmDialog dialog(
            _window,
            tr("Couldn't open this account"),
            tr("Its saved sign-in can't be read right now. Nothing has been "
               "deleted — you can try again later, or sign this account out "
               "of this device."),
            tr("Sign out account"),
            tr("Not now"),
            HistoryConfirmDialog::Attention);
        if (dialog.exec() == HistoryConfirmDialog::Accepted) {
            removeAccountEntry(_domain.indexOfDirName(targetDirName));
        }
        _switchingAccount = false;
        return;
    }

    // Freeze what is on screen before it goes, so the outgoing account can be
    // slid away over the incoming one once that has been built.
    QPixmap outgoing;
    QRect outgoingGeometry;
    if (_mainWidget) {
        outgoing = _mainWidget->grab();
        outgoingGeometry = _mainWidget->geometry();
    }

    // Tear the UI down but leave every bridge running: the account we are leaving
    // keeps syncing in the background, exactly as it did before it was shown.
    _notifications.reset();
    if (_mainWidget) {
        _mainWidget->disconnect(this);
        _mainWidget = nullptr;
    }
    _intro = nullptr;
    // In-flight media requests belong to the bridge that issued them; the rebuilt
    // UI must be free to ask its own bridge for the same URLs.
    MediaCache::resetPendingRequests();

    _domain.activate(index);
    // Written immediately rather than on the coalescing timer: which account is
    // active is what the next launch restores, and a crash in between would
    // otherwise reopen the wrong one. Only reached for a presentable target —
    // persisting a dead one as active is what made the next launch open the
    // login form instead of the app.
    saveSettings();

    // Keep the window exactly as the user left it — only the contents change.
    showMain(/*restoreWindowGeometry=*/false);
    // Straight from the live bridge's memory: this account has been syncing,
    // so there is nothing to block on.
    const auto rooms = target->bridge()->cachedRooms();
    if (!rooms.isEmpty() && _mainWidget) {
        _mainWidget->applyCachedRooms(rooms);
    }
    // Probe this account's own capabilities. Without it the settings page falls
    // back to the optimistic defaults, which claim more than a given homeserver
    // may actually support.
    watchBridgeStoreErrors();
    prefetchAccountCapabilities();
    refreshNotificationsBadge();
    Q_EMIT activeAccountProfileChanged();
    playAccountSwitchSlide(outgoing, outgoingGeometry, direction);
    _switchingAccount = false;
}

void AppController::removeAccountEntry(int index) {
    // Sign a non-active account out of this device at the user's request: its
    // secrets, its entry and its data dir all go. Unlike discardAccount this
    // also applies to entries that still claim a session — that claim is
    // exactly what makes a dead entry a ghost otherwise.
    const auto account = _domain.account(index);
    if (!account || index == _domain.activeIndex()) {
        return;
    }
    const auto dataDir = account->dataDir();
    if (_notifications) {
        _notifications->detachAccount(account->dirName());
    }
    account->deleteSecrets();
    _domain.remove(index);
    QDir(dataDir).removeRecursively();
    saveSettings();
}

void AppController::playAccountSwitchSlide(
        const QPixmap &outgoing,
        const QRect &geometry,
        int direction) {
    if (outgoing.isNull() || !geometry.isValid() || !_window || !_mainWidget) {
        return;
    }
    // Let the incoming account lay itself out before it is photographed, or the
    // animation would play over a half-built frame.
    _mainWidget->setGeometry(geometry);
    QCoreApplication::sendPostedEvents(_mainWidget, QEvent::LayoutRequest);
    const auto incoming = _mainWidget->grab();
    if (incoming.isNull()) {
        return;
    }

    // Parented to the window rather than the new central widget: it has to sit
    // above the account being revealed, not inside it.
    auto *slide = new AccountSwitchSlide(_window, outgoing, incoming, direction);
    slide->setGeometry(geometry);
    slide->show();
    slide->raise();

    auto *animation = new QVariantAnimation(slide);
    animation->setDuration(st::accountSwitchSlideDuration);
    // Symmetric ease-in-out: a gentle start reads smoother than OutQuint's
    // snap — and the rebuilt UI often drops a frame or two right after the
    // switch, which a slow first phase hides where a fast one amplified it.
    animation->setEasingCurve(QEasingCurve::InOutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    connect(animation, &QVariantAnimation::valueChanged, slide,
        [slide](const QVariant &value) {
            slide->setProgress(value.toReal());
        });
    connect(animation, &QVariantAnimation::finished, slide, [slide] {
        slide->deleteLater();
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void AppController::activateAccountByOrdinal(int ordinal) {
    // 1-based, matching the Ctrl+Shift+N the user pressed.
    activateAccount(ordinal - 1);
}

void AppController::activateAdjacentAccount(int delta) {
    const auto count = _domain.count();
    if (count < 2) {
        return;
    }
    const auto index = _domain.activeIndex() + delta;
    activateAccount(((index % count) + count) % count, delta > 0 ? 1 : -1);
}

void AppController::showAddAccountIntro() {
    if (!_domain.canAddAccount() || _switchingAccount) {
        return;
    }

    // The secrets the new sign-in is about to write need somewhere to go. In a
    // running session the store is already unlocked, so this all but always
    // passes; it matters on a device whose vault was never set up.
    if (checkSecretBackendForNewSession() != SecretSetup::Ready) {
        HistoryConfirmDialog dialog(
            _window,
            tr("Secure storage unavailable"),
            tr("TeleMatrix can't reach secure storage, so it can't save a new "
               "account's sign-in. Unlock your keyring and try again."),
            tr("OK"),
            QString(),
            HistoryConfirmDialog::Normal,
            0,
            -1,
            /*showCancel=*/false);
        dialog.exec();
        return;
    }

    auto pending = std::make_unique<Account>(_domain.allocateDirName());
    const auto dataDir = pending->dataDir();
    QDir().mkpath(dataDir);
    pending->setUnreadStateStore(std::make_unique<UnreadStateStore>(this));
    pending->setBridge(std::make_unique<ProtocolBridge>(dataDir));
    wireUnreadBadgeFeed(pending.get());
    wireSavedMessagesCache(pending.get());

    // Added to the domain so the sign-in machinery has an Account to fill in, but
    // deliberately NOT activated and NOT saved: until it has a session it is only
    // a half-finished attempt, and persisting it as the active account is what
    // made the next launch open the login form instead of the app.
    const auto index = _domain.add(std::move(pending));
    if (index < 0) {
        return;
    }
    const auto account = _domain.account(index);

    // An in-window popup card, not a window of its own: the account already
    // signed in stays visible (dimmed) behind it, and dismissing it cancels
    // adding an account rather than quitting the app. IntroWidget is laid out at
    // its designed proportions inside a large centered card (see
    // DialogsIntroBox / st::introBox*).
    //
    // The popup sits over the running (themed) app, so render the intro in the
    // live theme. Must run BEFORE the IntroWidget/card are built — their palette
    // caches and the card background read the intro colors at construction.
    // Qualified with :: because the local `intro` below shadows the namespace.
    ::intro::applyCurrentTheme();
    auto *intro = new IntroWidget(
        account->bridge(),
        IntroWidget::InitialStep::Login,
        nullptr);
    // A dialog over the running app: the version and key-storage lines belong
    // to the first-run stage, not here.
    intro->setEmbedded(true);
    auto *popup = new DialogsIntroBox(intro, _window);

    connect(account->bridge(), &ProtocolBridge::loginResult, popup,
            [this, account](bool success,
                            const QString &userId,
                            const QString &displayName,
                            const QString &avatarUrl) {
        if (success) {
            persistSessionFor(account, userId, displayName, avatarUrl);
        }
    });
    connect(intro, &IntroWidget::registrationAccepted, popup,
            [this, account](const QString &userId,
                            const QString &displayName,
                            const QString &avatarUrl) {
        persistSessionFor(account, userId, displayName, avatarUrl);
    });
    connect(intro, &IntroWidget::loginSuccess, popup, [this, account, popup](const QString &) {
        // What onLoginSuccess() does for the first account, minus showMain():
        // the switch to this account happens once the popup has closed.
        if (const auto b = account->bridge()) {
            b->setE2eeSearchEnabled(_settings.searchEncryptedRooms());
        }
        popup->accept();
    });

    // Resolved by dirName, never by the index captured at popup-open time: the
    // domain can shift while the popup is up (the same-user dedup in
    // persistSessionFor removes an entry), and a stale index then reads a
    // different account — skipping the discard (a ghost stays behind) or
    // activating the wrong account.
    connect(popup, &DialogsIntroBox::finished, this,
            [this, dirName = account->dirName()](int) {
        const auto account = _domain.account(_domain.indexOfDirName(dirName));
        if (!account) {
            return;
        }
        if (!account->settings().hasSession()) {
            // Closed without signing in: leave nothing behind. Deferred —
            // the secret wipe, bridge teardown (tm_destroy drains the tokio
            // runtime) and directory delete in this handler are what kept
            // the dialog visibly open for seconds after Cancel.
            QTimer::singleShot(0, this, [this, dirName] {
                discardAccount(_domain.indexOfDirName(dirName));
            });
            return;
        }
        saveSettings();
        // Deferred: this runs while the dialog is still finishing, and switching
        // rebuilds the widget tree the dialog is parented into.
        QTimer::singleShot(0, this, [this, dirName] {
            const auto index = _domain.indexOfDirName(dirName);
            if (index >= 0) {
                activateAccount(index);
            }
        });
    });

    popup->show();
    intro->setInnerFocus();
}

void AppController::discardAccount(int index) {
    // An account the user started adding and never signed into has no session,
    // no name and no data worth keeping — drop it rather than leave a nameless
    // row in the switcher and an empty directory on disk.
    const auto account = _domain.account(index);
    if (!account || account->settings().hasSession()) {
        return;
    }
    const auto dataDir = account->dataDir();
    if (_notifications) {
        _notifications->detachAccount(account->dirName());
    }
    account->deleteSecrets();
    _domain.remove(index);
    QDir(dataDir).removeRecursively();
    saveSettings();
}

void AppController::bringToFront() {
    if (_window) {
        _window->bringToFront();
    }
}

void AppController::saveSettingsDelayed() {
    _saveSettingsTimer.start(kSaveSettingsDelay);
}

void AppController::pushRecentEmoji() {
    const auto account = _domain.active();
    if (!account) {
        return;
    }
    account->markRecentEmojiTouchedLocally();
    if (const auto b = account->bridge()) {
        b->setRecentEmoji(FromRecentEmoji(account->settings().recentEmoji()));
    }
}

bool AppController::saveSettings() {
    _saveSettingsTimer.stop();
    const bool ok = Local::writeSettings(_settings, _domain.toIndex());
    if (!ok) {
        // A silent failure here is how ghost/duplicate account entries survive
        // on disk while the runtime state looks correct.
        qWarning() << "[settings] writeSettings failed;"
            << "account/session state NOT persisted";
    }
    return ok;
}

void AppController::refreshNotificationsBadge() {
    if (_notifications) {
        _notifications->refreshBadge();
    }
    // The Linux tray shows the same all-account total as the dock badge, but was
    // wired only to the active account's store — so a background account's unread
    // never reached it. This path fires for every account's unread change; refresh
    // the tray here too (null off Linux), skipping the OS write when unchanged. See MA-8.
    if (_tray) {
        const int total = _domain.totalUnreadBadge(_settings.includeMutedInBadge());
        if (total != _lastTrayUnread) {
            _lastTrayUnread = total;
            _tray->setUnreadCount(total);
        }
    }
}

void AppController::persistCurrentTheme() {
    _settings.setThemeId(_themeManager->themeId());
    _settings.setThemeMode(static_cast<int>(_themeManager->mode()));
    // A manual Day/Night switch turns "Auto-night mode" off in the ThemeManager;
    // persist that so it stays off across restarts and is reflected when the
    // Appearance page is next opened.
    _settings.setSystemDarkModeEnabled(_themeManager->systemDarkModeEnabled());
    saveSettingsDelayed();
}

void AppController::requestThemeSelector() {
    emit themeSelectorRequested();
}

void AppController::beginThemePreview() {
    if (_themePreviewing) {
        return;
    }
    _themeSnapshot = {
        _themeManager->themeId(),
        static_cast<int>(_themeManager->mode()),
        _themeManager->systemDarkModeEnabled(),
    };
    _themePreviewing = true;
}

void AppController::saveThemePreview() {
    _themePreviewing = false;
    persistCurrentTheme();
}

void AppController::endThemePreview() {
    if (!_themePreviewing) {
        return; // saveThemePreview() already committed this one
    }
    // Restore while still previewing, so the themeChanged this provokes doesn't
    // write settings that already hold exactly these values.
    const auto mode = static_cast<Theme::ThemeMode>(_themeSnapshot.mode);
    _themeManager->setThemeAndMode(_themeSnapshot.id, mode);
    // setThemeAndMode pins Day/Night, which turns auto-night off; the snapshot
    // knows whether it was on.
    _themeManager->setSystemDarkModeEnabled(_themeSnapshot.autoNight);
    _themePreviewing = false;
}

void AppController::notifyBackgroundDoodlesChanged(bool enabled) {
    emit backgroundDoodlesChanged(enabled);
}

void AppController::notifyLargeEmojiChanged(bool enabled) {
    emit largeEmojiChanged(enabled);
}

void AppController::notifyReplyButtonOnMessagesChanged(bool enabled) {
    emit replyButtonOnMessagesChanged(enabled);
}

void AppController::notifyReactionButtonOnMessagesChanged(bool enabled) {
    emit reactionButtonOnMessagesChanged(enabled);
}

void AppController::notifyHideSystemMessagesInPublicRoomsChanged(bool enabled) {
    emit hideSystemMessagesInPublicRoomsChanged(enabled);
}

void AppController::notifyIncludeMutedInFoldersChanged() {
    emit includeMutedInFoldersChanged();
}

void AppController::restartApplication() {
    saveSettings();
    const auto appPath = QApplication::applicationFilePath();
    auto args = QApplication::arguments().mid(1);
    // Mark the relaunch so the new instance waits for THIS one to release the
    // single-instance lock (and its exclusive resources) before taking over,
    // instead of deferring to it and exiting — which is what turned a restart
    // into a plain quit (both processes ended up exiting).
    if (!args.contains(QStringLiteral("--relaunched"))) {
        args.append(QStringLiteral("--relaunched"));
    }
    QProcess::startDetached(appPath, args);
    QApplication::quit();
}

void AppController::restartIntoPath(const QString &path) {
    saveSettings();
    if (path.isEmpty()) {
        // A helper process owns the relaunch (macOS and Windows). It is already
        // waiting on this PID, and it will start the NEW build once the swap is
        // done — so all this process may do is exit.
        //
        // It must NOT fall back to restartApplication(): that spawns
        // applicationFilePath(), i.e. the version being replaced. Both it and
        // the helper's relaunch then race for the single-instance lock, and the
        // old one wins (it starts polling seconds earlier), leaving the user on
        // the pre-update build after a restart that looked successful. On
        // Windows it is worse — the stray process holds TeleMatrix.exe open
        // exactly while the NSIS installer is trying to replace it.
        QApplication::quit();
        return;
    }
    auto args = QApplication::arguments().mid(1);
    if (!args.contains(QStringLiteral("--relaunched"))) {
        args.append(QStringLiteral("--relaunched"));
    }
    if (!QProcess::startDetached(path, args)) {
        // AppImageLauncher can interpose here (offering to "integrate" the file,
        // or having moved it). The swap already succeeded, so the update is
        // installed either way — the user just has to start it again.
        qWarning() << "[UPDATE] could not relaunch" << path;
    }
    QApplication::quit();
}

void AppController::setConnectingBottomSkip(int skip) {
    if (_mainWidget) {
        _mainWidget->setConnectingBottomSkip(skip);
    }
}

void AppController::notifyUpdatePolicyChanged() {
    if (!_updateService) {
        return;
    }
    const auto policy = _settings.updatePolicy();
    if (policy == static_cast<int>(Core::UpdateService::Policy::Off)) {
        // Off means exactly zero automatic network calls — including a check
        // that was already queued before the user turned it off.
        _updateCheckTimer.stop();
        _updateStartupTimer.stop();
        return;
    }
    if (!_updateCheckTimer.isActive()) {
        _updateCheckTimer.start();
        // First check shortly after startup (or right after the user turns
        // checking back on), not immediately — the first sync gets the network.
        // start() on an already-running single-shot timer just restarts it, so
        // repeated toggling can never stack up duplicate checks.
        _updateStartupTimer.start();
    }
}

void AppController::setLanguage(const QString &langCode) {
    if (_settings.languageId() == langCode) return;
    _settings.setLanguageId(langCode);
    saveSettings();
    restartApplication();
}

void AppController::showIntro(bool startOnLogin) {
    // The full-window intro is always light ("Dubai"), regardless of the
    // app theme. Reset the intro palette in case an Add-Account popup left it
    // themed (applyCurrentTheme). Visually a no-op on a fresh start.
    intro::applyLight();
    _intro = new IntroWidget(
        bridge(),
        startOnLogin ? IntroWidget::InitialStep::Login
                     : IntroWidget::InitialStep::Welcome,
        _window);
    _window->setCentralWidget(_intro);
    // Welcome screen always uses default centered size,
    // independent of user's saved window position.
    _window->useDefaultCentered();

    connect(_intro, &IntroWidget::loginSuccess,
            this, &AppController::onLoginSuccess);
    connect(_intro, &IntroWidget::registrationAccepted,
            this, &AppController::persistSession);
    // Persist the first-run backend choice at device level so it sticks past logout.
    connect(_intro, &IntroWidget::secretBackendChosen, this, [this](bool vault) {
        _settings.setPreferredSecretBackend(
            vault ? QStringLiteral("vault") : QStringLiteral("keychain"));
        saveSettings();
    });
}

void AppController::showMain(bool restoreWindowGeometry) {
    _mainWidget = new AppMainWidget(this, bridge(), _window);
    _window->setCentralWidget(_mainWidget);
    // The intro widget is owned by the window and will be deleted
    // when setCentralWidget replaces it.
    _intro = nullptr;
    // Restore the user's saved window position — but only when coming from the
    // intro, which centres the window at a fixed size. Switching accounts just
    // rebuilds this subtree inside a window the user has already sized and
    // placed, and re-applying the persisted geometry there would yank it back
    // (to the maximized state it was usually saved in).
    if (restoreWindowGeometry) {
        _window->restoreWindowState();
    }

    // Create notification system.
    _notifications = std::make_unique<Notifications::System>(
        bridge(), _window, _mainWidget, &_settings, unreadStateStore(), &_domain, this);

    // Platform notification manager. macOS has a native (UNUserNotificationCenter)
    // implementation; on other platforms we leave the manager unset for now —
    // Notifications::System null-guards every manager call, so notifications and
    // the dock/taskbar badge cleanly no-op until a native Win/Linux manager lands.
#ifdef Q_OS_MAC
    auto manager = std::make_unique<Notifications::MacManager>();
    _notifications->setManager(std::move(manager));
#elif defined(Q_OS_WIN)
    auto manager = std::make_unique<Notifications::WinManager>(_window);
    _notifications->setManager(std::move(manager));
#elif defined(Q_OS_LINUX)
    auto manager = std::make_unique<Notifications::LinuxManager>(_window);
    _notifications->setManager(std::move(manager));
#endif

    // Every signed-in account notifies, not just this one: they all keep syncing,
    // so a message on a background account has to reach the user.
    for (int i = 0; i < _domain.count(); ++i) {
        const auto account = _domain.account(i);
        if (account->bridge()) {
            _notifications->attachAccount(account->dirName(), account->bridge());
        }
    }

    // Click on notification -> navigate to that room.
    connect(_notifications.get(), &Notifications::System::activateRoom,
            this, [this](
                const QString &accountDirName,
                const QString &roomId,
                const QString &eventId) {
        if (_window) {
            _window->raise();
            _window->activateWindow();
        }
        // Deferred out of the signal emission: switching accounts destroys the
        // Notifications::System that is emitting this very signal (and the main
        // widget the room would open in), so both have to outlive the emit.
        QTimer::singleShot(0, this, [this, accountDirName, roomId, eventId] {
            // The toast may belong to an account that isn't showing. Switch to it
            // FIRST and only then navigate — the room lives in that account's
            // list, so opening it before the switch would look in the wrong one.
            auto index = _domain.indexOfDirName(accountDirName);
            if (index < 0) {
                // The notification outlived the notification system that recorded
                // which account it came from (it is rebuilt on every switch), and
                // desktop notifications can be clicked long after they arrive —
                // so fall back to asking which account actually holds the room.
                index = accountIndexForRoom(roomId);
            }
            if (index >= 0 && index != _domain.activeIndex()) {
                activateAccount(index);
            }
            // Roomless security alerts (a new login) carry an empty id: just
            // focus the app, where the banner offers the actions.
            if (_mainWidget && !roomId.isEmpty()) {
                // Jump to the message the toast was about; showRoomAtEvent falls
                // back to a plain open when the toast carried no event.
                _mainWidget->showRoomAtEvent(roomId, eventId);
            }
        });
    });

    // Inline reply / mark-as-read from a notification action -> the ORIGINATING
    // account's bridge. The toast may belong to a background account, so acting on
    // the active bridge would send from the wrong identity. See MA-3.
    connect(_notifications.get(), &Notifications::System::replyToRoom,
            this, [this](const QString &dir, const QString &roomId, const QString &text) {
        if (auto *b = bridgeForNotification(dir, roomId);
            b && !roomId.isEmpty() && !text.trimmed().isEmpty()) {
            b->sendMessage(roomId, text);
        }
    });
    connect(_notifications.get(), &Notifications::System::markReadRoom,
            this, [this](const QString &dir, const QString &roomId) {
        if (auto *b = bridgeForNotification(dir, roomId); b && !roomId.isEmpty()) {
            b->markRoomRead(roomId, true);
        }
    });

    // When user opens a room, clear its notifications.
    connect(_mainWidget, &AppMainWidget::activeRoomChanged,
            this, [this](const QString &roomId) {
        if (!roomId.isEmpty() && _notifications) {
            _notifications->clearFromRoom(roomId);
        }
    });

    // When a room's unread drops (read here or on another device), clear its
    // stale desktop notifications on a read-inbox update. clearFromRoom is
    // idempotent, so the overlap with the local-open path above is harmless.
    if (unreadStateStore()) {
        connect(unreadStateStore(), &UnreadStateStore::roomReadProgressed,
                this, [this](const QString &roomId) {
            if (!roomId.isEmpty() && _notifications) {
                _notifications->clearFromRoom(roomId);
            }
        });
    }

#ifdef Q_OS_LINUX
    // System-tray icon with the unread badge (Linux StatusNotifierItem). macOS
    // uses the dock badge and Windows the taskbar overlay, so the tray is Linux-
    // only; the count mirrors the dock badge (respects "include muted").
    _tray = std::make_unique<TrayIcon>(_window);
    if (unreadStateStore()) {
        const auto updateTray = [this] {
            if (_tray) {
                // Summed across accounts, like the dock badge: one tray icon has
                // to speak for every account that is still syncing.
                _tray->setUnreadCount(
                    _domain.totalUnreadBadge(_settings.includeMutedInBadge()));
            }
        };
        updateTray();
        connect(unreadStateStore(), &UnreadStateStore::totalUnreadChanged,
                this, [updateTray](int) { updateTray(); });
    }
#endif

    // When window becomes active, refresh the dock badge.
    connect(_window, &AppMainWindow::windowActiveChanged,
            this, [this](bool active) {
        if (active && _notifications) {
            _notifications->refreshBadge();
        }
    });


    // Logout from settings.
    connect(_mainWidget, &AppMainWidget::logoutRequested,
            this, &AppController::handleLogout);
}

void AppController::onLoginSuccess([[maybe_unused]] const QString &userId) {
    // Apply the persisted E2EE-search setting for this session (default on).
    if (bridge()) {
        bridge()->setE2eeSearchEnabled(_settings.searchEncryptedRooms());
    }
    prefetchAccountCapabilities();
    showMain();
}

void AppController::handleLogout() {
    // Confirmation dialog. On confirm it stays open
    // in a busy state (buttons disabled + centered spinner) while the async
    // teardown runs, rather than swapping in a separate "Clearing local data"
    // overlay. finishBusy() closes it once the fresh intro UI is built.
    auto *dialog = new HistoryConfirmDialog(
        _window,
        QString(),
        tr("Are you sure you want to sign out?"),
        tr("Sign out"),
        QString(),
        HistoryConfirmDialog::Attention,
        st::signOutConfirmWidth,
        st::boxButtonPadding.bottom() + Style::ConvertScale(10));

    dialog->setBusyOnConfirm([this, dialog] {
        const auto account = _domain.active();
        // Clear this account's persisted session and secrets immediately. Only
        // its own keys: a sibling account that stays signed in must keep working,
        // which is why this is not the wholesale clear it used to be.
        if (account) {
            account->deleteSecrets();
            account->clear();
        }
        saveSettings();
        // Nothing signed in anywhere any more: wipe the bundle wholesale so a
        // dead install leaves nothing behind in secure storage.
        if (!hasAnySession()) {
            ProtocolBridge::keychainClearAll();
        }

        // Tear down notifications before destroying the main widget.
        _notifications.reset();

        // Clear the media this account's storage backed. Other accounts stay
        // signed in and their cached media must keep working, so this is scoped
        // to the departing account's data dir rather than a wholesale clear.
        if (account) {
            MediaCache::clearForDataDir(account->dataDir());
        }
        if (!hasAnySession()) {
            MediaCache::clearAll();
        }
        HistoryMessage::clearEmojiImageCache();

        // Keep the old bridge alive until Rust logout finishes. The FFI callback is
        // delivered asynchronously; destroying the bridge immediately can leave the
        // callback with a dangling QObject.
        // Drop AppController's existing connections to the bridge first, so a late
        // signal during teardown can't land on AppController slots after release().
        bridge()->disconnect(this);
        auto *loggingOutBridge = account ? account->takeBridge().release() : nullptr;

        // Runs once — whichever of the loggedOut callback or the safety timeout fires first.
        auto done = std::make_shared<bool>(false);
        auto finishLogout = [this, dialog, done]() {
            if (*done) {
                return;
            }
            *done = true;

            // Other accounts are still signed in: hand the app to the next one
            // rather than dropping the user at the login form (tdesktop does the
            // same). The account that just left is removed, and its dir goes with
            // it — the Rust logout already trashed its stores.
            if (_domain.count() > 1) {
                const auto departingIndex = _domain.activeIndex();
                const auto departing = _domain.account(departingIndex);
                const auto departingDir = departing ? departing->dataDir() : QString();
                if (departing && _notifications) {
                    _notifications->detachAccount(departing->dirName());
                }
                _domain.remove(departingIndex);
                if (!departingDir.isEmpty()) {
                    QDir(departingDir).removeRecursively();
                }
                saveSettings();

                if (_mainWidget) {
                    disconnect(_mainWidget, &AppMainWidget::logoutRequested,
                               this, &AppController::handleLogout);
                }
                _mainWidget = nullptr;

                const auto next = _domain.active();
                if (next && !next->bridge()) {
                    startAccountSession(_domain.activeIndex());
                }
                // Handing over to the next account inside the same window: its
                // size and position are the user's, not ours to reset. If its
                // restore failed (unreadable access token), the bridge never
                // authenticated — showing it would present an empty "user
                // unknown" app, so land on the intro instead.
                if (next && next->bridge()
                    && next->state() != Account::State::RestoreFailed) {
                    showMain(/*restoreWindowGeometry=*/false);
                    if (_mainWidget) {
                        const auto rooms = next->bridge()->cachedRooms();
                        if (!rooms.isEmpty()) {
                            _mainWidget->applyCachedRooms(rooms);
                        }
                    }
                } else {
                    showIntro();
                }
                refreshNotificationsBadge();
                dialog->finishBusy();
                return;
            }

            // Same account slot, freshly signed out: give it a new bridge on its
            // own data dir so the intro can sign in again.
            const auto account = _domain.active();
            const QString dataDir = account ? account->dataDir() : QString();
            if (account) {
                account->setBridge(std::make_unique<ProtocolBridge>(dataDir));
                wireUnreadBadgeFeed(account);
                wireSavedMessagesCache(account);
            }
            connect(bridge(), &ProtocolBridge::loginResult,
                    this, [this](bool success, const QString &userId, const QString &displayName, const QString &avatarUrl) {
                if (success) {
                    persistSession(userId, displayName, avatarUrl);
                }
            });
            _storeErrorDialogShown = false;
            watchBridgeStoreErrors();

            // Drop the logout signal so a queued logoutRequested can't re-enter
            // handleLogout after the old main widget is replaced.
            if (_mainWidget) {
                disconnect(_mainWidget, &AppMainWidget::logoutRequested,
                           this, &AppController::handleLogout);
            }
            // The dialog is a window child, so it survives replacing _mainWidget.
            _mainWidget = nullptr;

            // Build the intro UI behind the busy dialog, then close the dialog:
            // its exec() returns and handleLogout deletes it, revealing the intro.
            showIntro();
            dialog->finishBusy();
        };

        if (loggingOutBridge) {
            // Only open the new bridge on the same data dir AFTER the old runtime is
            // fully torn down (tm_destroy returned), so the new tm_create cannot race
            // the old runtime's sqlite teardown on the shared store directory.
            connect(loggingOutBridge, &ProtocolBridge::shutdownComplete, this, finishLogout);
            connect(loggingOutBridge, &ProtocolBridge::loggedOut,
                    loggingOutBridge, [loggingOutBridge](bool success) {
                if (!success) {
                    qWarning() << "[logout] Rust logout reported failure;"
                        << "local data wipe may be incomplete";
                }
                // shutdownAsync (not deleteLater): FFI callbacks fire until the runtime
                // stops. It emits shutdownComplete when tm_destroy returns, which then
                // drives finishLogout().
                loggingOutBridge->shutdownAsync();
            });
            loggingOutBridge->logout();

            // Safety net if the loggedOut callback is lost: force the teardown, which
            // still emits shutdownComplete -> finishLogout.
            QPointer<ProtocolBridge> oldBridge(loggingOutBridge);
            QTimer::singleShot(15000, this, [oldBridge]() {
                if (oldBridge) {
                    oldBridge->shutdownAsync(); // idempotent
                }
            });
            // Ultimate backstop: if shutdownComplete itself never arrives (tm_destroy
            // hung past its cap), recreate the UI anyway so the user isn't stuck on the
            // dialog. finishLogout() is idempotent via its `done` guard.
            QTimer::singleShot(60000, this, finishLogout);
        } else {
            finishLogout();
        }
    });

    dialog->exec();
    dialog->deleteLater();
}

void AppController::watchBridgeStoreErrors() {
    // Wire once per bridge: this runs on start, every account switch, and the logout
    // handover, but Qt::UniqueConnection can't dedupe lambda connects, so a long-lived
    // background bridge would otherwise accumulate duplicate handler sets. Sever any
    // previous set on the live bridge before re-wiring. A destroyed()-driven QSet guard
    // was tried instead but crashed on quit: it fired mid-teardown, after this object's
    // members were already destructed → QHash abort. See MA-9.
    auto *b = bridge();
    if (!b) {
        return;
    }
    disconnect(b, &ProtocolBridge::syncStateChanged, this, nullptr);
    disconnect(b, &ProtocolBridge::accountSummaryReady, this, nullptr);
    disconnect(b, &ProtocolBridge::emailThreepidSupportProbed, this, nullptr);

    // Sync state 3 (SYNC_STATE_STORE_ERROR): the Rust side determined the
    // local encrypted store can no longer decrypt its own values. Retrying
    // sync can never heal it — the user has to act.
    connect(b, &ProtocolBridge::syncStateChanged,
            this, [this](int state) {
        if (state == 3) {
            qWarning() << "[storage] local store error reported by sync loop";
            showStoreErrorDialog();
        }
    });

    // Cache every account-summary result here (not just the ones a currently-open
    // Settings page happens to hear), so a page built later already knows the
    // account capabilities instead of starting from the optimistic default.
    // Bound to the account these signals were wired for, NOT to whichever account
    // happens to be active when they arrive. These are slow probes: switching
    // accounts while one is in flight would otherwise file one homeserver's
    // answer against another — which is how an account whose server cannot verify
    // email ended up being offered the verification field.
    const auto ownerDirName = activeDirName();
    connect(b, &ProtocolBridge::accountSummaryReady,
            this, [this, ownerDirName](
                bool success, const AccountSummary &summary, const QString &) {
        if (!success) {
            return;
        }
        const auto index = _domain.indexOfDirName(ownerDirName);
        if (const auto account = _domain.account(index)) {
            account->setCachedAccountSummary(summary);
        }
    });
    connect(b, &ProtocolBridge::emailThreepidSupportProbed,
            this, [this, ownerDirName](quint64, bool known, bool supported) {
        if (!known) {
            return;
        }
        const auto index = _domain.indexOfDirName(ownerDirName);
        if (const auto account = _domain.account(index)) {
            account->setEmailVerificationSupported(supported);
        }
    });
}

void AppController::prefetchAccountCapabilities() {
    if (!bridge()) {
        return;
    }
    // Skip the network probes if this account's capabilities are already cached
    // (e.g. re-entering it via an account switch). See MA-9.
    if (const auto account = _domain.active();
        account && account->cachedAccountSummaryLoaded()) {
        return;
    }
    bridge()->fetchAccountSummary();
    // Whether email can be verified at all is not in the capabilities: the spec
    // only has the aggregate m.3pid_changes, which reads true on a server that
    // simply has no mail configured. Ask the 3PID endpoint itself instead.
    const auto homeserver = accountSettings().sessionHomeserver();
    if (!homeserver.isEmpty()) {
        bridge()->probeEmailThreepidSupport(homeserver);
    }
}

void AppController::showStoreErrorDialog() {
    if (_storeErrorDialogShown || !_window) {
        return;
    }
    _storeErrorDialogShown = true;

    // Unrecoverable corruption: the logout is forced (there is no "stay" option),
    // but we warn first so the user can confirm their recovery key is backed up.
    // Whatever the user does with the dialog, the logout/cleanup proceeds.
    HistoryConfirmDialog dialog(
        _window,
        tr("Data is corrupted. Forcing logout"),
        tr("TeleMatrix's local data is corrupted and can't be repaired, so you "
           "will be signed out and the local data cleared.\n\n"
           "Make sure your recovery key is backed up — you'll need it to restore "
           "your encrypted message history after signing back in."),
        tr("Sign out"),
        QString(),
        HistoryConfirmDialog::Attention,
        0,
        -1,
        /*showCancel=*/false);
    dialog.exec();
    handleLogout();
}

AppController::SecretSetup AppController::checkSecretBackendForNewSession() {
    // 0 = KeychainReady, 3 = VaultUnlocked: a usable backend is already configured
    // (keychain reachable, or the intro choice step set up the vault). Proceed.
    const int storeState = ProtocolBridge::secretStoreState();
    if (storeState == 0 || storeState == 3) {
        return SecretSetup::Ready;
    }
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    // These platforms always ship a keychain, so state 1 is a failed *read* (a locked
    // keychain, or an item whose ACL belongs to a different build — see the ad-hoc note
    // in keychain.rs), not a device that lacks one. Reporting it as retryable keeps a
    // transient hiccup from stickily moving the device off a perfectly good keychain.
    if (storeState == 1) {
        return SecretSetup::KeychainError;
    }
#endif
    // Keychain unavailable on Linux (no Secret Service), or the vault is locked /
    // was never set up: the in-window create-password step is the way forward.
    return SecretSetup::NeedsMasterPassword;
}

QString AppController::domainDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString AppController::accountDataDir(const QString &dirName) {
    return domainDir() + QStringLiteral("/accounts/") + dirName;
}

bool AppController::unlockSecretStoreBlocking() {
    // Welcome-screen-style unlock shown full-window (the window is already shown).
    // The screen verifies the password itself (wrong → inline error) and the host
    // runs a nested loop until it unlocks or the user confirms a destructive reset.
    auto *screen = new IntroVaultUnlock();
    _window->setCentralWidget(screen); // the window takes ownership
    // Welcome-screen size + centered, like showIntro(); showMain()/showIntro()
    // restore the saved geometry afterward (unlock succeeds / reset falls to login).
    _window->useDefaultCentered();
    screen->activate();

    QEventLoop loop;
    bool unlocked = false;
    connect(screen, &IntroVaultUnlock::unlocked, this, [&] {
        unlocked = true;
        loop.quit();
    });
    connect(screen, &IntroVaultUnlock::resetRequested, this, [&] {
        HistoryConfirmDialog confirm(
            _window,
            tr("Reset local data?"),
            tr("Without your master password, TeleMatrix can't open your local "
               "data. Reset and sign in again? Your messages stay on the server "
               "and re-download after you sign in."),
            tr("Reset"),
            tr("Back"),
            HistoryConfirmDialog::Attention);
        if (confirm.exec() == HistoryConfirmDialog::Accepted) {
            unlocked = false;
            loop.quit();
        }
    });
    // Quitting (Cmd+Q / window close) requests the main loop to exit, but that
    // never unwinds this nested loop — break it explicitly, or the app hangs.
    connect(_window, &AppMainWindow::quitRequested, &loop, [this, &loop] {
        _startupQuitRequested = true;
        loop.quit();
    });
    loop.exec();

    if (_startupQuitRequested) {
        return false; // App is terminating; start() aborts, don't touch the vault.
    }
    if (!unlocked) {
        dropInsecureSession();
    }
    // The unlock screen stays as the central widget until the caller swaps in the
    // main shell (unlocked) or the login intro (reset), which deletes it.
    return unlocked;
}

void AppController::dropInsecureSession() {
    qWarning() << "Dropping insecure or incomplete saved Matrix session";
    if (const auto account = _domain.active()) {
        ClearLocalProtocolStorage(account->dataDir());
        account->deleteSecrets();
        account->clear();
    }
    if (!hasAnySession()) {
        ProtocolBridge::keychainClearAll();
    }
    saveSettings();
}

void AppController::startUnauthorisedCleanup() {
    if (!bridge()) {
        return;
    }
    // Block the welcome/login forms with a "Preparing…" overlay until the
    // leftover-data cleanup finishes (decision: disabled until done).
    if (_intro) {
        _intro->setEnabled(false);
    }
    auto *overlay = new SessionLoadingOverlay(
        tr("Preparing"),
        tr("Clearing leftover data…"),
        _window);
    overlay->show();
    overlay->raise();

    QPointer<SessionLoadingOverlay> overlayPtr = overlay;
    auto finish = [this, overlayPtr] {
        if (overlayPtr) {
            overlayPtr->hide();
            overlayPtr->deleteLater();
        }
        if (_intro) {
            _intro->setEnabled(true);
        }
    };
    // logout() wipes any local data left from a previous start even with no
    // active session (store, caches, keychain). Idempotent and fast when clean.
    connect(bridge(), &ProtocolBridge::loggedOut, this,
            [finish](bool) { finish(); }, Qt::SingleShotConnection);
    // Safety net so the forms are never left blocked if loggedOut is lost. A sign-in
    // can therefore begin while this cleanup is still running; Rust's logout guards
    // against that (it skips its destructive tail once auth_generation moves, and
    // the keychain wipe is epoch-checked under the write lock), so a belated wipe
    // can no longer take the new session's store or secrets with it.
    QTimer::singleShot(15000, this, finish);
    bridge()->logout();
}

} // namespace TeleMatrix
