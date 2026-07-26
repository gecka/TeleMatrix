// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>
#include <QSet>

#include "../protocol/protocol_types.h"

class QEventLoop;
class QLineEdit;
class QScrollArea;
class QTimer;
class QVariantAnimation;
class QLabel;
class QVBoxLayout;

namespace Ui {
class TextButton;
class CloseButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;
class InviteSearchListInner;

/// A single selected-user chip/pill in the MultiSelect bar.
/// 32px tall, rounded, removable.
struct InviteChip {
    QString userId;
    QString displayName;
    QRect rect;       // computed during layout
    QRect removeRect; // the "x" button hit area
};

/// Multi-select chip bar: wrapping flow layout of user chips + inline search input.
/// Serves as the peer-list box header.
class InviteChipBar final : public QWidget {
    Q_OBJECT

public:
    explicit InviteChipBar(QWidget *parent = nullptr);

    void addChip(const QString &userId, const QString &displayName);
    void removeChip(const QString &userId);
    bool hasChip(const QString &userId) const;
    int chipCount() const;
    QVector<InviteChip> chips() const;

    QLineEdit *inputField() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void chipAdded(const QString &userId);
    void chipRemoved(const QString &userId);
    void heightChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void relayout();

    QVector<InviteChip> _chips;
    QLineEdit *_input = nullptr;
    int _hoveredRemove = -1;
};

/// Modal overlay: "Invite Users" box shown after room creation.
/// Peer-list box with a multi-select chip header.
class InviteUsersBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit InviteUsersBox(
        const QString &roomId,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr,
        bool excludeExistingMembers = false);

    int exec();

    [[nodiscard]] QVector<QString> selectedUserIds() const;

Q_SIGNALS:
    void invitesSent();

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void tryAddUser();
    void startInvites();
    void sendNextInvite();
    void triggerDirectorySearch();
    void applySearchResults(const QString &query, const QVector<UserProfile> &results, bool limited);
    [[nodiscard]] bool isExcludedUser(const QString &userId) const;
    void clearSearchResults();
    void updateSearchResultsVisibility();
    void updateInviteButton();
    void setControlsEnabled(bool enabled);
    void setStatusText(const QString &text, bool error = false);

    QString _roomId;
    ProtocolBridge *_bridge = nullptr;

    QWidget *_panel = nullptr;
    InviteChipBar *_chipBar = nullptr;
    QScrollArea *_resultsScroll = nullptr;
    InviteSearchListInner *_resultsInner = nullptr;
    QLabel *_hintLabel = nullptr;
    ::Ui::CloseButton *_close = nullptr;
    ::Ui::TextButton *_skip = nullptr;
    ::Ui::TextButton *_invite = nullptr;
    QTimer *_searchTimer = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QVector<QString> _pendingInviteIds;
    QVector<QString> _failedInviteIds;
    QString _currentInviteUserId;
    QString _lastDirectoryQuery;
    QSet<QString> _excludedUserIds;
    int _successfulInvites = 0;
    bool _controlsEnabled = true;
    bool _invitesInFlight = false;
    bool _memberExclusionReady = true;
    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix
