// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <iterator>

#include "history/emoji_data.h"

using namespace TeleMatrix;

namespace {

constexpr EmojiCategory kCategories[] = {
    EmojiCategory::People,
    EmojiCategory::Nature,
    EmojiCategory::FoodDrink,
    EmojiCategory::Activity,
    EmojiCategory::TravelPlaces,
    EmojiCategory::Objects,
    EmojiCategory::Symbols,
    EmojiCategory::Flags,
};

} // namespace

// Consistency invariants only — we don't snapshot the (generated) emoji table,
// we assert it stays internally coherent.
class TestEmojiData : public QObject {
    Q_OBJECT

private slots:
    void categoriesPartitionAllEntriesByCount() {
        int sum = 0;
        for (const auto cat : kCategories) {
            sum += categoryEntries(cat).size();
        }
        QCOMPARE(sum, allEmojiEntries().size());
        QCOMPARE(static_cast<int>(std::size(kCategories)), kEmojiCategoryCount);
    }

    void everyEntryInCategoryBelongsToThatCategory() {
        for (const auto cat : kCategories) {
            for (const auto &entry : categoryEntries(cat)) {
                QCOMPARE(entry.category, cat);
            }
        }
    }

    void allEntriesAreGroupedInCategoryOrder() {
        // The combined list is documented as "stable order 1..8"; the category
        // discriminant must be non-decreasing across the whole list, and always
        // within bounds.
        int previous = 0;
        for (const auto &entry : allEmojiEntries()) {
            const int value = static_cast<int>(entry.category);
            QVERIFY(value >= 1 && value <= kEmojiCategoryCount);
            QVERIFY2(value >= previous, "allEmojiEntries() must be grouped by category");
            previous = value;
        }
    }
};

QTEST_GUILESS_MAIN(TestEmojiData)
#include "tst_emoji_data.moc"
