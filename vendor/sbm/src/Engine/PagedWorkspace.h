#pragma once

// Production Paged Workspace (Stage N2).
//
// This is a NEW on-disk format (magic SF3PWG01, current version 7) derived from the
// measured LayoutV5 experiments: the winning b8x8x256 chunk geometry with
// 8x8x16 pages, per-page CRCs, raw or zstd pages and page-level partial reads.
// The stable 64^3 workspace format stays untouched and read-only compatible.
//
// Differences from the experimental v5 container:
//   * a manifest-grade header: source identity (path/size/mtime/fingerprint),
//     real AxisDescriptors (origin/step/count or explicit values), LOD metadata,
//     coverage, codec, build generation and algorithm version;
//   * a persisted completion bitmap, so an interrupted build can resume and an
//     incomplete file can never be mistaken for a finished workspace;
//   * writes go to <name>.partial and are published with an atomic rename;
//   * the reader merges adjacent pages into physical ranges and returns clear
//     errors for corruption.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Data/Sgy/AxisDescriptor.h"
#include "Engine/Cache.h"
#include "Engine/Types.h"
#include "Engine/VolumeSource.h"

namespace seismic {
namespace engine {

constexpr char kPagedWorkspaceMagic[8] = { 'S', 'F', '3', 'P', 'W', 'G', '0', '1' };
constexpr std::uint32_t kPagedWorkspaceLegacyVersion = 6;
constexpr std::uint32_t kPagedWorkspaceVersion = 7;
constexpr std::uint32_t kPagedCodecRaw = 0;
constexpr std::uint32_t kPagedCodecZstd = 1;

struct PagedWorkspaceInfo {
    std::uint32_t version = kPagedWorkspaceVersion;
    std::uint32_t algorithmVersion = 1;

    // Geometry (measured winner of stage L).
    std::uint32_t chunkInlines = 8;
    std::uint32_t chunkXlines = 8;
    std::uint32_t chunkSamples = 256;
    std::uint32_t pageInlines = 8;
    std::uint32_t pageXlines = 8;
    std::uint32_t pageSamples = 16;

    // Real axes (a step-2 survey keeps step 2).
    AxisDescriptor inlineAxis;
    AxisDescriptor xlineAxis;
    std::uint32_t samples = 0;
    std::uint32_t sampleIntervalUs = 0;

    // Source identity.
    std::string sourcePath;
    std::uint64_t sourceSize = 0;
    std::int64_t sourceMtimeTicks = 0;
    std::uint64_t sourceFingerprint = 0;

    // LOD metadata.
    std::uint32_t lodLevel = 0;
    std::uint32_t lodMethod = 0;
    std::uint32_t lodFactorSamples = 1;
    std::uint32_t lodFactorInlines = 1;
    std::uint32_t lodFactorXlines = 1;
    std::uint32_t lodAlgorithmVersion = 0;
    std::uint64_t lodSourceHash = 0;

    // Codec, coverage, completion, generation.
    std::uint32_t codec = kPagedCodecRaw;
    std::uint32_t codecLevel = 3;
    int coverageInlineMin = 0;
    int coverageInlineMax = 0;
    int coverageXlineMin = 0;
    int coverageXlineMax = 0;
    bool complete = false;
    std::uint64_t buildGeneration = 0;

    std::uint32_t ChunksI() const;
    std::uint32_t ChunksX() const;
    std::uint32_t ChunksT() const;
    std::uint64_t ChunkCount() const;
    std::uint32_t PagesI() const;
    std::uint32_t PagesX() const;
    std::uint32_t PagesT() const;
    std::uint64_t PagesPerChunk() const;
    std::uint64_t PageCount() const;
    std::uint64_t PageBytes() const;
    std::uint64_t LogicalBytes() const;
    std::uint64_t ChunkOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const;
    std::uint64_t PageOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                              std::uint32_t pi, std::uint32_t px, std::uint32_t pt) const;
    std::string Describe() const;
};

std::filesystem::path PagedWorkspacePartialPath(const std::filesystem::path& finalPath);
std::uint64_t PagedWorkspaceFingerprint(const std::filesystem::path& path, std::uintmax_t fileSize);

class PagedWorkspaceWriter {
public:
    ~PagedWorkspaceWriter();

    // resume=true continues an existing <final>.partial (validated against the
    // given info); otherwise a fresh partial is created.
    bool Open(const std::filesystem::path& finalPath, const PagedWorkspaceInfo& info,
              bool resume, std::string& errorMessage);
    bool WriteChunk(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                    const float* values, std::string& errorMessage);
    bool Complete(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const;
    // Publishes atomically; refuses when any chunk is missing.
    bool Finalize(std::string& errorMessage);

    const PagedWorkspaceInfo& Info() const;
    std::uint64_t ChunksWritten() const;
    std::uint64_t BytesWritten() const;
    std::uint64_t CheckpointWrites() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class PagedWorkspaceReader {
public:
    struct Stats {
        std::uint64_t readCalls = 0;
        std::uint64_t readBytes = 0;
        std::uint64_t readRanges = 0;
        std::uint64_t pagesTouched = 0;
        std::uint64_t chunksTouched = 0;
        std::uint64_t checksumFailures = 0;
        std::uint64_t cacheHits = 0;
        std::uint64_t cacheMisses = 0;
        double wallMs = 0.0;
        double planMs = 0.0, readMs = 0.0, checksumMs = 0.0, decodeMs = 0.0, scatterMs = 0.0;
        // Existing readBytes/readCalls describe payload I/O. Directory/bitmap
        // I/O is separate; cache byte counters count owned buffers, not RSS.
        std::uint64_t headerReadBytes = 0;
        std::uint64_t directoryReadBytes = 0, directoryReadCalls = 0;
        std::uint64_t directoryCacheHits = 0, directoryCacheMisses = 0;
        std::uint64_t directoryCacheBytes = 0, directoryCachePeakBytes = 0;
        std::uint64_t directoryCacheBudgetBytes = 0;
    };

    // Both modes read only the versioned header at open. Normal reads fetch
    // directory/bitmap blocks on demand; metadataOnly additionally forbids
    // payload access and permits a header whose directories are not yet present.
    bool Open(const std::filesystem::path& path, std::string& errorMessage,
              bool metadataOnly = false);
    const PagedWorkspaceInfo& Info() const;
    const Stats& ReaderStats() const;
    void ResetReaderStats();

    // Exact axis box read; NaN outside the covered area. Fails when a requested
    // coordinate is not an exact axis element. At most 64M output floats,
    // 1M planned pages and 64MiB per decoded page; failure clears the output.
    bool ReadBox(int inlineIndex, int xlineIndex, int sampleIndex,
                 int inlines, int xlines, int samples,
                 std::vector<float>& out, std::string& errorMessage, CancelToken* cancel = nullptr,
                 bool timeDisplayLayout = false);
    bool ReadTrace(int inlineIndex, int xlineIndex, std::vector<float>& out, std::string& errorMessage);
    // Display conventions identical to the Direct SEG-Y source.
    bool ReadInlineSlice(int inlineIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadXlineSlice(int xlineIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadTimeSlice(int sampleIndex, std::vector<float>& out, std::string& errorMessage,
                       CancelToken* cancel = nullptr);

    // Raw partial page read (raw pages only).
    bool ReadPageBytes(std::uint64_t pageIndex, std::uint64_t byteOffset, std::uint64_t byteLength,
                       std::vector<unsigned char>& out, std::string& errorMessage);

    void SetPageCache(std::shared_ptr<ChunkCache> cache);
    std::shared_ptr<ChunkCache> PageCacheHandle() const;
    std::uint64_t CacheSourceKey() const;

    // Shared byte-bounded LRU for page-directory and completion-bitmap blocks.
    // Retained byte buffers default to 4MiB; 0 disables retention/read-ahead.
    // Configure before or after Open. Reader/Dataset handles remain single-thread.
    void SetDirectoryCacheBudget(std::size_t bytes);
    std::size_t DirectoryCacheBudget() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::size_t directoryCacheBudget_ = 4u * 1024u * 1024u;
};

// Full engine backend over a paged workspace.
class PagedWorkspaceVolumeSource final : public IVolumeSource {
public:
    static std::unique_ptr<PagedWorkspaceVolumeSource> Open(const std::filesystem::path& path, Status& status);

    const DatasetMetadata& Metadata() const override;
    Status ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) override;
    Status ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) override;
    Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel, const ProgressFn& progress) override;
    // Display navigation only: one page-aligned time slab, bounded to 16 MiB.
    Status ReadTimeSliceWindowed(int sampleIndex, Slice2D& out, CancelToken* cancel);
    Status ReadTimeSliceTiled(int sampleIndex, int tileSize, int focusInline, int focusXline,
                             const TimeTileCallback& publish, Slice2D& out, CancelToken* cancel);
    void ClearTimeWindow() { timeWindow_.clear(); timeWindow_.shrink_to_fit(); timeWindowBegin_ = -1; }
    Status ReadVoxelWindow(const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) override;
    Status ReadArbitrarySection(const SectionRequest& request, Slice2D& out, CancelToken* cancel) override;
    SourceStatistics Statistics() const override;
    void ResetStatistics() override;

    void SetTimeCachePath(const std::filesystem::path& path) { timeCachePath_ = path; }
    PagedWorkspaceReader& Reader() { return reader_; }
    const PagedWorkspaceReader& Reader() const { return reader_; }

private:
    std::filesystem::path timeCachePath_;
    PagedWorkspaceReader reader_;
    DatasetMetadata metadata_;
    mutable SourceStatistics stats_;
    std::vector<float> timeWindow_;
    int timeWindowBegin_ = -1;
    int timeWindowCount_ = 0;
    std::uint64_t timeWindowSourceKey_ = 0;
};

// Builds a paged workspace from a raw stable workspace (chunk-by-chunk, bounded
// source cache). Used by tests and by the transcode pipeline.
bool BuildPagedWorkspaceFromWorkspace(const std::filesystem::path& sourceBase,
                                      const std::filesystem::path& targetFile,
                                      const PagedWorkspaceInfo& infoTemplate,
                                      std::string& errorMessage,
                                      std::uint64_t& sourceChunksRead,
                                      PagedWorkspaceInfo* outInfo = nullptr);

} // namespace engine
} // namespace seismic
