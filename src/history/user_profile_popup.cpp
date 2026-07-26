// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "user_profile_popup.h"
#include "trust_shield.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "settings/dialogs/verify_session_dialog.h"
#include "styles/style_constants.h"
#include "ui/internal_choice_dialog.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/empty_userpic.h"
#include "ui/toast_widget.h"

namespace TeleMatrix {

namespace {

constexpr int kFullArcLength = 360 * 16;
constexpr int kQuarterArcLength = 90 * 16;

QImage loadTemplateIcon(const QString &basePath, const QColor &color) {
    return Style::IconProvider::tintedIcon(basePath, QString(), color);
}

QImage loadColorizedIcon(const QString &name, const QColor &color) {
    return loadTemplateIcon(
        QStringLiteral(":/settings_icons/") + name,
        color);
}

QRect closeButtonRect(int width, int topBarHeight) {
    return QRect(
        width - st::settingsCloseButtonSize,
        0,
        st::settingsCloseButtonSize,
        topBarHeight);
}

QSize iconLogicalSize(const QImage &icon, int fallbackSize) {
    if (icon.isNull()) {
        return QSize(fallbackSize, fallbackSize);
    }
    const auto dpr = icon.devicePixelRatio();
    return QSize(
        qRound(icon.width() / dpr),
        qRound(icon.height() / dpr));
}

QPoint closeIconTopLeft(int width, int topBarHeight, const QImage &icon) {
    const auto button = closeButtonRect(width, topBarHeight);
    const auto iconSize = iconLogicalSize(icon, st::userProfileCloseButtonSize);
    return QPoint(
        button.x() + st::userProfileCloseIconLeft,
        (topBarHeight - iconSize.height()) / 2);
}

QString formatLastSeenTimestamp(qint64 ts) {
    if (ts <= 0) {
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "last seen recently");
    }

    const auto lastSeenDt = QDateTime::fromSecsSinceEpoch(ts);
    const auto nowDt = QDateTime::currentDateTime();
    const auto diffSecs = lastSeenDt.secsTo(nowDt);

    if (diffSecs < 60) {
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "last seen just now");
    } else if (diffSecs < 3600) {
        const auto minutes = diffSecs / 60;
        return QCoreApplication::translate(
            "TeleMatrix::UserProfilePopup",
            "last seen %n minute(s) ago",
            nullptr,
            int(minutes));
    } else if (diffSecs < 43200) {
        const auto hours = diffSecs / 3600;
        return QCoreApplication::translate(
            "TeleMatrix::UserProfilePopup",
            "last seen %n hour(s) ago",
            nullptr,
            int(hours));
    }

    const auto locale = QLocale();
    if (lastSeenDt.date() == nowDt.date()) {
        const auto time = locale.toString(
            lastSeenDt.time(), QLocale::ShortFormat);
        return QCoreApplication::translate(
            "TeleMatrix::UserProfilePopup",
            "last seen today at %1").arg(time);
    } else if (lastSeenDt.date().addDays(1) == nowDt.date()) {
        const auto time = locale.toString(
            lastSeenDt.time(), QLocale::ShortFormat);
        return QCoreApplication::translate(
            "TeleMatrix::UserProfilePopup",
            "last seen yesterday at %1").arg(time);
    }

    const auto date = locale.toString(
        lastSeenDt.date(), QLocale::ShortFormat);
    return QCoreApplication::translate(
        "TeleMatrix::UserProfilePopup",
        "last seen %1").arg(date);
}

QString formatProfileStatus(const UserProfileDetails &details, const QString &fallbackUserId) {
    switch (details.presence) {
    case PresenceState::Online:
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "online");
    case PresenceState::Unavailable:
    case PresenceState::Offline:
        if (details.lastActiveTs > 0) {
            return formatLastSeenTimestamp(details.lastActiveTs);
        }
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "last seen recently");
    }
    return fallbackUserId;
}

qreal loadingSpinnerPhase() {
    const auto period = qMax(1, st::radialPeriod);
    return (QDateTime::currentMSecsSinceEpoch() % period) / qreal(period);
}

