// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#endif
#include "Engine/PagedWorkspace.h"

namespace {
using namespace seismic::engine;
using seismic::AxisDescriptor;
std::filesystem::path nativePath(const QString& text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}
template<class T> void append(QByteArray& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
template<class T> bool patch(const QString& path, quint64 offset, T value) {
    QFile file(path);
    return file.open(QIODevice::ReadWrite) && file.seek(qint64(offset)) &&
        file.write(reinterpret_cast<const char*>(&value), sizeof(value)) == sizeof(value);
}
quint64 directoryOffset(const PagedWorkspaceInfo& info) {
    return 4096 + (info.ChunkCount() + 7) / 8 + info.ChunkCount() * 12;
}
quint64 dataOffset(const PagedWorkspaceInfo& info) {
    return directoryOffset(info) + info.PageCount() * 32;
}
QByteArray header(const PagedWorkspaceInfo& info) {
    QByteArray bytes(kPagedWorkspaceMagic, 8);
    append<quint32>(bytes, 7); append<quint32>(bytes, 4096);
    for(quint32 n : {info.algorithmVersion, info.chunkInlines, info.chunkXlines, info.chunkSamples,
                    info.pageInlines, info.pageXlines, info.pageSamples, info.samples,
                    info.sampleIntervalUs, info.codec, info.codecLevel, info.lodLevel, info.lodMethod,
                    info.lodFactorSamples, info.lodFactorInlines, info.lodFactorXlines, info.lodAlgorithmVersion})
        append(bytes, n);
    for(quint64 n : std::array<quint64, 5>{info.lodSourceHash, info.sourceSize, quint64(info.sourceMtimeTicks),
                                         info.sourceFingerprint, info.buildGeneration}) append(bytes, n);
    for(qint32 n : {info.coverageInlineMin, info.coverageInlineMax, info.coverageXlineMin, info.coverageXlineMax})
        append(bytes, n);
    append<quint32>(bytes, info.complete ? 1 : 0); append<quint32>(bytes, 0);
    for(const auto& axis : {info.inlineAxis, info.xlineAxis}) {
        append<quint32>(bytes, 0); append<qint32>(bytes, axis.count);
        append<qint32>(bytes, axis.origin); append<qint32>(bytes, axis.step);
    }
    bytes.resize(4096, '\0');
    return bytes;
}
quint64 legacyChecksum(const QByteArray& bytes) {
    quint64 hash = 1099511628211ull;
    for(unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}
bool markSparse(QFile& file) {
#ifdef _WIN32
    DWORD returned = 0;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    return DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
#else
    Q_UNUSED(file)
    return true;
#endif
}
// Only a header, bitmap and two payload pages are physically written. The >1GiB
// directory is a sparse hole; no original file, full directory or full body copy.
bool sparseFixture(const QString& path, const PagedWorkspaceInfo& info) {
    QFile file(path);
    if(!file.open(QIODevice::ReadWrite) || !markSparse(file)) return false;
    if(file.write(header(info)) != 4096) return false;
    if(!file.resize(qint64(dataOffset(info) + 2 * info.PageBytes()))) return false;
    QByteArray bitmap(qsizetype((info.ChunkCount() + 7) / 8), char(0xff));
    if(!file.seek(4096) || file.write(bitmap) != bitmap.size()) return false;
    for(int k = 0; k < 2; ++k) {
        const quint64 page = k == 0 ? 0 : info.PageCount() - 1;
        QByteArray values;
        for(quint64 n = 0; n < info.PageBytes() / 4; ++n) append<float>(values, float(11 + k));
        const auto payloadAt = dataOffset(info) + k * info.PageBytes();
        QByteArray entry;
        append<quint64>(entry, payloadAt); append<quint32>(entry, quint32(values.size()));
        append<quint32>(entry, quint32(values.size())); append<quint64>(entry, legacyChecksum(values));
        append<quint32>(entry, kPagedCodecRaw); append<quint32>(entry, 0);
        if(!file.seek(qint64(directoryOffset(info) + page * 32)) || file.write(entry) != 32 ||
           !file.seek(qint64(payloadAt)) || file.write(values) != values.size()) return false;
    }
    return true;
}
PagedWorkspaceInfo largeInfo() {
    PagedWorkspaceInfo info;
    info.inlineAxis = AxisDescriptor::Uniform(1260, 1, 3901);
    info.xlineAxis = AxisDescriptor::Uniform(680, 1, 3241);
    info.samples = 4096; info.sampleIntervalUs = 2000;
    info.complete = true; info.buildGeneration = 1;
    return info;
}
PagedWorkspaceInfo smallInfo() {
    auto info = largeInfo();
    info.inlineAxis = AxisDescriptor::Uniform(100, 2, 5);
    info.xlineAxis = AxisDescriptor::Uniform(200, 3, 6);
    info.samples = 19;
    info.chunkInlines = info.chunkXlines = 4; info.chunkSamples = 16;
    info.pageInlines = info.pageXlines = 2; info.pageSamples = 4;
    return info;
}
float valueAt(int i, int x, int t) {
    return i == 2 && x == 3 && t == 9 ? std::numeric_limits<float>::quiet_NaN() : float(10000 * i + 100 * x + t);
}
bool smallFixture(const QString& path, PagedWorkspaceInfo info, std::string& error) {
    PagedWorkspaceWriter writer;
    if(!writer.Open(nativePath(path), info, false, error)) return false;
    std::vector<float> chunk(info.chunkInlines * info.chunkXlines * info.chunkSamples);
    for(unsigned ct = 0; ct < info.ChunksT(); ++ct)
        for(unsigned cx = 0; cx < info.ChunksX(); ++cx)
            for(unsigned ci = 0; ci < info.ChunksI(); ++ci) {
                for(unsigned t = 0; t < info.chunkSamples; ++t)
                    for(unsigned x = 0; x < info.chunkXlines; ++x)
                        for(unsigned i = 0; i < info.chunkInlines; ++i)
                            chunk[(t * info.chunkXlines + x) * info.chunkInlines + i] =
                                valueAt(ci * info.chunkInlines + i, cx * info.chunkXlines + x, ct * info.chunkSamples + t);
                if(!writer.WriteChunk(ci, cx, ct, chunk.data(), error)) return false;
            }
    return writer.Finalize(error);
}
}

class Sf3pDirectoryTests : public QObject {
    Q_OBJECT
private slots:
    void largeDirectoryStaysLazyAndBounded() {
        QTemporaryDir dir; const auto path = dir.filePath("large.sf3p"); const auto info = largeInfo();
        QCOMPARE(info.PageCount(), quint64(50720768));
        QCOMPARE(info.PageCount() * 32, quint64(1623064576));
        QVERIFY(sparseFixture(path, info));
        PagedWorkspaceReader reader; std::string error;
        reader.SetDirectoryCacheBudget(256u << 10);
        QVERIFY2(reader.Open(nativePath(path), error), error.c_str());
        QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(0));
        QCOMPARE(reader.ReaderStats().directoryCacheBytes, quint64(0));
        std::vector<float> values{99};
        QVERIFY(!reader.ReadBox(1260, 680, 0, 3901, 3241, 4096, values, error));
        QVERIFY(values.empty());
        QCOMPARE(reader.ReaderStats().headerReadBytes, quint64(4112));
        QVERIFY2(reader.ReadBox(1260, 680, 0, 1, 1, 1, values, error), error.c_str());
        QCOMPARE(values, std::vector<float>{11});
        QVERIFY2(reader.ReadBox(5160, 3920, 4095, 1, 1, 1, values, error), error.c_str());
        QCOMPARE(values, std::vector<float>{12});
        QVERIFY(reader.ReaderStats().directoryReadBytes <= (256u << 10));
        QVERIFY(reader.ReaderStats().directoryCachePeakBytes <= reader.DirectoryCacheBudget());
        reader.ResetReaderStats();
        QVERIFY(reader.ReadBox(5160, 3920, 4095, 1, 1, 1, values, error));
        QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(0));
        QVERIFY(reader.ReaderStats().directoryCacheHits >= 2);
        reader.SetDirectoryCacheBudget(64u << 10); reader.ResetReaderStats();
        for(int n = 0; n < 3; ++n) {
            QVERIFY(reader.ReadBox(1260, 680, 0, 1, 1, 1, values, error));
            QVERIFY(reader.ReadBox(5160, 3920, 4095, 1, 1, 1, values, error));
        }
        QVERIFY(reader.ReaderStats().directoryCachePeakBytes <= (64u << 10));
        reader.SetDirectoryCacheBudget(0); reader.ResetReaderStats();
        QVERIFY(reader.ReadBox(1260, 680, 0, 1, 1, 1, values, error));
        QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(33));
        QCOMPARE(reader.ReaderStats().directoryCacheBytes, quint64(0));
    }
    void roundTrip_data() {
        QTest::addColumn<int>("version"); QTest::addColumn<bool>("compressed");
        QTest::newRow("v6-raw") << 6 << false;
        QTest::newRow("v7-raw") << 7 << false;
#ifdef SEISMIC_HAVE_ZSTD
        QTest::newRow("v7-zstd") << 7 << true;
#endif
    }
    void widePlaneDirectoryReadsFollowChunkOrder() {
        QTemporaryDir dir; const auto path = dir.filePath("locality.sf3p");
        auto info = largeInfo(); info.inlineAxis = AxisDescriptor::Uniform(0, 1, 2048);
        info.xlineAxis = AxisDescriptor::Uniform(0, 1, 512); info.samples = 16;
        QVERIFY(sparseFixture(path, info));
        // All requested page-0 entries share a valid payload. Cancel before
        // decoding: the assertion measures metadata planning, not disk throughput.
        { QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
          QVERIFY(file.seek(qint64(directoryOffset(info)))); const auto firstEntry = file.read(32);
          QCOMPARE(firstEntry.size(), qsizetype(32));
          for(quint64 chunk = 0; chunk < info.ChunkCount(); ++chunk) {
              QVERIFY(file.seek(qint64(directoryOffset(info) + chunk * info.PagesPerChunk() * 32)));
              QCOMPARE(file.write(firstEntry), qint64(32));
          }
        }
        PagedWorkspaceReader reader; std::string error; reader.SetDirectoryCacheBudget(192u << 10);
        QVERIFY(reader.Open(nativePath(path), error));
        CancelToken cancel; cancel.SetPredicate([&] { return reader.ReaderStats().readBytes > 0; });
        std::vector<float> output;
        QVERIFY(!reader.ReadBox(0, 0, 0, 2048, 512, 1, output, error, &cancel));
        QVERIFY(output.empty()); QVERIFY(reader.ReaderStats().readBytes > 0);
        QVERIFY(reader.ReaderStats().directoryReadBytes <= info.PageCount() * 32 + (info.ChunkCount() + 7) / 8);
        QVERIFY(reader.ReaderStats().directoryCachePeakBytes <= (192u << 10));
    }
    void roundTrip() {
        QFETCH(int, version); QFETCH(bool, compressed);
        QTemporaryDir dir; const auto path = dir.filePath("roundtrip.sf3p");
        auto info = smallInfo(); info.version = version; info.codec = compressed ? kPagedCodecZstd : kPagedCodecRaw;
        std::string error; QVERIFY2(smallFixture(path, info, error), error.c_str());
        PagedWorkspaceReader reader; reader.SetDirectoryCacheBudget(2048);
        QVERIFY2(reader.Open(nativePath(path), error), error.c_str());
        std::vector<float> values;
        QVERIFY2(reader.ReadBox(102, 203, 3, 3, 4, 13, values, error), error.c_str());
        for(int t = 0; t < 13; ++t) for(int x = 0; x < 4; ++x) for(int i = 0; i < 3; ++i) {
            const auto actual = values[(t * 4 + x) * 3 + i]; const auto expected = valueAt(i + 1, x + 1, t + 3);
            if(std::isnan(expected)) QVERIFY(std::isnan(actual)); else QCOMPARE(actual, expected);
        }
        QVERIFY(reader.ReaderStats().directoryCachePeakBytes <= 2048);
        values = {99}; QVERIFY(!reader.ReadBox(101, 203, 3, 1, 1, 1, values, error)); QVERIFY(values.empty());
        QVERIFY(reader.ReadTimeSlice(3, values, error));
        for(int i = 0; i < 5; ++i) for(int x = 0; x < 6; ++x)
            QCOMPARE(values[(4 - i) * 6 + x], valueAt(i, x, 3));
    }
    void generationChangesCacheKeyWithSamePathSizeAndMtime() {
        QTemporaryDir dir; const auto path = dir.filePath("generation.sf3p"); std::string error;
        QVERIFY2(smallFixture(path, smallInfo(), error), error.c_str());
        const auto stamp = std::filesystem::last_write_time(nativePath(path));
        auto cache = std::make_shared<ChunkCache>(); std::uint64_t firstKey = 0;
        { PagedWorkspaceReader first; QVERIFY(first.Open(nativePath(path), error)); first.SetPageCache(cache);
          std::vector<float> values; QVERIFY(first.ReadBox(100, 200, 0, 1, 1, 1, values, error)); firstKey = first.CacheSourceKey(); }
        QVERIFY(patch<quint64>(path, 116, 2));
        std::filesystem::last_write_time(nativePath(path), stamp);
        PagedWorkspaceReader second; QVERIFY(second.Open(nativePath(path), error)); second.SetPageCache(cache);
        QVERIFY(firstKey != second.CacheSourceKey());
        std::vector<float> values; QVERIFY(second.ReadBox(100, 200, 0, 1, 1, 1, values, error));
        QCOMPARE(second.ReaderStats().cacheHits, quint64(0)); QVERIFY(second.ReaderStats().readBytes > 0);
    }
    void invalidEntries_data() {
        QTest::addColumn<int>("kind");
        for(int n = 0; n < 8; ++n) QTest::newRow(qPrintable(QString::number(n))) << n;
    }
    void invalidEntries() {
        QFETCH(int, kind); QTemporaryDir dir; const auto path = dir.filePath("bad.sf3p");
        const auto info = smallInfo(); std::string error; QVERIFY(smallFixture(path, info, error));
        const auto entry = directoryOffset(info);
        if(kind == 0) QVERIFY(patch<quint64>(path, entry, dataOffset(info) - 1));
        if(kind == 1) QVERIFY(patch<quint64>(path, entry, std::numeric_limits<quint64>::max()));
        if(kind == 2) QVERIFY(patch<quint32>(path, entry + 8, std::numeric_limits<quint32>::max()));
        if(kind == 3) QVERIFY(patch<quint32>(path, entry + 12, 4));
        if(kind == 4) QVERIFY(patch<quint32>(path, entry + 24, 777));
        if(kind == 5) QVERIFY(patch<quint64>(path, entry + 16, 0));
        if(kind == 6) QVERIFY(patch<quint8>(path, 4096, 0));
        if(kind == 7) QVERIFY(patch<quint32>(path, entry + 8, 8));
        PagedWorkspaceReader reader; QVERIFY(reader.Open(nativePath(path), error));
        std::vector<float> values{99}; QVERIFY(!reader.ReadBox(100, 200, 0, 1, 1, 1, values, error));
        QVERIFY(values.empty()); QVERIFY(!error.empty());
        if(kind != 5) QCOMPARE(reader.ReaderStats().readBytes, quint64(0));
    }
    void headerOnlyOverflowAndPartialSemantics() {
        QTemporaryDir dir; const auto path = dir.filePath("header.sf3p");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(header(largeInfo())), qint64(4096)); }
        PagedWorkspaceReader reader; std::string error;
        QVERIFY(reader.Open(nativePath(path), error, true)); QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(0));
        std::vector<float> values{99}; QVERIFY(!reader.ReadBox(1260, 680, 0, 1, 1, 1, values, error)); QVERIFY(values.empty());
        QVERIFY(!reader.Open(nativePath(path), error));
        QVERIFY(patch<quint32>(path, 20, 0)); QVERIFY(!reader.Open(nativePath(path), error, true));
        auto badAxis = largeInfo(); badAxis.inlineAxis = AxisDescriptor::Uniform(-2000000000, 2000000000, 3);
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(header(badAxis)), qint64(4096)); }
        QVERIFY(!reader.Open(nativePath(path), error, true));
        auto overflowing = largeInfo(); overflowing.inlineAxis.count = overflowing.xlineAxis.count = std::numeric_limits<int>::max();
        overflowing.inlineAxis.origin = overflowing.xlineAxis.origin = 0;
        overflowing.samples = std::numeric_limits<int>::max();
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(header(overflowing)), qint64(4096)); }
        QVERIFY(!reader.Open(nativePath(path), error, true));
        auto info = smallInfo(); info.complete = false; const auto final = dir.filePath("partial.sf3p");
        { PagedWorkspaceWriter writer; QVERIFY(writer.Open(nativePath(final), info, false, error));
          std::vector<float> chunk(4 * 4 * 16, 7); QVERIFY(writer.WriteChunk(0, 0, 0, chunk.data(), error)); }
        const auto partial = nativePath(final + ".partial"); QVERIFY(reader.Open(partial, error)); QVERIFY(!reader.Info().complete);
        QVERIFY(reader.ReadBox(100, 200, 0, 1, 1, 1, values, error)); QCOMPARE(values[0], 7.0f);
        QVERIFY(!reader.ReadBox(108, 215, 18, 1, 1, 1, values, error)); QVERIFY(values.empty());
        Status status; QVERIFY(!PagedWorkspaceVolumeSource::Open(partial, status));
    }
    void truncationAfterOpenClearsOutputAndCountsPartialDirectoryRead() {
        QTemporaryDir dir; const auto path = dir.filePath("truncated.sf3p"); const auto info = smallInfo();
        std::string error; QVERIFY(smallFixture(path, info, error));
        PagedWorkspaceReader reader; reader.SetDirectoryCacheBudget(0); QVERIFY(reader.Open(nativePath(path), error));
        { QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(qint64(directoryOffset(info) + 8))); }
        std::vector<float> values{99}; QVERIFY(!reader.ReadBox(100, 200, 0, 1, 1, 1, values, error));
        QVERIFY(values.empty()); QCOMPARE(reader.ReaderStats().directoryReadCalls, quint64(2));
        QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(9));
    }
    void cancelledAndOverflowingRequestsClearOutput() {
        QTemporaryDir dir; const auto path = dir.filePath("cancel.sf3p"); std::string error;
        QVERIFY(smallFixture(path, smallInfo(), error)); PagedWorkspaceReader reader; QVERIFY(reader.Open(nativePath(path), error));
        std::vector<float> values{99}; CancelToken cancel;
        cancel.SetPredicate([&] { return reader.ReaderStats().readBytes > 0; });
        QVERIFY(!reader.ReadBox(100, 200, 0, 5, 6, 19, values, error, &cancel));
        QVERIFY(reader.ReaderStats().readBytes > 0); QVERIFY(values.empty());
        std::vector<unsigned char> bytes{99};
        QVERIFY(!reader.ReadPageBytes(0, 1, std::numeric_limits<quint64>::max(), bytes, error)); QVERIFY(bytes.empty());
        QVERIFY(!reader.ReadBox(100, 200, 0, std::numeric_limits<int>::max(), 1, 1, values, error)); QVERIFY(values.empty());
    }
    void existingReadOnlyWindow() {
        const auto path = qEnvironmentVariable("PALEO_SF3P_READONLY_PATH");
        if(path.isEmpty()) QSKIP("Optional bounded read-only verification requires PALEO_SF3P_READONLY_PATH");
        const QFileInfo before(path); QVERIFY(before.isFile());
        const auto sizeBefore = before.size(); const auto modifiedBefore = before.lastModified();
        PagedWorkspaceReader reader; std::string error; QVERIFY2(reader.Open(nativePath(path), error), error.c_str());
        QCOMPARE(reader.ReaderStats().directoryReadBytes, quint64(0));
        std::vector<float> value;
        QVERIFY2(reader.ReadBox(reader.Info().inlineAxis.ValueAt(0), reader.Info().xlineAxis.ValueAt(0),
                               0, 1, 1, 1, value, error), error.c_str());
        QCOMPARE(value.size(), size_t(1));
        const QFileInfo after(path); QCOMPARE(sizeBefore, after.size()); QCOMPARE(modifiedBefore, after.lastModified());
        qInfo() << "read-only SF3P bytes: header" << reader.ReaderStats().headerReadBytes
                << "directory+bitmap" << reader.ReaderStats().directoryReadBytes
                << "payload" << reader.ReaderStats().readBytes
                << "directory retained/peak" << reader.ReaderStats().directoryCacheBytes << reader.ReaderStats().directoryCachePeakBytes;
    }
    void cancellationAtFinalScatter_data() {
        QTest::addColumn<bool>("warm");
        QTest::newRow("after-final-decode") << false;
        QTest::newRow("after-final-cache-hit") << true;
    }
    void cancellationAtFinalScatter() {
        QFETCH(bool, warm);
        QTemporaryDir dir; const auto path = dir.filePath("final-cancel.sf3p"); std::string error;
        QVERIFY(smallFixture(path, smallInfo(), error));
        PagedWorkspaceReader reader; QVERIFY(reader.Open(nativePath(path), error));
        auto cache = std::make_shared<ChunkCache>(); reader.SetPageCache(cache);
        std::vector<float> values;
        if(warm) QVERIFY(reader.ReadBox(100, 200, 0, 1, 1, 1, values, error));
        reader.ResetReaderStats(); cache->ResetStats(); values = {99};
        CancelToken cancel;
        // Find/Insert happens after the preceding poll and before the final
        // scatter. This exercises late cancellation without thread timing.
        cancel.SetPredicate([&] { const auto stats = cache->Stats(); return warm ? stats.hits > 0 : stats.inserts > 0; });
        QVERIFY(!reader.ReadBox(100, 200, 0, 1, 1, 1, values, error, &cancel));
        QVERIFY(values.empty()); QCOMPARE(error, std::string("request cancelled"));
        if(warm) { QCOMPARE(reader.ReaderStats().cacheHits, quint64(1)); QCOMPARE(reader.ReaderStats().readBytes, quint64(0)); }
        else { QCOMPARE(cache->Stats().inserts, quint64(1)); QVERIFY(reader.ReaderStats().readBytes > 0); }
    }
};
QTEST_GUILESS_MAIN(Sf3pDirectoryTests)
#include "sf3p_directory_tests.moc"
