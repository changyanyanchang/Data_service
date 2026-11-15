#include "paleo_data/service.h"
#include "store/project_store.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& value) { return value.toUtf8().toStdString(); }
void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
QByteArray segy() {
    constexpr int samples = 6, traceBytes = 240 + samples * 4;
    QByteArray bytes(3600 + 2 * traceBytes, 0);
    qToBigEndian<quint16>(2000, bytes.data() + 3216); qToBigEndian<quint16>(samples, bytes.data() + 3220);
    qToBigEndian<quint16>(5, bytes.data() + 3224); qToBigEndian<quint16>(0x0100, bytes.data() + 3500);
    qToBigEndian<quint16>(1, bytes.data() + 3502);
    for (int trace = 0; trace < 2; ++trace) {
        auto* header = bytes.data() + 3600 + trace * traceBytes;
        qToBigEndian<qint32>(trace == 0 ? 0 : 17, header + 4);
        qToBigEndian<quint16>(samples, header + 114); qToBigEndian<quint16>(2000, header + 116);
        qToBigEndian<qint32>(101 + trace, header + 188); qToBigEndian<qint32>(201, header + 192);
        for (int sample = 0; sample < samples; ++sample) {
            const float value = float(trace * 10 + sample - 2); quint32 bits; std::memcpy(&bits, &value, 4);
            qToBigEndian<quint32>(bits, header + 240 + sample * 4);
        }
    }
    return bytes;
}
struct Fixture {
    QTemporaryDir temporary; DataService service, reader; QByteArray bytes;
    std::string error, sha; long long sourceId = 0, volumeId = 0; int calls = 0;
    explicit Fixture(const std::function<void(QByteArray&)>& edit = {}) : bytes(segy()) {
        require(temporary.isValid(), "temporary directory"); if (edit) edit(bytes);
        const auto directory = temporary.path() + "/project", file = temporary.path() + "/source.sgy";
        require(service.createProject(u(directory), "authenticated seismic fixture", error), error);
        QFile source(file); require(source.open(QIODevice::WriteOnly) && source.write(bytes) == bytes.size(), "fixture write"); source.close();
        sha = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
        ParsedFile parsed; parsed.kind = "seismic"; parsed.status = "parsed";
        SeismicVolume volume; volume.volumeName = "trace fixture"; volume.dataFile = u(file); volume.fileFormat = "SEG-Y";
        volume.amplitudeUnit = "relative_amplitude"; volume.sampling = {6, 0, 2, "ms", "TWT", "source_zero"};
        volume.geometry.traceCount = 2; parsed.volumes.push_back(volume);
        require(service.importPreparedFile(u(file), u(file), u(file), sha, parsed, sourceId, error), error);
        std::vector<DataEntry> entries; require(service.listData("volumes", 0, entries, error) && entries.size() == 1, error);
        volumeId = entries[0].id; require(reader.openProjectReadOnly(u(directory), error), error);
    }
    SeismicWindowDescriptor descriptor() {
        SeismicWindowDescriptor out; require(reader.describeSeismicWindow(volumeId, out, error), error); return out;
    }
    SeismicWindowQuery query() { return {descriptor().cut, sha, 1, 1, 3}; }
    SeismicAuthenticatedSource source() {
        // Immutable, completely hashed in-memory test source. The application
        // integration separately verifies the real filesystem range provider.
        const auto snapshot = bytes;
        return {sha, snapshot.size(), [this, snapshot](auto offset, int count, auto& out, auto&, const auto& cancelled) {
            ++calls; if (cancelled && cancelled()) return false;
            if (offset < 0 || offset > snapshot.size() || count > snapshot.size() - offset) return false;
            const auto selected = snapshot.mid(offset, count); out.assign(selected.begin(), selected.end()); return true;
        }};
    }
    void edit(const QString& sql, const QVariantList& arguments = {}) {
        detail::ProjectStore writer; require(writer.open(QString::fromStdString(service.projectPath()), false, {}, error), error);
        detail::Transaction transaction(writer, error); require(transaction.active, error);
        require(writer.exec(sql, arguments, error) && transaction.commit(error), error);
    }
};
void valuesAndExactSourceCut() {
    Fixture f; const auto query = f.query(); const auto source = f.source(); SeismicWindow out; SeismicWindowReadStats stats;
    require(f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error, {}, &stats), f.error);
    require(out.axis == std::vector<double>({2, 4, 6}) && out.amplitudes == std::vector<double>({9, 10, 11}), "exact selected trace window");
    require(out.traceIndex == 1 && out.traceNumber == 17 && out.inlineNumber == 102 && out.crosslineNumber == 201, "actual header identity");
    require(out.nullMask == std::vector<std::uint8_t>({0, 0, 0}) && out.sourceSha256 == f.sha, "typed values/proof identity");
    require(out.descriptor.cut.volumeRevision == query.expected.volumeRevision &&
        out.descriptor.cut.model.sequence == query.expected.model.sequence, "actual consumed metadata cut");
    require(f.calls == 3 && stats.sourceReadCalls == 3 && stats.requestedSourceBytes == 400 + 240 + 12 &&
        stats.metadataBytesRead > 0 && stats.outputBytes == 51 && stats.reservedBytes >= 51, "bounded source requests and honest statistics");
    auto zero = query; zero.traceIndex = 0; zero.firstSample = 0;
    require(f.reader.readAuthenticatedSeismicWindow(zero, source, out, f.error) && out.traceNumber == 0 &&
        out.amplitudes == std::vector<double>({-2, -1, 0}), "negative and zero amplitude/source trace number preserved");
    require(QFile::remove(f.temporary.path() + "/source.sgy"), "remove source path");
    require(f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error), "SDK exclusively uses authenticated host bytes, no hidden path read");
}
void staticBudgetsAndFailureHaveNoPartialValues() {
    Fixture f; const auto source = f.source(); const auto original = f.query(); SeismicWindow out; SeismicWindowReadStats stats;
    auto query = original; query.sampleCount = 8193;
    require(!f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error, {}, &stats) &&
        f.calls == 0 && stats.metadataBytesRead == 0 && out.axis.empty(), "oversized shape rejected before metadata/source I/O");
    query = original; query.firstSample = 5;
    require(!f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error) && f.calls == 0, "window not clamped");
    auto bad = source; bad.sha256 = std::string(64, 'f');
    require(!f.reader.readAuthenticatedSeismicWindow(original, bad, out, f.error) && f.calls == 0, "source anchor mismatch rejected before I/O");
    SeismicWindowReadOptions tiny; tiny.maxReadBytes = 100;
    require(!f.reader.readAuthenticatedSeismicWindow(original, source, out, f.error, tiny) && f.calls == 0, "byte budget includes metadata and source ranges");
    tiny = {}; tiny.maxWorkingBytes = 1;
    require(!f.reader.readAuthenticatedSeismicWindow(original, source, out, f.error, tiny) && f.calls == 0, "working admission before source I/O");
    bad = source; int calls = 0; const auto reader = bad.read;
    bad.read = [&](auto offset, auto count, auto& bytes, auto& error, const auto& cancel) {
        if (++calls == 3) return false; return reader(offset, count, bytes, error, cancel);
    };
    require(!f.reader.readAuthenticatedSeismicWindow(original, bad, out, f.error) && out.axis.empty() &&
        out.amplitudes.empty() && out.sourceSha256.empty(), "failed amplitude authentication returns no earlier header/partial array");
    bad = source; bad.read = [](auto, auto, auto& bytes, auto&, const auto&) { bytes = {1}; return true; };
    require(!f.reader.readAuthenticatedSeismicWindow(original, bad, out, f.error) && out.axis.empty(), "short provider result rejected");
}
void unsupportedLayoutsAndNullsAreExplicit() {
    for (const auto& edit : std::vector<std::function<void(QByteArray&)>>{
        [](auto& b) { qToBigEndian<quint16>(1, b.data() + 3224); },
        [](auto& b) { qToBigEndian<quint16>(0, b.data() + 3502); },
        [](auto& b) { qToBigEndian<qint16>(-1, b.data() + 3504); },
        [](auto& b) { qToBigEndian<quint16>(1000, b.data() + 3600 + 264 + 116); },
        [](auto& b) { qToBigEndian<qint16>(2, b.data() + 3600 + 264 + 108); }
    }) {
        Fixture f(edit); auto query = f.query(); SeismicWindow out;
        require(!f.reader.readAuthenticatedSeismicWindow(query, f.source(), out, f.error) && out.axis.empty(), "unsupported or changed SEG-Y layout rejected");
    }
    Fixture f([](auto& b) { qToBigEndian<quint32>(0x7fc00000, b.data() + 3600 + 264 + 240 + 8); });
    SeismicWindow out; require(f.reader.readAuthenticatedSeismicWindow(f.query(), f.source(), out, f.error), f.error);
    require(out.amplitudes == std::vector<double>({9, 0, 11}) && out.nullMask == std::vector<std::uint8_t>({0, 1, 0}), "explicit nonfinite missing policy, no interpolation");
}
void revisionMetadataAndCancellationBoundaries() {
    Fixture f; auto query = f.query(); const auto source = f.source(); SeismicWindow out;
    f.edit("UPDATE volumes SET amplitude_unit='mV' WHERE id=?", {f.volumeId});
    require(!f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error) && f.calls == 0 &&
        f.error == "SeismicWindowStaleReadCut", "metadata/head drift rejected before source bytes");
    query = f.query(); SeismicWindowReadOptions cancel; int checks = 0; cancel.cancelled = [&] { return ++checks > 8; };
    require(!f.reader.readAuthenticatedSeismicWindow(query, source, out, f.error, cancel) && out.axis.empty(), "cancellation leaves no output");
    f.edit("UPDATE volumes SET sampling=? WHERE id=?", {QString(1024 * 1024, 'x'), f.volumeId});
    SeismicWindowDescriptor descriptor; SeismicWindowReadStats stats;
    require(!f.reader.describeSeismicWindow(f.volumeId, descriptor, f.error, {}, &stats) && descriptor.cut.volumeId == 0 &&
        stats.metadataBytesRead < 1024 && f.error == "SeismicWindowMetadataBudgetExceeded", "SQL guards oversized metadata before materialization");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        valuesAndExactSourceCut(); std::cout << "PASS exact window and cut\n";
        staticBudgetsAndFailureHaveNoPartialValues(); std::cout << "PASS budgets and atomic failures\n";
        unsupportedLayoutsAndNullsAreExplicit(); std::cout << "PASS explicit layout and missing values\n";
        revisionMetadataAndCancellationBoundaries(); std::cout << "PASS revision, metadata and cancellation\n";
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
    return 0;
}
