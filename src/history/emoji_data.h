// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QVector>

namespace TeleMatrix {

enum class EmojiCategory {
    People = 1,
    Nature = 2,
    FoodDrink = 3,
    Activity = 4,
    TravelPlaces = 5,
    Objects = 6,
    Symbols = 7,
    Flags = 8,
};

constexpr int kEmojiCategoryCount = 8;

struct EmojiEntry {
    QString emoji;
    QString name;
    EmojiCategory category;
};

/// Display name for a category (used as section header).
QString categoryName(EmojiCategory cat);

/// All emoji entries for a given category.
const QVector<EmojiEntry> &categoryEntries(EmojiCategory cat);

/// All emoji entries across every category (stable order 1..8).
const QVector<EmojiEntry> &allEmojiEntries();

} // namespace TeleMatrix
