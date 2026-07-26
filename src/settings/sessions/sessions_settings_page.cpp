// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/sessions/sessions_settings_page.h"

#include "app/app_controller.h"
#include "protocol/protocol_bridge.h"
#include "settings/dialogs/interactive_auth_dialog.h"
#include "settings/settings_common_widgets.h"
#include "settings/sessions/current_session_card.h"
#include "settings/sessions/session_confirm_dialog.h"
#include "settings/sessions/session_loading_overlay.h"
#include "settings/sessions/session_rename_dialog.h"
#include "settings/sessions/session_row.h"
#include "styles/style_constants.h"
#include "theme/theme_manager.h"
#include "ui/safe_url.h"
#include "ui/widgets/scroll_area.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>

namespace TeleMatrix {
namespace {

QString formatLastSeen(qint64 ts, bool hasTs) {
    if (!hasTs || ts == 0) {
        return QCoreApplication::translate("SettingsWidget", "Unknown");
    }
    return QDateTime::fromSecsSinceEpoch(ts)
        .toLocalTime()
        .toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

QString buildSessionSubtitle(const DeviceSession &s) {
    QStringList parts;
    if (!s.deviceId.isEmpty()) {
        parts << s.deviceId;
    }
    if (!s.os.isEmpty()) {
        parts << s.os;
    } else if (!s.deviceModel.isEmpty()) {
        parts << s.deviceModel;
    }
    if (!s.appName.isEmpty()) {
        auto app = s.appName;
        if (!s.appVersion.isEmpty()) {
            app += QStringLiteral(" ") + s.appVersion;
        }
        parts << app;
    } else if (!s.browser.isEmpty()) {
        parts << s.browser;
    }
    if (!s.lastSeenIp.isEmpty()) {
        parts << s.lastSeenIp;
    }
    const auto ts = formatLastSeen(s.lastSeenTs, s.hasLastSeenTs);
    if (!ts.isEmpty()) {
        parts << ts;
    }
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

QString passwordAuthJson(
        const QString &userId,
        const QString &password,
        const QString &challengeJson = QString()) {
    QString session;
    const auto challenge = QJsonDocument::fromJson(challengeJson.toUtf8());
    if (challenge.isObject()) {
        session = challenge.object().value(QStringLiteral("session")).toString();
    }

    QJsonObject identifier;
    identifier[QStringLiteral("type")] = QStringLiteral("m.id.user");
    identifier[QStringLiteral("user")] = userId;

    QJsonObject auth;
    auth[QStringLiteral("type")] = QStringLiteral("m.login.password");
    if (!session.isEmpty()) {
        auth[QStringLiteral("session")] = session;
    }
    auth[QStringLiteral("identifier")] = identifier;
    auth[QStringLiteral("password")] = password;
    return QString::fromUtf8(QJsonDocument(auth).toJson(QJsonDocument::Compact));
}

QString passwordFromAuthJson(const QString &authJson) {
    const auto doc = QJsonDocument::fromJson(authJson.toUtf8());
    return doc.isObject()
        ? doc.object().value(QStringLiteral("password")).toString()
        : QString();
}

// The portal's devices page (MSC4191), not its generic account page. The
// per-session deep link (device_id-based actions) errors out on matrix.org's
// portal, so the sessions list is the closest page that works. The Rust layer
// may hand us a URL already deep-linked to a per-device action — replace it
// rather than stacking a second action param.
QString sessionsManagementUrl(const QString &accountManagementUrl) {
    auto url = QUrl(accountManagementUrl);
    auto query = QUrlQuery(url);
    query.removeAllQueryItems(QStringLiteral("action"));
    query.removeAllQueryItems(QStringLiteral("device_id"));
    query.addQueryItem(
        QStringLiteral("action"),
        QStringLiteral("org.matrix.sessions_list"));
    url.setQuery(query);
    return url.toString();
}

class SessionSpinner final : public QWidget {
public:
    explicit SessionSpinner(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(20, 20);
        _timer.setInterval(33);
        QObject::connect(&_timer, &QTimer::timeout, this, [this] {
            _angle = (_angle + 30) % 360;
            update();
        });
        _timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto pen = QPen(st::windowActiveTextFg, 2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(rect().adjusted(3, 3, -3, -3), (_angle + 35) * 16, 285 * 16);
    }

private:
    QTimer _timer;
    int _angle = 0;
};

class SessionLoadingRow final : public QWidget {
public:
    explicit SessionLoadingRow(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedHeight(72);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(
            st::settingsButtonPaddingLeft,
            0,
            st::settingsButtonPaddingRight,
            0);
        layout->setSpacing(12);
        layout->addWidget(new SessionSpinner(this), 0, Qt::AlignVCenter);

        auto *label = new QLabel(text, this);
        label->setFont(st::baseFont(13));
        {
            QPalette pal = label->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            label->setPalette(pal);
        }
        layout->addWidget(label, 1, Qt::AlignVCenter);
    }
};

// "Other Sessions" filter tab painted with live st:: colors (replaces an
// inline stylesheet): active tab gets a 2px underline + bold text, hover
// brightens the label to the active color.
class FilterTabButton final : public QAbstractButton {
public:
    FilterTabButton(const QString &text, bool active, QWidget *parent)
        : QAbstractButton(parent)
        , _active(active) {
        setText(text);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setFixedHeight(32);
        QFont f = st::baseFont(13);
        f.setWeight(active ? QFont::DemiBold : QFont::Normal);
        setFont(f);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto fg = (_active || _hovered)
            ? st::windowActiveTextFg
            : st::windowSubTextFg;
        p.setFont(font());
        p.setPen(fg);
        p.drawText(rect(), Qt::AlignCenter, text());
        if (_active) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowActiveTextFg);
            p.drawRect(0, height() - 2, width(), 2);
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(text()) + 24, 32);
    }

private:
    bool _active = false;
    bool _hovered = false;
};

} // namespace

SessionsSettingsPage::SessionsSettingsPage(
        AppController *controller,
        QWidget *parent)
    : ::Ui::RpWidget(parent)
    , _controller(controller) {
    _rootLayout = new QVBoxLayout(this);
    _rootLayout->setContentsMargins(0, 0, 0, 0);
    _rootLayout->setSpacing(0);
    _rootLayout->addStretch(1);

    // This page is long-lived; its labels color themselves from st:: at build
    // time. Rebuild on theme change so they pick up the new theme's tokens.
    if (auto *tm = _controller ? _controller->themeManager() : nullptr) {
        connect(tm, &Theme::ThemeManager::themeChanged, this,
                [this](bool, Theme::ThemeMode) { rebuildUi(_lastDeviceList); });
    }

    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        // Learn up front whether this homeserver manages the account on the web.
        // Without it the only way to find out is to attempt a deletion and read
        // the failure — which means asking for a password that cannot work.
        connect(bridge, &ProtocolBridge::accountManagementProbed,
            this, [this](quint64, bool available, const QString &url) {
                if (available) {
                    _accountManagementUrl = url;
                }
            });
        if (_controller) {
            const auto homeserver =
                _controller->accountSettings().sessionHomeserver();
            if (!homeserver.isEmpty()) {
                bridge->probeAccountManagement(homeserver);
            }
        }
        connect(bridge, &ProtocolBridge::ownDevicesReady,
                this, [this](bool success, const DeviceSessionList &list) {
            if (success) {
                _listLoaded = true;
                rebuildUi(list);
            } else {
                _listLoaded = false;
                showError();
                qWarning() << "Failed to fetch own devices";
            }
        });
        connect(bridge, &ProtocolBridge::devicesDeleted,
                this, [this](bool success, const DeleteDevicesResult &result) {
            if (success && result.completed) {
                hideActionPreloader();
                _pendingDeleteDeviceIds.clear();
                _pendingDeletePassword.clear();
                _pendingDeleteAuthRetries = 0;
                refreshList();
            } else if (!result.challengeJson.isEmpty()) {
                if (!_pendingDeletePassword.isEmpty()
                    && _pendingDeleteAuthRetries == 0) {
                    ++_pendingDeleteAuthRetries;
                    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                        bridge->deleteDevices(
                            _pendingDeleteDeviceIds,
                            passwordAuthJson(
                                _controller ? _controller->userId() : QString(),
                                _pendingDeletePassword,
                                result.challengeJson));
                    } else {
                        hideActionPreloader();
                        _pendingDeleteDeviceIds.clear();
                        _pendingDeletePassword.clear();
                        _pendingDeleteAuthRetries = 0;
                    }
                    return;
                }

                hideActionPreloader();
                _pendingDeletePassword.clear();
                _pendingDeleteAuthRetries = 0;

                // A delegated-auth homeserver can still answer with a UIA
                // challenge it will never accept — the session is only removable
                // on its website. Say so instead of asking for a password that
                // cannot succeed.
                if (!_accountManagementUrl.isEmpty()) {
                    _pendingDeleteDeviceIds.clear();
                    const auto url =
                        sessionsManagementUrl(_accountManagementUrl);
                    auto *webDialog = new SessionConfirmDialog(
                        tr("Sign out session"),
                        tr("This account is managed on the web. Open your account "
                           "page in the browser to sign out this session?"),
                        tr("Open page"),
                        this);
                    if (webDialog->exec() == SessionConfirmDialog::Accepted) {
                        OpenSafeExternalUrl(url);
                    }
                    webDialog->deleteLater();
                    return;
                }

                auto *dialog = new InteractiveAuthDialog(
                    _controller ? _controller->userId() : QString(),
                    result.challengeJson,
                    this,
                    tr("Confirm your identity"),
                    tr("Enter your password to continue."),
                    tr("Confirm"));
                if (dialog->exec() == InteractiveAuthDialog::Accepted) {
                    const auto authJson = dialog->authJson();
                    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                        _pendingDeletePassword = passwordFromAuthJson(authJson);
                        _pendingDeleteAuthRetries = 1;
                        showActionPreloader(
                            tr("Signing out"),
                            tr("Please wait while the session is signed out."));
                        bridge->deleteDevices(_pendingDeleteDeviceIds, authJson);
                    } else {
                        _pendingDeleteDeviceIds.clear();
                        _pendingDeletePassword.clear();
                        _pendingDeleteAuthRetries = 0;
                    }
                } else {
                    _pendingDeleteDeviceIds.clear();
                    _pendingDeletePassword.clear();
                    _pendingDeleteAuthRetries = 0;
                }
                dialog->deleteLater();
            } else if (!result.accountManagementUrl.isEmpty()) {
                // MAS/OAuth homeserver (e.g. matrix.org): the legacy device
                // endpoint is disabled, so sessions are managed on the web.
                hideActionPreloader();
                _pendingDeleteDeviceIds.clear();
                _pendingDeletePassword.clear();
                _pendingDeleteAuthRetries = 0;
                const auto url =
                    sessionsManagementUrl(result.accountManagementUrl);
                auto *dialog = new SessionConfirmDialog(
                    tr("Sign out session"),
                    tr("This account is managed on the web. Open your account "
                       "page in the browser to sign out this session?"),
                    tr("Open page"),
                    this);
                if (dialog->exec() == SessionConfirmDialog::Accepted) {
                    OpenSafeExternalUrl(url);
                }
                dialog->deleteLater();
            } else {
                hideActionPreloader();
                _pendingDeleteDeviceIds.clear();
                _pendingDeletePassword.clear();
                _pendingDeleteAuthRetries = 0;
                qWarning() << "Device deletion failed";
                auto *dialog = new SessionConfirmDialog(
                    tr("Sign out session"),
                    tr("Could not sign out this session. Please try again."),
                    tr("OK"),
                    this);
                dialog->exec();
                dialog->deleteLater();
            }
        });
        connect(bridge, &ProtocolBridge::deviceRenamed,
                this, [this](bool success) {
            hideActionPreloader();
            if (success) {
                refreshList();
            }
        });
    }
}