void paintLoadingSpinner(QPainter &p, const QRect &area) {
    const int spinnerSize = st::userProfileCloseButtonSize;
    const QRect spinnerRect(
        area.center().x() - spinnerSize / 2,
        area.center().y() - spinnerSize / 2,
        spinnerSize,
        spinnerSize);
    const auto arcRect = QRectF(spinnerRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);
    QPen pen(st::windowActiveTextFg);
    pen.setWidthF(st::uploadRadialLine);
    pen.setCapStyle(Qt::RoundCap);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(
        arcRect,
        kQuarterArcLength - qRound(loadingSpinnerPhase() * kFullArcLength),
        -kFullArcLength / 4);
    p.restore();
}

QString powerLevelValueText(qint64 level) {
    if (level == 100) {
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "Admin (%1)").arg(level);
    } else if (level == 50) {
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "Moderator (%1)").arg(level);
    } else if (level == 0) {
        return QCoreApplication::translate("TeleMatrix::UserProfilePopup", "Default (%1)").arg(level);
    }
    return QString::number(level);
}

MemberRole roleForPowerLevel(qint64 level) {
    if (level >= 100) {
        return MemberRole::Administrator;
    } else if (level >= 50) {
        return MemberRole::Moderator;
    }
    return MemberRole::User;
}

struct PowerLevelChoice {
    qint64 level = 0;
    QString title;
    bool enabled = true;
};

QVector<PowerLevelChoice> powerLevelChoices(qint64 current, qint64 maxAssignable) {
    QVector<PowerLevelChoice> result;
    const auto addChoice = [&](qint64 level, const QString &title) {
        result.push_back({
            level,
            title,
            level <= maxAssignable || level == current,
        });
    };

    const auto knownLevel = [](qint64 level) {
        return level == 100 || level == 50 || level == 0;
    };
    if (!knownLevel(current)) {
        addChoice(current, powerLevelValueText(current));
    }
    addChoice(100, powerLevelValueText(100));
    addChoice(50, powerLevelValueText(50));
    addChoice(0, powerLevelValueText(0));
    return result;
}

} // namespace

