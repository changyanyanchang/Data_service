#pragma once
#include "model_changes.h"
#include "metadata_read.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace paleo::data {
inline constexpr int SeismicWindowMaxSamples = 8192;
// A trusted host supplies an authenticated range reader anchored to this exact
// complete SHA-256. The callback is a C++ embedding boundary, never a tool path,
// a quick fingerprint, or a claim that an ordinary file handle is a snapshot.
struct SeismicAuthenticatedSource {
    std::string sha256;
    std::int64_t byteSize = 0;
    std::function<bool(std::int64_t offset, int count, std::vector<std::uint8_t>& bytes,
        std::string& error, const std::function<bool()>& cancelled)> read;
};
struct SeismicWindowReadCut {
    ModelChangeHead model;
    std::int64_t volumeId = 0, sourceId = 0;
    std::string volumeRevision;
};
struct SeismicWindowDescriptor {
    SeismicWindowReadCut cut;
    std::string amplitudeUnit, timeUnit, timeType, timeReference;
    std::string registeredSourceFingerprint; // May be quick; never used as a complete-file proof.
    int totalSamples = 0;
    double firstTime = 0, sampleInterval = 0;
};
struct SeismicWindowQuery {
    SeismicWindowReadCut expected;
    std::string sourceSha256;
    std::int64_t traceIndex = -1; // Exact zero-based file ordinal; no nearest/grid guessing.
    int firstSample = 0, sampleCount = 0;
};
struct SeismicWindowReadOptions {
    MetadataReadOptions metadata{4096, 8192};
    std::int64_t maxReadBytes = 1024 * 1024, maxWorkingBytes = 2 * 1024 * 1024;
    std::function<bool()> cancelled;
};
struct SeismicWindowReadStats {
    std::int64_t metadataBytesRead = 0, requestedSourceBytes = 0, sourceReadCalls = 0;
    std::int64_t outputBytes = 0, reservedBytes = 0;
    // The embedding host separately reports actual full verification/chunk I/O.
    // These counters measure bytes requested from its authenticated callback.
};
struct SeismicWindow {
    SeismicWindowDescriptor descriptor;
    std::string sourceSha256;
    std::int64_t traceIndex = -1, traceNumber = -1;
    int inlineNumber = 0, crosslineNumber = 0, firstSample = 0;
    std::vector<double> axis, amplitudes;
    std::vector<std::uint8_t> nullMask; // One byte/sample, 1 missing, 0 present; missing values are zero.
};
}
