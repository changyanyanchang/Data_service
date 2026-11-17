#include "seismic_window_metadata.h"
#include "bounded_metadata.h"
#include "project_store.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace paleo::data::detail {
bool seismicWindowOptions(const SeismicWindowReadOptions& options, std::string& error) {
    if (!metadataBudgetValid(options.metadata) || options.metadata.maxFieldBytes > 8192 ||
        options.metadata.maxTextBytes > 16384 || options.maxReadBytes < 1 || options.maxReadBytes > 8 * 1024 * 1024 ||
        options.maxWorkingBytes < 16384 + options.metadata.maxTextBytes * 4 || options.maxWorkingBytes > 8 * 1024 * 1024) {
        error = "SeismicWindowInvalidBudget"; return false;
    }
    return true;
}
bool seismicWindowStopped(const SeismicWindowReadOptions& options, std::string& error) {
    if (options.cancelled && options.cancelled()) { error = "SeismicWindowCancelled"; return true; }
    return false;
}
namespace {
bool readTexts(const ProjectStore& store, const QString& from, const QStringList& fields, const QVariantList& arguments,
    const SeismicWindowReadOptions& options, SeismicWindowReadStats& stats, QVector<QByteArray>& values, std::string& error,
    const QString& numeric, QSqlQuery& row) {
    auto remaining = options.metadata;
    remaining.maxTextBytes = std::min<long long>(remaining.maxTextBytes - stats.metadataBytesRead,
        options.maxReadBytes - stats.metadataBytesRead);
    if (remaining.maxTextBytes < 1) { error = "SeismicWindowMetadataBudgetExceeded"; return false; }
    const auto guard = metadataGuard(fields, remaining);
    if (!store.exec("SELECT " + numeric + "," + guard + "," + guardedMetadataColumns(fields, guard) + " FROM " + from,
        arguments, error, &row)) return false;
    if (!row.next()) { error = "SeismicWindowMetadataUnavailable"; return false; }
    if (!row.value(2).toBool()) { error = "SeismicWindowMetadataBudgetExceeded"; return false; }
    long long bytes = 0;
    if (!metadataTextValues(row, 3, fields, remaining, values, bytes, error, "SeismicWindowMetadata")) return false;
    stats.metadataBytesRead += bytes; return true;
}
bool finiteNumber(const QJsonObject& object, const char* key, double& result) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
    result = value.toDouble(); return true;
}
bool declared(const std::string& value, std::size_t limit) {
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos) return false;
    const auto text = q(value).trimmed().toCaseFolded();
    return !text.isEmpty() && !QStringList{"unknown", "unspecified", "undefined", "?", "n/a"}.contains(text);
}
bool sourceFingerprint(const std::string& value) {
    const auto offset = value.rfind("quick:", 0) == 0 ? std::size_t(6) : std::size_t(0);
    return value.size() == offset + 64 && std::all_of(value.begin() + offset, value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}
bool seismicWindowDescriptor(const ProjectStore& store, std::int64_t volumeId, SeismicWindowDescriptor& result,
    const SeismicWindowReadOptions& options, SeismicWindowReadStats& stats, std::string& error) {
    if (!metadataBudgetAvailable(store, error, "SeismicWindowMetadata")) return false;
    // Both schema and identity fields are bounded before Qt materializes TEXT.
    QSqlQuery schema;
    if (!store.exec("SELECT typeof(value)='text' AND octet_length(value)=1 AND value='1' FROM ds_meta WHERE key='model_change_schema'",
        {}, error, &schema) || !schema.next() || !schema.value(0).toBool()) {
        error = "SeismicWindowReadCutUnavailable"; return false;
    }
    schema.finish(); QSqlQuery row; QVector<QByteArray> texts;
    if (!readTexts(store, "pds_model_state WHERE singleton=1 AND typeof(sequence)='integer' AND typeof(unmanaged)='integer'", {"store_id", "generation"}, {}, options, stats, texts,
        error, "sequence,unmanaged", row)) return false;
    SeismicWindowDescriptor pending;
    pending.cut.model = {texts[0].toStdString(), texts[1].toStdString(), row.value(0).toLongLong(), true};
    if (row.value(1).toBool() || pending.cut.model.sequence < 0 || texts[0].isEmpty() || texts[0].size() > 64 ||
        texts[1].isEmpty() || texts[1].size() > 64 || texts[0].contains('\0') || texts[1].contains('\0')) { error = "SeismicWindowReadCutUnavailable"; return false; }
    row.finish();
    if (seismicWindowStopped(options, error)) return false;
    if (!readTexts(store, "volumes v JOIN sources s ON s.id=v.source_id WHERE v.id=? AND typeof(v.source_id)='integer' AND typeof(s.active)='integer'", {"v.amplitude_unit", "v.sampling", "s.hash"},
        {qlonglong(volumeId)}, options, stats, texts, error, "v.source_id,s.active", row)) return false;
    pending.cut.volumeId = volumeId; pending.cut.sourceId = row.value(0).toLongLong();
    if (!row.value(1).toBool() || pending.cut.sourceId < 1) { error = "SeismicWindowSourceUnavailable"; return false; }
    pending.amplitudeUnit = texts[0].toStdString(); pending.registeredSourceFingerprint = texts[2].toStdString();
    if (!sourceFingerprint(pending.registeredSourceFingerprint)) { error = "SeismicWindowSourceUnavailable"; return false; }
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(texts[1], &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { error = "SeismicWindowAxisUndeclared"; return false; }
    const auto sampling = document.object(); const auto count = sampling.value("sampleCount");
    if (!count.isDouble() || count.toDouble() != std::floor(count.toDouble()) || count.toDouble() < 1 || count.toDouble() > 65535 ||
        !finiteNumber(sampling, "firstTime", pending.firstTime) || !finiteNumber(sampling, "sampleInterval", pending.sampleInterval) ||
        pending.sampleInterval <= 0) { error = "SeismicWindowAxisUndeclared"; return false; }
    pending.totalSamples = count.toInt(); pending.timeUnit = s(sampling.value("timeUnit").toString());
    pending.timeType = s(sampling.value("timeType").toString()); pending.timeReference = s(sampling.value("timeReference").toString());
    if (pending.timeUnit != "ms" || (pending.timeType != "TWT" && pending.timeType != "OWT") ||
        !declared(pending.timeReference, 256) || !declared(pending.amplitudeUnit, 128)) {
        error = "SeismicWindowAxisUndeclared"; return false;
    }
    const QJsonArray identity{QString::number(volumeId), QString::number(pending.cut.sourceId),
        QString::fromUtf8(texts[0]), QString::fromUtf8(texts[1]), QString::fromUtf8(texts[2])};
    pending.cut.volumeRevision = "seismic-window-descriptor.v1:" + QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex().toStdString();
    result = std::move(pending); return true;
}
}
