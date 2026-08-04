// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "ui/widgets/input_fields.h"

using namespace Ui;

namespace {

// st::defaultInputField's shape at scale 100: a flat field (borderRadius 0) with a
// 28px top strip reserved for the floating caption.
[[nodiscard]] st::InputFieldStyle FlatStyle() {
    st::InputFieldStyle style;
    style.heightMin = 55;
    style.heightMax = 148;
    style.border = 1;
    style.borderActive = 2;
    style.borderRadius = 0;
    style.textMargins = QMargins(0, 28, 0, 4);
    style.placeholderMargins = QMargins();
    return style;
}

[[nodiscard]] InputChrome::State StateFor(
        const st::InputFieldStyle &style,
        const QRect &rect,
        qreal focusedProgress) {
    InputChrome::State state;
    state.style = &style;
    state.rect = rect;
    state.textMargins = style.textMargins;
    state.focusedProgress = focusedProgress;
    return state;
}

// The surface a QTextEdit's paintEvent can reach: its viewport, inset by the text
// margins. This is what the four popup forms were painting their chrome onto.
[[nodiscard]] QRect ViewportSurface(const QRect &rect, const QMargins &textMargins) {
    return rect.marginsRemoved(textMargins);
}

} // namespace

class TestInputFieldChrome : public QObject {
    Q_OBJECT
private slots:
    // The regression: the underline sits on the field's last row, so a surface inset
    // by the text margins clips it away completely. Painting the chrome onto a
    // QTextEdit's viewport is therefore always wrong, at any size — it must go on a
    // surface covering the whole field.
    void underlineEscapesTheViewportSurface() {
        const auto style = FlatStyle();
        const QRect rect(0, 0, 300, 55);
        const auto underline = UnderlineRect(StateFor(style, rect, 0.));

        QVERIFY(!underline.isEmpty());
        QVERIFY(rect.contains(underline));
        QVERIFY(!ViewportSurface(rect, style.textMargins).intersects(underline));
    }

    // Same for the compact (single-field) metrics — a shorter field does not save it.
    void underlineEscapesTheViewportSurfaceWhenCompact() {
        auto style = FlatStyle();
        const auto metrics = InputChrome::FieldMetrics(style, /*floating=*/false);
        const QRect rect(0, 0, 300, metrics.minHeight);
        const auto underline = UnderlineRect(StateFor(style, rect, 0.));

        QVERIFY(rect.contains(underline));
        QVERIFY(!ViewportSurface(rect, metrics.textMargins).intersects(underline));
    }

    void underlineSitsOnTheBottomRow() {
        const auto style = FlatStyle();
        const QRect rect(0, 0, 300, 55);
        const auto underline = UnderlineRect(StateFor(style, rect, 0.));

        QCOMPARE(underline.bottom(), rect.bottom());
        QCOMPARE(underline.height(), 1);
        QCOMPARE(underline.width(), rect.width());
    }

    void underlineThickensOnFocus() {
        const auto style = FlatStyle();
        const QRect rect(0, 0, 300, 55);
        const auto underline = UnderlineRect(StateFor(style, rect, 1.));

        QCOMPARE(underline.height(), 2);
        QCOMPARE(underline.bottom(), rect.bottom());
    }

    void noUnderlineWhenBorderDisabled() {
        auto style = FlatStyle();
        style.border = 0;
        QVERIFY(UnderlineRect(StateFor(style, QRect(0, 0, 300, 55), 0.)).isEmpty());
        // Focus still draws one: borderActive is clamped to at least 1px.
        QCOMPARE(UnderlineRect(StateFor(style, QRect(0, 0, 300, 55), 1.)).height(), 2);
    }

    // FieldMetrics is shared by InputField and EmojiInputField so the two cannot
    // drift; these are the numbers each mode is expected to produce.
    void floatingFieldKeepsTheCaptionStrip() {
        const auto style = FlatStyle();
        const auto metrics = InputChrome::FieldMetrics(style, /*floating=*/true);

        QCOMPARE(metrics.minHeight, 55);
        QCOMPARE(metrics.maxHeight, 148);
        QCOMPARE(metrics.textMargins.top(), 28);
    }

    void compactFieldDropsTheCaptionStrip() {
        const auto style = FlatStyle();
        const auto metrics = InputChrome::FieldMetrics(style, /*floating=*/false);

        QCOMPARE(metrics.minHeight, 38);
        QCOMPARE(metrics.maxHeight, 38); // fixed: no caption to grow for
        QCOMPARE(metrics.textMargins.top(), 8);
        QCOMPARE(metrics.textMargins.bottom(), 4);
    }

    // A round filter field (dialogsFilter) is never compact, floating or not.
    void roundFieldKeepsItsOwnMetrics() {
        auto style = FlatStyle();
        style.borderRadius = 18;
        style.heightMin = 35;
        style.heightMax = 35;
        style.textMargins = QMargins(36, 0, 36, 0);
        const auto metrics = InputChrome::FieldMetrics(style, /*floating=*/false);

        QCOMPARE(metrics.minHeight, 35);
        QCOMPARE(metrics.textMargins.top(), 0);
        QCOMPARE(metrics.textMargins.left(), 36);
    }
};

QTEST_APPLESS_MAIN(TestInputFieldChrome)
#include "tst_input_field_chrome.moc"