UserProfilePopup::UserProfilePopup(
    const QString &roomId,
    const QString &userId,
    const QString &knownDisplayName,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent)
    , _roomId(roomId)
    , _userId(userId)
    , _knownDisplayName(knownDisplayName)
    , _bridge(bridge)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    _details.roomId = _roomId;
    _details.userId = _userId;
    // Show the timeline name straight away (and keep it across the async fetch below).
    if (!_knownDisplayName.isEmpty()) {
        _details.displayName = _knownDisplayName;
    }
    _closeIcon = loadColorizedIcon(QStringLiteral("info_close"), st::settingsCloseIconFg);
    _closeIconOver = loadColorizedIcon(QStringLiteral("info_close"), st::settingsCloseIconFgOver);
    _copyIcon = loadTemplateIcon(
        QStringLiteral(":/telematrix/icons/chat/mini_copy"),
        st::userProfileStatusFg);
    _copyIconOver = loadTemplateIcon(
        QStringLiteral(":/telematrix/icons/chat/mini_copy"),
        st::windowActiveTextFg);
    _loadingTimer = new QTimer(this);
    _loadingTimer->setInterval(33);
    connect(_loadingTimer, &QTimer::timeout, this, [this] {
        if (!_detailsFetchFinished) {
            update();
        }
    });

    if (_bridge) {
        connect(_bridge, &ProtocolBridge::userProfileDetailsReady,
                this, [this](const QString &roomId,
                             const QString &userId,
                             bool success,
                             const UserProfileDetails &details) {
            if (roomId != _roomId || userId != _userId) {
                return;
            }
            if (!_detailsFetchFinished && _loadingTimer) {
                _loadingTimer->stop();
            }
            const bool hadDetails = _detailsReady;
            _detailsFetchFinished = true;
            if (success) {
                _detailsReady = true;
                // Cross-signing trust is delivered by a separate async query +
                // live signals (applyTrust), never by the profile-details FFI —
                // so preserve it across this wholesale overwrite, otherwise a
                // details result that lands after the (faster, local) trust query
                // resets the shield to Unverified and the action row reverts from
                // "Withdraw verification" back to "Verify User".
                const auto knownTrust = _details.trustState;
                _details = details;
                _details.trustState = knownTrust;
                if (_details.roomId.isEmpty()) {
                    _details.roomId = _roomId;
                }
                if (_details.userId.isEmpty()) {
                    _details.userId = _userId;
                }
                // The timeline name wins over the fetched (possibly global/preview) one.
                if (!_knownDisplayName.isEmpty()) {
                    _details.displayName = _knownDisplayName;
                } else if (_details.displayName.isEmpty()) {
                    _details.displayName = _userId;
                }
                _statusText = formatProfileStatus(_details, _userId);
            } else if (!hadDetails) {
                _detailsReady = false;
                _details = UserProfileDetails();
                _details.roomId = _roomId;
                _details.userId = _userId;
                _details.displayName = _knownDisplayName.isEmpty()
                    ? _userId
                    : _knownDisplayName;
                _statusText = tr("User info unavailable");
            }
            buildActions();
            if (_detailsReady
                && _details.avatarUrl.startsWith(QStringLiteral("mxc://"))
                && MediaCache::needsResolution(_details.avatarUrl)) {
                MediaCache::markRequested(_details.avatarUrl);
                _bridge->resolveAvatar(_details.avatarUrl);
            }
            update();
        });
        connect(_bridge, &ProtocolBridge::presenceChanged,
                this, [this](const QString &userId, int state, qint64 lastActiveTs) {
            if (userId != _userId) {
                return;
            }
            if (!_detailsReady) {
                return;
            }
            switch (state) {
            case 1:
                _details.presence = PresenceState::Online;
                break;
            case 2:
                _details.presence = PresenceState::Unavailable;
                break;
            default:
                _details.presence = PresenceState::Offline;
                break;
            }
            _details.lastActiveTs = lastActiveTs;
            _statusText = formatProfileStatus(_details, _userId);
            update();
        });
        connect(_bridge, &ProtocolBridge::userIgnoredSet,
                this, [this](bool success, bool ignored) {
            if (!_ignoreUpdatePending) {
                return;
            }
            _ignoreUpdatePending = false;
            if (!success) {
                return;
            }
            _details.isIgnored = ignored;
            buildActions();
            update();
        });
        connect(_bridge, &ProtocolBridge::userPowerLevelSet,
                this, [this](
                    const QString &roomId,
                    const QString &userId,
                    bool success,
                    qint64 powerLevel) {
            if (!_powerLevelUpdatePending || roomId != _roomId || userId != _userId) {
                return;
            }
            _powerLevelUpdatePending = false;
            if (success) {
                _details.powerLevel = powerLevel;
                _details.role = roleForPowerLevel(powerLevel);
                _bridge->getUserProfileDetailsAsync(_roomId, _userId);
            }
            buildActions();
            update();
        });
        connect(_bridge, &ProtocolBridge::directRoomCreated,
                this, [this](const QString &userId, bool success, const QString &roomId) {
            if (!_directRoomPending || userId != _userId) {
                return;
            }
            _directRoomPending = false;
            if (success && !roomId.isEmpty()) {
                _details.dmRoomId = roomId;
                emit openRoomRequested(roomId);
                emit closeRequested();
                return;
            }
            buildActions();
            update();
        });
        connect(_bridge, &ProtocolBridge::mediaResolved,
                this, [this](bool, const QString &mxcUrl, const QString &) {
            if (mxcUrl == _details.avatarUrl) {
                update();
            }
        });
        const auto applyTrust = [this](const QString &userId, int state) {
            if (userId != _userId) {
                return;
            }
            _details.trustState = static_cast<UserTrustState>(state);
            buildActions();
            update();
        };
        connect(_bridge, &ProtocolBridge::userTrustStateResult, this, applyTrust);
        connect(_bridge, &ProtocolBridge::userTrustChanged, this, applyTrust);
        _loadingTimer->start();
        _bridge->getUserProfileDetailsAsync(_roomId, _userId);
        _bridge->userTrustState(_userId);
    } else {
        _detailsFetchFinished = true;
        _details.roomId = _roomId;
        _details.userId = _userId;
        _details.displayName = _userId;
        _statusText = tr("User info unavailable");
    }

    buildActions();
}