void SessionsSettingsPage::refreshList() {
    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        showLoading();
        bridge->getOwnDevices();
    }
}

void SessionsSettingsPage::clearBody() {
    clearSettingsLayout(_rootLayout);
}

void SessionsSettingsPage::showLoading() {
    clearBody();
    _rootLayout->addSpacing(14);
    _rootLayout->addWidget(new SessionLoadingRow(
        tr("Loading sessions..."),
        this));
    _rootLayout->addStretch(1);
}

void SessionsSettingsPage::showError() {
    clearBody();
    _rootLayout->addSpacing(14);
    auto *label = new QLabel(tr("Failed to load sessions."), this);
    label->setFont(st::baseFont(13));
    {
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        label->setPalette(pal);
    }
    label->setContentsMargins(
        st::settingsButtonPaddingLeft,
        12,
        st::settingsButtonPaddingLeft,
        12);
    _rootLayout->addWidget(label);
    _rootLayout->addStretch(1);
}

void SessionsSettingsPage::showActionPreloader(
        const QString &title,
        const QString &text) {
    hideActionPreloader();
    _actionPreloader = new SessionLoadingOverlay(title, text, this);
    _actionPreloader->show();
    _actionPreloader->raise();
}

void SessionsSettingsPage::hideActionPreloader() {
    if (!_actionPreloader) {
        return;
    }
    _actionPreloader->hide();
    _actionPreloader->deleteLater();
    _actionPreloader = nullptr;
}

