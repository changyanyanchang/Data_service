#include "store/seismic_window_metadata.h"
#include <QByteArray>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace paleo::data::detail {
namespace {
bool read(const SeismicAuthenticatedSource& source, std::int64_t offset, int count, QByteArray& bytes,
    const SeismicWindowReadOptions& options, SeismicWindowReadStats& stats, std::string& error) {
    if (seismicWindowStopped(options, error)) return false;
    if (offset < 0 || count < 1 || offset > source.byteSize || count > source.byteSize - offset ||
        stats.metadataBytesRead + stats.requestedSourceBytes > options.maxReadBytes - count) {
        error = "SeismicWindowReadBudgetExceeded"; return false;
    }
    std::vector<std::uint8_t> output; std::string detail;
    ++stats.sourceReadCalls; stats.requestedSourceBytes += count;
    const bool ok = source.read(offset, count, output, detail, options.cancelled);
    if (seismicWindowStopped(options, error)) return false;
    if (!ok || output.size() != std::size_t(count)) { error = "SeismicWindowAuthenticatedReadFailed"; return false; }
    bytes = QByteArray(reinterpret_cast<const char*>(output.data()), count); return true;
}
}
bool decodeAuthenticatedSeismicWindow(const SeismicWindowQuery& query, const SeismicAuthenticatedSource& source,
    const SeismicWindowDescriptor& descriptor, SeismicWindow& result, const SeismicWindowReadOptions& options,
    SeismicWindowReadStats& stats, std::string& error) {
    const auto reserved = std::int64_t(query.sampleCount) * 25 + 16384 + options.metadata.maxTextBytes * 4;
    if (reserved > options.maxWorkingBytes) { error = "SeismicWindowWorkingBudgetExceeded"; return false; }
    stats.reservedBytes = reserved;
    QByteArray binary;
    if (!read(source, 3200, 400, binary, options, stats, error)) return false;
    const auto sampleCount = qFromBigEndian<quint16>(binary.constData() + 20);
    const auto intervalUs = qFromBigEndian<quint16>(binary.constData() + 16);
    const auto extended = qFromBigEndian<qint16>(binary.constData() + 304);
    if (qFromBigEndian<quint16>(binary.constData() + 24) != 5 ||
        qFromBigEndian<quint16>(binary.constData() + 300) != 0x0100 ||
        qFromBigEndian<quint16>(binary.constData() + 302) != 1 || extended < 0 || extended > 32 ||
        sampleCount != descriptor.totalSamples || intervalUs < 1 ||
        std::abs(double(intervalUs) / 1000 - descriptor.sampleInterval) > 1e-9) {
        error = "SeismicWindowUnsupportedOrChangedLayout"; return false;
    }
    const std::int64_t firstTrace = 3600 + std::int64_t(extended) * 3200;
    const std::int64_t traceBytes = 240 + std::int64_t(sampleCount) * 4;
    if (source.byteSize <= firstTrace || (source.byteSize - firstTrace) % traceBytes ||
        query.traceIndex >= (source.byteSize - firstTrace) / traceBytes) {
        error = "SeismicWindowInvalidTraceIndex"; return false;
    }
    const auto traceOffset = firstTrace + query.traceIndex * traceBytes;
    QByteArray header;
    if (!read(source, traceOffset, 240, header, options, stats, error)) return false;
    const auto traceNs = qFromBigEndian<quint16>(header.constData() + 114);
    const auto traceDt = qFromBigEndian<quint16>(header.constData() + 116);
    const auto scalar = qFromBigEndian<qint16>(header.constData() + 214);
    const double factor = scalar > 0 ? scalar : scalar < 0 ? 1.0 / -double(scalar) : 1.0;
    const double delay = qFromBigEndian<qint16>(header.constData() + 108) * factor;
    if ((traceNs && traceNs != sampleCount) || (traceDt && traceDt != intervalUs) ||
        !std::isfinite(delay) || std::abs(delay - descriptor.firstTime) > 1e-9) {
        error = "SeismicWindowTraceAxisMismatch"; return false;
    }
    QByteArray values;
    if (!read(source, traceOffset + 240 + std::int64_t(query.firstSample) * 4, query.sampleCount * 4,
        values, options, stats, error)) return false;
    SeismicWindow pending; pending.descriptor = descriptor; pending.sourceSha256 = source.sha256;
    pending.traceIndex = query.traceIndex; pending.firstSample = query.firstSample;
    pending.traceNumber = qFromBigEndian<qint32>(header.constData() + 4);
    pending.inlineNumber = qFromBigEndian<qint32>(header.constData() + 188);
    pending.crosslineNumber = qFromBigEndian<qint32>(header.constData() + 192);
    pending.axis.reserve(query.sampleCount); pending.amplitudes.reserve(query.sampleCount); pending.nullMask.reserve(query.sampleCount);
    for (int index = 0; index < query.sampleCount; ++index) {
        if (seismicWindowStopped(options, error)) return false;
        const auto bits = qFromBigEndian<quint32>(values.constData() + index * 4); float value;
        static_assert(sizeof(value) == sizeof(bits)); std::memcpy(&value, &bits, sizeof(value));
        const auto axis = descriptor.firstTime + (query.firstSample + index) * descriptor.sampleInterval;
        if (!std::isfinite(axis)) { error = "SeismicWindowAxisOverflow"; return false; }
        pending.axis.push_back(axis); pending.amplitudes.push_back(std::isfinite(value) ? double(value) : 0);
        pending.nullMask.push_back(std::isfinite(value) ? 0 : 1);
    }
    stats.outputBytes = std::int64_t(query.sampleCount) * 17; result = std::move(pending); return true;
}
}
