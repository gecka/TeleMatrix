// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QLineEdit>
#include <QPushButton>

#include <functional>

/// Shared controls for the intro flow, implementing the 2026-07 redesign.
///
/// These used to be per-file copies: `IntroLineEdit` was duplicated in five
/// steps and `IntroLinkButton` in six, each drifting slightly. The redesign
/// touches every one of them, so they live here once instead — a sixth copy of
/// the new field style is exactly the thing that goes stale.
///
/// Everything paints from the live `intro::` palette, so the full-window intro
/// (always warm-paper light) and the in-app Add-Account popup (live app theme)
/// share one implementation.
///
/// Deliberately in the GLOBAL `intro` namespace, matching `intro_colors.h`. A
/// `TeleMatrix::intro` would shadow it from inside `namespace TeleMatrix`, and
/// every existing `intro::bgActive` in the intro would silently stop resolving.
namespace intro {

/// Design metrics, in logical px. `st::baseFont` applies the interface scale, so
/// these are pre-scale values straight from the spec.
///
/// Fractional sizes round DOWN (12.5 -> 12, 13.5 -> 13), except the 14.5px
/// action size which stays 15 to keep buttons and card titles clearly above the
/// body copy. Rounding everything up made both descriptions sit a size too close
/// to their headings.
namespace metrics {
inline constexpr int fieldHeight = 44;
inline constexpr int fieldRadius = 9;
inline constexpr int fieldPaddingH = 13;
inline constexpr int buttonHeight = 46;
inline constexpr int buttonRadius = 9;
inline constexpr int cardRadius = 11;
inline constexpr int cardPaddingV = 15;
inline constexpr int cardPaddingH = 17;
inline constexpr int panelRadius = 14;
inline constexpr int headingSize = 23;
inline constexpr int labelSize = 12;
inline constexpr int fieldSize = 15;
inline constexpr int bodySize = 13;   // 13.5 in the spec
inline constexpr int smallSize = 12;  // 12.5 in the spec
} // namespace metrics

/// h2: 23px/700, -.02em. Used for every screen heading.
[[nodiscard]] QFont headingFont();
/// 14.5px/600 button and card-title text.
[[nodiscard]] QFont actionFont();

/// Single-line field: h44, radius 9, translucent fill, 1px border that turns
/// accent-coloured on focus.
class Field : public QLineEdit {
public:
    explicit Field(QWidget *parent);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *e) override;

private:
    void applyPalette();
};

/// Primary action: h46, radius 9, white on accent.
class FilledButton : public QPushButton {
public:
    FilledButton(const QString &text, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool _hovered = false;
};

/// Secondary action: h46, radius 9, bordered and translucent rather than filled.
class GhostButton : public QPushButton {
public:
    GhostButton(const QString &text, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool _hovered = false;
};

/// Text-only link. `muted` renders it in the secondary grey instead of the
/// accent — the design uses that for "Skip for now", which should not compete
/// with the real action.
class LinkButton : public QPushButton {
public:
    LinkButton(const QString &text, QWidget *parent, bool muted = false);

    void setMuted(bool muted);

protected:
    [[nodiscard]] QSize sizeHint() const override;
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool _muted = false;
    bool _hovered = false;
};

/// Bordered translucent card. With `selectable` it grows a radio dot on the left
/// and paints a 2px accent ring when chosen (the "Protect your data" options);
/// without, it is a plain clickable card that only highlights its border on
/// hover (the "Verify this session" methods).
class OptionCard : public QPushButton {
public:
    OptionCard(
        const QString &title,
        const QString &description,
        bool selectable,
        QWidget *parent);

    void setSelected(bool selected);
    [[nodiscard]] bool selected() const { return _selected; }
    /// Replace the description. Used where the reason a card is unavailable is
    /// more useful than its generic blurb.
    void setDescription(const QString &description);

    /// Height needed for the given width, since the description wraps.
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    [[nodiscard]] int textLeft() const;

    QString _title;
    QString _description;
    bool _selectable = false;
    bool _selected = false;
    bool _hovered = false;
};

} // namespace intro