void SessionsSettingsPage::beginDeleteSessions(
        const QStringList &deviceIds,
        const QString &title,
        const QString &description,
        const QString &confirmText) {
    if (deviceIds.isEmpty()) {
        return;
    }
    // Nothing to authenticate with: a delegated-auth homeserver removes sessions
    // on its own website, so asking for a password here would be asking for a
    // credential this account does not have.
    if (!_accountManagementUrl.isEmpty()) {
        const auto url = sessionsManagementUrl(_accountManagementUrl);
        auto *webDialog = new SessionConfirmDialog(
            tr("Sign out session"),
            tr("This account is managed on the web. Open your account page in "
               "the browser to sign out this session?"),
            tr("Open page"),
            this);
        if (webDialog->exec() == SessionConfirmDialog::Accepted) {
            OpenSafeExternalUrl(url);
        }
        webDialog->deleteLater();
        return;
    }
    auto *dialog = new InteractiveAuthDialog(
        _controller ? _controller->userId() : QString(),
        QString(),
        this,
        title,
        description,
        confirmText);
    if (dialog->exec() == InteractiveAuthDialog::Accepted) {
        const auto authJson = dialog->authJson();
        if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
            _pendingDeleteDeviceIds = deviceIds;
            _pendingDeletePassword = passwordFromAuthJson(authJson);
            _pendingDeleteAuthRetries = 0;
            showActionPreloader(
                tr("Sign out session"),
                tr("Please wait while the session change is completed."));
            bridge->deleteDevices(_pendingDeleteDeviceIds, authJson);
        }
    }
    dialog->deleteLater();
}

