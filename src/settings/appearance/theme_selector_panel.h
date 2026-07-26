// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QScrollArea;
class QVariantAnimation;

namespace Ui { class TextButton; }

namespace TeleMatrix {

class AppController;
namespace Theme { class ThemeManager; }

/// The theme cards: one row per theme, a day and a night card side by side.
/// Each card is a schematic app window (wallpaper gradient, received + sent
/// bubble, radio) rather than a flat colour swatch.
class ThemeSelectorInner final : public QWidget {
    Q_OBJECT

public:
    ThemeSelectorInner(Theme::ThemeManager *themeManager, QWidget *parent = nullptr);

    /// Cards are sized from the available width, so the panel must ask for the
    /// height it will need before the scroll area has laid us out.
    [[nodiscard]] int contentHeightFor(int width) const;

    /// Where the current theme sits, so the panel can open scrolled to it.
    /// Null when no card matches (a theme this build doesn't ship).
    [[nodiscard]] QRect selectedBlockRect() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    // One card. The pixmap is the wallpaper gradient with the two bubbles drawn
    // in; the radio is painted on top so selection changes need no rebuild.
    // Built on first paint: with twenty themes only the handful of cards
    // actually scrolled into view ever composite a gradient.
    struct Card {
        QString themeId;
        QString name;
        bool night = false;
        QPixmap pixmap; // dropped on resize / dpr change
        QColor accent;  // radio dot, contrast-corrected against the wallpaper
        QColor ring;    // radio outline when unselected
    };

    [[nodiscard]] int themeCount() const;
    [[nodiscard]] int cardWidthFor(int width) const;
    [[nodiscard]] int cardHeightFor(int width) const;
    [[nodiscard]] int themeBlockHeightFor(int width) const;
    [[nodiscard]] QRect cardRect(int themeIndex, bool night) const;
    [[nodiscard]] int cardAt(const QPoint &pos) const; // index into _cards, -1 none
    [[nodiscard]] bool isSelected(const Card &card) const;

    void dropCards();       // on resize / dpr change
    void buildCard(Card &card, QSize size, qreal dpr);

    Theme::ThemeManager *_themeManager = nullptr;
    QVector<Card> _cards;
    QSize _builtFor;
    qreal _builtDpr = 0.;
    int _hovered = -1;
};

/// The "Color theme" picker: a side panel that slides in from the right edge,
/// occupying a side column. It does not dim the app -- the point is
/// to watch the real chat re-skin behind it as you click through the themes.
///
/// Clicking a card applies its theme immediately but does not write it to disk.
/// Save persists the applied theme and closes. Escape (or the close button)
/// simply closes: the previewed theme stays for this session, and the next
/// start comes up with whatever was last saved.
class ThemeSelectorPanel final : public QWidget {
    Q_OBJECT

public:
    ThemeSelectorPanel(AppController *controller, QWidget *parent);

    void showAnimated();
    void hideAnimated();
    [[nodiscard]] bool isShown() const;

Q_SIGNALS:
    /// Emitted once the panel has finished sliding out (saved or not).
    void closed();

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    enum class State { Hidden, Opening, Visible, Closing };

    [[nodiscard]] int panelWidth() const;
    void startAnimation(qreal to);
    void applyProgress(qreal progress);
    void syncGeometry();
    void layoutBars(); // the close button; everything else is in the layout
    void applyTheme(); // re-skin our own chrome after a live theme change
    void scrollToSelected();
    void save();

    AppController *_controller = nullptr;
    Theme::ThemeManager *_themeManager = nullptr;
    QLabel *_title = nullptr;
    QWidget *_topBar = nullptr;
    QWidget *_close = nullptr;
    ThemeSelectorInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    ::Ui::TextButton *_apply = nullptr;

    QVariantAnimation *_animation = nullptr;
    qreal _progress = 0.;
    State _state = State::Hidden;
};

} // namespace TeleMatrix
