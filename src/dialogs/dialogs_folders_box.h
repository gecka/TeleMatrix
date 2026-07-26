// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QEventLoop;
class QScrollArea;
class QVariantAnimation;

namespace TeleMatrix {

/// One manageable (custom) folder shown in the Folders popup.
struct FolderManagerEntry {
    int id = 0;
    QString name;
    int chatCount = 0;
};

/// Inner list for the Folders popup, a Settings → Folders layout:
/// a "My folders" subtitle, one two-line row per custom folder (icon + name +
/// "N chats" + trash), then a filled-circle "Create new folder" row.
class FolderManagerInner final : public QWidget {
    Q_OBJECT

public:
    explicit FolderManagerInner(QWidget *parent = nullptr);

    void setFolders(const QVector<FolderManagerEntry> &folders);

    // Folders marked for removal (trash clicked); applied when the popup closes.
    [[nodiscard]] QVector<int> removedIds() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void createRequested();
    void editRequested(int filterId);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    // Element index: -1 none, 0..n-1 folder, n = "Create new folder".
    int elementAt(const QPoint &pos) const;
    int folderTop(int index) const;
    QRect trashRect(int rowTop) const;
    int contentHeight() const;

    QVector<FolderManagerEntry> _entries;
    QSet<int> _removed;          // ids marked for removal (grayed, "Undo")
    int _hovered = -1;
    bool _hoverTrash = false;
};

/// Modal "Folders" manager popup (opened by the sidebar Edit button). Chrome-less
/// to match the folders section: the scrollable list IS the content.
class DialogsFoldersBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit DialogsFoldersBox(QWidget *parent = nullptr);

    /// Refresh the listed folders (safe to call while the popup is open).
    void setFolders(const QVector<FolderManagerEntry> &folders);

    int exec();

signals:
    void createFolderRequested();
    void editFolderRequested(int filterId);
    void deleteFolderRequested(int filterId);
    void folderOrderChanged(const QVector<int> &folderIds);

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void reject();
    void relayoutList();

    QWidget *_panel = nullptr;
    FolderManagerInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix
