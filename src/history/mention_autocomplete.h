// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>

#include "protocol/protocol_types.h"

namespace Ui {
class ScrollArea;
} // namespace Ui

namespace TeleMatrix {

/// Floating popup showing filtered room members for @mention autocomplete.
/// A paint-based row list (Inner)
/// hosted in a scroll area, clamped to ~4.5 visible rows and anchored flush
/// above the composer.
class MentionAutocomplete : public QWidget {
    Q_OBJECT

public:
    explicit MentionAutocomplete(QWidget *parent);

    void setMembers(const QVector<UserProfile> &members);
    void updateFilter(const QString &query);
    bool moveSelection(int delta);
    bool chooseSelected();
    // Repaint the visible rows (e.g. when avatars finish loading).
    void refresh();

signals:
    void mentionChosen(
        const QString &userId,
        const QString &displayName,
        bool roomMention);

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    class Inner;

    void relayout();
    void ensureSelectedVisible();

    ::Ui::ScrollArea *_scroll = nullptr;
    Inner *_inner = nullptr;
};

} // namespace TeleMatrix
