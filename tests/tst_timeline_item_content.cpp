// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "protocol/protocol_types.h"
#include "protocol/timeline_item_content.h"

using namespace TeleMatrix;

namespace {

TimelineItem withContent(TimelineContent content) {
    TimelineItem item;
    item.content = std::move(content);
    return item;
}

TimelineMediaContent sampleMedia() {
    TimelineMediaContent media;
    media.url = "mxc://x/y";
    media.mime = "image/png";
    media.filename = "pic.png";
    media.caption = "a caption";
    media.size = 1234;
    media.width = 64;
    media.height = 48;
    return media;
}

} // namespace

class TestTimelineItemContent : public QObject {
    Q_OBJECT

private slots:
    void contentTypeDispatch() {
        QCOMPARE(contentType(withContent(TimelineTextContent{})), ContentType::Text);
        QCOMPARE(contentType(withContent(TimelineImageContent{})), ContentType::Image);
        QCOMPARE(contentType(withContent(TimelineFileContent{})), ContentType::File);
        QCOMPARE(contentType(withContent(TimelineVideoContent{})), ContentType::Video);
        QCOMPARE(contentType(withContent(TimelineAudioContent{})), ContentType::Audio);
        QCOMPARE(contentType(withContent(TimelineServiceContent{})), ContentType::Service);
        QCOMPARE(contentType(withContent(TimelinePollContent{})), ContentType::Poll);
        QCOMPARE(contentType(withContent(TimelineUnableToDecryptContent{})),
                 ContentType::UnableToDecrypt);
    }

    void typedAccessorsMatchVariant() {
        const auto text = withContent(TimelineTextContent{"hello", "<b>hello</b>"});
        QVERIFY(textContent(text) != nullptr);
        QCOMPARE(textContent(text)->body, QString("hello"));
        // Wrong-type accessors return nullptr.
        QCOMPARE(imageContent(text), nullptr);
        QCOMPARE(mediaContent(text), nullptr);
        QCOMPARE(pollContent(text), nullptr);
    }

    // mediaContent() chains across the 4 media variants (image/video/file/audio)
    // and returns nullptr for the non-media ones.
    void mediaContentChaining() {
        TimelineImageContent img;
        img.media = sampleMedia();
        const auto image = withContent(img);
        QVERIFY(imageContent(image) != nullptr);
        QVERIFY(mediaContent(image) != nullptr);
        QCOMPARE(mediaContent(image)->filename, QString("pic.png"));
        QCOMPARE(mediaContent(image), &imageContent(image)->media);
        QCOMPARE(fileContent(image), nullptr);

        TimelineFileContent fileC;
        fileC.media = sampleMedia();
        const auto file = withContent(fileC);
        QVERIFY(fileContent(file) != nullptr);
        QVERIFY(mediaContent(file) != nullptr);

        TimelineVideoContent vid;
        vid.media = sampleMedia();
        QVERIFY(mediaContent(withContent(vid)) != nullptr);

        TimelineAudioContent aud;
        aud.media = sampleMedia();
        aud.isVoice = true;
        const auto audio = withContent(aud);
        QVERIFY(audioContent(audio) != nullptr);
        QVERIFY(audioContent(audio)->isVoice);
        QVERIFY(mediaContent(audio) != nullptr);
    }

    void nonMediaVariantsHaveNoMediaContent() {
        QCOMPARE(mediaContent(withContent(TimelineTextContent{})), nullptr);
        QCOMPARE(mediaContent(withContent(TimelineServiceContent{})), nullptr);
        QCOMPARE(mediaContent(withContent(TimelinePollContent{})), nullptr);
        QCOMPARE(mediaContent(withContent(TimelineUnableToDecryptContent{})), nullptr);
    }
};

QTEST_GUILESS_MAIN(TestTimelineItemContent)
#include "tst_timeline_item_content.moc"
