// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_voice_recorder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QMediaDevices>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace TeleMatrix {

namespace {

constexpr auto kPreferredSampleRate = 48000;
constexpr auto kFallbackSampleRate = 44100;
constexpr auto kWaveformSamples = 100;
constexpr auto kLevelBucketsPerSecond = 50;

[[nodiscard]] int bytesPerSample(QAudioFormat::SampleFormat format) {
    switch (format) {
    case QAudioFormat::UInt8: return 1;
    case QAudioFormat::Int16: return 2;
    case QAudioFormat::Int32: return 4;
    case QAudioFormat::Float: return 4;
    case QAudioFormat::NSampleFormats:
    case QAudioFormat::Unknown:
        break;
    }
    return 0;
}

[[nodiscard]] bool supportedSampleFormat(QAudioFormat::SampleFormat format) {
    return bytesPerSample(format) > 0;
}

[[nodiscard]] QAudioFormat makeFormat(
        int sampleRate,
        int channels,
        QAudioFormat::SampleFormat sampleFormat) {
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(qMax(1, channels));
    format.setSampleFormat(sampleFormat);
    return format;
}

[[nodiscard]] QAudioFormat chooseFormat(
        const QAudioDevice &device,
        QString *error) {
    const auto preferred = device.preferredFormat();
    const auto preferredRate = preferred.sampleRate() > 0
        ? preferred.sampleRate()
        : kFallbackSampleRate;
    const auto preferredChannels = preferred.channelCount() > 0
        ? preferred.channelCount()
        : 1;

    const std::array<int, 4> rates = {
        kPreferredSampleRate,
        kFallbackSampleRate,
        preferredRate,
        16000,
    };
    const std::array<int, 2> channels = {
        1,
        preferredChannels,
    };
    const std::array<QAudioFormat::SampleFormat, 4> sampleFormats = {
        QAudioFormat::Int16,
        QAudioFormat::Float,
        QAudioFormat::Int32,
        QAudioFormat::UInt8,
    };

    for (const auto sampleFormat : sampleFormats) {
        for (const auto sampleRate : rates) {
            for (const auto channelCount : channels) {
                const auto format = makeFormat(sampleRate, channelCount, sampleFormat);
                if (device.isFormatSupported(format)) {
                    return format;
                }
            }
        }
    }

    if (supportedSampleFormat(preferred.sampleFormat())
        && preferred.sampleRate() > 0
        && preferred.channelCount() > 0
        && device.isFormatSupported(preferred)) {
        return preferred;
    }

    if (error) {
        *error = QObject::tr("No supported microphone recording format.");
    }
    return QAudioFormat();
}

[[nodiscard]] quint32 checkedRiffSize(quint64 dataSize) {
    const auto maxData = quint64(std::numeric_limits<quint32>::max()) - 36;
    return quint32(qMin(dataSize, maxData) + 36);
}

[[nodiscard]] quint32 checkedDataSize(quint64 dataSize) {
    return quint32(qMin(dataSize, quint64(std::numeric_limits<quint32>::max())));
}

} // namespace

class WavCaptureDevice final : public QIODevice {
public:
    explicit WavCaptureDevice(const QAudioFormat &format)
        : _format(format) {
    }

    bool start(QString *error) {
        const auto tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const auto tempDir = tempRoot.isEmpty() ? QDir::tempPath() : tempRoot;
        _file.setFileTemplate(QDir(tempDir).filePath(QStringLiteral("telematrix-voice-XXXXXX.wav")));
        _file.setAutoRemove(false);

        if (!_file.open()) {
            if (error) {
                *error = QObject::tr("Could not create a temporary voice message file.");
            }
            return false;
        }
        _path = _file.fileName();
        if (!writeHeader(0)) {
            if (error) {
                *error = QObject::tr("Could not initialize the voice message file.");
            }
            _file.close();
            QFile::remove(_path);
            _path.clear();
            return false;
        }

        _samplesPerLevel = qMax(1, _format.sampleRate() / kLevelBucketsPerSecond);
        open(QIODevice::WriteOnly);
        return true;
    }

    HistoryVoiceRecorder::Result finish() {
        if (isOpen()) {
            close();
        }
        if (_file.isOpen()) {
            writeHeader(_dataBytes);
            _file.flush();
            _file.close();
        }
        HistoryVoiceRecorder::Result result;
        result.path = _path;
        result.durationMs = durationMs();
        result.size = _dataBytes + 44;
        result.waveform = buildWaveform();
        _path.clear();
        return result;
    }

    void cancel() {
        if (isOpen()) {
            close();
        }
        if (_file.isOpen()) {
            _file.close();
        }
        if (!_path.isEmpty()) {
            QFile::remove(_path);
            _path.clear();
        }
    }

    [[nodiscard]] quint64 durationMs() const {
        const auto frameBytes = bytesPerFrame();
        if (frameBytes <= 0 || _format.sampleRate() <= 0) {
            return 0;
        }
        const auto frames = _dataBytes / quint64(frameBytes);
        return (frames * 1000) / quint64(_format.sampleRate());
    }

protected:
    qint64 readData(char *, qint64) override {
        return -1;
    }

    qint64 writeData(const char *data, qint64 len) override {
        if (len <= 0 || !_file.isOpen()) {
            return 0;
        }
        const auto written = _file.write(data, len);
        if (written > 0) {
            _dataBytes += quint64(written);
            processSamples(data, written);
        }
        return written;
    }

private:
    [[nodiscard]] int bytesPerFrame() const {
        const auto sampleBytes = bytesPerSample(_format.sampleFormat());
        const auto channels = _format.channelCount();
        return (sampleBytes > 0 && channels > 0)
            ? sampleBytes * channels
            : 0;
    }

    void processSamples(const char *data, qint64 len) {
        const auto frameBytes = bytesPerFrame();
        const auto sampleBytes = bytesPerSample(_format.sampleFormat());
        if (frameBytes <= 0 || sampleBytes <= 0 || _format.channelCount() <= 0) {
            return;
        }

        _pending.append(data, int(len));
        const auto fullBytes = (_pending.size() / frameBytes) * frameBytes;
        const auto *buffer = _pending.constData();
        for (auto offset = 0; offset < fullBytes; offset += frameBytes) {
            auto frameLevel = 0.0f;
            for (auto channel = 0; channel != _format.channelCount(); ++channel) {
                const auto sample = buffer + offset + channel * sampleBytes;
                frameLevel = std::max(frameLevel, sampleAmplitude(sample));
            }
            appendFrameLevel(frameLevel);
        }
        if (fullBytes > 0) {
            _pending.remove(0, fullBytes);
        }
    }

    [[nodiscard]] float sampleAmplitude(const char *sample) const {
        switch (_format.sampleFormat()) {
        case QAudioFormat::UInt8: {
            const auto value = static_cast<unsigned char>(sample[0]);
            return std::clamp(std::abs(int(value) - 128) / 128.0f, 0.0f, 1.0f);
        }
        case QAudioFormat::Int16: {
            qint16 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return std::clamp(std::abs(float(value)) / 32768.0f, 0.0f, 1.0f);
        }
        case QAudioFormat::Int32: {
            qint32 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return std::clamp(std::abs(double(value)) / 2147483648.0, 0.0, 1.0);
        }
        case QAudioFormat::Float: {
            float value = 0.0f;
            std::memcpy(&value, sample, sizeof(value));
            return std::clamp(std::abs(value), 0.0f, 1.0f);
        }
        case QAudioFormat::NSampleFormats:
        case QAudioFormat::Unknown:
            break;
        }
        return 0.0f;
    }

    void appendFrameLevel(float level) {
        _currentLevel = std::max(_currentLevel, level);
        ++_samplesInLevel;
        if (_samplesInLevel >= _samplesPerLevel) {
            _levels.push_back(_currentLevel);
            _currentLevel = 0.0f;
            _samplesInLevel = 0;
        }
    }

    [[nodiscard]] QByteArray buildWaveform() const {
        QVector<float> levels = _levels;
        if (_samplesInLevel > 0) {
            levels.push_back(_currentLevel);
        }
        if (levels.isEmpty()) {
            return QByteArray();
        }

        QByteArray result;
        result.reserve(kWaveformSamples);
        for (auto i = 0; i != kWaveformSamples; ++i) {
            const auto start = (i * levels.size()) / kWaveformSamples;
            auto end = ((i + 1) * levels.size()) / kWaveformSamples;
            if (end <= start) {
                end = start + 1;
            }
            auto maxLevel = 0.0f;
            for (auto j = start; j < end && j < levels.size(); ++j) {
                maxLevel = std::max(maxLevel, levels[j]);
            }
            const auto sample = qBound(0, qRound(maxLevel * 31.0f), 31);
            result.append(char(sample));
        }
        return result;
    }

    bool writeHeader(quint64 dataSize) {
        if (!_file.isOpen() || !_file.seek(0)) {
            return false;
        }

        const auto sampleBytes = bytesPerSample(_format.sampleFormat());
        const auto channels = qMax(1, _format.channelCount());
        const auto sampleRate = qMax(1, _format.sampleRate());
        const auto bitsPerSample = sampleBytes * 8;
        const auto blockAlign = channels * sampleBytes;
        const auto byteRate = sampleRate * blockAlign;
        const auto audioFormat = (_format.sampleFormat() == QAudioFormat::Float)
            ? quint16(3)
            : quint16(1);

        QDataStream out(&_file);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4);
        out << checkedRiffSize(dataSize);
        out.writeRawData("WAVE", 4);
        out.writeRawData("fmt ", 4);
        out << quint32(16);
        out << audioFormat;
        out << quint16(channels);
        out << quint32(sampleRate);
        out << quint32(byteRate);
        out << quint16(blockAlign);
        out << quint16(bitsPerSample);
        out.writeRawData("data", 4);
        out << checkedDataSize(dataSize);
        _file.seek(qint64(dataSize) + 44);
        return out.status() == QDataStream::Ok;
    }

    QAudioFormat _format;
    QTemporaryFile _file;
    QString _path;
    quint64 _dataBytes = 0;
    QByteArray _pending;
    QVector<float> _levels;
    int _samplesPerLevel = 1;
    int _samplesInLevel = 0;
    float _currentLevel = 0.0f;
};

HistoryVoiceRecorder::HistoryVoiceRecorder(QObject *parent)
    : QObject(parent) {
}

HistoryVoiceRecorder::~HistoryVoiceRecorder() {
    cancel();
}

bool HistoryVoiceRecorder::start(QString *error) {
    cancel();

    const auto device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        if (error) {
            *error = tr("No microphone input is available.");
        }
        return false;
    }

    const auto format = chooseFormat(device, error);
    if (!format.isValid()) {
        return false;
    }

    auto capture = std::make_unique<WavCaptureDevice>(format);
    if (!capture->start(error)) {
        return false;
    }

    auto source = std::make_unique<QAudioSource>(device, format);
    QObject::connect(source.get(), &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState
            && _recording
            && _source
            && _source->error() != QAudio::NoError) {
            emit failed(tr("Microphone recording failed."));
        }
    });
    source->start(capture.get());
    if (source->error() != QAudio::NoError) {
        capture->cancel();
        if (error) {
            *error = tr("Microphone recording failed to start.");
        }
        return false;
    }

    _device = std::move(capture);
    _source = std::move(source);
    _recording = true;
    return true;
}

HistoryVoiceRecorder::Result HistoryVoiceRecorder::stop() {
    if (!_recording || !_device) {
        return {};
    }

    _recording = false;
    if (_source) {
        _source->stop();
        _source.reset();
    }

    auto result = _device->finish();
    _device.reset();
    return result;
}

void HistoryVoiceRecorder::cancel() {
    _recording = false;
    if (_source) {
        _source->stop();
        _source.reset();
    }
    if (_device) {
        _device->cancel();
        _device.reset();
    }
}

bool HistoryVoiceRecorder::isRecording() const {
    return _recording;
}

quint64 HistoryVoiceRecorder::durationMs() const {
    return _device ? _device->durationMs() : 0;
}

} // namespace TeleMatrix