void SessionsSettingsPage::onRenameSession(
        const QString &deviceId,
        const QString &currentName) {
    auto *dialog = new SessionRenameDialog(currentName, this);
    if (dialog->exec() == SessionRenameDialog::Accepted) {
        const auto newName = dialog->text();
        if (!newName.isEmpty() && newName != currentName) {
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                showActionPreloader(
                    tr("Renaming session"),
                    tr("Please wait while the session name is updated."));
                bridge->renameDevice(deviceId, newName);
            }
        }
    }
    dialog->deleteLater();
}

void SessionsSettingsPage::requestSignOut(const QString &deviceId) {
    if (deviceId.isEmpty()) {
        return;
    }
    beginDeleteSessions(
        { deviceId },
        tr("Sign out session"),
        tr("Enter your password to sign out this session."),
        tr("Sign out"));
}

void SessionsSettingsPage::onTerminateAllOtherSessions() {
    QStringList otherIds;
    for (const auto &s : _lastDeviceList.sessions) {
        if (!s.isCurrent) {
            otherIds.append(s.deviceId);
        }
    }
    if (otherIds.isEmpty()) {
        return;
    }

    beginDeleteSessions(
        otherIds,
        tr("Terminate Other Sessions"),
        tr("Enter your password to terminate all other sessions."),
        tr("Terminate"));
}

