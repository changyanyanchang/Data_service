#include "store/project_store.h"
#include "store/bounded_metadata.h"
#include "store/seismic_window_metadata.h"
#include <algorithm>

namespace paleo::data {
namespace {
bool sameCut(const SeismicWindowReadCut& a, const SeismicWindowReadCut& b) {
    return a.model.available && b.model.available && a.model.storeId == b.model.storeId &&
        a.model.generation == b.model.generation && a.model.sequence == b.model.sequence &&
        a.volumeId == b.volumeId && a.sourceId == b.sourceId && a.volumeRevision == b.volumeRevision;
}
bool hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}
bool DataService::describeSeismicWindow(long long volumeId, SeismicWindowDescriptor& result, std::string& error,
    const SeismicWindowReadOptions& options, SeismicWindowReadStats* stats) const {
    result = {}; error.clear(); SeismicWindowReadStats usage; SeismicWindowDescriptor pending;
    const auto perform = [&] {
        if (volumeId < 1) { error = "SeismicWindowInvalidRequest"; return false; }
        if (!detail::seismicWindowOptions(options, error) || detail::seismicWindowStopped(options, error)) return false;
        detail::MetadataReadSnapshot snapshot(impl_->store, error);
        if (!snapshot.active() || !detail::seismicWindowDescriptor(impl_->store, volumeId, pending, options, usage, error) ||
            detail::seismicWindowStopped(options, error) || !snapshot.finish(error)) return false;
        result = std::move(pending); return true;
    };
    const bool ok = perform(); if (stats) *stats = usage; return ok;
}
bool DataService::readAuthenticatedSeismicWindow(const SeismicWindowQuery& query, const SeismicAuthenticatedSource& source,
    SeismicWindow& result, std::string& error, const SeismicWindowReadOptions& options, SeismicWindowReadStats* stats) const {
    result = {}; error.clear(); SeismicWindowReadStats usage;
    const auto perform = [&] {
        if (query.expected.volumeId < 1 || query.expected.sourceId < 1 || !query.expected.model.available ||
            query.expected.model.storeId.empty() || query.expected.model.storeId.size() > 64 ||
            query.expected.model.generation.empty() || query.expected.model.generation.size() > 64 || query.expected.model.sequence < 0 ||
            query.expected.volumeRevision.empty() || query.expected.volumeRevision.size() > 128 || query.traceIndex < 0 ||
            query.firstSample < 0 || query.sampleCount < 1 || query.sampleCount > SeismicWindowMaxSamples ||
            !hash(query.sourceSha256) || source.sha256 != query.sourceSha256 || !source.read ||
            source.byteSize < 3844 || source.byteSize > 64LL * 1024 * 1024 * 1024) {
            error = "SeismicWindowInvalidRequest"; return false;
        }
        if (!detail::seismicWindowOptions(options, error) || detail::seismicWindowStopped(options, error)) return false;
        detail::MetadataReadSnapshot snapshot(impl_->store, error); if (!snapshot.active()) return false;
        SeismicWindowDescriptor descriptor;
        if (!detail::seismicWindowDescriptor(impl_->store, query.expected.volumeId, descriptor, options, usage, error)) return false;
        if (!sameCut(query.expected, descriptor.cut)) { error = "SeismicWindowStaleReadCut"; return false; }
        if (hash(descriptor.registeredSourceFingerprint) && descriptor.registeredSourceFingerprint != source.sha256) {
            error = "SeismicWindowSourceMismatch"; return false;
        }
        if (query.firstSample >= descriptor.totalSamples || query.sampleCount > descriptor.totalSamples - query.firstSample) {
            error = "SeismicWindowOutsideTrace"; return false;
        }
        SeismicWindow pending;
        if (!detail::decodeAuthenticatedSeismicWindow(query, source, descriptor, pending, options, usage, error) ||
            detail::seismicWindowStopped(options, error) || !snapshot.finish(error)) return false;
        result = std::move(pending); return true;
    };
    const bool ok = perform(); if (stats) *stats = usage; return ok;
}
}
