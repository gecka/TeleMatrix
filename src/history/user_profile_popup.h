// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QImage>
#include <QString>
#include <QVector>
#include <functional>

#include "../protocol/protocol_types.h"

class QKeyEvent;
class QTimer;

namespace TeleMatrix {

class ProtocolBridge;

/// User profile popup shown when clicking a sender avatar in the timeline.
/// Displayed as a layer via LayerStackWidget.
class UserProfilePopup : public QWidget {
    Q_OBJECT

public:
    explicit UserProfilePopup(
        const QString &roomId,
        const QString &userId,
        // The name the user is looking at in the timeline, when opened from a message. Authoritative
        // over the fetched profile — in unjoined/preview rooms the fetch returns the global name,
        // which can differ from or be missing the room member name. Empty when opened from a link.
        const QString &knownDisplayName,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    QSize sizeHint() const override;

Q_SIGNALS:
    void closeRequested();
    void mentionRequested(const QString &roomId, const QString &userId, const QString &displayName);
    void openRoomRequested(const QString &roomId);
    void sizeHintChanged();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    struct ActionRow {
        QString label;
        QString value;
        bool isDanger = false;
        bool enabled = true;
        std::function<void()> callback;
        bool valueActive = false;
        bool muteLabelWhenDisabled = true;
    };

    void buildActions();
    void paintTopBar(QPainter &p);
    void paintCover(QPainter &p);
    void paintActions(QPainter &p);
    void showPowerLevelDialog();
    QRect actionRowRect(int index) const;
    int actionRowAt(QPoint pos) const;
    int contentStartY() const;
    QRect userIdCopyRect() const;

    QString _roomId;
    QString _userId;
    QString _knownDisplayName;
    ProtocolBridge *_bridge = nullptr;

    UserProfileDetails _details;
    QString _statusText;

    QVector<ActionRow> _actions;
    int _hoveredAction = -1;
    bool _closeHovered = false;
    bool _userIdCopyHovered = false;
    bool _ignoreUpdatePending = false;
    bool _powerLevelUpdatePending = false;
    bool _directRoomPending = false;
    bool _detailsFetchFinished = false;
    bool _detailsReady = false;

    QImage _closeIcon;
    QImage _closeIconOver;
    QImage _copyIcon;
    QImage _copyIconOver;
    QTimer *_loadingTimer = nullptr;
};

} // namespace TeleMatrix