QSize UserProfilePopup::sizeHint() const {
    const int actionsH = _actions.size() * st::userProfileActionRowHeight;
    const int totalH = contentStartY() + actionsH + st::userProfileBottomSkip;
    return QSize(st::userProfilePopupWidth, totalH);
}

int UserProfilePopup::contentStartY() const {
    const int topBarH = st::userProfileTopBarHeight;
    const int avatarSize = st::userProfileAvatarSize;
    const int avatarY = topBarH + st::userProfileAvatarTop;
    const auto nameFm = QFontMetrics(st::userProfileNameFont());
    const int nameY = avatarY + avatarSize + st::userProfileNameTop;
    const auto idFm = QFontMetrics(st::userProfileStatusFont());
    const int idY = nameY + nameFm.height() + st::userProfileStatusTop;
    const auto statusFm = QFontMetrics(st::userProfileStatusFont());
    const int statusY = idY + idFm.height() + st::userProfileStatusTop;
    return statusY + statusFm.height() + st::userProfileActionsTopSkip;
}

QRect UserProfilePopup::userIdCopyRect() const {
    const int topBarH = st::userProfileTopBarHeight;
    const int avatarY = topBarH + st::userProfileAvatarTop;
    const int nameY = avatarY
        + st::userProfileAvatarSize
        + st::userProfileNameTop;
    const auto nameFm = QFontMetrics(st::userProfileNameFont());
    const int idY = nameY + nameFm.height() + st::userProfileStatusTop;
    const auto idFm = QFontMetrics(st::userProfileStatusFont());
    const auto idText = idFm.elidedText(
        _userId,
        Qt::ElideRight,
        width() - st::userProfileUserIdSideSkip);
    const int idTextWidth = idFm.horizontalAdvance(idText);
    const int blockWidth = idTextWidth
        + st::userProfileCopyIconSkip
        + st::userProfileCopyIconSize;
    const int textX = (width() - blockWidth) / 2;
    const int iconX = textX + idTextWidth + st::userProfileCopyIconSkip;
    const int iconY = idY + (idFm.height() - st::userProfileCopyIconSize) / 2;
    return QRect(
        iconX,
        iconY,
        st::userProfileCopyIconSize,
        st::userProfileCopyIconSize);
}

void UserProfilePopup::buildActions() {
    _actions.clear();

    const auto finish = [this] {
        updateGeometry();
        if (auto *parent = parentWidget()) {
            parent->update();
        }
        emit sizeHintChanged();
    };

    if (!_detailsFetchFinished) {
        finish();
        return;
    }

    ProtocolBridge::SessionInfo session;
    if (_bridge) {
        session = _bridge->cachedSessionInfo();
    }
    const bool isSelf = (session.userId == _userId);

    if (_detailsReady) {
        ActionRow powerLevelRow;
        powerLevelRow.label = tr("Power level");
        powerLevelRow.value = _powerLevelUpdatePending
            ? tr("Updating...")
            : powerLevelValueText(_details.powerLevel);
        powerLevelRow.enabled = !_powerLevelUpdatePending && _details.canChangePowerLevel;
        powerLevelRow.callback = [this] {
            showPowerLevelDialog();
        };
        powerLevelRow.valueActive = powerLevelRow.enabled;
        powerLevelRow.muteLabelWhenDisabled = false;
        _actions.append(std::move(powerLevelRow));
    }

    if (!isSelf) {
        _actions.append({
            _directRoomPending
                ? tr("Opening chat...")
                : tr("Send message"),
            QString(),
            false,
            !_directRoomPending && (!_details.dmRoomId.isEmpty() || _bridge != nullptr),
            [this] {
                if (!_details.dmRoomId.isEmpty()) {
                    emit openRoomRequested(_details.dmRoomId);
                    emit closeRequested();
                    return;
                }
                if (_bridge && !_directRoomPending) {
                    _directRoomPending = true;
                    buildActions();
                    update();
                    _bridge->createDirectRoom(_userId);
                }
            }
        });
    }

    _actions.append({
        tr("Copy profile link"),
        QString(),
        false,
        true,
        [this] {
            const auto link = QStringLiteral("https://matrix.to/#/%1").arg(_userId);
            QApplication::clipboard()->setText(link);
            ::Ui::ShowToast(tr("Link copied to clipboard"));
        }
    });

    if (!_detailsReady) {
        finish();
        return;
    }

    if (!isSelf) {
        // "Mention" action.
        _actions.append({
            tr("Mention in Chat"),
            QString(),
            false,
            true,
            [this] {
                emit mentionRequested(
                    _roomId,
                    _userId,
                    _details.displayName.isEmpty() ? _userId : _details.displayName);
                emit closeRequested();
            }
        });
        // Verify User (cross-signing). Requires OUR own verification to be set up
        // first; until then the row is disabled with an explanation.
        {
            const bool ourVerificationReady = _bridge && _bridge->isDeviceVerified();
            const auto trust = _details.trustState;
            if (trust == UserTrustState::Violation) {
                // Their cross-signing identity changed (verification violation).
                // A published signature can't be retracted, so we can't truly
                // "un-verify" — but withdrawing acknowledges the new identity and
                // clears the warning: the SDK's withdraw_verification() pins the
                // current key and drops the previously-verified latch, resolving
                // the state to Unverified.
                _actions.append({
                    tr("Withdraw verification"),
                    tr("Identity changed"),
                    true,
                    true,
                    [this] {
                        if (_bridge) {
                            _bridge->withdrawUserVerification(_userId);
                        }
                    },
                    false,
                    true,
                });
            } else if (trust == UserTrustState::Unverified) {
                // Unverified: offer to verify. A verified user (incl. verified
                // with unverified sessions) gets no row — you cannot un-verify
                // another user in Matrix, so there is nothing actionable to show.
                _actions.append({
                    tr("Verify User"),
                    ourVerificationReady
                        ? QString()
                        : tr("Set up verification first"),
                    false,
                    ourVerificationReady,
                    [this] {
                        if (!_bridge || !_bridge->isDeviceVerified()) {
                            return;
                        }
                        const auto name = _details.displayName.isEmpty()
                            ? _userId
                            : _details.displayName;
                        auto *dialog = new VerifySessionDialog(
                            _bridge,
                            window(),
                            VerifySessionDialog::StartMode::Emoji,
                            QString(),
                            _userId,
                            name);
                        dialog->exec();
                        dialog->deleteLater();
                    },
                    false,
                    true,
                });
            }
        }
        _actions.append({
            _details.isIgnored
                ? tr("Unignore User")
                : tr("Ignore User"),
            QString(),
            !_details.isIgnored,
            !_ignoreUpdatePending,
            [this] {
                if (_bridge && !_ignoreUpdatePending) {
                    _ignoreUpdatePending = true;
                    _bridge->setUserIgnored(_userId, !_details.isIgnored);
                }
            }
        });

        if (_details.canKick && _details.membership != MembershipState::Ban) {
            _actions.append({
                tr("Remove from Room"),
                QString(),
                true,
                true,
                [this] {
                    if (_bridge) {
                        _bridge->kickUser(_roomId, _userId);
                    }
                    emit closeRequested();
                }
            });
        }

        if (_details.canBan) {
            if (_details.membership == MembershipState::Ban) {
                _actions.append({
                    tr("Unban User"),
                    QString(),
                    false,
                    true,
                    [this] {
                        if (_bridge) {
                            _bridge->unbanUser(_roomId, _userId);
                        }
                        emit closeRequested();
                    }
                });
            } else {
                _actions.append({
                    tr("Ban User"),
                    QString(),
                    true,
                    true,
                    [this] {
                        if (_bridge) {
                            _bridge->banUser(_roomId, _userId);
                        }
                        emit closeRequested();
                    }
                });
            }
        }
    }

    finish();
}

