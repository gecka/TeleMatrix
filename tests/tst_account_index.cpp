// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "app/account_index.h"

using namespace TeleMatrix;

namespace {

AccountIndexEntry MakeEntry(
        const QString &dirName,
        const QString &userId,
        const QString &homeserver = QStringLiteral("https://hs.example.org")) {
    AccountIndexEntry entry;
    entry.dirName = dirName;
    entry.settings.setSessionHomeserver(homeserver);
    entry.settings.setSessionUserId(userId);
    entry.settings.setSessionDeviceId(QStringLiteral("DEV-") + dirName);
    return entry;
}

} // namespace

class TestAccountIndex : public QObject {
    Q_OBJECT

private slots:
    // The account list and the pointer into it are written together, so a
    // round-trip must keep every account's own settings with the right account.
    void roundTripsAccountsAndActivePointer() {
        AccountIndex index;
        index.entries.push_back(MakeEntry("0", "@alice:example.org"));
        index.entries.back().settings.setPinnedRoomIds({"!a:x"});
        index.entries.push_back(MakeEntry("1", "@bob:other.org", "https://other.org"));
        index.entries.back().settings.setPinnedRoomIds({"!b:x"});
        index.activeIndex = 1;

        const auto parsed = ParseAccountIndex(SerializeAccountIndex(index));

        QCOMPARE(parsed.entries.size(), 2);
        QCOMPARE(parsed.activeIndex, 1);
        QCOMPARE(parsed.entries[0].dirName, QString("0"));
        QCOMPARE(parsed.entries[0].settings.sessionUserId(), QString("@alice:example.org"));
        QCOMPARE(parsed.entries[0].settings.pinnedRoomIds(), QVector<QString>({"!a:x"}));
        QCOMPARE(parsed.entries[1].dirName, QString("1"));
        QCOMPARE(parsed.entries[1].settings.sessionUserId(), QString("@bob:other.org"));
        QCOMPARE(parsed.entries[1].settings.sessionHomeserver(), QString("https://other.org"));
        QCOMPARE(parsed.entries[1].settings.pinnedRoomIds(), QVector<QString>({"!b:x"}));
    }

    void emptyIndexHasNoActiveAccount() {
        const auto parsed = ParseAccountIndex(SerializeAccountIndex(AccountIndex{}));
        QVERIFY(parsed.entries.isEmpty());
        QCOMPARE(parsed.activeIndex, -1);
    }

    // A hand-edited or truncated file must not leave the active pointer aimed
    // past the end of the list — everything downstream indexes with it.
    void outOfRangeActivePointerIsClamped() {
        AccountIndex index;
        index.entries.push_back(MakeEntry("0", "@alice:example.org"));
        index.activeIndex = 7;
        QCOMPARE(ParseAccountIndex(SerializeAccountIndex(index)).activeIndex, 0);

        index.activeIndex = -5;
        QCOMPARE(ParseAccountIndex(SerializeAccountIndex(index)).activeIndex, 0);
    }

    // Two accounts pointed at one directory would open the same SQLite files
    // twice and corrupt each other, so a duplicate name is dropped, not renamed.
    void duplicateAndNamelessEntriesAreDropped() {
        QJsonArray accounts;
        accounts.append(QJsonObject{{"dirName", "0"}});
        accounts.append(QJsonObject{{"dirName", "0"}});  // duplicate
        accounts.append(QJsonObject{{"session", QJsonObject{}}}); // no dirName
        accounts.append(QJsonObject{{"dirName", "1"}});

        const auto parsed = ParseAccountIndex(QJsonObject{
            {"accounts", accounts},
            {"activeAccount", 0},
        });

        QCOMPARE(parsed.entries.size(), 2);
        QCOMPARE(parsed.entries[0].dirName, QString("0"));
        QCOMPARE(parsed.entries[1].dirName, QString("1"));
    }

    void neverParsesMoreThanTheAccountLimit() {
        QJsonArray accounts;
        for (int i = 0; i < kMaxAccounts + 4; ++i) {
            accounts.append(QJsonObject{{"dirName", QString::number(i)}});
        }
        const auto parsed = ParseAccountIndex(QJsonObject{
            {"accounts", accounts},
            {"activeAccount", kMaxAccounts + 2},
        });
        QCOMPARE(parsed.entries.size(), kMaxAccounts);
        // The clamp uses the list we actually kept, not the one on disk.
        QCOMPARE(parsed.activeIndex, kMaxAccounts - 1);
    }

    // Directory names are persisted, so a name freed by a sign-out is safe to
    // hand to the next account — the gap gets filled rather than growing forever.
    void dirNameAllocationFillsGaps() {
        QCOMPARE(FirstFreeAccountDirName({}), QString("0"));
        QCOMPARE(FirstFreeAccountDirName({"0"}), QString("1"));
        QCOMPARE(FirstFreeAccountDirName({"0", "1", "2"}), QString("3"));
        // Account "1" signed out: its directory name is reused next.
        QCOMPARE(FirstFreeAccountDirName({"0", "2"}), QString("1"));
    }

    // Removing an account must not silently switch the user to a different
    // account than the one that slid into place.
    void activeIndexTracksRemovals() {
        // Removing one BEFORE the active account shifts it down.
        QCOMPARE(ActiveIndexAfterRemoval(/*active=*/2, /*removed=*/0, /*newCount=*/2), 1);
        // Removing one AFTER leaves it alone.
        QCOMPARE(ActiveIndexAfterRemoval(/*active=*/0, /*removed=*/2, /*newCount=*/2), 0);
        // Removing the active account promotes the next one in order...
        QCOMPARE(ActiveIndexAfterRemoval(/*active=*/1, /*removed=*/1, /*newCount=*/3), 1);
        // ...or the new last one, when the active account was at the end.
        QCOMPARE(ActiveIndexAfterRemoval(/*active=*/2, /*removed=*/2, /*newCount=*/2), 1);
        // Removing the only account leaves nothing active.
        QCOMPARE(ActiveIndexAfterRemoval(/*active=*/0, /*removed=*/0, /*newCount=*/0), -1);
    }

    // Signing one account out must delete exactly its own keys: any extra would
    // take a sibling's secrets with it, any missing would outlive the account.
    void secretKeysAreScopedToOneAccount() {
        const auto alice = AccountSecretKeys(
            "0", "https://hs.example.org", "@alice:example.org", "DEV1");
        const auto bob = AccountSecretKeys(
            "1", "https://other.org", "@bob:other.org", "DEV2");

        QCOMPARE(alice.size(), 6);
        QCOMPARE(bob.size(), 6);
        for (const auto &key : alice) {
            QVERIFY2(!bob.contains(key), qPrintable("shared key: " + key));
        }

        QVERIFY(alice.contains(
            "v1|session_access_token|https://hs.example.org|@alice:example.org|DEV1"));
        QVERIFY(alice.contains(
            "v1|sdk_store_passphrase|https://hs.example.org|@alice:example.org|DEV1"));
        QVERIFY(alice.contains(
            "v1|search_passphrase|https://hs.example.org|@alice:example.org|DEV1"));
        QVERIFY(alice.contains("v1|local_cache|0|app_cache_passphrase"));
        QVERIFY(alice.contains("v1|local_cache|0|preview_cache_passphrase"));
        QVERIFY(alice.contains("v1|local_cache|0|media_cache_passphrase"));
    }

    // An account that never finished signing in has no session secrets to name,
    // but its local caches were opened at creation and do have keys.
    void secretKeysOmitSessionKeysWithoutASession() {
        const auto keys = AccountSecretKeys("2", QString(), QString(), QString());
        QCOMPARE(keys.size(), 3);
        for (const auto &key : keys) {
            QVERIFY(key.startsWith("v1|local_cache|2|"));
        }
    }
};

QTEST_GUILESS_MAIN(TestAccountIndex)
#include "tst_account_index.moc"
