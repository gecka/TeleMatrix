// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

#include <functional>

class QAction;

namespace TeleMatrix::HistoryPopupMenuStyle {

enum class Variant {
    Default,
    WithIcons,
    Folders,
};

class PopupMenu : public QWidget {
    Q_OBJECT
public:
    explicit PopupMenu(Variant variant, QWidget *parent = nullptr);
    ~PopupMenu() override;

    QAction *addAction(const QString &text);
    QAction *addAction(const QString &text, std::function<void()> callback);
    void addSeparator();
    void setSubmenu(QAction *action, PopupMenu *submenu);
    QAction *addSubmenu(PopupMenu *submenu);

    void setTitle(const QString &title);
    [[nodiscard]] QString title() const;

    void popup(const QPoint &globalPos);
    void hideMenu();

    [[nodiscard]] QList<QAction *> actions() const;
    [[nodiscard]] Variant variant() const;

    // For mouse delegation from parent popup.
    void forwardMouseMove(const QPoint &globalPos);
    void forwardMousePress(const QPoint &globalPos);

    void setReactionStrip(const QString &eventId);

signals:
    void aboutToHide();
    void reactionChosen(const QString &eventId, const QString &emojiKey);
    void reactionExpandRequested(const QString &eventId, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    struct Item {
        QAction *action = nullptr;
        bool isSeparator = false;
        PopupMenu *submenu = nullptr;
        QRect rect;
        int height = 0;
        std::function<void()> callback;
    };

    void recalculateLayout();
    [[nodiscard]] int calculateBodyWidth() const;
    [[nodiscard]] const QMargins &itemPadding() const;
    [[nodiscard]] int scrollPaddingTop() const;
    [[nodiscard]] int scrollPaddingBottom() const;
    [[nodiscard]] int itemIndexAt(const QPoint &localPos) const;
    [[nodiscard]] bool containsGlobal(const QPoint &globalPos) const;
    void setActiveIndex(int index);
    void showSubmenuForItem(int index);
    void hideActiveSubmenu();
    void triggerAction(int index);
    void navigateItems(int delta);

    const Variant _variant;
    QVector<Item> _items;
    int _bodyWidth = 0;
    int _activeIndex = -1;
    PopupMenu *_visibleSubmenu = nullptr;
    PopupMenu *_parentMenu = nullptr;
    QString _title;

    // Reaction strip
    static constexpr int kStripHeight = 40;
    static constexpr int kStripCellSize = 32;
    static constexpr int kStripEmojiSize = 21;
    static constexpr int kStripSkipX = 7;
    static constexpr int kStripRadius = 20;
    static constexpr int kStripBubbleRight = 20;
    static constexpr int kStripColumns = 8;
    static constexpr int kBubbleTriangleH = 6;
    static constexpr int kStripTotalHeight = kStripHeight + kBubbleTriangleH;
    static constexpr int kStripMenuGap = 4;  // visual gap between strip and menu

    bool _hasReactionStrip = false;
    QString _reactionEventId;
    int _hoveredEmojiIndex = -1;
    static const QStringList &stripEmoji();
};

PopupMenu *createStyledMenu(QWidget *parent, Variant variant = Variant::Default);
void setActionIconName(QAction *action, const QString &iconName);
void setActionRightIconName(QAction *action, const QString &iconName);

} // namespace TeleMatrix::HistoryPopupMenuStyle