void UserProfilePopup::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // The profile header and the action section are separate painted areas
    // inside the same rounded layer.
    {
        PainterHighQualityEnabler hq(p);
        const auto fullRect = rect();
        p.setPen(Qt::NoPen);
        p.setBrush(st::userProfileTopBg);
        p.drawRoundedRect(fullRect, st::boxRadius, st::boxRadius);

        const int actionsTop = _detailsFetchFinished
            ? qBound(
                0,
                contentStartY()
                    - st::userProfileActionsTopSkip
                    + st::userProfileTopBottomPadding,
                height())
            : height();
        if (actionsTop < height()) {
            QPainterPath clip;
            clip.addRoundedRect(
                QRectF(fullRect),
                st::boxRadius,
                st::boxRadius);
            p.save();
            p.setClipPath(clip);
            p.fillRect(
                QRect(0, actionsTop, width(), height() - actionsTop),
                st::userProfileActionsBg);
            p.restore();
        }
    }

    paintTopBar(p);
    paintCover(p);
    paintActions(p);
}

void UserProfilePopup::paintTopBar(QPainter &p) {
    const int w = width();
    const int h = st::userProfileTopBarHeight;

    // Title.
    p.setPen(st::boxTitleFg);
    p.setFont(*st::boxTitleFont);
    p.drawText(
        st::boxTitlePosition.x(),
        st::boxTitlePosition.y() + p.fontMetrics().ascent(),
        tr("User Info"));

    // Close button: same icon asset, size and placement as RoomSettingsWidget.
    const auto &icon = _closeHovered ? _closeIconOver : _closeIcon;
    const auto iconTopLeft = closeIconTopLeft(w, h, icon);
    if (!icon.isNull()) {
        p.drawImage(iconTopLeft, icon);
    } else {
        p.setPen(QPen(_closeHovered ? st::boxTitleCloseFgOver : st::boxTitleCloseFg, 2));
        p.drawLine(
            iconTopLeft,
            iconTopLeft + QPoint(
                st::userProfileCloseButtonSize,
                st::userProfileCloseButtonSize));
        p.drawLine(
            iconTopLeft + QPoint(st::userProfileCloseButtonSize, 0),
            iconTopLeft + QPoint(0, st::userProfileCloseButtonSize));
    }
}

void UserProfilePopup::paintCover(QPainter &p) {
    const int w = width();
    const int topBarH = st::userProfileTopBarHeight;
    const int avatarSize = st::userProfileAvatarSize;
    const int avatarX = (w - avatarSize) / 2;
    const int avatarY = topBarH + st::userProfileAvatarTop;

    if (!_detailsFetchFinished) {
        paintLoadingSpinner(
            p,
            QRect(0, topBarH, w, contentStartY() - topBarH));
        return;
    }

    // Avatar.
    const qreal dpr = devicePixelRatioF();
    bool painted = false;
    if (!_details.avatarUrl.isEmpty()) {
        const auto pix = MediaCache::loadAvatarPixmap(_details.avatarUrl, avatarSize, dpr);
        if (!pix.isNull()) {
            p.drawPixmap(avatarX, avatarY, pix);
            painted = true;
        }
    }
    if (!painted) {
        ::Ui::EmptyUserpic::paint(p, _userId, _details.displayName, avatarX, avatarY, avatarSize);
    }

    // Display name.
    const int nameY = avatarY + avatarSize + st::userProfileNameTop;
    p.setPen(st::userProfileNameFg);
    p.setFont(st::userProfileNameFont());
    const auto nameRect = QRect(0, nameY, w, p.fontMetrics().height() * 2);
    const auto displayName = _details.displayName.isEmpty()
        ? _userId
        : _details.displayName;
    const auto elidedName = p.fontMetrics().elidedText(displayName, Qt::ElideRight, w - 48);
    p.drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop, elidedName);

    // Trust badge to the right of the (centered) display name — "verified" /
    // "unverified sessions" / "identity changed".
    if (const auto trustText = trustBadgeText(_details.trustState); !trustText.isEmpty()) {
        const QFontMetrics badgeFm(st::baseFont(10, true));
        const int badgeH = 16;
        const int badgeW = badgeFm.horizontalAdvance(trustText) + 10;
        // Name font is still active on the painter here.
        const int nameWidth = p.fontMetrics().horizontalAdvance(elidedName);
        const int badgeX = (w + nameWidth) / 2 + st::userProfileCopyIconSkip;
        const int badgeY = nameY + (p.fontMetrics().height() - badgeH) / 2;
        const QRect badgeRect(badgeX, badgeY, badgeW, badgeH);
        p.setPen(Qt::NoPen);
        p.setBrush(trustBadgeColor(_details.trustState));
        p.drawRoundedRect(badgeRect, 5, 5);
        p.setFont(st::baseFont(10, true));
        p.setPen(st::activeButtonFg);
        p.drawText(badgeRect, Qt::AlignCenter, trustText);
    }

    const int idY = nameY + QFontMetrics(st::userProfileNameFont()).height() + st::userProfileStatusTop;
    p.setPen(st::userProfileUserIdFg);
    p.setFont(st::userProfileStatusFont());
    const auto idMetrics = p.fontMetrics();
    const auto idText = idMetrics.elidedText(
        _userId,
        Qt::ElideRight,
        w - st::userProfileUserIdSideSkip);
    const auto idTextWidth = idMetrics.horizontalAdvance(idText);
    const auto copyRect = userIdCopyRect();
    const int idBlockWidth = idTextWidth
        + st::userProfileCopyIconSkip
        + copyRect.width();
    const int idTextX = (w - idBlockWidth) / 2;
    p.drawText(
        QRect(idTextX, idY, idTextWidth, idMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        idText);
    const auto &copyIcon = _userIdCopyHovered ? _copyIconOver : _copyIcon;
    if (!copyIcon.isNull()) {
        p.drawImage(copyRect, copyIcon);
    }

    const int statusY = idY + idMetrics.height() + st::userProfileStatusTop;
    p.setPen(st::userProfileStatusFg);
    p.setFont(st::userProfileStatusFont());
    p.drawText(QRect(0, statusY, w, p.fontMetrics().height()),
               Qt::AlignHCenter, _statusText);
}

void UserProfilePopup::paintActions(QPainter &p) {
    for (int i = 0; i < _actions.size(); ++i) {
        const auto &action = _actions[i];
        const auto rowRect = actionRowRect(i);

        // Hover highlight.
        if (action.enabled && i == _hoveredAction) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::userProfileActionBgOver);
            p.drawRect(rowRect);
        }

        // Label.
        const auto labelColor = (!action.enabled && action.muteLabelWhenDisabled)
            ? st::windowSubTextFg
            : action.isDanger
            ? st::userProfileActionDangerFg
            : st::userProfileActionFg;
        p.setPen(labelColor);
        p.setFont(st::userProfileActionFont());
        auto textRect = rowRect.adjusted(
            st::userProfileActionLeftPad,
            0,
            -st::userProfileActionLeftPad,
            0);
        if (!action.value.isEmpty()) {
            const auto valueWidth = QFontMetrics(st::userProfileActionFont())
                .horizontalAdvance(action.value);
            const auto valueRect = QRect(
                textRect.right() - valueWidth,
                textRect.top(),
                valueWidth,
                textRect.height());
            p.setPen(action.valueActive
                ? st::windowActiveTextFg
                : st::windowSubTextFg);
            p.drawText(valueRect, Qt::AlignVCenter | Qt::AlignRight, action.value);
            textRect.setRight(valueRect.left() - st::userProfileActionValueSkip);
            p.setPen(labelColor);
        }
        p.drawText(
            textRect,
            Qt::AlignVCenter | Qt::AlignLeft,
            action.label);
    }
}

