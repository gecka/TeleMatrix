// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <memory>

class QAudioSource;

namespace TeleMatrix {

class WavCaptureDevice;

class HistoryVoiceRecorder final : public QObject {
    Q_OBJECT

public:
    struct Result {
        QString path;
        quint64 durationMs = 0;
        quint64 size = 0;
        QByteArray waveform;
    };

    explicit HistoryVoiceRecorder(QObject *parent = nullptr);
    ~HistoryVoiceRecorder() override;

    bool start(QString *error = nullptr);
    Result stop();
    void cancel();

    [[nodiscard]] bool isRecording() const;
    [[nodiscard]] quint64 durationMs() const;

signals:
    void failed(const QString &error);

private:
    std::unique_ptr<QAudioSource> _source;
    std::unique_ptr<WavCaptureDevice> _device;
    bool _recording = false;
};

} // namespace TeleMatrix
