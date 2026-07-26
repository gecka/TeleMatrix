// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_view_poll.h"

#include <algorithm>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QPainterPath>
#include <QRect>

#include "../../styles/style_constants.h"
#include "../../ui/painter.h"

namespace TeleMatrix {
namespace HistoryViewPoll {
namespace {

[[nodiscard]] bool showResults(const TimelineItem &item) {
    const auto poll = pollContent(item);
    // Once you've voted (or the poll is closed / disclosed) switch to the
    // tallied results with your choice highlighted, matching Telegram.
    return poll && (poll->isClosed || poll->kind == PollKind::Disclosed
        || poll->hasVoted);
}

[[nodiscard]] bool canInteract(const TimelineItem &item) {
    const auto poll = pollContent(item);
    return poll && !poll->isClosed;
}

[[nodiscard]] int questionHeight(const QString &question, int width) {
    const QFontMetrics fm(static_cast<const QFont &>(st::semiboldFont));
    return fm.boundingRect(
        QRect(0, 0, width, 10000),
        Qt::AlignLeft | Qt::TextWordWrap,
        question).height();
}

[[nodiscard]] int optionTextHeight(
    const QString &text,
    int width,
    const QFont &font) {
    const QFontMetrics fm(font);
    return fm.boundingRect(
        QRect(0, 0, width, 10000),
        Qt::AlignLeft | Qt::TextWordWrap,
        text).height();
}

[[nodiscard]] int optionRowHeight(
    const PollOptionInfo &option,
    int textWidth,
    bool resultsVisible) {
    auto height = st::historyPollAnswerPadding.top()
        + optionTextHeight(option.text, textWidth, st::msgFont)
        + st::historyPollAnswerPadding.bottom();
    if (resultsVisible) {
        height += st::historyPollFillingBottom + st::historyPollFillingHeight;
    }
    return height;
}

struct PollLayoutMetrics {
    int leftInset = st::historyPollAnswerPadding.left();
    int textWidth = 1;
    int resultsChoiceSpace = 0;
};

[[nodiscard]] PollLayoutMetrics computeLayoutMetrics(
    const TimelineItem &item,
    int width) {
    PollLayoutMetrics result;
    const auto resultsVisible = showResults(item);
    const auto poll = pollContent(item);
    if (!poll) {
        return result;
    }

    if (resultsVisible) {
        const QFontMetrics percentFm(static_cast<const QFont &>(st::semiboldFont));
        auto maxPercentWidth = 0;
        for (const auto &option : poll->options) {
            maxPercentWidth = qMax(
                maxPercentWidth,
                percentFm.horizontalAdvance(QStringLiteral("%1%").arg(option.votePercent)));
        }

        const auto reserveResultsChoiceSpace = std::any_of(
            poll->options.cbegin(),
            poll->options.cend(),
            [](const PollOptionInfo &option) { return option.isChosen; });
        result.resultsChoiceSpace = reserveResultsChoiceSpace
            ? (st::historyPollRadioSize + st::historyPollPercentSkip)
            : 0;
        result.leftInset = qMax(
            st::historyPollAnswerPadding.left(),
            maxPercentWidth + st::historyPollPercentSkip + result.resultsChoiceSpace);
    }

    result.textWidth = qMax(
        1,
        width
            - result.leftInset
            - st::historyPollAnswerPadding.right()
            - st::historyPollFillingRight);
    return result;
}

[[nodiscard]] QString totalVotesLabel(const TimelineItem &item) {
    const auto poll = pollContent(item);
    return QCoreApplication::translate(
        "HistoryViewPoll",
        "%n vote(s)",
        "",
        poll ? poll->totalVoters : 0);
}

[[nodiscard]] int voteButtonHeight() {
    return 28;
}

[[nodiscard]] QRect voteButtonRect(int contentX, int contentY, int width, int top) {
    const QFontMetrics fm(static_cast<const QFont &>(st::semiboldFont));
    const auto voteText = QCoreApplication::translate("HistoryViewPoll", "Vote");
    const auto buttonWidth = qMax(64, fm.horizontalAdvance(voteText) + 24);
    return QRect(contentX, contentY + top, buttonWidth, voteButtonHeight());
}

void paintCheckMark(QPainter &p, const QPoint &center, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    p.save();
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(
        QPointF(center.x() - 4.0, center.y() + 0.5),
        QPointF(center.x() - 1.0, center.y() + 3.5));
    p.drawLine(
        QPointF(center.x() - 1.0, center.y() + 3.5),
        QPointF(center.x() + 5.0, center.y() - 3.0));
    p.restore();
}

[[nodiscard]] QRect choiceMarkerRect(
    int x,
    int textTop,
    int textHeight) {
    return QRect(
        x + ((st::historyPollAnswerPadding.left() - st::historyPollRadioSize) / 2),
        textTop + ((textHeight - st::historyPollRadioSize) / 2),
        st::historyPollRadioSize,
        st::historyPollRadioSize);
}

[[nodiscard]] QRect resultsChoiceBadgeRect(int textLeft, int barTop) {
    return QRect(
        textLeft - st::historyPollPercentSkip - st::historyPollRadioSize,
        barTop - ((st::historyPollRadioSize - st::historyPollFillingHeight) / 2),
        st::historyPollRadioSize,
        st::historyPollRadioSize);
}

void paintChoiceBadge(
    QPainter &p,
    const QRect &rect,
    const QColor &fillColor,
    const QColor &iconColor) {
    PainterHighQualityEnabler hq(p);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawEllipse(rect);
    p.restore();
    paintCheckMark(p, rect.center(), iconColor);
}

} // namespace

int contentHeight(const TimelineItem &item, int innerWidth) {
    const auto poll = pollContent(item);
    if (!poll) {
        return 0;
    }
    const QFontMetrics subtitleFm(static_cast<const QFont &>(st::msgDateFont));
    const auto resultsVisible = showResults(item);
    const auto layout = computeLayoutMetrics(item, innerWidth);

    auto height = st::historyPollQuestionTop;
    height += questionHeight(poll->question, innerWidth);
    height += st::historyPollSubtitleSkip;
    height += subtitleFm.height();

    for (const auto &option : poll->options) {
        height += st::historyPollAnswersSkip;
        height += optionRowHeight(option, layout.textWidth, resultsVisible);
    }

    height += st::historyPollTotalVotesSkip;
    height += subtitleFm.height();

    if (poll->isMultiChoice && canInteract(item)) {
        height += st::historyPollBottomButtonTop;
        height += voteButtonHeight();
    }

    height += st::historyPollBottomPadding;
    return height;
}

void paintContent(
    QPainter &p,
    const TimelineItem &item,
    [[maybe_unused]] const MessagePaintContext &ctx,
    int x,
    int y,
    int width,
    bool isOutgoing) {
    const auto poll = pollContent(item);
    if (!poll) {
        return;
    }

    const auto resultsVisible = showResults(item);
    const auto questionFont = static_cast<const QFont &>(st::semiboldFont);
    const auto optionFont = static_cast<const QFont &>(st::msgFont);
    const auto infoFont = static_cast<const QFont &>(st::msgDateFont);
    const auto percentFont = static_cast<const QFont &>(st::semiboldFont);
    const QFontMetrics questionFm(questionFont);
    const QFontMetrics optionFm(optionFont);
    const QFontMetrics infoFm(infoFont);
    const QFontMetrics percentFm(percentFont);

    const auto textColor = isOutgoing ? st::historyTextOutFg : st::historyTextInFg;
    const auto subtitleColor = isOutgoing ? st::msgOutDateFg : st::msgInDateFg;
    const auto accentColor = isOutgoing ? st::msgFileOutBg : st::msgFileInBg;
    const auto trackColor = st::withAlpha(accentColor, 48);
    const auto choiceColor = st::windowBg;
    const auto layout = computeLayoutMetrics(item, width);

    auto top = y + st::historyPollQuestionTop;

    p.setFont(questionFont);
    p.setPen(textColor);
    const QRect questionRect(x, top, width, 10000);
    const auto questionBound = questionFm.boundingRect(
        questionRect,
        Qt::AlignLeft | Qt::TextWordWrap,
        poll->question);
    p.drawText(questionRect, Qt::AlignLeft | Qt::TextWordWrap, poll->question);
    top += questionBound.height();

    top += st::historyPollSubtitleSkip;
    p.setFont(infoFont);
    p.setPen(subtitleColor);
    p.drawText(x, top + infoFm.ascent(), poll->subtitle);
    top += infoFm.height();

    for (const auto &option : poll->options) {
        top += st::historyPollAnswersSkip;

        const auto rowTop = top;
        const auto textTop = rowTop + st::historyPollAnswerPadding.top();
        const auto textLeft = x + layout.leftInset;
        const QRect textRect(textLeft, textTop, layout.textWidth, 10000);
        const auto textBound = optionFm.boundingRect(
            textRect,
            Qt::AlignLeft | Qt::TextWordWrap,
            option.text);

        if (resultsVisible) {
            p.setFont(percentFont);
            p.setPen(textColor);
            const auto percentText = QStringLiteral("%1%").arg(option.votePercent);
            const auto percentWidth = percentFm.horizontalAdvance(percentText);
            const QRect percentRect(
                textLeft
                    - layout.resultsChoiceSpace
                    - percentWidth
                    - st::historyPollPercentSkip,
                textTop + st::historyPollPercentTop,
                percentWidth,
                percentFm.height());
            p.drawText(percentRect, Qt::AlignLeft | Qt::AlignVCenter, percentText);

            p.setFont(optionFont);
            p.setPen(textColor);
            p.drawText(textRect, Qt::AlignLeft | Qt::TextWordWrap, option.text);

            const auto barLeft = textLeft;
            const auto barTop = textTop + textBound.height() + st::historyPollFillingBottom;
            const auto barWidthMax = qMax(
                st::historyPollFillingMin,
                width
                    - layout.leftInset
                    - st::historyPollAnswerPadding.right()
                    - st::historyPollFillingRight);
            const auto fillWidth = qMax(
                option.voteCount > 0 ? st::historyPollFillingMin : 0,
                static_cast<int>(option.filling * barWidthMax));

            PainterHighQualityEnabler hq(p);
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(trackColor);
            p.drawRoundedRect(
                QRectF(barLeft, barTop, barWidthMax, st::historyPollFillingHeight),
                st::historyPollFillingRadius,
                st::historyPollFillingRadius);

            auto fillColor = accentColor;
            if (poll->isQuiz && option.isCorrect) {
                fillColor = st::windowBgActive;
            }
            p.setBrush(fillColor);
            p.drawRoundedRect(
                QRectF(barLeft, barTop, fillWidth, st::historyPollFillingHeight),
                st::historyPollFillingRadius,
                st::historyPollFillingRadius);
            p.restore();

            if (option.isChosen) {
                paintChoiceBadge(
                    p,
                    resultsChoiceBadgeRect(textLeft, barTop),
                    fillColor,
                    choiceColor);
            }
        } else {
            const auto radioRect = choiceMarkerRect(x, textTop, optionFm.height());

            PainterHighQualityEnabler hq(p);
            p.save();
            // Compose with the inherited opacity (a message being deleted is
            // painted dimmed) instead of forcing an absolute value.
            const auto previousOpacity = p.opacity();
            p.setPen(QPen(subtitleColor, 1.5));
            p.setBrush(Qt::NoBrush);
            p.setOpacity(previousOpacity * st::historyPollRadioOpacity);
            p.drawEllipse(radioRect);
            if (option.isChosen) {
                p.setOpacity(previousOpacity);
                p.setPen(Qt::NoPen);
                p.setBrush(accentColor);
                p.drawEllipse(radioRect);
            }
            p.restore();

            if (option.isChosen) {
                paintCheckMark(p, radioRect.center(), choiceColor);
            }

            p.setFont(optionFont);
            p.setPen(textColor);
            p.drawText(textRect, Qt::AlignLeft | Qt::TextWordWrap, option.text);
        }

        top = rowTop + optionRowHeight(option, layout.textWidth, resultsVisible);
    }

    top += st::historyPollTotalVotesSkip;
    p.setFont(infoFont);
    p.setPen(subtitleColor);
    p.drawText(x, top + infoFm.ascent(), totalVotesLabel(item));
    top += infoFm.height();

    if (poll->isMultiChoice && canInteract(item)) {
        top += st::historyPollBottomButtonTop;
        const auto button = voteButtonRect(x, y, width, top - y);
        PainterHighQualityEnabler hq(p);
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(st::lightButtonBgOver);
        p.drawRoundedRect(button, button.height() / 2.0, button.height() / 2.0);
        p.setFont(st::semiboldFont);
        p.setPen(st::lightButtonFg);
        p.drawText(button, Qt::AlignCenter, QCoreApplication::translate("HistoryViewPoll", "Vote"));
        p.restore();
    }
}

QString optionAt(
    const TimelineItem &item,
    [[maybe_unused]] const MessagePaintContext &ctx,
    int contentX,
    int contentY,
    int width,
    QPoint pos) {
    const auto poll = pollContent(item);
    if (!poll) {
        return {};
    }
    if (!canInteract(item)) {
        return {};
    }

    const auto resultsVisible = showResults(item);
    const auto layout = computeLayoutMetrics(item, width);

    auto top = contentY + st::historyPollQuestionTop;
    top += questionHeight(poll->question, width);
    top += st::historyPollSubtitleSkip + st::msgDateFont->height;

    for (const auto &option : poll->options) {
        top += st::historyPollAnswersSkip;
        const auto rowHeight = optionRowHeight(option, layout.textWidth, resultsVisible);
        const QRect rowRect(contentX, top, width, rowHeight);
        if (rowRect.contains(pos)) {
            return option.id;
        }
        top += rowHeight;
    }

    return {};
}

bool voteButtonAt(
    const TimelineItem &item,
    [[maybe_unused]] const MessagePaintContext &ctx,
    int contentX,
    int contentY,
    int width,
    QPoint pos) {
    const auto poll = pollContent(item);
    if (!poll || !poll->isMultiChoice || !canInteract(item)) {
        return false;
    }

    const auto resultsVisible = showResults(item);
    const auto layout = computeLayoutMetrics(item, width);

    auto top = st::historyPollQuestionTop;
    top += questionHeight(poll->question, width);
    top += st::historyPollSubtitleSkip + st::msgDateFont->height;
    for (const auto &option : poll->options) {
        top += st::historyPollAnswersSkip;
        top += optionRowHeight(option, layout.textWidth, resultsVisible);
    }
    top += st::historyPollTotalVotesSkip + st::msgDateFont->height;
    top += st::historyPollBottomButtonTop;

    return voteButtonRect(contentX, contentY, width, top).contains(pos);
}

} // namespace HistoryViewPoll
} // namespace TeleMatrix