QRect UserProfilePopup::actionRowRect(int index) const {
    return QRect(
        0,
        contentStartY() + index * st::userProfileActionRowHeight,
        width(),
        st::userProfileActionRowHeight);
}

void UserProfilePopup::showPowerLevelDialog() {
    if (!_bridge || !_detailsReady || !_details.canChangePowerLevel || _powerLevelUpdatePending) {
        return;
    }

    QVector<Ui::InternalChoiceEntry> entries;
    const auto choices = powerLevelChoices(
        _details.powerLevel,
        _details.maxAssignablePowerLevel);
    entries.reserve(choices.size());
    for (const auto &choice : choices) {
        entries.push_back({
            QString::number(choice.level),
            choice.title,
            QString(),
            st::baseFont(14),
            choice.enabled,
        });
    }

    Ui::InternalChoiceDialog dialog(
        this,
        tr("Power level"),
        std::move(entries),
        QString::number(_details.powerLevel));
    if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
        return;
    }
    bool parsed = false;
    const auto level = dialog.chosenId().toLongLong(&parsed);
    if (!parsed) {
        return;
    }
    if (level == _details.powerLevel || level > _details.maxAssignablePowerLevel) {
        return;
    }
    _powerLevelUpdatePending = true;
    buildActions();
    update();
    _bridge->setUserPowerLevel(_roomId, _userId, level);
}

int UserProfilePopup::actionRowAt(QPoint pos) const {
    const int cs = contentStartY();

    if (pos.y() < cs) return -1;
    const int row = (pos.y() - cs) / st::userProfileActionRowHeight;
    if (row < 0 || row >= _actions.size()) return -1;
    return row;
}

void UserProfilePopup::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }

    if (closeButtonRect(width(), st::userProfileTopBarHeight).contains(e->pos())) {
        emit closeRequested();
        return;
    }
    if (_detailsFetchFinished
        && userIdCopyRect().adjusted(-4, -4, 4, 4).contains(e->pos())) {
        QApplication::clipboard()->setText(_userId);
        ::Ui::ShowToast(tr("User ID copied to clipboard"));
        return;
    }

    // Action row hit test.
    const int row = actionRowAt(e->pos());
    if (row >= 0
        && row < _actions.size()
        && _actions[row].enabled
        && _actions[row].callback) {
        _actions[row].callback();
    }
}

void UserProfilePopup::mouseMoveEvent(QMouseEvent *e) {
    const bool closeHovered = closeButtonRect(
        width(),
        st::userProfileTopBarHeight).contains(e->pos());
    const bool copyHovered = _detailsFetchFinished
        && !closeHovered
        && userIdCopyRect().adjusted(-4, -4, 4, 4).contains(e->pos());
    const int rawRow = (closeHovered || copyHovered) ? -1 : actionRowAt(e->pos());
    const int row = (rawRow >= 0
            && rawRow < _actions.size()
            && _actions[rawRow].enabled)
        ? rawRow
        : -1;
    if (closeHovered != _closeHovered
        || copyHovered != _userIdCopyHovered
        || row != _hoveredAction) {
        _closeHovered = closeHovered;
        _userIdCopyHovered = copyHovered;
        _hoveredAction = row;
        setCursor((_closeHovered || _userIdCopyHovered || row >= 0)
            ? Qt::PointingHandCursor
            : Qt::ArrowCursor);
        update();
    }
}

void UserProfilePopup::leaveEvent(QEvent *e) {
    if (_closeHovered || _userIdCopyHovered || _hoveredAction >= 0) {
        _closeHovered = false;
        _userIdCopyHovered = false;
        _hoveredAction = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
    QWidget::leaveEvent(e);
}

void UserProfilePopup::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        emit closeRequested();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace TeleMatrix
