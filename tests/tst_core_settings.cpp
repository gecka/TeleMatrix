// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "core/core_settings.h"

using namespace TeleMatrix::Core;

class TestCoreSettings : public QObject {
    Q_OBJECT

private slots:
    // Full toJson -> addFromJson -> getters round-trip. This pins serialization
    // of the persisted settings surface, including the window position struct.
    void roundTripsScalarAndContainerFields() {
        Settings s1;
        WindowPosition pos;
        pos.moncrc = 12345;
        pos.maximized = 1;
        pos.scale = 150;
        pos.x = 10;
        pos.y = 20;
        pos.w = 800;
        pos.h = 600;
        s1.setWindowPosition(pos);
        s1.setDialogsWidth(451);
        s1.setThemeMode(1);
        s1.setThemeId("andalusia");
        s1.setConfigScale(125);
        s1.setLanguageId("es");
        s1.setSendSubmitWay(1);
        s1.setCompressImages(false);
        s1.setMediaSaveDir("/tmp/saved-media");
        s1.setDesktopNotify(false);
        s1.setSoundNotify(false);
        s1.setShowMessagePreview(false);
        s1.setIncludeMutedInBadge(true);

        const auto json = s1.toJson();

        Settings s2;
        QVERIFY(s2.addFromJson(json));

        const auto &rp = s2.windowPosition();
        QCOMPARE(rp.moncrc, 12345);
        QCOMPARE(rp.maximized, 1);
        QCOMPARE(rp.scale, 150);
        QCOMPARE(rp.rect(), QRect(10, 20, 800, 600));

        QCOMPARE(s2.dialogsWidth(), 451);
        QCOMPARE(s2.themeMode(), 1);
        QCOMPARE(s2.themeId(), QString("andalusia"));
        QCOMPARE(s2.configScale(), 125);
        QCOMPARE(s2.languageId(), QString("es"));
        QCOMPARE(s2.sendSubmitWay(), 1);
        QCOMPARE(s2.compressImages(), false);
        QCOMPARE(s2.mediaSaveDir(), QString("/tmp/saved-media"));
        QCOMPARE(s2.desktopNotify(), false);
        QCOMPARE(s2.soundNotify(), false);
        QCOMPARE(s2.showMessagePreview(), false);
        QCOMPARE(s2.includeMutedInBadge(), true);
    }

    // The same round-trip for the per-account half of the split.
    void accountSettingsRoundTripSessionFoldersAndPins() {
        AccountSettings a1;
        a1.setPinnedRoomIds({"!a:x", "!b:x"});
        a1.setCustomFolders({CustomFolder{3, "Work"}, CustomFolder{4, "Family"}});
        a1.setFolderOrder({4, 3});
        a1.setFoldersServerMigrated(true);
        a1.setSessionHomeserver("https://hs.example.org");
        a1.setSessionUserId("@me:example.org");
        a1.setSessionDeviceId("DEV123");
        a1.setSessionSecretBackend("vault");
        a1.incrementRecentReaction(QString::fromUtf8("\xF0\x9F\x91\x8D"));

        AccountSettings a2;
        QVERIFY(a2.addFromJson(a1.toJson()));

        QCOMPARE(a2.pinnedRoomIds(), QVector<QString>({"!a:x", "!b:x"}));
        QCOMPARE(a2.folderOrder(), QVector<int>({4, 3}));
        QVERIFY(a2.foldersServerMigrated());
        QCOMPARE(a2.sessionHomeserver(), QString("https://hs.example.org"));
        QCOMPARE(a2.sessionUserId(), QString("@me:example.org"));
        QCOMPARE(a2.sessionDeviceId(), QString("DEV123"));
        QCOMPARE(a2.sessionSecretBackend(), QString("vault"));
        QVERIFY(a2.hasSession());
        QCOMPARE(a2.recentReactions().size(), 1);

        const auto &folders = a2.customFolders();
        QCOMPARE(folders.size(), 2);
        QCOMPARE(folders[0].id, 3);
        QCOMPARE(folders[0].name, QString("Work"));
        QCOMPARE(folders[1].id, 4);
        QCOMPARE(folders[1].name, QString("Family"));
    }

    // Signing one account out must wipe that account's data and nothing else —
    // the device settings and every sibling account are separate objects now.
    void accountClearWipesOnlyThatAccount() {
        AccountSettings signedOut;
        signedOut.setSessionHomeserver("https://hs.example.org");
        signedOut.setSessionUserId("@me:example.org");
        signedOut.setSessionDeviceId("DEV123");
        signedOut.setPinnedRoomIds({"!a:x"});
        signedOut.setCustomFolders({CustomFolder{3, "Work"}});

        AccountSettings sibling;
        sibling.setSessionHomeserver("https://other.example.org");
        sibling.setSessionUserId("@other:example.org");
        sibling.setSessionDeviceId("DEV456");
        sibling.setPinnedRoomIds({"!b:x"});

        signedOut.clear();

        QVERIFY(!signedOut.hasSession());
        QVERIFY(signedOut.pinnedRoomIds().isEmpty());
        QVERIFY(signedOut.customFolders().isEmpty());
        // The sibling is untouched.
        QVERIFY(sibling.hasSession());
        QCOMPARE(sibling.pinnedRoomIds(), QVector<QString>({"!b:x"}));
    }

    void defaultsRoundTripToThemselves() {
        // A fresh Settings serialized and reloaded must be unchanged, so an empty
        // settings file never silently mutates defaults.
        Settings s1;
        Settings s2;
        QVERIFY(s2.addFromJson(s1.toJson()));
        QCOMPARE(s2.themeMode(), s1.themeMode());
        QCOMPARE(s2.themeId(), s1.themeId());
        QCOMPARE(s2.dialogsWidth(), s1.dialogsWidth());
        QCOMPARE(s2.updatePolicy(), s1.updatePolicy());
        QCOMPARE(s2.installBetaVersions(), s1.installBetaVersions());
    }

    // The dialogs width used to be stored as a fraction of the window width,
    // which could not be restored faithfully and always came back at the
    // minimum. Such a file must load as "never set" (so the column takes the
    // default) rather than carrying the unusable ratio forward.
    void legacyDialogsWidthRatioIsNotMigrated() {
        Settings fresh;
        QCOMPARE(fresh.dialogsWidth(), 0);

        auto legacy = fresh.toJson();
        legacy.remove(QStringLiteral("dialogsWidth"));
        legacy[QStringLiteral("dialogsWidthRatio")] = 0.2506;

        Settings loaded;
        loaded.setDialogsWidth(451);
        QVERIFY(loaded.addFromJson(legacy));
        QCOMPARE(loaded.dialogsWidth(), 451); // absent key leaves the value alone
        QVERIFY(!loaded.toJson().contains(QStringLiteral("dialogsWidthRatio")));
    }

    // The beta channel is opt-in, and an upgrade must not move anyone onto it:
    // a settings file written before the setting existed has no "beta" key, and
    // has to load as false rather than inheriting whatever was in memory.
    void installBetaVersionsRoundTripsAndDefaultsOff() {
        Settings fresh;
        QCOMPARE(fresh.installBetaVersions(), false);

        Settings s1;
        s1.setInstallBetaVersions(true);
        Settings s2;
        QVERIFY(s2.addFromJson(s1.toJson()));
        QCOMPARE(s2.installBetaVersions(), true);

        // An "updates" object from before this setting existed.
        Settings old;
        QJsonObject json;
        json[QStringLiteral("updates")] = QJsonObject{
            { QStringLiteral("policy"), 2 },
        };
        QVERIFY(old.addFromJson(json));
        QCOMPARE(old.updatePolicy(), 2);
        QCOMPARE(old.installBetaVersions(), false);
    }

    // The device-level backend preference must round-trip and, crucially, survive
    // an account signing out — it drives the backend choice for the NEXT sign-in.
    void preferredSecretBackendRoundTrips() {
        Settings s1;
        s1.setPreferredSecretBackend("vault");
        Settings s2;
        QVERIFY(s2.addFromJson(s1.toJson()));
        QCOMPARE(s2.preferredSecretBackend(), QString("vault"));
    }

    void preferredSecretBackendSurvivesAccountSignOut() {
        Settings device;
        device.setPreferredSecretBackend("vault");

        AccountSettings account;
        account.setSessionHomeserver("https://hs.example.org");
        account.setSessionUserId("@me:example.org");
        account.setSessionDeviceId("DEV123");
        account.setSessionSecretBackend("vault");

        account.clear();

        // The account-scoped marker is wiped; the device-level preference is kept
        // because it now lives in a different object entirely.
        QVERIFY(account.sessionSecretBackend().isEmpty());
        QVERIFY(!account.hasSession());
        QCOMPARE(device.preferredSecretBackend(), QString("vault"));
    }

    // Dubai carries the original colours, so an install that never picks a
    // theme keeps the look it always had.
    void themeIdDefaultsToDubaiAndSurvivesAThemelessFile() {
        Settings s1;
        QCOMPARE(s1.themeId(), QString("dubai"));

        // A settings file written before themes existed carries no theme.id;
        // the default must survive rather than becoming empty.
        auto json = s1.toJson();
        auto theme = json.value("theme").toObject();
        theme.remove("id");
        json["theme"] = theme;

        Settings s2;
        QVERIFY(s2.addFromJson(json));
        QCOMPARE(s2.themeId(), QString("dubai"));
    }
};

QTEST_GUILESS_MAIN(TestCoreSettings)
#include "tst_core_settings.moc"
