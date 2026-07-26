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
class QLineEdit;
class QScrollArea;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// Inner list widget: custom-painted vertical member list
/// (peer-list-item style rows).
class MemberListInner final : public QWidget {
    Q_OBJECT

public:
    explicit MemberListInner(QWidget *parent = nullptr);

    void setMembers(const QVector<UserProfile> &members);
    void setFilter(const QString &query);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void memberClicked(const QString &userId, const QString &displayName);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int indexAt(const QPoint &pos) const;
    void paintRow(QPainter &p, int filteredIndex, bool hovered);
    int contentHeight() const;

    QVector<UserProfile> _allMembers;
    QVector<int> _filtered;
    int _hovered = -1;
    // False until members have been supplied, so the empty state can say "Loading…" rather than
    // "No members found" while the async fetch is still in flight.
    bool _loaded = false;
};

/// Modal overlay dialog: "Show messages from" member picker.
/// A search-from peer-list box.
class MemberPickerBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit MemberPickerBox(
        const QVector<UserProfile> &members,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    // Populate (or replace) the member list after construction — lets the box open immediately in
    // a "Loading…" state and fill in when the async members fetch lands.
    void setMembers(const QVector<UserProfile> &members);

    int exec();

    [[nodiscard]] QString selectedUserId() const;
    [[nodiscard]] QString selectedDisplayName() const;

Q_SIGNALS:
    void memberSelected(const QString &userId, const QString &displayName);

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    QWidget *_panel = nullptr;
    MemberListInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    QLineEdit *_searchField = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ProtocolBridge *_bridge = nullptr;

    QString _selectedUserId;
    QString _selectedDisplayName;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix
