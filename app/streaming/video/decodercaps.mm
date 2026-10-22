#include "decodercaps.h"
#include "decodercaps-test-frames.h"

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QVector>

#include <sys/sysctl.h>

namespace {

QMutex s_ProbeLock;

struct TestFrame
{
    QSize size;
    const unsigned char* data;
    size_t length;
};

// Test frames for one encoding mode, largest first; empty for modes without one.
QVector<TestFrame> testFrames(const QString& encodingMode, QString& profile)
{
    const bool fullChroma = encodingMode == QLatin1String("hevc-10-444-nvenc") ||
            encodingMode == QLatin1String("hevc-10-444-videotoolbox");
    const bool main10 = encodingMode == QLatin1String("hevc-10-420-nvenc") ||
            encodingMode == QLatin1String("hevc-10-420-videotoolbox");
    if (fullChroma) {
        profile = QStringLiteral("hevc-rext-444-10");
        return {{QSize(8192, 4320), k_DecoderCapsRext444_8192x4320, sizeof(k_DecoderCapsRext444_8192x4320)},
                {QSize(7680, 4320), k_DecoderCapsRext444_7680x4320, sizeof(k_DecoderCapsRext444_7680x4320)}};
    }
    if (main10) {
        profile = QStringLiteral("hevc-main10");
        return {{QSize(8192, 4320), k_DecoderCapsMain10_8192x4320, sizeof(k_DecoderCapsMain10_8192x4320)},
                {QSize(7680, 4320), k_DecoderCapsMain10_7680x4320, sizeof(k_DecoderCapsMain10_7680x4320)}};
    }
    return {};
}

QString sysctlString(const char* name)
{
    size_t length = 0;
    if (sysctlbyname(name, nullptr, &length, nullptr, 0) != 0 || length == 0) return QString();
    QByteArray value(int(length), '\0');
    if (sysctlbyname(name, value.data(), &length, nullptr, 0) != 0) return QString();
    return QString::fromUtf8(value.constData());
}

// Chip and OS build: a new macOS or another Mac is probed again.
QString cacheKey(const QString& profile)
{
    const QByteArray machine = (sysctlString("machdep.cpu.brand_string") + QLatin1Char('|') +
                                sysctlString("kern.osversion")).toUtf8();
    return QStringLiteral("decoder-caps/%1/%2")
            .arg(QString::fromLatin1(QCryptographicHash::hash(machine, QCryptographicHash::Sha256).toHex().left(16)),
                 profile);
}

// Splits Annex B into NAL units (without start codes).
QVector<QByteArray> nalUnits(const QByteArray& stream)
{
    QVector<QByteArray> units;
    int start = -1;
    for (int index = 0; index + 2 < stream.size(); ++index) {
        if (stream.at(index) == 0 && stream.at(index + 1) == 0 && stream.at(index + 2) == 1) {
            if (start >= 0) {
                int end = index;
                while (end > start && stream.at(end - 1) == 0) --end;
                units.append(stream.mid(start, end - start));
            }
            start = index + 3;
            index += 2;
        }
    }
    if (start >= 0 && start < stream.size()) units.append(stream.mid(start));
    return units;
}

void decodeCallback(void* context, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image,
                    CMTime, CMTime)
{
    auto* result = static_cast<QSize*>(context);
    if (status == noErr && image != nullptr) {
        *result = QSize(int(CVPixelBufferGetWidth(image)), int(CVPixelBufferGetHeight(image)));
    }
}

// Decodes one IDR in hardware; true when VideoToolbox returns its image.
bool decodes(const TestFrame& frame)
{
    const QByteArray stream = qUncompress(frame.data, int(frame.length));
    if (stream.isEmpty()) return false;
    QVector<QByteArray> parameterSets;
    QByteArray sample;
    for (const QByteArray& unit : nalUnits(stream)) {
        if (unit.size() < 2) continue;
        const int type = (uchar(unit.at(0)) >> 1) & 0x3f;
        if (type == 32 || type == 33 || type == 34) {
            parameterSets.append(unit);
        } else if (type <= 21) {
            // VCL: 4-byte big-endian length prefixes (hvcC NAL length size 4).
            const quint32 length = quint32(unit.size());
            const char prefix[4] = {char(length >> 24), char(length >> 16), char(length >> 8), char(length)};
            sample.append(prefix, 4);
            sample.append(unit);
        }
    }
    if (parameterSets.size() != 3 || sample.isEmpty()) return false;

    const uint8_t* pointers[3];
    size_t sizes[3];
    for (int index = 0; index < 3; ++index) {
        pointers[index] = reinterpret_cast<const uint8_t*>(parameterSets.at(index).constData());
        sizes[index] = size_t(parameterSets.at(index).size());
    }
    CMVideoFormatDescriptionRef format = nullptr;
    if (CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, pointers, sizes, 4, nullptr,
                                                            &format) != noErr) {
        return false;
    }
    const void* keys[] = {kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                       &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
    QSize decoded;
    VTDecompressionOutputCallbackRecord callback = {decodeCallback, &decoded};
    VTDecompressionSessionRef session = nullptr;
    OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, specification, nullptr, &callback,
                                                   &session);
    CFRelease(specification);
    bool ok = false;
    if (status == noErr) {
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, sample.data(), size_t(sample.size()),
                                               kCFAllocatorNull, nullptr, 0, size_t(sample.size()), 0,
                                               &block) == noErr) {
            CMSampleBufferRef buffer = nullptr;
            const size_t sampleSize = size_t(sample.size());
            if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1, &sampleSize,
                                          &buffer) == noErr) {
                VTDecodeInfoFlags flags = 0;
                status = VTDecompressionSessionDecodeFrame(session, buffer, 0, nullptr, &flags);
                VTDecompressionSessionWaitForAsynchronousFrames(session);
                ok = status == noErr && decoded == frame.size;
                CFRelease(buffer);
            }
            CFRelease(block);
        }
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
    }
    CFRelease(format);
    return ok;
}

QString sizeText(const QSize& size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QSize parseSize(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('x'));
    if (parts.size() != 2) return QSize();
    const QSize size(parts.at(0).toInt(), parts.at(1).toInt());
    return size.width() > 0 && size.height() > 0 ? size : QSize();
}

}

QSize DecoderCaps::cachedMaximum(const QString& encodingMode)
{
    QString profile;
    if (testFrames(encodingMode, profile).isEmpty()) return QSize();
    QSettings settings;
    return parseSize(settings.value(cacheKey(profile)).toString());
}

QSize DecoderCaps::probe(const QString& encodingMode)
{
    QString profile;
    const QVector<TestFrame> frames = testFrames(encodingMode, profile);
    if (frames.isEmpty()) return QSize();
    QMutexLocker lock(&s_ProbeLock);
    QSize maximum = fallbackMaximum();
    for (const TestFrame& frame : frames) {
        if (decodes(frame)) {
            maximum = frame.size;
            break;
        }
    }
    qInfo() << "Hardware decoder maximum for" << profile << ":" << maximum;
    QSettings settings;
    settings.setValue(cacheKey(profile), sizeText(maximum));
    return maximum;
}

QSize DecoderCaps::maximum(const QString& encodingMode)
{
    const QSize cached = cachedMaximum(encodingMode);
    return cached.isValid() ? cached : probe(encodingMode);
}
