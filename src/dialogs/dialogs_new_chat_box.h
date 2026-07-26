// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>

#include "../protocol/protocol_types.h"

class QEventLoop;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QScrollArea;
class QTimer;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

class NewChatUserListInner final : public QWidget {
    Q_OBJECT

public:
    explicit NewChatUserListInner(QWidget *parent = nullptr);

    void setUsers(const QVector<UserProfile> &users);
    void setStatusText(const QString &text);
    [[nodiscard]] QVector<UserProfile> users() const;

    // Keep the list at least as tall as the viewport, so a short result set still fills the box
    // and the empty-state text centres over the whole of it (mirrors the explore-rooms box).
    void setViewportHeight(int height);

    // Natural height for the current rows (min the visible-rows floor), ignoring the viewport —
    // the box sizes itself to this so it adapts to the amount of content (tdesktop-style).
    [[nodiscard]] int preferredHeight() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void userClicked(const QString &userId, const QString &displayName);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    [[nodiscard]] int indexAt(const QPoint &pos) const;
    [[nodiscard]] int contentHeight() const;
    void paintRow(QPainter &p, int index, bool hovered);

    QVector<UserProfile> _users;
    QString _statusText;
    int _hovered = -1;
    int _viewportHeight = 0;
};

class DialogsNewChatBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode {
        Rejected = 0,
        Accepted = 1,
    };

    explicit DialogsNewChatBox(
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    [[nodiscard]] QString selectedUserId() const;
    [[nodiscard]] QString selectedDisplayName() const;

Q_SIGNALS:
    void userSelected(const QString &userId, const QString &displayName);

private:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void chooseUser(const QString &userId, const QString &displayName);
    void scheduleSearch(const QString &query);
    void triggerDirectorySearch();
    void applySearchResults(
        const QString &query,
        const QVector<UserProfile> &results);
    void clearResults(const QString &statusText);
    void showInitialUsers(const QString &query = QString());
    void resolveAvatars(const QVector<UserProfile> &users);
    // Grow the scrollable list (and the box with it) to fill the available window height.
    void updateListHeight();

    ProtocolBridge *_bridge = nullptr;
    QWidget *_panel = nullptr;
    NewChatUserListInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    QLineEdit *_searchField = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    QTimer *_searchTimer = nullptr;

    QString _ownUserId;
    QString _lastDirectoryQuery;
    QString _selectedUserId;
    QString _selectedDisplayName;
    QVector<UserProfile> _initialUsers;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix
