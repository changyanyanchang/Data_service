// Repository fixtures are generated synthetic examples; no private source data is bundled.
// Public SEG-Y contract checks. All fixtures are generated; real survey files are never changed.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#endif

using namespace paleo::data;
namespace {
int scenarios = 0;
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void done(const char* name) { ++scenarios; std::cout << "PASS " << name << '\n'; }
void write(QFile& file, const QByteArray& data) {
    require(file.write(data) == data.size(), "cannot write generated fixture");
}
template<class T> void put(QByteArray& data, int at, T value, bool little) {
    if (little) qToLittleEndian<T>(value, data.data() + at);
    else qToBigEndian<T>(value, data.data() + at);
}
int width(int format) {
    switch (format) {
    case 3: case 11: return 2;
    case 6: case 9: case 12: return 8;
    case 8: case 16: return 1;
    default: return 4;
    }
}
bool unsignedFormat(int format) { return format == 10 || format == 11 || format == 12 || format == 16; }
float expected(int il, int xl, int sample, int format = 5) {
    return float(il * 100 + xl * 10 + sample - (unsignedFormat(format) ? 0 : 3));
}
quint32 ibm(float value) {
    if (value == 0) return 0;
    const quint32 sign = value < 0 ? 0x80000000u : 0;
    double magnitude = std::abs(double(value));
    int exponent = 64;
    while (magnitude >= 1) { magnitude /= 16; ++exponent; }
    while (magnitude < 1.0 / 16) { magnitude *= 16; --exponent; }
    return sign | (quint32(exponent) << 24) | quint32(magnitude * 16777216.0);
}
QByteArray samples(int il, int xl, int count, int format, bool little) {
    QByteArray result(count * width(format), '\0');
    for (int sample = 0; sample < count; ++sample) {
        const float value = expected(il, xl, sample, format);
        const int offset = sample * width(format);
        switch (format) {
        case 1: put(result, offset, ibm(value), little); break;
        case 2: put(result, offset, qint32(value), little); break;
        case 3: put(result, offset, qint16(value), little); break;
        case 5: { quint32 bits; std::memcpy(&bits, &value, 4); put(result, offset, bits, little); break; }
        case 6: { const double number = value; quint64 bits; std::memcpy(&bits, &number, 8); put(result, offset, bits, little); break; }
        case 8: result[offset] = char(qint8(value)); break;
        case 9: put(result, offset, qint64(value), little); break;
        case 10: put(result, offset, quint32(value), little); break;
        case 11: put(result, offset, quint16(value), little); break;
        case 12: put(result, offset, quint64(value), little); break;
        case 16: result[offset] = char(quint8(value)); break;
        }
    }
    return result;
}
struct Fixture {
    int format = 5;
    bool little = false;
    int extended = 0;
    int inlineCount = 3, crosslineCount = 4, sampleCount = 8;
    int missingOrdinal = -1;
    int mismatchOrdinal = -1;
    int mismatchKind = 0; // 1=inline, 2=dt, 3=delay, 4=ns, 5=coordinate units
    bool coordinates = true;
    bool duplicateKeys = false;
    bool legacyKeys = false;
    bool zeroFileNumber = false;
};
QByteArray header(int ordinal, int il, int xl, const Fixture& spec, int delay = 100) {
    QByteArray h(240, '\0');
    put(h, 0, qint32(ordinal + 1), spec.little);
    put(h, 4, qint32(spec.zeroFileNumber ? 0 : ordinal + 1), spec.little);
    put(h, 28, qint16(1), spec.little);
    put(h, 70, qint16(-10), spec.little);
    put(h, 88, qint16(1), spec.little);
    put(h, 108, qint16(delay), spec.little);
    put(h, 114, quint16(spec.sampleCount), spec.little);
    put(h, 116, quint16(2000), spec.little);
    int inlineNumber = spec.duplicateKeys ? 100 : 100 + il * 2;
    if (ordinal == spec.mismatchOrdinal && spec.mismatchKind == 1) inlineNumber += 999;
    const int crosslineNumber = spec.duplicateKeys ? 200 : 200 + xl * 2;
    put(h, spec.legacyKeys ? 8 : 188, qint32(inlineNumber), spec.little);
    put(h, spec.legacyKeys ? 20 : 192, qint32(crosslineNumber), spec.little);
    if (spec.coordinates) {
        put(h, 180, qint32(5000000 + xl * 500), spec.little);
        put(h, 184, qint32(40000000 + il * 1000), spec.little);
    }
    if (ordinal == spec.mismatchOrdinal) {
        if (spec.mismatchKind == 2) put(h, 116, quint16(4000), spec.little);
        if (spec.mismatchKind == 3) put(h, 108, qint16(delay + 2), spec.little);
        if (spec.mismatchKind == 4) put(h, 114, quint16(spec.sampleCount - 1), spec.little);
        if (spec.mismatchKind == 5) put(h, 88, quint16(2), spec.little);
    }
    return h;
}
void prefix(QFile& file, const Fixture& spec) {
    QByteArray text(3200, ' ');
    const QByteArray label("C01 SYNTHETIC SEG-Y ACCESS TEST: NO CRS OR TWT DECLARATION");
    std::memcpy(text.data(), label.constData(), size_t(label.size()));
    write(file, text);
    QByteArray binary(400, '\0');
    put(binary, 16, quint16(2000), spec.little);
    put(binary, 20, quint16(spec.sampleCount), spec.little);
    put(binary, 24, quint16(spec.format), spec.little);
    put(binary, 54, quint16(1), spec.little);
    put(binary, 300, quint16(0x100), spec.little);
    put(binary, 302, quint16(1), spec.little);
    put(binary, 304, qint16(spec.extended), spec.little);
    write(file, binary);
    for (int i = 0; i < spec.extended; ++i) write(file, QByteArray(3200, ' '));
}
void fixture(const QString& path, const Fixture& spec = {}) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "fixture path must be new");
    prefix(file, spec);
    for (int il = 0; il < spec.inlineCount; ++il) for (int xl = 0; xl < spec.crosslineCount; ++xl) {
        const int ordinal = il * spec.crosslineCount + xl;
        if (ordinal == spec.missingOrdinal) continue;
        write(file, header(ordinal, il, xl, spec));
        write(file, samples(il, xl, spec.sampleCount, spec.format, spec.little));
    }
}
long long importVolume(DataService& service, const QString& path, std::string& error, bool mayReject = false) {
    ImportOptions options; options.copySources = false;
    ImportReport report;
    const bool ok = service.importFile(utf8(path), options, report, error);
    if (!ok || report.failed) {
        if (mayReject) return 0;
        throw std::runtime_error("fixture import failed (" + utf8(path) + "): " + error + (report.errors.empty() ? "" : report.errors.front()));
    }
    std::vector<DataEntry> volumes;
    require(service.listData("volumes", 0, volumes, error), error);
    for (const auto& volume : volumes) {
        SeismicVolume registered;
        require(service.readVolume(volume.id, registered, error), error);
        if (registered.dataFile == utf8(path)) return volume.id;
    }
    if (mayReject) return 0;
    throw std::runtime_error("fixture not registered as a volume");
}
bool sparseLargeFixture(const QString& path) {
#ifdef _WIN32
    QFile file(path);
    require(file.open(QIODevice::ReadWrite | QIODevice::NewOnly), "new sparse fixture");
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    DWORD returned = 0;
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) return false;
    Fixture spec; spec.sampleCount = 4096;
    prefix(file, spec);
    constexpr qint64 traceCount = 131072;
    const qint64 record = 240 + qint64(spec.sampleCount) * 4;
    require(file.resize(3600 + traceCount * record), "sparse file logical extent");
    for (qint64 ordinal : {qint64(0), qint64(1), qint64(2), traceCount - 1}) {
        require(file.seek(3600 + ordinal * record), "sparse seek is 64bit");
        write(file, header(int(ordinal), int(ordinal / 512), int(ordinal % 512), spec, 0));
        write(file, samples(0, 0, spec.sampleCount, 5, false));
    }
    require(file.flush(), "sparse flush");
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(reinterpret_cast<LPCWSTR>(path.utf16()), &high);
    const quint64 allocated = (quint64(high) << 32) | low;
    require(allocated < 4 * 1024 * 1024, "sparse fixture must allocate less than 4MB");
    return true;
#else
    (void)path;
    return false;
#endif
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("paleo-segyio-access-tests");
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "temporary test directory");
        DataService service; std::string error;
        require(service.createProject(utf8(temp.path() + "/project"), "Generated SEG-Y tests", error), error);
        const QString standardPath = temp.path() + "/中文 标准地震.sgy";
        fixture(standardPath);
        const auto standard = importVolume(service, standardPath, error);
        const auto original = parseFile(utf8(standardPath));
        require(original.volumes.size() == 1 && original.volumes[0].geometry.coordinateSystem.empty() &&
            original.volumes[0].sampling.timeType.empty(), "source parser must preserve undeclared CRS and TWT");
        SeismicVolume volume;
        require(service.readVolume(standard, volume, error), error);
        require(volume.traces.empty() && volume.sampling.timeType.empty(), "amplitudes stay external and undeclared TWT stays unknown");
        ProjectCrsInfo project; require(service.readProjectCrsInfo(project, error), error);
        require(project.selectionBasis == "default" && project.coordinateSystem == "EPSG:32650" && project.coordinateUnit == "m" &&
            volume.geometry.coordinateSystem == project.coordinateSystem && volume.geometry.coordinateUnit == project.coordinateUnit,
            "effective volume coordinates inherit the explicit project default without rewriting the source declaration");
        SeismicFileInfo info;
        require(service.inspectSeismic(standard, info, error), error);
        require(info.traceCount == 12 && info.sampleCount == 8 && info.sampleFormat == 5 && info.byteOrder == "big-endian", "binary metadata decoded");
        require(info.inlineMin == 100 && info.inlineMax == 104 && info.inlineStep == 2 && info.crosslineCount == 4, "native step2 geometry");
        require(info.geometryStatus == "sampled-regular" && info.engine.find("segyio") != std::string::npos, "actual engine and sampled verification status");
        done("undeclared source CRS/TWT, effective project coordinates, UTF-8 paths and external data");

        SeismicTraceResult trace;
        SeismicTraceReadOptions traceOptions; traceOptions.firstSample = 1; traceOptions.sampleCount = 3;
        require(service.readSeismicTrace(standard, 5, traceOptions, trace, error), error);
        require(trace.amplitudes == std::vector<float>({108, 109, 110}) && trace.firstTime == 102 && trace.sampleInterval == 2, "native trace window and time axis");
        require(trace.inlineNumber == 102 && trace.crosslineNumber == 202 && trace.x == 500050 && trace.y == 4000100, "scaled coordinates and real identifiers");
        require(service.readSeismicTrace(standard, 0, {}, trace, error), error);
        require(trace.amplitudes[0] == -3 && trace.amplitudes[3] == 0, "negative and zero amplitudes are valid");
        done("trace subwindow and coordinate scalar");

        Fixture zeroNumber; zeroNumber.zeroFileNumber=true;
        const auto zeroNumberPath=temp.path()+"/zero-source-number.sgy"; fixture(zeroNumberPath,zeroNumber);
        const auto zeroNumberId=importVolume(service,zeroNumberPath,error);
        require(service.readSeismicTrace(zeroNumberId,5,{},trace,error)&&trace.traceNumber==0&&trace.traceIndex==5,"source number zero is retained, not replaced with sequence-line field");
        done("source trace number zero is preserved");

        SectionReadOptions lineOptions; lineOptions.firstTime = 102; lineOptions.lastTime = 106; lineOptions.maxColumns = 4;
        SeismicSectionResult line;
        require(service.readSeismicLine(standard, "inline", 102, lineOptions, line, error), error);
        require(line.columns == 4 && line.samples == 3 && line.amplitudes.size() == 12 && line.firstTime == 102, "line bounded shape");
        if (!(line.amplitudes.front() == 98 && line.amplitudes.back() == 130 && line.traceIndexes == std::vector<long long>({4, 5, 6, 7}))) {
            std::cerr << "inline diagnostic first=" << line.amplitudes.front() << " last=" << line.amplitudes.back() << " indexes=";
            for (auto value : line.traceIndexes) std::cerr << value << ',';
            std::cerr << '\n';
        }
        require(line.amplitudes.front() == 98 && line.amplitudes.back() == 130 && line.traceIndexes == std::vector<long long>({4, 5, 6, 7}), "column-major values and physical trace identities");
        require(service.readSeismicLine(standard, "crossline", 204, lineOptions, line, error), error);
        require(line.columns == 3 && line.amplitudes.front() == 18 && line.amplitudes.back() == 220, "crossline orientation");
        done("inline and crossline native extraction");

        TimeSliceReadOptions sliceOptions; sliceOptions.maxInlineCount = 3; sliceOptions.maxCrosslineCount = 4;
        SeismicTimeSliceResult slice;
        require(service.readSeismicTimeSlice(standard, 104.1, sliceOptions, slice, error), error);
        require(slice.actualTime == 104 && slice.inlineNumbers == std::vector<int>({100,102,104}) && slice.crosslineNumbers == std::vector<int>({200,202,204,206}), "native slice axes");
        require(slice.amplitudes.size() == 12 && slice.amplitudes.front() == -1 && slice.amplitudes.back() == 229, "nearest sample and row-major slice");
        done("native time slice without CRS confirmation");

        for (const auto& item : std::vector<std::pair<int, bool>>{{5,true},{1,false},{2,false},{3,false},{6,false},{9,false},{10,false},{11,false},{12,false}}) {
            Fixture spec; spec.format = item.first; spec.little = item.second;
            const QString path = temp.path() + QString("/format-%1-%2.sgy").arg(spec.format).arg(spec.little);
            fixture(path, spec); const auto id = importVolume(service, path, error);
            require(service.inspectSeismic(id, info, error), error);
            require(info.sampleFormat == spec.format && info.byteOrder == (spec.little ? "little-endian" : "big-endian"), "format/endian declaration");
            require(service.readSeismicTrace(id, 5, {}, trace, error), error);
            for (int sample = 0; sample < 8; ++sample) require(trace.amplitudes[size_t(sample)] == expected(1,1,sample,spec.format), "decoded typed sample must match independently generated value");
        }
        done("little-endian IEEE, IBM, signed/unsigned integer and double decoding");

        for (int format : {8,16}) {
            Fixture spec; spec.format = format; spec.inlineCount = 1; spec.crosslineCount = 1;
            const QString path = temp.path() + QString("/format-byte-%1.sgy").arg(format);
            fixture(path, spec); const auto id = importVolume(service, path, error);
            require(service.readSeismicTrace(id, 0, {}, trace, error), error);
            for (int sample = 0; sample < 8; ++sample) require(trace.amplitudes[size_t(sample)] == expected(0,0,sample,format), "single-byte sample conversion");
        }
        done("signed and unsigned byte sample decoding");

        Fixture ext; ext.extended = 2;
        const QString extPath = temp.path() + "/extended.sgy"; fixture(extPath, ext);
        const auto extId = importVolume(service, extPath, error);
        require(service.inspectSeismic(extId, info, error), error);
        require(info.extendedTextHeaders == 2 && info.firstTraceOffset == 10000, "extended headers move physical trace origin");
        require(service.readSeismicTrace(extId, 11, {}, trace, error) && trace.amplitudes.back() == 234, error);
        done("positive extended textual headers");

        Fixture unknownExt; unknownExt.extended = -1;
        const QString unknownExtPath = temp.path() + "/unknown-extended-count.sgy"; fixture(unknownExtPath, unknownExt);
        const auto unknownExtId = importVolume(service, unknownExtPath, error, true);
        require(unknownExtId == 0 || !service.inspectSeismic(unknownExtId, info, error), "negative extended-header count must not silently assume offset3600");
        done("unknown extended-header count rejected explicitly");

        Fixture holes; holes.missingOrdinal = 6;
        const QString holesPath = temp.path() + "/holes.sgy"; fixture(holesPath, holes);
        const auto holesId = importVolume(service, holesPath, error);
        require(service.inspectSeismic(holesId, info, error), error);
        require(info.geometryStatus == "full-header-index" && info.traceCount == 11, "small irregular file indexed by real headers");
        require(service.readSeismicTimeSlice(holesId, 104, sliceOptions, slice, error), error);
        require(slice.amplitudes.size() == 12 && std::isnan(slice.amplitudes[6]) && slice.amplitudes[7] == 129, "missing grid cell remains NaN, not neighboring trace");
        require(service.readSeismicLine(holesId, "inline", 102, lineOptions, line, error), error);
        require(line.traceIndexes[2] == -1 && std::isnan(line.amplitudes[6]), "missing section column identity and samples");
        done("small irregular grid, missing-cell preservation");

        Fixture legacy; legacy.legacyKeys = true; legacy.coordinates = false;
        const QString legacyPath = temp.path() + "/legacy-no-coordinates.sgy"; fixture(legacyPath, legacy);
        const auto legacyId = importVolume(service, legacyPath, error);
        require(service.readSeismicLine(legacyId, "inline", 102, lineOptions, line, error), error);
        require(line.inlineNumbers.front() == 102 && std::isnan(line.x.front()), "legacy keys and unknown XY still permit native reads");
        done("legacy header words and absent coordinates");

        Fixture duplicate; duplicate.duplicateKeys = true;
        const QString duplicatePath = temp.path() + "/duplicate-keys.sgy"; fixture(duplicatePath, duplicate);
        const auto duplicateId = importVolume(service, duplicatePath, error);
        require(service.readSeismicTrace(duplicateId, 5, {}, trace, error) && trace.amplitudes[0] == 107, error);
        require(!service.readSeismicLine(duplicateId, "inline", 100, lineOptions, line, error) && line.amplitudes.empty(), "duplicate grid keys cannot silently choose one trace");
        done("duplicate keys allow raw traces, reject ambiguous grid");

        trace.amplitudes = {99}; traceOptions.firstSample = -1;
        require(!service.readSeismicTrace(standard, 0, traceOptions, trace, error) && trace.amplitudes.empty(), "invalid window clears stale values");
        traceOptions = {}; traceOptions.maxSamples = 2;
        require(!service.readSeismicTrace(standard, 0, traceOptions, trace, error) && trace.amplitudes.empty(), "trace sample budget enforced");
        require(!service.readSeismicTrace(standard, 12, {}, trace, error) && trace.amplitudes.empty(), "trace range checked");
        require(!service.readSeismicLine(standard, "inline", 101, lineOptions, line, error) && line.amplitudes.empty(), "off-grid line must not snap to a neighbor");
        require(!service.readSeismicLine(standard, "bad-direction", 100, lineOptions, line, error), "unknown line direction rejected");
        lineOptions.firstTime = 300;
        require(!service.readSeismicLine(standard, "inline", 100, lineOptions, line, error) && line.amplitudes.empty(), "nonoverlapping time window rejected");
        sliceOptions.maxValues = 4;
        require(!service.readSeismicTimeSlice(standard, 104, sliceOptions, slice, error) && slice.amplitudes.empty(), "slice budget enforced");
        require(!service.readSeismicTimeSlice(standard, 99, {}, slice, error) && slice.amplitudes.empty(), "outside time rejected");
        done("invalid ranges/options and bounded memory");

        const auto cancel = [] { return true; };
        info.sourceFile = "stale";
        require(!service.inspectSeismic(standard, info, error, cancel) && info.sourceFile.empty(), "inspection cancellation clears result");
        trace.amplitudes = {99};
        require(!service.readSeismicTrace(standard, 0, {}, trace, error, cancel) && trace.amplitudes.empty(), "trace cancellation clears result");
        line.amplitudes = {99};
        require(!service.readSeismicLine(standard, "inline", 100, {}, line, error, cancel) && line.amplitudes.empty(), "line cancellation clears result");
        slice.amplitudes = {99};
        require(!service.readSeismicTimeSlice(standard, 104, {}, slice, error, cancel) && slice.amplitudes.empty(), "slice cancellation clears result");
        int polls = 0;
        require(!service.readSeismicTimeSlice(standard, 104, {}, slice, error, [&] { return ++polls > 10; }) && slice.amplitudes.empty(), "mid-operation cancellation gives no partial grid");
        done("preflight and mid-operation cancellation");

        const QString truncated = temp.path() + "/truncated.sgy"; fixture(truncated);
        { QFile file(truncated); require(file.open(QIODevice::ReadWrite), "truncate fixture"); require(file.resize(file.size()-1), "truncate one sample byte"); }
        const auto truncatedId = importVolume(service, truncated, error, true);
        require(truncatedId == 0 || !service.inspectSeismic(truncatedId, info, error), "truncated fixed-length file rejected");
        done("truncated layout rejected");

        // Ordinal7 is outside the bounded regular-layout probes for this 13x17 fixture.
        // Native line extraction must still re-check it before trusting its samples.
        Fixture wrongKey; wrongKey.inlineCount = 13; wrongKey.crosslineCount = 17; wrongKey.mismatchOrdinal = 7; wrongKey.mismatchKind = 1;
        const QString wrongKeyPath = temp.path() + "/unexpected-key.sgy"; fixture(wrongKeyPath, wrongKey);
        const auto wrongKeyId = importVolume(service, wrongKeyPath, error);
        require(service.inspectSeismic(wrongKeyId, info, error), error);
        require(info.geometryStatus == "sampled-regular", "test exercises a mismatch beyond sampled validation");
        SectionReadOptions allColumns; allColumns.maxColumns = 17;
        require(!service.readSeismicLine(wrongKeyId, "inline", 100, allColumns, line, error) && line.amplitudes.empty(), "per-trace mapping mismatch rejected without wrong amplitudes");
        done("actual header revalidation after sampled geometry");

        Fixture wrongUnits; wrongUnits.inlineCount=13; wrongUnits.crosslineCount=17;
        wrongUnits.mismatchOrdinal=7; wrongUnits.mismatchKind=5;
        const QString wrongUnitsPath=temp.path()+"/unexpected-coordinate-unit.sgy"; fixture(wrongUnitsPath,wrongUnits);
        const auto wrongUnitsId=importVolume(service,wrongUnitsPath,error);
        require(service.readSeismicLine(wrongUnitsId,"inline",100,allColumns,line,error),error);
        require(std::isnan(line.x[7]) && std::isfinite(line.amplitudes[7*line.samples]),"unit conflict hides invalid XY, preserves native amplitude");
        SeismicVolume confirmed; require(service.readVolume(wrongUnitsId,confirmed,error),error);
        confirmed.geometry.coordinateSystem="TEST_ONLY_CRS"; confirmed.geometry.coordinateUnit="m";
        confirmed.sampling.timeType="TWT"; confirmed.sampling.timeReference="TEST_ONLY_DATUM";
        require(service.saveVolume(wrongUnitsId,confirmed,error),error);
        SeismicSection testPath; testPath.parentVolumeId=int(wrongUnitsId); testPath.sectionName="test coordinate validation";
        testPath.coordinateSystem=confirmed.geometry.coordinateSystem; testPath.coordinateUnit="m";
        testPath.pathPoints={{500000,4000000},{500800,4000000}}; long long sectionId=0;
        require(service.saveSection(sectionId,testPath,error),error);
        require(!service.extractSection(sectionId,allColumns,line,error)&&line.amplitudes.empty(),"strict XY extraction rejects unprobed angular-unit trace");
        done("per-trace coordinate units cannot bypass spatial validation");

        for (int kind : {2,3,4}) {
            Fixture badAxis; badAxis.mismatchOrdinal = 5; badAxis.mismatchKind = kind;
            const QString path = temp.path() + QString("/bad-axis-%1.sgy").arg(kind); fixture(path, badAxis);
            const auto id = importVolume(service, path, error, true);
            if (id) {
                require(!service.readSeismicTrace(id, 5, {}, trace, error) && trace.amplitudes.empty(), "inconsistent per-trace interval/delay/count rejected");
                require(!service.inspectSeismic(id, info, error), "inspection reports incompatible common axis");
            }
        }
        done("per-trace interval, delay and sample-count validation");

        const QString sparsePath = temp.path() + "/sparse-over-2gb.sgy";
        if (sparseLargeFixture(sparsePath)) {
            const auto id = importVolume(service, sparsePath, error);
            require(service.inspectSeismic(id, info, error), error);
            require(info.fileBytes > 2147483647LL && info.traceCount == 131072, "64bit physical file metadata");
            require(info.geometryStatus == "unavailable", "large sparse fixture is not misrepresented as full geometry");
            SeismicTraceReadOptions last; last.firstSample = 4093; last.sampleCount = 3;
            require(service.readSeismicTrace(id, 131071, last, trace, error), error);
            require(trace.amplitudes == std::vector<float>({4090,4091,4092}), "read after 2GB uses 64bit byte offset");
            done("sparse >2GB source with <4MB physical allocation");
        } else std::cout << "SKIP sparse fixture: platform/filesystem does not support Windows sparse files\n";

        service.closeProject();
        require(service.openProject(utf8(temp.path() + "/project"), error), error);
        require(service.readSeismicTrace(standard, 5, {}, trace, error) && trace.amplitudes.front() == 107, error);
        require(service.readVolume(standard, volume, error) && volume.traces.empty() && volume.dataFile == utf8(standardPath), "reopen retains reference without volume payload");
        done("project reopen preserves external-only source access");
        std::cout << "SEG-Y access: " << scenarios << " scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SEG-Y access test failed: " << exception.what() << '\n';
        return 1;
    }
}
