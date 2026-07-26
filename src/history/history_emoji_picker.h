// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QScrollArea;
class QScrollBar;
class QVBoxLayout;

namespace Ui {
class InputField;
} // namespace Ui

namespace TeleMatrix {

class AppController;
class EmojiSectionedGrid;
class EmojiTabBar;

class HistoryEmojiPicker final : public QWidget {
    Q_OBJECT

public:
    enum class PickerMode { Compose, Reaction };

    explicit HistoryEmojiPicker(
        AppController *controller,
        QWidget *parent = nullptr);

    /// Apply ConvertScale to emoji panel pixel constants (call once at startup).
    static void initEmojiPanelPxValues();

    [[nodiscard]] static int shadowExtend();
    [[nodiscard]] static int panelWidth();
    [[nodiscard]] static int minBodyHeight();
    [[nodiscard]] static int maxBodyHeight();
    QSize sizeHint() const override;

    void setMode(PickerMode mode);
    void setReactionTarget(const QString &eventId);

signals:
    void emojiSelected(const QString &emoji);
    void reactionSelected(const QString &eventId, const QString &emoji);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void rebuildGrid();
    void scrollToSection(int sectionIndex);
    void onScrollChanged();
    void refreshScaleDependentUi();
    void applyScrollStyle();

    Ui::InputField *_search = nullptr;
    QScrollArea *_scroll = nullptr;
    EmojiSectionedGrid *_grid = nullptr;
    EmojiTabBar *_tabBar = nullptr;
    QVector<int> _sectionTabIndex; // grid section -> footer tab (sectionInfos) index
    QVBoxLayout *_rootLayout = nullptr;
    QWidget *_searchWrap = nullptr;
    QVBoxLayout *_searchLayout = nullptr;
    bool _scrollingToSection = false;
    bool _hideAfterEmojiSelection = false;
    bool _arrowOverrideActive = false;
    AppController *_controller = nullptr;
    PickerMode _pickerMode = PickerMode::Compose;
    QString _reactionTargetEventId;
    bool _gridDirty = true;
};

} // namespace TeleMatrix