void SessionsSettingsPage::rebuildUi(const DeviceSessionList &list) {
    _lastDeviceList = list;
    clearBody();

    // Fixed header (current session + divider + "Other Sessions" title +
    // filter tabs). Only the session list built below it scrolls.
    auto *header = new ::Ui::RpWidget(this);
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    headerLayout->addSpacing(14);

    const DeviceSession *currentSession = nullptr;
    for (const auto &s : list.sessions) {
        if (s.isCurrent) {
            currentSession = &s;
            break;
        }
    }

    if (currentSession) {
        addSettingsSectionTitle(header, headerLayout, tr("Current Session"));

        auto *card = new CurrentSessionCard(*currentSession, header);
        connect(card, &CurrentSessionCard::renameRequested,
            this, &SessionsSettingsPage::onRenameSession);
        connect(card, &CurrentSessionCard::signOutRequested,
            this, [this](const QString &deviceId) {
                beginDeleteSessions(
                    QStringList{deviceId},
                    tr("Sign out"),
                    tr("Enter your password to sign out this session."),
                    tr("Sign out"));
            });
        headerLayout->addWidget(card);

        bool hasOtherSessions = false;
        for (const auto &s : list.sessions) {
            if (!s.isCurrent) {
                hasOtherSessions = true;
                break;
            }
        }
        if (hasOtherSessions) {
            auto *terminateLink = new SettingsLinkButton(
                tr("Terminate all other sessions"),
                st::attentionButtonFg,
                header);
            terminateLink->setClickedCallback([this] {
                onTerminateAllOtherSessions();
            });
            headerLayout->addWidget(terminateLink);
        }
    }

    headerLayout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsDivider(header, headerLayout);
    headerLayout->addSpacing(st::settingsCheckboxesSkip);

    addSettingsSectionTitle(header, headerLayout, tr("Other Sessions"));

    auto *filterBar = new QWidget(header);
    filterBar->setFixedHeight(36);
    auto *filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(
        st::settingsButtonPaddingLeft,
        0,
        st::settingsButtonPaddingRight,
        0);
    filterLayout->setSpacing(0);

    struct TabInfo {
        QString label;
        SessionsFilter filter;
    };
    const QVector<TabInfo> tabs = {
        { tr("All"),        SessionsFilter::All },
        { tr("Verified"),   SessionsFilter::Verified },
        { tr("Unverified"), SessionsFilter::Unverified },
        { tr("Inactive"),   SessionsFilter::Inactive },
    };

    for (const auto &tab : tabs) {
        const bool active = (_filter == tab.filter);
        auto *tabBtn = new FilterTabButton(tab.label, active, filterBar);
        connect(tabBtn, &QAbstractButton::clicked, this, [this, f = tab.filter] {
            _filter = f;
            rebuildUi(_lastDeviceList);
        });
        filterLayout->addWidget(tabBtn);
    }
    filterLayout->addStretch(1);

    headerLayout->addWidget(filterBar);
    headerLayout->addSpacing(4);

    _rootLayout->addWidget(header);

    // Scrollable session list — the only part that scrolls.
    auto *listScroll = new ::Ui::ScrollArea(this);
    listScroll->setWidgetResizable(true);
    listScroll->setFrameShape(QFrame::NoFrame);
    listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *listContent = new ::Ui::RpWidget(listScroll);
    auto *listLayout = new QVBoxLayout(listContent);
    listLayout->setContentsMargins(0, 0, 0, 20);
    listLayout->setSpacing(0);

    QVector<const DeviceSession*> otherSessions;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    constexpr qint64 kInactiveThreshold = 7776000;

    for (const auto &s : list.sessions) {
        if (s.isCurrent) {
            continue;
        }

        switch (_filter) {
        case SessionsFilter::All:
            otherSessions.append(&s);
            break;
        case SessionsFilter::Verified:
            if (s.verificationState == DeviceVerificationState::Verified) {
                otherSessions.append(&s);
            }
            break;
        case SessionsFilter::Unverified:
            if (s.verificationState == DeviceVerificationState::Unverified) {
                otherSessions.append(&s);
            }
            break;
        case SessionsFilter::Inactive:
            if (s.hasLastSeenTs && s.lastSeenTs > 0
                && (now - s.lastSeenTs) > kInactiveThreshold) {
                otherSessions.append(&s);
            }
            break;
        }
    }

    if (otherSessions.isEmpty()) {
        auto *emptyLabel = new QLabel(
            _filter == SessionsFilter::All
                ? tr("No other sessions")
                : tr("No sessions match this filter"),
            listContent);
        emptyLabel->setFont(st::baseFont(13));
        {
            QPalette pal = emptyLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            emptyLabel->setPalette(pal);
        }
        emptyLabel->setContentsMargins(st::settingsButtonPaddingLeft, 12, 0, 12);
        listLayout->addWidget(emptyLabel);
    } else {
        for (const auto *session : otherSessions) {
            auto *row = new SessionRow(*session, buildSessionSubtitle(*session), listContent);
            connect(row, &SessionRow::renameRequested,
                this, &SessionsSettingsPage::onRenameSession);
            connect(row, &SessionRow::signOutRequested,
                this, [this](const QString &deviceId) {
                    beginDeleteSessions(
                        QStringList{deviceId},
                        tr("Sign out"),
                        tr("Enter your password to sign out this session."),
                        tr("Sign out"));
                });
            listLayout->addWidget(row);
        }
    }

    listLayout->addStretch(1);
    listScroll->setWidget(listContent);
    _rootLayout->addWidget(listScroll, 1);
}

} // namespace TeleMatrix
