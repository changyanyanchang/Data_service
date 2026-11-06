#include "Engine/PagedWorkspace.h"
#include "Engine/TimePlaneCache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <deque>
#include <limits>
#include <list>
#include <set>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef SEISMIC_HAVE_ZSTD
#include <zstd.h>
#endif

#include "Data/Sgy/SgyFileReader.h"
#include "Engine/WorkspaceFormat.h"

namespace seismic {
namespace engine {
namespace {

constexpr std::uint64_t kFnvOffset = 1099511628211ull;
constexpr std::uint64_t kLegacyHeaderBytes = 256;
constexpr std::uint64_t kHeaderAlignment = 4096;
constexpr std::uint64_t kMaxHeaderBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kFlushEveryChunks = 32;
constexpr std::uint64_t kMaxDecodedPageBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxBoxValues = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaxPlannedPages = 1024u * 1024u;

bool CheckedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if(b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}
bool CheckedMultiply(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if(a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}
std::uint32_t CeilDivide(std::uint32_t a, std::uint32_t b) {
    return b == 0 ? 0 : a / b + (a % b != 0);
}
bool ValidReadAxis(const AxisDescriptor& axis) {
    if(axis.count <= 0 || !axis.Validate()) return false;
    if(axis.encoding != AxisDescriptor::Encoding::Uniform) return true;
    const auto displacement = std::int64_t(axis.count - 1) * axis.step;
    const auto last = std::int64_t(axis.origin) + displacement;
    // Existing AxisDescriptor::ValueAt evaluates index*step in int. Refuse
    // metadata that would overflow that intermediate even when the sum fits.
    return displacement >= std::numeric_limits<int>::min() &&
        displacement <= std::numeric_limits<int>::max() &&
        last >= std::numeric_limits<int>::min() && last <= std::numeric_limits<int>::max();
}
bool ExactReadIndex(const AxisDescriptor& axis, int value, int& index) {
    if(axis.encoding != AxisDescriptor::Encoding::Uniform) return axis.ExactIndexOf(value, index);
    const auto difference = std::int64_t(value) - axis.origin;
    if(difference % axis.step != 0) return false;
    const auto position = difference / axis.step;
    if(position < 0 || position >= axis.count) return false;
    index = static_cast<int>(position);
    return true;
}

double MsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
}

double MicrosSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

bool Cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->IsCancelled();
}

std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t hash = kFnvOffset) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for(std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T>
void Append(std::vector<unsigned char>& out, const T& value) {
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Consume(const std::vector<unsigned char>& data, std::size_t& offset, T& value) {
    if(offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

std::uint64_t AxisValuesBytes(const AxisDescriptor& axis) {
    return axis.encoding == AxisDescriptor::Encoding::Explicit
               ? static_cast<std::uint64_t>(axis.values.size()) * 4
               : 0;
}

void AppendAxis(std::vector<unsigned char>& out, const AxisDescriptor& axis) {
    Append<std::uint32_t>(out, static_cast<std::uint32_t>(axis.encoding));
    Append<std::int32_t>(out, axis.count);
    Append<std::int32_t>(out, axis.origin);
    Append<std::int32_t>(out, axis.step);
    if(axis.encoding == AxisDescriptor::Encoding::Explicit) {
        for(int value : axis.values) {
            Append<std::int32_t>(out, value);
        }
    }
}

void AppendHeader(std::vector<unsigned char>& out, const PagedWorkspaceInfo& info,
                  bool complete, std::uint32_t headerBytes) {
    out.insert(out.end(), kPagedWorkspaceMagic, kPagedWorkspaceMagic + 8);
    Append<std::uint32_t>(out, info.version);
    if(info.version >= kPagedWorkspaceVersion) {
        // v7 makes the header self-describing. v6 used a fixed 256-byte area,
        // which could not hold a normal Windows path plus real/explicit axes.
        Append<std::uint32_t>(out, headerBytes);
    }
    Append<std::uint32_t>(out, info.algorithmVersion);
    Append<std::uint32_t>(out, info.chunkInlines);
    Append<std::uint32_t>(out, info.chunkXlines);
    Append<std::uint32_t>(out, info.chunkSamples);
    Append<std::uint32_t>(out, info.pageInlines);
    Append<std::uint32_t>(out, info.pageXlines);
    Append<std::uint32_t>(out, info.pageSamples);
    Append<std::uint32_t>(out, info.samples);
    Append<std::uint32_t>(out, info.sampleIntervalUs);
    Append<std::uint32_t>(out, info.codec);
    Append<std::uint32_t>(out, info.codecLevel);
    Append<std::uint32_t>(out, info.lodLevel);
    Append<std::uint32_t>(out, info.lodMethod);
    Append<std::uint32_t>(out, info.lodFactorSamples);
    Append<std::uint32_t>(out, info.lodFactorInlines);
    Append<std::uint32_t>(out, info.lodFactorXlines);
    Append<std::uint32_t>(out, info.lodAlgorithmVersion);
    Append<std::uint64_t>(out, info.lodSourceHash);
    Append<std::uint64_t>(out, info.sourceSize);
    Append<std::uint64_t>(out, static_cast<std::uint64_t>(info.sourceMtimeTicks));
    Append<std::uint64_t>(out, info.sourceFingerprint);
    Append<std::uint64_t>(out, info.buildGeneration);
    Append<std::int32_t>(out, info.coverageInlineMin);
    Append<std::int32_t>(out, info.coverageInlineMax);
    Append<std::int32_t>(out, info.coverageXlineMin);
    Append<std::int32_t>(out, info.coverageXlineMax);
    Append<std::uint32_t>(out, complete ? 1u : 0u);
    Append<std::uint32_t>(out, static_cast<std::uint32_t>(info.sourcePath.size()));
    out.insert(out.end(), info.sourcePath.begin(), info.sourcePath.end());
    AppendAxis(out, info.inlineAxis);
    AppendAxis(out, info.xlineAxis);
}

std::uint64_t HeaderBytesForInfo(const PagedWorkspaceInfo& info) {
    if(info.version == kPagedWorkspaceLegacyVersion) {
        return kLegacyHeaderBytes;
    }
    if(info.version != kPagedWorkspaceVersion ||
       info.sourcePath.size() > std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    std::vector<unsigned char> required;
    AppendHeader(required, info, false, 0);
    const std::uint64_t bytes = std::max<std::uint64_t>(
        kHeaderAlignment,
        (static_cast<std::uint64_t>(required.size()) + kHeaderAlignment - 1) /
            kHeaderAlignment * kHeaderAlignment);
    return bytes <= kMaxHeaderBytes ? bytes : 0;
}

bool SerializeHeader(const PagedWorkspaceInfo& info, bool complete, std::uint64_t headerBytes,
                     std::vector<unsigned char>& out, std::string& errorMessage) {
    if(headerBytes == 0 || headerBytes > kMaxHeaderBytes ||
       headerBytes > std::numeric_limits<std::uint32_t>::max()) {
        errorMessage = "the paged workspace header size is invalid";
        return false;
    }
    out.clear();
    AppendHeader(out, info, complete, static_cast<std::uint32_t>(headerBytes));
    if(out.size() > headerBytes) {
        errorMessage = "the paged workspace header does not fit its reserved area";
        return false;
    }
    out.resize(static_cast<std::size_t>(headerBytes), 0);
    return true;
}

bool ConsumeAxis(const std::vector<unsigned char>& data, std::size_t& offset, AxisDescriptor& axis) {
    std::uint32_t encoding = 0;
    std::int32_t count = 0;
    std::int32_t origin = 0;
    std::int32_t step = 0;
    if(!Consume(data, offset, encoding) || !Consume(data, offset, count) ||
       !Consume(data, offset, origin) || !Consume(data, offset, step)) {
        return false;
    }
    if(encoding > 1 || count < 0 ||
       (encoding == 1 && static_cast<std::uint64_t>(count) > (data.size() - offset) / 4)) {
        return false;
    }
    axis.encoding = encoding == 1 ? AxisDescriptor::Encoding::Explicit
                                  : AxisDescriptor::Encoding::Uniform;
    axis.count = count;
    axis.origin = origin;
    axis.step = step;
    axis.values.clear();
    if(axis.encoding == AxisDescriptor::Encoding::Explicit) {
        for(int i = 0; i < count; ++i) {
            std::int32_t value = 0;
            if(!Consume(data, offset, value)) {
                return false;
            }
            axis.values.push_back(value);
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------------- info

std::uint32_t PagedWorkspaceInfo::ChunksI() const {
    return CeilDivide(static_cast<std::uint32_t>(inlineAxis.count), chunkInlines);
}
std::uint32_t PagedWorkspaceInfo::ChunksX() const {
    return CeilDivide(static_cast<std::uint32_t>(xlineAxis.count), chunkXlines);
}
std::uint32_t PagedWorkspaceInfo::ChunksT() const {
    return CeilDivide(samples, chunkSamples);
}
std::uint64_t PagedWorkspaceInfo::ChunkCount() const {
    return static_cast<std::uint64_t>(ChunksI()) * ChunksX() * ChunksT();
}
std::uint32_t PagedWorkspaceInfo::PagesI() const {
    return CeilDivide(chunkInlines, pageInlines);
}
std::uint32_t PagedWorkspaceInfo::PagesX() const {
    return CeilDivide(chunkXlines, pageXlines);
}
std::uint32_t PagedWorkspaceInfo::PagesT() const {
    return CeilDivide(chunkSamples, pageSamples);
}
std::uint64_t PagedWorkspaceInfo::PagesPerChunk() const {
    return static_cast<std::uint64_t>(PagesI()) * PagesX() * PagesT();
}
std::uint64_t PagedWorkspaceInfo::PageCount() const {
    return ChunkCount() * PagesPerChunk();
}
std::uint64_t PagedWorkspaceInfo::PageBytes() const {
    return static_cast<std::uint64_t>(pageInlines) * pageXlines * pageSamples * 4ull;
}
std::uint64_t PagedWorkspaceInfo::LogicalBytes() const {
    return static_cast<std::uint64_t>(inlineAxis.count) * static_cast<std::uint64_t>(xlineAxis.count) *
           static_cast<std::uint64_t>(samples) * 4ull;
}
std::uint64_t PagedWorkspaceInfo::ChunkOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const {
    return static_cast<std::uint64_t>(ci) + static_cast<std::uint64_t>(ChunksI()) *
        (cx + static_cast<std::uint64_t>(ChunksX()) * ct);
}
std::uint64_t PagedWorkspaceInfo::PageOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                                              std::uint32_t pi, std::uint32_t px, std::uint32_t pt) const {
    const std::uint64_t within = static_cast<std::uint64_t>(pi) +
        static_cast<std::uint64_t>(PagesI()) *
            (px + static_cast<std::uint64_t>(PagesX()) * pt);
    return ChunkOrdinal(ci, cx, ct) * PagesPerChunk() + within;
}

std::string PagedWorkspaceInfo::Describe() const {
    return "paged v" + std::to_string(version) + " " + std::to_string(inlineAxis.count) + "x" +
           std::to_string(xlineAxis.count) + "x" + std::to_string(samples) +
           " chunk=" + std::to_string(chunkInlines) + "x" + std::to_string(chunkXlines) + "x" +
           std::to_string(chunkSamples) +
           " page=" + std::to_string(pageInlines) + "x" + std::to_string(pageXlines) + "x" +
           std::to_string(pageSamples) +
           " codec=" + std::to_string(codec) + " lod=" + std::to_string(lodLevel) +
           (complete ? " complete" : " INCOMPLETE") +
           " inline[" + inlineAxis.Describe() + "] xline[" + xlineAxis.Describe() + "]";
}

std::filesystem::path PagedWorkspacePartialPath(const std::filesystem::path& finalPath) {
    std::filesystem::path partial = finalPath;
    partial += L".partial";
    return partial;
}

std::uint64_t PagedWorkspaceFingerprint(const std::filesystem::path& path, std::uintmax_t fileSize) {
    // Same idea as the index cache: size + first/last 32 KiB. Cheap and strong
    // enough to detect a replaced data set of the same dimensions.
    std::uint64_t hash = kFnvOffset;
    hash = Fnv1a(&fileSize, sizeof(fileSize), hash);
    std::ifstream in(path, std::ios::binary);
    if(!in) {
        return hash;
    }
    std::vector<char> buffer(32 * 1024);
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    hash = Fnv1a(buffer.data(), static_cast<std::size_t>(in.gcount()), hash);
    if(fileSize > buffer.size()) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(fileSize - buffer.size()), std::ios::beg);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash = Fnv1a(buffer.data(), static_cast<std::size_t>(in.gcount()), hash);
    }
    return hash;
}

// ----------------------------------------------------------------- writer

struct PagedWorkspaceWriter::Impl {
    PagedWorkspaceInfo info;
    std::filesystem::path finalPath;
    std::filesystem::path partialPath;
    std::fstream file;
    std::vector<unsigned char> bitmap;
    std::uint64_t headerBytes = 0;
    std::uint64_t bitmapOffset = 0;
    std::uint64_t chunkDirOffset = 0;
    std::uint64_t pageDirOffset = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataWritten = 0;
    std::uint64_t pageOrdinal = 0;
    std::uint64_t chunksWritten = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t checkpointWrites = 0;
    bool dirtyBitmap = false;
    std::uint64_t chunksSinceFlush = 0;
    std::vector<float> pageBuffer;
    std::vector<unsigned char> compressed;
};

PagedWorkspaceWriter::~PagedWorkspaceWriter() {
    // A writer that is destroyed without Finalize (cancel, scope exit, error)
    // must still persist its completion bitmap so a later resume keeps the work
    // that was already written. A hard process crash can still lose up to the
    // checkpoint cadence (kFlushEveryChunks).
    if(!impl_) {
        return;
    }
    if(impl_->file.is_open() && impl_->dirtyBitmap) {
        impl_->file.seekp(static_cast<std::streamoff>(impl_->bitmapOffset), std::ios::beg);
        impl_->file.write(reinterpret_cast<const char*>(impl_->bitmap.data()),
                          static_cast<std::streamsize>(impl_->bitmap.size()));
        impl_->file.flush();
        impl_->dirtyBitmap = false;
    }
}

const PagedWorkspaceInfo& PagedWorkspaceWriter::Info() const {
    static const PagedWorkspaceInfo empty;
    return impl_ ? impl_->info : empty;
}
std::uint64_t PagedWorkspaceWriter::ChunksWritten() const { return impl_ ? impl_->chunksWritten : 0; }
std::uint64_t PagedWorkspaceWriter::BytesWritten() const { return impl_ ? impl_->bytesWritten : 0; }
std::uint64_t PagedWorkspaceWriter::CheckpointWrites() const { return impl_ ? impl_->checkpointWrites : 0; }

bool PagedWorkspaceWriter::Open(const std::filesystem::path& finalPath, const PagedWorkspaceInfo& info,
                                bool resume, std::string& errorMessage) {
    impl_ = std::make_shared<Impl>();
    impl_->info = info;
    impl_->finalPath = finalPath;
    impl_->partialPath = PagedWorkspacePartialPath(finalPath);
    std::error_code directoryError;
    if(!finalPath.parent_path().empty()) {
        std::filesystem::create_directories(finalPath.parent_path(), directoryError);
    }

    const std::uint64_t chunkCount = info.ChunkCount();
    const std::uint64_t pageCount = info.PageCount();
    const std::uint64_t bitmapBytes = (chunkCount + 7) / 8;
    impl_->bitmap.assign(static_cast<std::size_t>(bitmapBytes), 0);
    impl_->headerBytes = HeaderBytesForInfo(info);
    if(impl_->headerBytes == 0) {
        errorMessage = "the paged workspace metadata needs an unsupported header size";
        return false;
    }
    impl_->bitmapOffset = impl_->headerBytes;
    impl_->chunkDirOffset = impl_->bitmapOffset + bitmapBytes;
    impl_->pageDirOffset = impl_->chunkDirOffset + chunkCount * 12;
    impl_->dataOffset = impl_->pageDirOffset + pageCount * 32;
    impl_->pageBuffer.assign(static_cast<std::size_t>(info.PageBytes() / 4), 0.0f);

    // A crash before the initial header write can leave a zero-byte file.
    // It contains no recoverable work. Reinitialize it, but still reject any
    // nonempty malformed partial rather than silently discarding user data.
    const bool hasPartial = std::filesystem::exists(impl_->partialPath, directoryError);
    std::error_code sizeError;
    const bool emptyPartial = hasPartial &&
        std::filesystem::file_size(impl_->partialPath, sizeError) == 0 && !sizeError;
    if(resume && hasPartial && !emptyPartial) {
        // Resume: read the existing header/dirs/bitmap and validate them against
        // the requested info; continue appending pages.
        PagedWorkspaceReader probe;
        std::string probeError;
        if(!probe.Open(impl_->partialPath, probeError)) {
            errorMessage = "cannot resume the partial workspace: " + probeError;
            return false;
        }
        const PagedWorkspaceInfo& existing = probe.Info();
        if(existing.version != info.version || existing.chunkInlines != info.chunkInlines ||
           existing.chunkXlines != info.chunkXlines || existing.chunkSamples != info.chunkSamples ||
           existing.pageInlines != info.pageInlines || existing.pageXlines != info.pageXlines ||
           existing.pageSamples != info.pageSamples || existing.samples != info.samples ||
           existing.inlineAxis.count != info.inlineAxis.count ||
           existing.xlineAxis.count != info.xlineAxis.count ||
           existing.inlineAxis.origin != info.inlineAxis.origin ||
           existing.inlineAxis.step != info.inlineAxis.step ||
           existing.xlineAxis.origin != info.xlineAxis.origin ||
           existing.xlineAxis.step != info.xlineAxis.step ||
           existing.sourceSize != info.sourceSize ||
           existing.sourceMtimeTicks != info.sourceMtimeTicks ||
           existing.codec != info.codec || existing.lodLevel != info.lodLevel) {
            errorMessage = "the partial workspace does not match the requested build";
            return false;
        }
        // Read the bitmap and the directories from the partial file.
        std::ifstream in(impl_->partialPath, std::ios::binary);
        if(!in) {
            errorMessage = "cannot read the partial workspace";
            return false;
        }
        in.seekg(static_cast<std::streamoff>(impl_->bitmapOffset), std::ios::beg);
        in.read(reinterpret_cast<char*>(impl_->bitmap.data()),
                static_cast<std::streamsize>(impl_->bitmap.size()));
        if(!in) {
            errorMessage = "the partial workspace bitmap is truncated";
            return false;
        }
        for(std::uint64_t page = 0; page < pageCount; ++page) {
            std::uint64_t offset = 0;
            std::uint32_t stored = 0;
            std::uint32_t raw = 0;
            std::uint64_t crc = 0;
            std::uint32_t codec = 0;
            in.seekg(static_cast<std::streamoff>(impl_->pageDirOffset + page * 32), std::ios::beg);
            in.read(reinterpret_cast<char*>(&offset), 8);
            in.read(reinterpret_cast<char*>(&stored), 4);
            in.read(reinterpret_cast<char*>(&raw), 4);
            in.read(reinterpret_cast<char*>(&crc), 8);
            in.read(reinterpret_cast<char*>(&codec), 4);
            if(stored > 0) {
                impl_->pageOrdinal = page + 1;
                impl_->dataWritten = std::max(impl_->dataWritten, offset - impl_->dataOffset + stored);
                impl_->bytesWritten += stored;
            }
        }
        for(std::uint64_t chunk = 0; chunk < chunkCount; ++chunk) {
            const std::size_t byte = static_cast<std::size_t>(chunk / 8);
            if((impl_->bitmap[byte] & (1u << (chunk % 8))) != 0) {
                ++impl_->chunksWritten;
            }
        }
        impl_->file.open(impl_->partialPath, std::ios::binary | std::ios::in | std::ios::out);
        if(!impl_->file) {
            errorMessage = "cannot reopen the partial workspace for writing";
            return false;
        }
        return true;
    }

    impl_->file.open(impl_->partialPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if(!impl_->file) {
        errorMessage = "cannot create the partial workspace";
        return false;
    }
    // Header + directories are written up front so the file is a valid (but
    // incomplete) workspace from the first byte on.
    std::vector<unsigned char> header;
    if(!SerializeHeader(info, info.complete, impl_->headerBytes, header, errorMessage)) {
        return false;
    }
    impl_->file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    impl_->file.write(reinterpret_cast<const char*>(impl_->bitmap.data()),
                      static_cast<std::streamsize>(impl_->bitmap.size()));
    std::vector<unsigned char> zeroes(static_cast<std::size_t>(chunkCount * 12 + pageCount * 32), 0);
    impl_->file.write(reinterpret_cast<const char*>(zeroes.data()),
                      static_cast<std::streamsize>(zeroes.size()));
    impl_->file.flush();
    return static_cast<bool>(impl_->file);
}

bool PagedWorkspaceWriter::Complete(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const {
    if(!impl_) {
        return false;
    }
    const std::uint64_t chunk = impl_->info.ChunkOrdinal(ci, cx, ct);
    const std::size_t byte = static_cast<std::size_t>(chunk / 8);
    if(byte >= impl_->bitmap.size()) {
        return false;
    }
    return (impl_->bitmap[byte] & (1u << (chunk % 8))) != 0;
}

bool PagedWorkspaceWriter::WriteChunk(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                                      const float* values, std::string& errorMessage) {
    if(!impl_) {
        errorMessage = "the paged writer is not open";
        return false;
    }
    const PagedWorkspaceInfo& info = impl_->info;
    const std::uint64_t chunkOrdinal = info.ChunkOrdinal(ci, cx, ct);
    if(Complete(ci, cx, ct)) {
        return true;
    }
    std::vector<unsigned char> pageDirEntry;
    pageDirEntry.reserve(32);
    for(std::uint32_t a = 0; a < info.PagesI(); ++a) {
        for(std::uint32_t b = 0; b < info.PagesX(); ++b) {
            for(std::uint32_t c = 0; c < info.PagesT(); ++c) {
                const std::uint32_t i0 = a * info.pageInlines;
                const std::uint32_t x0 = b * info.pageXlines;
                const std::uint32_t t0 = c * info.pageSamples;
                for(std::uint32_t s = 0; s < info.pageSamples; ++s) {
                    const std::uint32_t t = t0 + s;
                    for(std::uint32_t xx = 0; xx < info.pageXlines; ++xx) {
                        const std::uint32_t x = x0 + xx;
                        const float* row = values +
                            (static_cast<std::size_t>(t) * info.chunkXlines + x) * info.chunkInlines + i0;
                        float* dst = impl_->pageBuffer.data() +
                            (static_cast<std::size_t>(s) * info.pageXlines + xx) * info.pageInlines;
                        for(std::uint32_t ii = 0; ii < info.pageInlines; ++ii) {
                            dst[ii] = i0 + ii < info.chunkInlines
                                ? row[ii]
                                : std::numeric_limits<float>::quiet_NaN();
                        }
                    }
                }
                const std::size_t rawBytes = impl_->pageBuffer.size() * sizeof(float);
                const unsigned char* payload =
                    reinterpret_cast<const unsigned char*>(impl_->pageBuffer.data());
                std::uint32_t storedBytes = static_cast<std::uint32_t>(rawBytes);
                std::uint32_t codec = kPagedCodecRaw;
#ifdef SEISMIC_HAVE_ZSTD
                if(info.codec == kPagedCodecZstd) {
                    impl_->compressed.resize(ZSTD_compressBound(rawBytes));
                    const std::size_t written = ZSTD_compress(impl_->compressed.data(),
                                                              impl_->compressed.size(), payload,
                                                              rawBytes, info.codecLevel);
                    if(ZSTD_isError(written)) {
                        errorMessage = std::string("zstd page compress failed: ") + ZSTD_getErrorName(written);
                        return false;
                    }
                    payload = impl_->compressed.data();
                    storedBytes = static_cast<std::uint32_t>(written);
                    codec = kPagedCodecZstd;
                }
#else
                if(info.codec == kPagedCodecZstd) {
                    errorMessage = "this build has no zstd support for paged workspaces";
                    return false;
                }
#endif
                const std::uint64_t crc = Fnv1a(payload, storedBytes);
                const std::uint64_t pageOffset = impl_->dataOffset + impl_->dataWritten;
                impl_->file.seekp(static_cast<std::streamoff>(pageOffset), std::ios::beg);
                impl_->file.write(reinterpret_cast<const char*>(payload),
                                  static_cast<std::streamsize>(storedBytes));
                if(!impl_->file) {
                    errorMessage = "cannot write a paged page";
                    return false;
                }
                const std::uint64_t within =
                    static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(info.PagesI()) *
                        (static_cast<std::uint64_t>(b) + static_cast<std::uint64_t>(info.PagesX()) * c);
                const std::uint64_t dirIndex = chunkOrdinal * info.PagesPerChunk() + within;
                std::vector<unsigned char> entry;
                entry.reserve(32);
                Append<std::uint64_t>(entry, pageOffset);
                Append<std::uint32_t>(entry, storedBytes);
                Append<std::uint32_t>(entry, static_cast<std::uint32_t>(rawBytes));
                Append<std::uint64_t>(entry, crc);
                Append<std::uint32_t>(entry, codec);
                impl_->file.seekp(static_cast<std::streamoff>(impl_->pageDirOffset + dirIndex * 32),
                                  std::ios::beg);
                impl_->file.write(reinterpret_cast<const char*>(entry.data()),
                                  static_cast<std::streamsize>(entry.size()));
                impl_->dataWritten += storedBytes;
                impl_->bytesWritten += storedBytes;
                ++impl_->pageOrdinal;
            }
        }
    }
    impl_->bitmap[static_cast<std::size_t>(chunkOrdinal / 8)] |=
        static_cast<unsigned char>(1u << (chunkOrdinal % 8));
    impl_->dirtyBitmap = true;
    ++impl_->chunksWritten;
    ++impl_->chunksSinceFlush;
    if(impl_->chunksSinceFlush >= kFlushEveryChunks) {
        // Checkpoint: persist the completion bitmap and flush the data so a
        // crash keeps every fully written chunk.
        impl_->file.seekp(static_cast<std::streamoff>(impl_->bitmapOffset), std::ios::beg);
        impl_->file.write(reinterpret_cast<const char*>(impl_->bitmap.data()),
                          static_cast<std::streamsize>(impl_->bitmap.size()));
        impl_->file.flush();
        impl_->dirtyBitmap = false;
        impl_->chunksSinceFlush = 0;
        ++impl_->checkpointWrites;
        if(!impl_->file) {
            errorMessage = "cannot checkpoint the paged workspace";
            return false;
        }
    }
    return true;
}

bool PagedWorkspaceWriter::Finalize(std::string& errorMessage) {
    if(!impl_) {
        errorMessage = "the paged writer is not open";
        return false;
    }
    if(impl_->chunksWritten != impl_->info.ChunkCount()) {
        errorMessage = "the paged workspace is incomplete (" + std::to_string(impl_->chunksWritten) +
                       " of " + std::to_string(impl_->info.ChunkCount()) + " chunks)";
        return false;
    }
    // Mark complete and persist the bitmap.
    impl_->info.complete = true;
    std::vector<unsigned char> header;
    if(!SerializeHeader(impl_->info, true, impl_->headerBytes, header, errorMessage)) {
        return false;
    }
    impl_->file.seekp(0, std::ios::beg);
    impl_->file.write(reinterpret_cast<const char*>(header.data()),
                      static_cast<std::streamsize>(header.size()));
    impl_->file.seekp(static_cast<std::streamoff>(impl_->bitmapOffset), std::ios::beg);
    impl_->file.write(reinterpret_cast<const char*>(impl_->bitmap.data()),
                      static_cast<std::streamsize>(impl_->bitmap.size()));
    impl_->file.flush();
    if(!impl_->file) {
        errorMessage = "cannot finalize the paged workspace";
        return false;
    }
    impl_->file.close();

    // Atomic publish: readers only ever see a complete file.
#ifdef _WIN32
    if(!MoveFileExW(impl_->partialPath.c_str(), impl_->finalPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errorMessage = "MoveFileExW failed while publishing the paged workspace";
        return false;
    }
#else
    std::error_code renameError;
    std::filesystem::rename(impl_->partialPath, impl_->finalPath, renameError);
    if(renameError) {
        errorMessage = "rename failed while publishing the paged workspace";
        return false;
    }
#endif
    impl_.reset();
    return true;
}

// ----------------------------------------------------------------- reader

struct PagedWorkspaceReader::Impl {
    PagedWorkspaceInfo info;
    std::vector<unsigned char> header;
    std::uint64_t headerBytes = 0;
    std::uint64_t bitmapOffset = 0;
    std::uint64_t bitmapBytes = 0;
    std::uint64_t pageDirOffset = 0;
    std::uint64_t pageDirBytes = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t fileBytes = 0;
    std::uint64_t chunkCount = 0, pageCount = 0, pagesPerChunk = 0, pageBytes = 0;
    std::shared_ptr<std::ifstream> file;
    Stats stats;
    std::shared_ptr<ChunkCache> pageCache;
    std::uint64_t cacheSourceKey = 0;
    bool open = false;
    bool metadataOnly = false;

    struct DirectoryBlock {
        std::vector<unsigned char> bytes;
        std::list<std::uint64_t>::iterator lru;
    };
    std::list<std::uint64_t> directoryLru;
    std::unordered_map<std::uint64_t, DirectoryBlock> directoryBlocks;

    void TrimDirectory(std::uint64_t incoming = 0) {
        while(!directoryLru.empty() &&
              stats.directoryCacheBytes > stats.directoryCacheBudgetBytes - incoming) {
            const auto key = directoryLru.back();
            stats.directoryCacheBytes -= directoryBlocks.at(key).bytes.size();
            directoryBlocks.erase(key);
            directoryLru.pop_back();
        }
    }

    bool ReadAt(std::uint64_t offset, std::size_t bytes, unsigned char* out,
                std::string& errorMessage) {
        if(offset > fileBytes || bytes > fileBytes - offset) {
            errorMessage = "the paged directory or payload range is outside the file";
            return false;
        }
        file->clear();
        file->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file->read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(bytes));
        stats.directoryReadBytes += static_cast<std::uint64_t>(file->gcount());
        ++stats.directoryReadCalls;
        if(!*file) {
            errorMessage = "a paged directory read failed";
            return false;
        }
        return true;
    }

    bool DirectoryBytes(std::uint64_t section, std::uint64_t sectionBytes,
                        std::uint64_t relative, std::size_t bytes, unsigned char* out,
                        std::string& errorMessage) {
        if(relative > sectionBytes || bytes > sectionBytes - relative) {
            errorMessage = "the paged directory entry is out of range";
            return false;
        }
        const auto blockBytes = std::min<std::uint64_t>(64u << 10,
            (stats.directoryCacheBudgetBytes / 32) * 32);
        if(blockBytes == 0) {
            ++stats.directoryCacheMisses;
            return ReadAt(section + relative, bytes, out, errorMessage);
        }
        const auto base = relative / blockBytes * blockBytes;
        const auto key = section + base;
        const auto inBlock = static_cast<std::size_t>(relative - base);
        auto found = directoryBlocks.find(key);
        if(found == directoryBlocks.end()) {
            ++stats.directoryCacheMisses;
            const auto count = static_cast<std::size_t>(std::min(blockBytes, sectionBytes - base));
            TrimDirectory(count); // Evict before allocating, not after peak growth.
            DirectoryBlock block;
            block.bytes.resize(count);
            if(!ReadAt(key, count, block.bytes.data(), errorMessage)) return false;
            directoryLru.push_front(key);
            block.lru = directoryLru.begin();
            found = directoryBlocks.emplace(key, std::move(block)).first;
            stats.directoryCacheBytes += count;
            stats.directoryCachePeakBytes = std::max(stats.directoryCachePeakBytes,
                                                      stats.directoryCacheBytes);
        } else {
            ++stats.directoryCacheHits;
            directoryLru.splice(directoryLru.begin(), directoryLru, found->second.lru);
        }
        if(inBlock > found->second.bytes.size() || bytes > found->second.bytes.size() - inBlock) {
            errorMessage = "the paged directory entry crosses a cache block";
            return false;
        }
        std::memcpy(out, found->second.bytes.data() + inBlock, bytes);
        return true;
    }

    bool ValidateLayout(std::string& errorMessage) {
        const auto& v = info;
        const auto validDimension = [](std::uint32_t n) {
            return n > 0 && n <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
        };
        std::uint64_t chunksIX = 0, pagesIX = 0, pagesT = 0, chunkDirBytes = 0;
        bool valid = ValidReadAxis(v.inlineAxis) && ValidReadAxis(v.xlineAxis) &&
            validDimension(v.samples) && validDimension(v.chunkInlines) &&
            validDimension(v.chunkXlines) && validDimension(v.chunkSamples) &&
            validDimension(v.pageInlines) && validDimension(v.pageXlines) &&
            validDimension(v.pageSamples) && v.pageInlines <= v.chunkInlines &&
            v.pageXlines <= v.chunkXlines && v.pageSamples <= v.chunkSamples &&
            (v.codec == kPagedCodecRaw || v.codec == kPagedCodecZstd) &&
            CheckedMultiply(v.ChunksI(), v.ChunksX(), chunksIX) &&
            CheckedMultiply(chunksIX, v.ChunksT(), chunkCount) &&
            CheckedMultiply(v.PagesI(), v.PagesX(), pagesIX) &&
            CheckedMultiply(pagesIX, v.PagesT(), pagesPerChunk) &&
            CheckedMultiply(chunkCount, pagesPerChunk, pageCount) &&
            CheckedMultiply(v.pageInlines, v.pageXlines, pagesIX) &&
            CheckedMultiply(pagesIX, v.pageSamples, pagesT) &&
            CheckedMultiply(pagesT, sizeof(float), pageBytes) &&
            pageBytes <= kMaxDecodedPageBytes &&
            CheckedMultiply(chunkCount, 12, chunkDirBytes) &&
            CheckedMultiply(pageCount, 32, pageDirBytes);
        if(valid) {
            bitmapBytes = chunkCount / 8 + (chunkCount % 8 != 0);
            bitmapOffset = headerBytes;
            valid = CheckedAdd(bitmapOffset, bitmapBytes, pageDirOffset) &&
                CheckedAdd(pageDirOffset, chunkDirBytes, pageDirOffset) &&
                CheckedAdd(pageDirOffset, pageDirBytes, dataOffset) &&
                dataOffset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
        }
        if(!valid) errorMessage = "the paged workspace layout is invalid, overflowing or exceeds page limits";
        return valid;
    }

    struct PageEntry {
        std::uint64_t offset = 0;
        std::uint32_t storedBytes = 0;
        std::uint32_t rawBytes = 0;
        std::uint64_t crc = 0;
        std::uint32_t codec = 0;
    };
    bool Entry(std::uint64_t pageIndex, PageEntry& entry, std::string& errorMessage) {
        if(pageIndex >= pageCount) {
            errorMessage = "the page index is out of range";
            return false;
        }
        const auto chunk = pageIndex / pagesPerChunk;
        unsigned char bit = 0;
        if(!DirectoryBytes(bitmapOffset, bitmapBytes, chunk / 8, 1, &bit, errorMessage)) return false;
        if((bit & (1u << (chunk % 8))) == 0) {
            errorMessage = "the requested paged chunk is incomplete";
            return false;
        }
        unsigned char bytes[32];
        if(!DirectoryBytes(pageDirOffset, pageDirBytes, pageIndex * 32, sizeof(bytes), bytes, errorMessage)) return false;
        std::memcpy(&entry.offset, bytes, 8);
        std::memcpy(&entry.storedBytes, bytes + 8, 4);
        std::memcpy(&entry.rawBytes, bytes + 12, 4);
        std::memcpy(&entry.crc, bytes + 16, 8);
        std::memcpy(&entry.codec, bytes + 24, 4);
        // Stored bytes may be slightly larger than raw bytes for tiny zstd pages.
        const auto maxStored = pageBytes + pageBytes / 128 + (128u << 10);
        if(entry.rawBytes != pageBytes || entry.storedBytes == 0 || entry.storedBytes > maxStored ||
           (entry.codec != kPagedCodecRaw && entry.codec != kPagedCodecZstd) ||
           (entry.codec == kPagedCodecRaw && entry.storedBytes != entry.rawBytes) ||
           entry.offset < dataOffset || entry.offset > fileBytes || entry.storedBytes > fileBytes - entry.offset) {
            errorMessage = "a paged directory entry has invalid codec, size or payload bounds";
            return false;
        }
        return true;
    }
};

const PagedWorkspaceInfo& PagedWorkspaceReader::Info() const {
    static const PagedWorkspaceInfo empty;
    return impl_ ? impl_->info : empty;
}
const PagedWorkspaceReader::Stats& PagedWorkspaceReader::ReaderStats() const {
    static const Stats empty;
    return impl_ ? impl_->stats : empty;
}
void PagedWorkspaceReader::ResetReaderStats() {
    if(impl_) {
        const auto current = impl_->stats.directoryCacheBytes;
        const auto budget = impl_->stats.directoryCacheBudgetBytes;
        impl_->stats = Stats{};
        impl_->stats.directoryCacheBytes = current;
        impl_->stats.directoryCachePeakBytes = current;
        impl_->stats.directoryCacheBudgetBytes = budget;
    }
}
void PagedWorkspaceReader::SetPageCache(std::shared_ptr<ChunkCache> cache) {
    if(impl_) {
        impl_->pageCache = std::move(cache);
    }
}
std::shared_ptr<ChunkCache> PagedWorkspaceReader::PageCacheHandle() const {
    return impl_ ? impl_->pageCache : nullptr;
}
std::uint64_t PagedWorkspaceReader::CacheSourceKey() const {
    return impl_ ? impl_->cacheSourceKey : 0;
}

void PagedWorkspaceReader::SetDirectoryCacheBudget(std::size_t bytes) {
    directoryCacheBudget_ = bytes;
    if(impl_) {
        // Block alignment depends on the budget. Drop old blocks on any change.
        impl_->directoryBlocks.clear();
        impl_->directoryLru.clear();
        impl_->stats.directoryCacheBytes = 0;
        impl_->stats.directoryCacheBudgetBytes = bytes;
    }
}
std::size_t PagedWorkspaceReader::DirectoryCacheBudget() const {
    return impl_ ? static_cast<std::size_t>(impl_->stats.directoryCacheBudgetBytes) : directoryCacheBudget_;
}

bool PagedWorkspaceReader::Open(const std::filesystem::path& path, std::string& errorMessage,
                                bool metadataOnly) {
    errorMessage.clear();
    impl_ = std::make_shared<Impl>();
    impl_->stats.directoryCacheBudgetBytes = directoryCacheBudget_;
    impl_->file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if(!*impl_->file) {
        errorMessage = "cannot open the paged workspace";
        return false;
    }
    impl_->file->seekg(0, std::ios::end);
    const auto lengthAtOpen = impl_->file->tellg();
    if(lengthAtOpen < 0) {
        errorMessage = "cannot determine the paged workspace length";
        return false;
    }
    impl_->fileBytes = static_cast<std::uint64_t>(lengthAtOpen);
    impl_->file->seekg(0, std::ios::beg);
    std::vector<unsigned char> prefix(16, 0);
    impl_->file->read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    impl_->stats.headerReadBytes += static_cast<std::uint64_t>(impl_->file->gcount());
    if(!*impl_->file || std::memcmp(prefix.data(), kPagedWorkspaceMagic, 8) != 0) {
        errorMessage = "the paged workspace magic is wrong";
        return false;
    }
    std::uint32_t diskVersion = 0;
    std::memcpy(&diskVersion, prefix.data() + 8, sizeof(diskVersion));
    if(diskVersion == kPagedWorkspaceLegacyVersion) {
        impl_->headerBytes = kLegacyHeaderBytes;
    } else if(diskVersion == kPagedWorkspaceVersion) {
        std::uint32_t diskHeaderBytes = 0;
        std::memcpy(&diskHeaderBytes, prefix.data() + 12, sizeof(diskHeaderBytes));
        impl_->headerBytes = diskHeaderBytes;
        if(impl_->headerBytes < kHeaderAlignment || impl_->headerBytes > kMaxHeaderBytes ||
           (impl_->headerBytes % kHeaderAlignment) != 0) {
            errorMessage = "the paged workspace header size is invalid";
            return false;
        }
    } else {
        errorMessage = "the paged workspace version is not supported";
        return false;
    }
    std::vector<unsigned char> header(static_cast<std::size_t>(impl_->headerBytes), 0);
    impl_->file->clear();
    impl_->file->seekg(0, std::ios::beg);
    impl_->file->read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    impl_->stats.headerReadBytes += static_cast<std::uint64_t>(impl_->file->gcount());
    if(!*impl_->file) {
        errorMessage = "the paged workspace header is truncated";
        return false;
    }
    std::size_t offset = 8;
    PagedWorkspaceInfo& info = impl_->info;
    std::uint32_t completeFlag = 0;
    std::uint32_t pathLen = 0;
    std::uint32_t serializedHeaderBytes = 0;
    if(!Consume(header, offset, info.version) ||
       (info.version >= kPagedWorkspaceVersion &&
        (!Consume(header, offset, serializedHeaderBytes) ||
         serializedHeaderBytes != impl_->headerBytes)) ||
       !Consume(header, offset, info.algorithmVersion) ||
       !Consume(header, offset, info.chunkInlines) || !Consume(header, offset, info.chunkXlines) ||
       !Consume(header, offset, info.chunkSamples) || !Consume(header, offset, info.pageInlines) ||
       !Consume(header, offset, info.pageXlines) || !Consume(header, offset, info.pageSamples) ||
       !Consume(header, offset, info.samples) || !Consume(header, offset, info.sampleIntervalUs) ||
       !Consume(header, offset, info.codec) || !Consume(header, offset, info.codecLevel) ||
       !Consume(header, offset, info.lodLevel) || !Consume(header, offset, info.lodMethod) ||
       !Consume(header, offset, info.lodFactorSamples) ||
       !Consume(header, offset, info.lodFactorInlines) ||
       !Consume(header, offset, info.lodFactorXlines) ||
       !Consume(header, offset, info.lodAlgorithmVersion) ||
       !Consume(header, offset, info.lodSourceHash) || !Consume(header, offset, info.sourceSize) ||
       !Consume(header, offset, info.sourceMtimeTicks) ||
       !Consume(header, offset, info.sourceFingerprint) ||
       !Consume(header, offset, info.buildGeneration) ||
       !Consume(header, offset, info.coverageInlineMin) ||
       !Consume(header, offset, info.coverageInlineMax) ||
       !Consume(header, offset, info.coverageXlineMin) ||
       !Consume(header, offset, info.coverageXlineMax) || !Consume(header, offset, completeFlag) ||
       !Consume(header, offset, pathLen)) {
        errorMessage = "the paged workspace header is truncated";
        return false;
    }
    if(info.version != kPagedWorkspaceLegacyVersion && info.version != kPagedWorkspaceVersion) {
        errorMessage = "the paged workspace version is not supported";
        return false;
    }
    if(offset + pathLen > header.size()) {
        errorMessage = "the paged workspace source path is truncated";
        return false;
    }
    info.sourcePath.assign(reinterpret_cast<const char*>(header.data() + offset), pathLen);
    offset += pathLen;
    if(!ConsumeAxis(header, offset, info.inlineAxis) || !ConsumeAxis(header, offset, info.xlineAxis)) {
        errorMessage = "the paged workspace axes are truncated";
        return false;
    }
    if(completeFlag > 1) {
        errorMessage = "the paged workspace completion flag is invalid";
        return false;
    }
    if(!impl_->ValidateLayout(errorMessage)) return false;
    info.complete = completeFlag != 0;
    impl_->header = std::move(header);
    // Bind all axes, source metadata, format and generation, plus actual derivative identity.
    impl_->cacheSourceKey = Fnv1a(impl_->header.data(), impl_->header.size(), kFnvOffset);
    const auto canonical = std::filesystem::absolute(path).lexically_normal().u8string();
    impl_->cacheSourceKey = Fnv1a(canonical.data(), canonical.size(), impl_->cacheSourceKey);
    std::error_code identityError;
    const auto stamp = std::filesystem::last_write_time(path, identityError).time_since_epoch().count();
    if(!identityError) impl_->cacheSourceKey = Fnv1a(&stamp, sizeof(stamp), impl_->cacheSourceKey);
    impl_->cacheSourceKey = Fnv1a(&impl_->fileBytes, sizeof(impl_->fileBytes), impl_->cacheSourceKey);
    if(metadataOnly) {
        impl_->metadataOnly = true;
        impl_->open = true;
        return true;
    }
    if(impl_->dataOffset > impl_->fileBytes) {
        errorMessage = "the paged workspace directory is truncated";
        return false;
    }
    impl_->open = true;
    return true;
}

bool PagedWorkspaceReader::ReadPageBytes(std::uint64_t pageIndex, std::uint64_t byteOffset,
                                         std::uint64_t byteLength, std::vector<unsigned char>& out,
                                         std::string& errorMessage) {
    out.clear();
    errorMessage.clear();
    if(!impl_ || !impl_->open || impl_->metadataOnly || pageIndex >= impl_->pageCount) {
        errorMessage = "the page index is out of range";
        return false;
    }
    Impl::PageEntry entry;
    if(!impl_->Entry(pageIndex, entry, errorMessage)) return false;
    if(entry.codec != kPagedCodecRaw) {
        errorMessage = "raw partial reads need raw pages";
        return false;
    }
    if(byteOffset > entry.storedBytes || byteLength > entry.storedBytes - byteOffset) {
        errorMessage = "the partial read exceeds the page";
        return false;
    }
    out.resize(static_cast<std::size_t>(byteLength));
    impl_->file->clear();
    impl_->file->seekg(static_cast<std::streamoff>(entry.offset + byteOffset), std::ios::beg);
    impl_->file->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(byteLength));
    if(!*impl_->file) {
        out.clear();
        errorMessage = "a paged partial page read failed";
        return false;
    }
    ++impl_->stats.readCalls;
    impl_->stats.readBytes += byteLength;
    ++impl_->stats.readRanges;
    ++impl_->stats.pagesTouched;
    return true;
}

bool PagedWorkspaceReader::ReadBox(int inlineIndex, int xlineIndex, int sampleIndex,
                                   int inlines, int xlines, int samples,
                                   std::vector<float>& out, std::string& errorMessage, CancelToken* cancel,
                                   bool timeDisplayLayout) {
    out.clear();
    errorMessage.clear();
    struct ClearFailedOutput {
        std::vector<float>& output;
        bool success = false;
        ~ClearFailedOutput() { if(!success) output.clear(); }
    } outputGuard{out};
    auto cancelled = [&]() {
        if(!Cancelled(cancel)) return false;
        out.clear();
        errorMessage = "request cancelled";
        return true;
    };
    if(cancelled()) return false;
    if(!impl_ || !impl_->open) {
        errorMessage = "the paged workspace is not open";
        return false;
    }
    if(impl_->metadataOnly) {
        errorMessage = "the paged workspace was opened for metadata only";
        return false;
    }
    const PagedWorkspaceInfo& info = impl_->info;
    if(inlines <= 0 || xlines <= 0 || samples <= 0 || (timeDisplayLayout && samples != 1)) {
        errorMessage = "the requested box is empty";
        return false;
    }
    // Exact axis gate: only real axis elements can be requested.
    int iIndex = 0;
    int xIndex = 0;
    if(!ExactReadIndex(info.inlineAxis, inlineIndex, iIndex)) {
        errorMessage = "inline " + std::to_string(inlineIndex) + " is not present in this workspace";
        return false;
    }
    if(!ExactReadIndex(info.xlineAxis, xlineIndex, xIndex)) {
        errorMessage = "xline " + std::to_string(xlineIndex) + " is not present in this workspace";
        return false;
    }
    if(inlines > info.inlineAxis.count - iIndex || xlines > info.xlineAxis.count - xIndex ||
       sampleIndex < 0 || sampleIndex > static_cast<int>(info.samples) ||
       samples > static_cast<int>(info.samples) - sampleIndex) {
        errorMessage = "the requested box is outside the workspace";
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t count = 0;
    if(!CheckedMultiply(inlines, xlines, count) || !CheckedMultiply(count, samples, count) ||
       count > kMaxBoxValues || count > out.max_size()) {
        errorMessage = "the requested paged box exceeds the 64M value limit";
        return false;
    }
    out.assign(static_cast<std::size_t>(count),
               std::numeric_limits<float>::quiet_NaN());

    const std::uint32_t ci0 = static_cast<std::uint32_t>(iIndex) / info.chunkInlines;
    const std::uint32_t ci1 = static_cast<std::uint32_t>(iIndex + inlines - 1) / info.chunkInlines;
    const std::uint32_t cx0 = static_cast<std::uint32_t>(xIndex) / info.chunkXlines;
    const std::uint32_t cx1 = static_cast<std::uint32_t>(xIndex + xlines - 1) / info.chunkXlines;
    const std::uint32_t ct0 = static_cast<std::uint32_t>(sampleIndex) / info.chunkSamples;
    const std::uint32_t ct1 = static_cast<std::uint32_t>(sampleIndex + samples - 1) / info.chunkSamples;

    struct Needed {
        std::uint64_t page = 0;
        std::uint32_t i0 = 0, x0 = 0, t0 = 0;
        Impl::PageEntry entry;
    };
    std::vector<Needed> needed;
    for(std::uint32_t ct = ct0; ct <= ct1; ++ct) {
        // ChunkOrdinal stores ci consecutively. Keep metadata lookup in that
        // order so a wide time plane does not reread every directory block per IL.
        for(std::uint32_t cx = cx0; cx <= cx1; ++cx) {
            if(cancelled()) return false;
            for(std::uint32_t ci = ci0; ci <= ci1; ++ci) {
                // Bound planning by the requested support, not all pages in an
                // unusually large chunk declared by an untrusted header.
                const auto firstPage = [](int begin, std::uint64_t chunkBegin, std::uint32_t page) {
                    return begin > chunkBegin ? static_cast<std::uint32_t>((begin - chunkBegin) / page) : 0u;
                };
                const auto endPage = [](int end, std::uint64_t chunkBegin, std::uint32_t page, std::uint32_t count) {
                    const auto extent = static_cast<std::uint64_t>(end) - chunkBegin;
                    return static_cast<std::uint32_t>(std::min<std::uint64_t>(count, extent / page + (extent % page != 0)));
                };
                const auto ib = std::uint64_t(ci) * info.chunkInlines;
                const auto xb = std::uint64_t(cx) * info.chunkXlines;
                const auto tb = std::uint64_t(ct) * info.chunkSamples;
                for(std::uint32_t pi = firstPage(iIndex, ib, info.pageInlines);
                    pi < endPage(iIndex + inlines, ib, info.pageInlines, info.PagesI()); ++pi) {
                    const auto pageI = std::uint64_t(ci) * info.chunkInlines + std::uint64_t(pi) * info.pageInlines;
                    if(pageI >= static_cast<std::uint64_t>(iIndex + inlines) || pageI + info.pageInlines <= static_cast<std::uint64_t>(iIndex)) {
                        continue;
                    }

                    for(std::uint32_t px = firstPage(xIndex, xb, info.pageXlines);
                        px < endPage(xIndex + xlines, xb, info.pageXlines, info.PagesX()); ++px) {
                        const auto pageX = std::uint64_t(cx) * info.chunkXlines + std::uint64_t(px) * info.pageXlines;
                        // Bounds are axis INDICES (xIndex), never axis values.
                        if(pageX >= static_cast<std::uint64_t>(xIndex + xlines) ||
                           pageX + info.pageXlines <= static_cast<std::uint64_t>(xIndex)) {
                            continue;
                        }

                        for(std::uint32_t pt = firstPage(sampleIndex, tb, info.pageSamples);
                            pt < endPage(sampleIndex + samples, tb, info.pageSamples, info.PagesT()); ++pt) {
                            const auto pageT = std::uint64_t(ct) * info.chunkSamples + std::uint64_t(pt) * info.pageSamples;
                            if(pageT >= static_cast<std::uint64_t>(sampleIndex + samples) ||
                               pageT + info.pageSamples <= static_cast<std::uint64_t>(sampleIndex)) {
                                continue;
                            }

                            Needed item;
                            item.page = info.PageOrdinal(ci, cx, ct, pi, px, pt);
                            item.i0 = static_cast<std::uint32_t>(pageI);
                            item.x0 = static_cast<std::uint32_t>(pageX);
                            item.t0 = static_cast<std::uint32_t>(pageT);
                            if(needed.size() >= kMaxPlannedPages) {
                                errorMessage = "the requested paged box exceeds the 1M planned-page limit";
                                return false;
                            }
                            needed.push_back(item);
                        }
                    }
                }
            }
        }
    }

    impl_->stats.chunksTouched += static_cast<std::uint64_t>(ci1-ci0+1) * (cx1-cx0+1) * (ct1-ct0+1);
    auto cacheKeyFor = [&](std::uint64_t page) {
        ChunkCacheKey key;
        key.source = impl_->cacheSourceKey;
        key.level = info.lodLevel;
        key.cs = static_cast<std::uint32_t>(page >> 40);
        key.ci = static_cast<std::uint32_t>((page >> 20) & 0xFFFFFu);
        key.cx = static_cast<std::uint32_t>(page & 0xFFFFFu);
        return key;
    };
    const double scatterBefore = impl_->stats.scatterMs;
    auto scatter = [&](const Needed& item, const std::vector<float>& values) {
        const auto stamp = std::chrono::steady_clock::now();
        const int t0 = std::max(sampleIndex, static_cast<int>(item.t0));
        const int t1 = static_cast<int>(std::min<std::uint64_t>(sampleIndex + samples, std::uint64_t(item.t0) + info.pageSamples));
        const int x0 = std::max(xIndex, static_cast<int>(item.x0));
        const int x1 = static_cast<int>(std::min<std::uint64_t>(xIndex + xlines, std::uint64_t(item.x0) + info.pageXlines));
        const int i0 = std::max(iIndex, static_cast<int>(item.i0));
        const int i1 = static_cast<int>(std::min<std::uint64_t>(iIndex + inlines, std::uint64_t(item.i0) + info.pageInlines));
        for(int t=t0; t<t1; ++t) for(int x=x0; x<x1; ++x) for(int i=i0; i<i1; ++i) {
            const std::size_t src = (static_cast<std::size_t>(t-item.t0)*info.pageXlines + (x-item.x0))*info.pageInlines + (i-item.i0);
            const std::size_t dst = timeDisplayLayout
                ? static_cast<std::size_t>(inlines-1-(i-iIndex))*xlines + (x-xIndex)
                : (static_cast<std::size_t>(t-sampleIndex)*xlines+(x-xIndex))*inlines+(i-iIndex);
            out[dst] = values[src];
        }
        impl_->stats.scatterMs += MsSince(stamp);
    };
    // Check verified decoded pages BEFORE planning physical I/O. Do not retain
    // all hits outside the byte-budgeted cache: scatter and release each one.
    std::size_t missCount = 0;
    for(Needed& item : needed) {
        if(cancelled()) return false;
        ++impl_->stats.pagesTouched;
        if(!impl_->Entry(item.page, item.entry, errorMessage)) return false;
        auto cached = impl_->pageCache ? impl_->pageCache->Find(cacheKeyFor(item.page)) : nullptr;
        if(cached && cached->size()*sizeof(float) == item.entry.rawBytes) {
            ++impl_->stats.cacheHits;
            scatter(item, *cached);
        } else {
            if(impl_->pageCache) ++impl_->stats.cacheMisses;
            needed[missCount++] = item;
        }
    }
    needed.resize(missCount);
    std::sort(needed.begin(), needed.end(), [&](const Needed& a, const Needed& b) {
        return a.entry.offset < b.entry.offset;
    });
    impl_->stats.planMs += MsSince(start) - (impl_->stats.scatterMs - scatterBefore);
    std::vector<unsigned char> rangeBytes;
    std::size_t index = 0;
    while(index < needed.size()) {
        if(cancelled()) return false;
        std::size_t end = index+1;
        const auto rangeStart = needed[index].entry.offset;
        auto rangeEnd = rangeStart + needed[index].entry.storedBytes;
        // Bound merged buffers, including during full-volume requests.
        while(end < needed.size() && needed[end].entry.offset == rangeEnd &&
              rangeEnd-rangeStart + needed[end].entry.storedBytes <= (8u<<20)) {
            rangeEnd += needed[end++].entry.storedBytes;
        }
        rangeBytes.resize(static_cast<std::size_t>(rangeEnd-rangeStart));
        const auto ioStart = std::chrono::steady_clock::now();
        impl_->file->clear();
        impl_->file->seekg(static_cast<std::streamoff>(rangeStart), std::ios::beg);
        impl_->file->read(reinterpret_cast<char*>(rangeBytes.data()), static_cast<std::streamsize>(rangeBytes.size()));
        impl_->stats.readMs += MsSince(ioStart);
        if(!*impl_->file) { errorMessage = "a paged range read failed"; return false; }
        ++impl_->stats.readCalls;
        ++impl_->stats.readRanges;
        impl_->stats.readBytes += rangeBytes.size();
        for(std::size_t k=index; k<end; ++k) {
            if(cancelled()) return false;
            const Needed& item = needed[k];
            const auto& entry = item.entry;
            const auto* payload = rangeBytes.data() + (entry.offset-rangeStart);
            const auto crcStart = std::chrono::steady_clock::now();
            const auto crc = Fnv1a(payload, entry.storedBytes);
            impl_->stats.checksumMs += MsSince(crcStart);
            if(crc != entry.crc) {
                ++impl_->stats.checksumFailures;
                errorMessage = "a paged page checksum failed (page " + std::to_string(item.page) + ")";
                return false;
            }
            const auto decodeStart = std::chrono::steady_clock::now();
            auto decoded = std::make_shared<std::vector<float>>(entry.rawBytes/4);
            if(entry.codec == kPagedCodecRaw) {
                std::memcpy(decoded->data(), payload, entry.rawBytes);
            } else {
#ifdef SEISMIC_HAVE_ZSTD
                const auto written = ZSTD_decompress(decoded->data(), entry.rawBytes, payload, entry.storedBytes);
                if(ZSTD_isError(written) || written != entry.rawBytes) {
                    errorMessage = "a paged zstd page failed to decompress";
                    return false;
                }
#else
                errorMessage = "this build has no zstd support for paged workspaces";
                return false;
#endif
            }
            impl_->stats.decodeMs += MsSince(decodeStart);
            if(impl_->pageCache) impl_->pageCache->Insert(cacheKeyFor(item.page), decoded);
            scatter(item, *decoded);
        }
        index = end;
    }
    // Cancellation may arrive during the final decode/scatter (or final cache
    // hit). Do not return a complete result without polling that last boundary.
    if(cancelled()) return false;
    impl_->stats.wallMs += MsSince(start);
    outputGuard.success = true;
    return true;
}

bool PagedWorkspaceReader::ReadTrace(int inlineIndex, int xlineIndex, std::vector<float>& out,
                                     std::string& errorMessage) {
    return ReadBox(inlineIndex, xlineIndex, 0, 1, 1, static_cast<int>(Info().samples), out, errorMessage);
}

bool PagedWorkspaceReader::ReadInlineSlice(int inlineIndex, std::vector<float>& out,
                                           std::string& errorMessage) {
    out.clear();
    const PagedWorkspaceInfo& info = Info();
    std::vector<float> box;
    if(!ReadBox(inlineIndex, info.xlineAxis.ValueAt(0), 0, 1, info.xlineAxis.count,
                static_cast<int>(info.samples), box, errorMessage)) {
        return false;
    }
    // Direct convention: row = samples-1-sample, column = xline index.
    out.assign(box.size(), std::numeric_limits<float>::quiet_NaN());
    for(int t = 0; t < static_cast<int>(info.samples); ++t) {
        for(int x = 0; x < info.xlineAxis.count; ++x) {
            out[static_cast<std::size_t>(info.samples - 1 - t) * info.xlineAxis.count + x] =
                box[static_cast<std::size_t>(t) * info.xlineAxis.count + x];
        }
    }
    return true;
}

bool PagedWorkspaceReader::ReadXlineSlice(int xlineIndex, std::vector<float>& out,
                                          std::string& errorMessage) {
    out.clear();
    const PagedWorkspaceInfo& info = Info();
    std::vector<float> box;
    if(!ReadBox(info.inlineAxis.ValueAt(0), xlineIndex, 0, info.inlineAxis.count, 1,
                static_cast<int>(info.samples), box, errorMessage)) {
        return false;
    }
    out.assign(box.size(), std::numeric_limits<float>::quiet_NaN());
    for(int t = 0; t < static_cast<int>(info.samples); ++t) {
        for(int i = 0; i < info.inlineAxis.count; ++i) {
            out[static_cast<std::size_t>(info.samples - 1 - t) * info.inlineAxis.count + i] =
                box[static_cast<std::size_t>(t) * info.inlineAxis.count + i];
        }
    }
    return true;
}

bool PagedWorkspaceReader::ReadTimeSlice(int sampleIndex, std::vector<float>& out,
                                         std::string& errorMessage, CancelToken* cancel) {
    const auto& info = Info();
    return ReadBox(info.inlineAxis.ValueAt(0), info.xlineAxis.ValueAt(0), sampleIndex,
                   info.inlineAxis.count, info.xlineAxis.count, 1, out, errorMessage, cancel, true);
}

// ------------------------------------------------------------ volume source

std::unique_ptr<PagedWorkspaceVolumeSource> PagedWorkspaceVolumeSource::Open(
    const std::filesystem::path& path, Status& status) {
    auto source = std::unique_ptr<PagedWorkspaceVolumeSource>(new PagedWorkspaceVolumeSource());
    std::string error;
    if(!source->reader_.Open(path, error)) {
        status = Status::Error(StatusCode::IoError, error);
        return nullptr;
    }
    const PagedWorkspaceInfo& info = source->reader_.Info();
    if(!info.complete) {
        status = Status::Error(StatusCode::NotIndexed,
                               "the paged workspace is incomplete and must not be opened as a workspace");
        return nullptr;
    }
    DatasetMetadata& metadata = source->metadata_;
    metadata.name = path.filename().u8string();
    metadata.path = path;
    metadata.sampleCount = static_cast<int>(info.samples);
    metadata.sampleIntervalUs = static_cast<int>(info.sampleIntervalUs);
    metadata.traceCount = static_cast<std::int64_t>(info.inlineAxis.count) * info.xlineAxis.count;
    metadata.inlineAxis = info.inlineAxis;
    metadata.xlineAxis = info.xlineAxis;
    metadata.inlineMin = info.inlineAxis.count > 0 ? info.inlineAxis.ValueAt(0) : 0;
    metadata.inlineMax = info.inlineAxis.count > 0 ? info.inlineAxis.ValueAt(info.inlineAxis.count - 1) : 0;
    metadata.xlineMin = info.xlineAxis.count > 0 ? info.xlineAxis.ValueAt(0) : 0;
    metadata.xlineMax = info.xlineAxis.count > 0 ? info.xlineAxis.ValueAt(info.xlineAxis.count - 1) : 0;
    metadata.ruleBased = false;
    metadata.indexComplete = true;
    status = Status::Ok();
    return source;
}

const DatasetMetadata& PagedWorkspaceVolumeSource::Metadata() const {
    return metadata_;
}

Status PagedWorkspaceVolumeSource::ReadTrace(int inlineNo, int xlineNo, TraceData& out,
                                             CancelToken* cancel) {
    if(Cancelled(cancel)) {
        return Status::Error(StatusCode::Cancelled, "request cancelled");
    }
    std::vector<float> samples;
    std::string error;
    if(!reader_.ReadTrace(inlineNo, xlineNo, samples, error)) {
        return Status::Error(error.find("is not present") != std::string::npos
                                 ? StatusCode::NotFound
                                 : StatusCode::IoError,
                             error);
    }
    out.samples = std::move(samples);
    out.sampleCount = static_cast<int>(out.samples.size());
    ++stats_.requests;
    stats_.tracesRead += 1;
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel,
                                              int maxColumns) {
    const PagedWorkspaceInfo& info = reader_.Info();
    int index = 0;
    if(!info.inlineAxis.ExactIndexOf(inlineNo, index)) {
        ++stats_.requests;
        return Status::Error(StatusCode::NotFound, "inline " + std::to_string(inlineNo) + " is not present");
    }
    if(Cancelled(cancel)) {
        return Status::Error(StatusCode::Cancelled, "request cancelled");
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> values;
    std::string error;
    if(!reader_.ReadInlineSlice(inlineNo, values, error)) {
        return Status::Error(error.find("is not present") != std::string::npos
                                 ? StatusCode::NotFound
                                 : StatusCode::IoError,
                             error);
    }
    const int width = info.xlineAxis.count;
    int displayWidth = width;
    const int height = static_cast<int>(info.samples);
    if(maxColumns > 0 && width > maxColumns) {
        // Pack sampled columns into a dense image. Holes in a full-width
        // image are missing data, not downsampling, and render as stripes.
        displayWidth = maxColumns;
        std::vector<float> reduced(static_cast<std::size_t>(displayWidth) * height);
        for(int x = 0; x < displayWidth; ++x) {
            const int sourceX = static_cast<int>(static_cast<long long>(x) * (width - 1) /
                                                std::max(1, displayWidth - 1));
            for(int row = 0; row < height; ++row) {
                reduced[static_cast<std::size_t>(row) * displayWidth + x] =
                    values[static_cast<std::size_t>(row) * width + sourceX];
            }
        }
        values.swap(reduced);
        out.columnsRead = displayWidth;
        out.totalColumns = width;
    } else {
        out.columnsRead = width;
        out.totalColumns = width;
    }
    out.width = displayWidth;
    out.height = height;
    out.values = std::move(values);
    for(float value : out.values) {
        if(std::isnan(value)) {
            continue;
        }
        out.valueMin = std::min(out.valueMin, value);
        out.valueMax = std::max(out.valueMax, value);
    }
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
    stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel,
                                                 int maxColumns) {
    const PagedWorkspaceInfo& info = reader_.Info();
    int index = 0;
    if(!info.xlineAxis.ExactIndexOf(xlineNo, index)) {
        ++stats_.requests;
        return Status::Error(StatusCode::NotFound, "xline " + std::to_string(xlineNo) + " is not present");
    }
    if(Cancelled(cancel)) {
        return Status::Error(StatusCode::Cancelled, "request cancelled");
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> values;
    std::string error;
    if(!reader_.ReadXlineSlice(xlineNo, values, error)) {
        return Status::Error(error.find("is not present") != std::string::npos
                                 ? StatusCode::NotFound
                                 : StatusCode::IoError,
                             error);
    }
    const int width = info.inlineAxis.count;
    int displayWidth = width;
    const int height = static_cast<int>(info.samples);
    if(maxColumns > 0 && width > maxColumns) {
        displayWidth = maxColumns;
        std::vector<float> reduced(static_cast<std::size_t>(displayWidth) * height);
        for(int i = 0; i < displayWidth; ++i) {
            const int sourceI = static_cast<int>(static_cast<long long>(i) * (width - 1) /
                                                std::max(1, displayWidth - 1));
            for(int row = 0; row < height; ++row) {
                reduced[static_cast<std::size_t>(row) * displayWidth + i] =
                    values[static_cast<std::size_t>(row) * width + sourceI];
            }
        }
        values.swap(reduced);
        out.columnsRead = displayWidth;
        out.totalColumns = width;
    } else {
        out.columnsRead = width;
        out.totalColumns = width;
    }
    out.width = displayWidth;
    out.height = height;
    out.values = std::move(values);
    for(float value : out.values) {
        if(std::isnan(value)) {
            continue;
        }
        out.valueMin = std::min(out.valueMin, value);
        out.valueMax = std::max(out.valueMax, value);
    }
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
    stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) {
    return ReadTimeSlice(sampleIndex, out, cancel, {});
}

Status PagedWorkspaceVolumeSource::ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel,
                                                 const ProgressFn& progress) {
    const PagedWorkspaceInfo& info = reader_.Info();
    if(sampleIndex < 0 || sampleIndex >= static_cast<int>(info.samples)) {
        ++stats_.requests;
        return Status::Error(StatusCode::NotFound, "sample is outside the volume");
    }
    if(Cancelled(cancel)) {
        return Status::Error(StatusCode::Cancelled, "request cancelled");
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> values;
    std::string error;
    if(!reader_.ReadTimeSlice(sampleIndex, values, error, cancel)) {
        out = {};
        return Status::Error(Cancelled(cancel) ? StatusCode::Cancelled : StatusCode::IoError, error);
    }
    out.width = info.xlineAxis.count;
    out.height = info.inlineAxis.count;
    out.values = std::move(values);
    out.columnsRead = out.width;
    out.totalColumns = out.width;
    for(float value : out.values) {
        if(std::isnan(value)) {
            continue;
        }
        out.valueMin = std::min(out.valueMin, value);
        out.valueMax = std::max(out.valueMax, value);
    }
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
    stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    (void)progress;
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadTimeSliceTiled(int sampleIndex, int tileSize, int focusInline, int focusXline,
    const TimeTileCallback& publish, Slice2D& out, CancelToken* cancel) {
    out = {};
    const auto& info = reader_.Info();
    if(sampleIndex < 0 || sampleIndex >= static_cast<int>(info.samples) || tileSize < 8 || tileSize > 1024)
        return Status::Error(StatusCode::InvalidArgument, "invalid tiled time request");
    if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled, "request cancelled");
    const auto start = std::chrono::steady_clock::now();
    const auto before = reader_.ReaderStats();
    const int width = info.xlineAxis.count, height = info.inlineAxis.count;
    if(TimePlaneCache::Read(timeCachePath_, reader_.CacheSourceKey(), width,height,sampleIndex,out,cancel)) {
        std::cout << "[TIME-CACHE] source hit sample=" << sampleIndex << " ms=" << MsSince(start) << std::endl;
        return Status::Ok();
    }
    int fi = height/2, fx = width/2;
    int exact = 0;
    if(info.inlineAxis.ExactIndexOf(focusInline, exact)) fi = exact;
    if(info.xlineAxis.ExactIndexOf(focusXline, exact)) fx = exact;
    const int fy = height-1-fi;
    struct Tile { int x, y, w, h; };
    std::vector<Tile> tiles;
    for(int y=0; y<height; y+=tileSize) for(int x=0; x<width; x+=tileSize)
        tiles.push_back({x,y,std::min(tileSize,width-x),std::min(tileSize,height-y)});
    std::stable_sort(tiles.begin(), tiles.end(), [fx,fy](const Tile& a, const Tile& b) {
        auto distance = [fx,fy](const Tile& t) {
            const long long dx = fx-std::clamp(fx,t.x,t.x+t.w-1);
            const long long dy = fy-std::clamp(fy,t.y,t.y+t.h-1);
            return dx*dx+dy*dy;
        };
        return distance(a)<distance(b);
    });
    Slice2D full;
    full.width=width; full.height=height;
    full.valueMin=std::numeric_limits<float>::max();
    full.valueMax=std::numeric_limits<float>::lowest();
    const std::size_t pixels=static_cast<std::size_t>(width)*height;
    const int pageSpan=static_cast<int>(info.pageSamples);
    const int pageBegin=(sampleIndex/pageSpan)*pageSpan;
    int batchCount=timeCachePath_.empty() ? 0 : std::min(4,std::min(pageSpan,static_cast<int>(info.samples)-pageBegin));
    if(pixels>TimePlaneCache::Budget/sizeof(float)/std::max(1,batchCount)) batchCount=0;
    const int batchBegin=batchCount ? std::clamp(sampleIndex-1,pageBegin,
        std::min(pageBegin+pageSpan,static_cast<int>(info.samples))-batchCount) : sampleIndex;
    std::vector<std::vector<float>> planes;
    if(batchCount) {
        planes.resize(batchCount);
        for(auto& p:planes) p.assign(pixels,std::numeric_limits<float>::quiet_NaN());
    } else full.values.assign(pixels,std::numeric_limits<float>::quiet_NaN());
    int completed=0;
    double publishMs=0;
    for(const auto& box : tiles) {
        if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled, "request cancelled");
        Slice2D tile;
        tile.width=box.w; tile.height=box.h;
        std::string error;
        if(batchCount) {
            std::vector<float> slab;
            if(!reader_.ReadBox(info.inlineAxis.ValueAt(height-box.y-box.h),info.xlineAxis.ValueAt(box.x),
                batchBegin,box.h,box.w,batchCount,slab,error,cancel))
                return Status::Error(Cancelled(cancel)?StatusCode::Cancelled:StatusCode::IoError,error);
            tile.values.resize(static_cast<std::size_t>(box.w)*box.h);
            for(int t=0;t<batchCount;++t) for(int y=0;y<box.h;++y) for(int x=0;x<box.w;++x) {
                const float v=slab[(static_cast<std::size_t>(t)*box.w+x)*box.h+box.h-1-y];
                planes[t][static_cast<std::size_t>(box.y+y)*width+box.x+x]=v;
                if(batchBegin+t==sampleIndex) tile.values[static_cast<std::size_t>(y)*box.w+x]=v;
            }
        } else {
            if(!reader_.ReadBox(info.inlineAxis.ValueAt(height-box.y-box.h),info.xlineAxis.ValueAt(box.x),
                sampleIndex,box.h,box.w,1,tile.values,error,cancel,true))
                return Status::Error(Cancelled(cancel)?StatusCode::Cancelled:StatusCode::IoError,error);
            for(int y=0;y<box.h;++y)
                std::copy_n(tile.values.data()+static_cast<std::size_t>(y)*box.w,box.w,
                    full.values.data()+static_cast<std::size_t>(box.y+y)*width+box.x);
        }
        for(float v:tile.values) if(std::isfinite(v)) {
            full.valueMin=std::min(full.valueMin,v); full.valueMax=std::max(full.valueMax,v);
        }
        ++completed;
        const auto publishStart=std::chrono::steady_clock::now();
        if(publish && !publish(box.x,box.y,std::move(tile),completed,static_cast<int>(tiles.size())))
            return Status::Error(StatusCode::Cancelled,"tile publication cancelled");
        publishMs+=MsSince(publishStart);
    }
    if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled,"request cancelled");
    if(batchCount) {
        const auto cacheStart=std::chrono::steady_clock::now();
        const bool saved=TimePlaneCache::Write(timeCachePath_,reader_.CacheSourceKey(),
            width,height,batchBegin,planes,cancel);
        std::cout << "[TIME-CACHE] batch begin=" << batchBegin << " count=" << batchCount
                  << " bytes=" << pixels*4*batchCount << " saved=" << saved
                  << " write_ms=" << MsSince(cacheStart) << std::endl;
        if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled,"cache publication cancelled");
        full.values=std::move(planes[sampleIndex-batchBegin]);
    }
    if(full.valueMin > full.valueMax) { full.valueMin=0.0f; full.valueMax=1.0f; }
    full.columnsRead=full.totalColumns=width;
    out=std::move(full);
    ++stats_.requests;
    const auto after=reader_.ReaderStats();
    stats_.bytesRead+=after.readBytes-before.readBytes;
    std::cout << "[TIME-L0] tiles=" << completed << " wall_ms=" << MsSince(start)
              << " plan_ms=" << after.planMs-before.planMs << " read_ms=" << after.readMs-before.readMs
              << " crc_ms=" << after.checksumMs-before.checksumMs << " decode_ms=" << after.decodeMs-before.decodeMs
              << " scatter_ms=" << after.scatterMs-before.scatterMs << " publish_ms=" << publishMs
              << " read_calls=" << after.readCalls-before.readCalls << " bytes=" << after.readBytes-before.readBytes
              << " hits=" << after.cacheHits-before.cacheHits << " misses=" << after.cacheMisses-before.cacheMisses << std::endl;
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadTimeSliceWindowed(int sampleIndex, Slice2D& out, CancelToken* cancel) {
    out = {};
    const auto& info = reader_.Info();
    if(sampleIndex < 0 || sampleIndex >= static_cast<int>(info.samples))
        return Status::Error(StatusCode::NotFound, "sample is outside the volume");
    if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled, "request cancelled");
    const std::size_t pixels = static_cast<std::size_t>(info.inlineAxis.count) * info.xlineAxis.count;
    const int span = static_cast<int>(std::max<std::uint32_t>(1, std::min<std::uint32_t>(16, info.pageSamples)));
    // Never expand full-resolution L0/L1 into a giant in-memory slab.
    if(pixels == 0 || pixels > (16u * 1024u * 1024u) / sizeof(float) / span)
        return Status::Error(StatusCode::Unsupported, "time preview window exceeds 16 MiB budget");
    const auto start = std::chrono::steady_clock::now();
    const int begin = (sampleIndex / span) * span;
    const int count = std::min(span, static_cast<int>(info.samples) - begin);
    if(timeWindowSourceKey_ != reader_.CacheSourceKey() || timeWindowBegin_ != begin ||
       timeWindowCount_ != count || timeWindow_.empty()) {
        ClearTimeWindow();
        std::vector<float> box;
        std::string error;
        if(!reader_.ReadBox(info.inlineAxis.ValueAt(0), info.xlineAxis.ValueAt(0), begin,
                           info.inlineAxis.count, info.xlineAxis.count, count, box, error, cancel))
            return Status::Error(Cancelled(cancel) ? StatusCode::Cancelled : StatusCode::IoError, error);
        if(Cancelled(cancel)) return Status::Error(StatusCode::Cancelled, "request cancelled");
        timeWindow_ = std::move(box);
        timeWindowBegin_ = begin;
        timeWindowCount_ = count;
        timeWindowSourceKey_ = reader_.CacheSourceKey();
    }
    out.width = info.xlineAxis.count;
    out.height = info.inlineAxis.count;
    out.values.resize(pixels);
    const std::size_t offset = static_cast<std::size_t>(sampleIndex - begin) * pixels;
    for(int i = 0; i < out.height; ++i) {
        if(Cancelled(cancel)) { out = {}; return Status::Error(StatusCode::Cancelled, "request cancelled"); }
        for(int x = 0; x < out.width; ++x) {
            const float value = timeWindow_[offset + static_cast<std::size_t>(x) * out.height + i];
            out.values[static_cast<std::size_t>(out.height - 1 - i) * out.width + x] = value;
            if(!std::isnan(value)) { out.valueMin = std::min(out.valueMin, value); out.valueMax = std::max(out.valueMax, value); }
        }
    }
    out.columnsRead = out.totalColumns = out.width;
    ++stats_.requests;
    stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadVoxelWindow(const VoxelWindowRequest& request,
                                                   VoxelWindow& out, CancelToken* cancel) {
    if(Cancelled(cancel)) {
        return Status::Error(StatusCode::Cancelled, "request cancelled");
    }
    const PagedWorkspaceInfo& info = reader_.Info();
    int iIndex = 0;
    int xIndex = 0;
    if(!info.inlineAxis.ExactIndexOf(request.inlineBegin, iIndex) ||
       !info.xlineAxis.ExactIndexOf(request.xlineBegin, xIndex)) {
        ++stats_.requests;
        return Status::Error(StatusCode::NotFound, "the voxel window origin is not on the axes");
    }
    std::vector<float> box;
    std::string error;
    if(!reader_.ReadBox(request.inlineBegin, request.xlineBegin, request.sampleBegin,
                        request.inlineCount, request.xlineCount, request.sampleCount, box, error)) {
        return Status::Error(StatusCode::IoError, error);
    }
    out.box = request;
    // Box layout: ((il) * xlineCount + xl) * sampleCount + s (t ascending).
    out.values.assign(box.size(), std::numeric_limits<float>::quiet_NaN());
    for(int i = 0; i < request.inlineCount; ++i) {
        for(int x = 0; x < request.xlineCount; ++x) {
            for(int s = 0; s < request.sampleCount; ++s) {
                out.values[(static_cast<std::size_t>(i) * request.xlineCount + x) * request.sampleCount + s] =
                    box[(static_cast<std::size_t>(s) * request.xlineCount + x) * request.inlineCount + i];
            }
        }
    }
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(request.inlineCount) * request.xlineCount;
    return Status::Ok();
}

Status PagedWorkspaceVolumeSource::ReadArbitrarySection(const SectionRequest& request, Slice2D& out,
                                                        CancelToken* cancel) {
    if(request.pathPoints.size() < 2) {
        return Status::Error(StatusCode::InvalidArgument, "a section needs at least two path points");
    }
    const PagedWorkspaceInfo& info = reader_.Info();
    const int maxColumns = std::max(1, request.maxColumns);
    struct AxisPoint { int i = 0; int x = 0; };
    std::vector<AxisPoint> path;
    path.reserve(request.pathPoints.size());
    for(const PathPoint& point : request.pathPoints) {
        const int inlineFirst = info.inlineAxis.ValueAt(0);
        const int inlineLast = info.inlineAxis.ValueAt(info.inlineAxis.count - 1);
        const int xlineFirst = info.xlineAxis.ValueAt(0);
        const int xlineLast = info.xlineAxis.ValueAt(info.xlineAxis.count - 1);
        if(point.inlineNo < std::min(inlineFirst, inlineLast) ||
           point.inlineNo > std::max(inlineFirst, inlineLast) ||
           point.xlineNo < std::min(xlineFirst, xlineLast) ||
           point.xlineNo > std::max(xlineFirst, xlineLast)) {
            return Status::Error(StatusCode::NotFound, "a section point is outside the workspace axes");
        }
        AxisPoint mapped;
        if(!info.inlineAxis.NearestIndexOf(point.inlineNo, mapped.i) ||
           !info.xlineAxis.NearestIndexOf(point.xlineNo, mapped.x)) {
            return Status::Error(StatusCode::NotFound, "a section point is outside the workspace axes");
        }
        if(path.empty() || path.back().i != mapped.i || path.back().x != mapped.x) {
            path.push_back(mapped);
        }
    }
    if(path.size() < 2) {
        return Status::Error(StatusCode::InvalidArgument, "the section collapses to one workspace trace");
    }
    std::vector<double> segmentLengths(path.size() - 1, 0.0);
    double totalLength = 0.0;
    for(std::size_t k = 1; k < path.size(); ++k) {
        const double di = static_cast<double>(path[k].i - path[k - 1].i);
        const double dx = static_cast<double>(path[k].x - path[k - 1].x);
        segmentLengths[k - 1] = std::sqrt(di * di + dx * dx);
        totalLength += segmentLengths[k - 1];
    }
    if(totalLength <= 0.0) {
        return Status::Error(StatusCode::InvalidArgument, "the section path has zero length");
    }
    const int columns = std::max(2, std::min(maxColumns,
        static_cast<int>(std::ceil(totalLength)) + 1));
    const int height = static_cast<int>(info.samples);

    out.width = columns;
    out.height = height;
    out.values.assign(static_cast<std::size_t>(columns) * height,
                      std::numeric_limits<float>::quiet_NaN());
    out.distances.assign(static_cast<std::size_t>(columns), 0.0f);
    out.inlineNos.assign(static_cast<std::size_t>(columns), 0.0f);
    out.xlineNos.assign(static_cast<std::size_t>(columns), 0.0f);
    out.traceIndices.assign(static_cast<std::size_t>(columns), -1);
    out.validMask.assign(static_cast<std::size_t>(columns), 0);

    std::vector<float> trace;
    std::string error;
    int validColumns = 0;
    std::size_t segment = 0;
    double segmentStart = 0.0;
    for(int column = 0; column < columns; ++column) {
        if(Cancelled(cancel)) {
            return Status::Error(StatusCode::Cancelled, "request cancelled");
        }
        const double target = static_cast<double>(column) * totalLength /
                              static_cast<double>(columns - 1);
        while(segment + 1 < segmentLengths.size() &&
              target > segmentStart + segmentLengths[segment]) {
            segmentStart += segmentLengths[segment];
            ++segment;
        }
        const double local = segmentLengths[segment] > 0.0
            ? std::clamp((target - segmentStart) / segmentLengths[segment], 0.0, 1.0)
            : 0.0;
        const int inlineAxisIndex = std::clamp(
            static_cast<int>(std::llround(path[segment].i +
                local * (path[segment + 1].i - path[segment].i))), 0, info.inlineAxis.count - 1);
        const int xlineAxisIndex = std::clamp(
            static_cast<int>(std::llround(path[segment].x +
                local * (path[segment + 1].x - path[segment].x))), 0, info.xlineAxis.count - 1);
        const int inlineNo = info.inlineAxis.ValueAt(inlineAxisIndex);
        const int xlineNo = info.xlineAxis.ValueAt(xlineAxisIndex);
        out.inlineNos[static_cast<std::size_t>(column)] = static_cast<float>(inlineNo);
        out.xlineNos[static_cast<std::size_t>(column)] = static_cast<float>(xlineNo);
        out.distances[static_cast<std::size_t>(column)] = static_cast<float>(target);
        if(!reader_.ReadTrace(inlineNo, xlineNo, trace, error)) {
            // Missing coordinates stay invalid; they are never snapped.
            continue;
        }
        out.validMask[static_cast<std::size_t>(column)] = 1;
        out.traceIndices[static_cast<std::size_t>(column)] = column;
        ++validColumns;
        for(int s = 0; s < height && s < static_cast<int>(trace.size()); ++s) {
            out.values[static_cast<std::size_t>(height - 1 - s) * columns + column] =
                trace[static_cast<std::size_t>(s)];
        }
    }
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(validColumns);
    out.columnsRead = validColumns;
    out.totalColumns = columns;
    out.sectionTracesRead = validColumns;
    out.sectionMissingColumns = columns - validColumns;
    if(validColumns == 0) {
        return Status::Error(StatusCode::NotFound, "no trace of the requested section exists");
    }
    return Status::Ok();
}

SourceStatistics PagedWorkspaceVolumeSource::Statistics() const {
    return stats_;
}

void PagedWorkspaceVolumeSource::ResetStatistics() {
    stats_ = SourceStatistics{};
    reader_.ResetReaderStats();
}

// ------------------------------------------------- builder from a workspace

bool BuildPagedWorkspaceFromWorkspace(const std::filesystem::path& sourceBase,
                                      const std::filesystem::path& targetFile,
                                      const PagedWorkspaceInfo& infoTemplate,
                                      std::string& errorMessage,
                                      std::uint64_t& sourceChunksRead,
                                      PagedWorkspaceInfo* outInfo) {
    WorkspaceReader source;
    if(!source.Open(sourceBase, errorMessage)) {
        return false;
    }
    const WorkspaceInfo& src = source.Info();
    if(src.codec != kCodecRaw) {
        errorMessage = "the paged builder needs a raw source workspace";
        return false;
    }
    PagedWorkspaceInfo info = infoTemplate;
    info.samples = src.samples;
    info.sampleIntervalUs = src.sampleIntervalUs;
    info.inlineAxis = AxisDescriptor::Uniform(src.inlineMin, 1, static_cast<int>(src.inlines));
    info.xlineAxis = AxisDescriptor::Uniform(src.xlineMin, 1, static_cast<int>(src.xlines));
    info.sourceSize = src.sourceIdentityHash;
    info.coverageInlineMin = info.inlineAxis.ValueAt(0);
    info.coverageInlineMax = info.inlineAxis.ValueAt(info.inlineAxis.count - 1);
    info.coverageXlineMin = info.xlineAxis.ValueAt(0);
    info.coverageXlineMax = info.xlineAxis.ValueAt(info.xlineAxis.count - 1);

    PagedWorkspaceWriter writer;
    if(!writer.Open(targetFile, info, false, errorMessage)) {
        return false;
    }
    const std::uint32_t srcS = src.chunkSamples;
    const std::uint32_t srcI = src.chunkInlines;
    const std::uint32_t srcX = src.chunkXlines;
    struct CacheEntry {
        std::uint64_t key = 0;
        std::vector<float> values;
    };
    std::deque<CacheEntry> cache;
    sourceChunksRead = 0;
    auto fetchChunk = [&](std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) -> const float* {
        const std::uint64_t key = (static_cast<std::uint64_t>(cs) * src.ChunksI() + ci) * src.ChunksX() + cx;
        for(const CacheEntry& entry : cache) {
            if(entry.key == key) {
                return entry.values.data();
            }
        }
        CacheEntry entry;
        entry.key = key;
        if(!source.ReadChunk(cs, ci, cx, entry.values, errorMessage)) {
            return nullptr;
        }
        ++sourceChunksRead;
        cache.push_back(std::move(entry));
        if(cache.size() > 64) {
            cache.pop_front();
        }
        return cache.back().values.data();
    };

    std::vector<float> chunkValues(static_cast<std::size_t>(info.chunkInlines) * info.chunkXlines *
                                   info.chunkSamples);
    for(std::uint32_t ct = 0; ct < info.ChunksT(); ++ct) {
        for(std::uint32_t ci = 0; ci < info.ChunksI(); ++ci) {
            const std::uint32_t csi = (ci * info.chunkInlines) / srcI;
            for(std::uint32_t cx = 0; cx < info.ChunksX(); ++cx) {
                std::fill(chunkValues.begin(), chunkValues.end(),
                          std::numeric_limits<float>::quiet_NaN());
                const std::uint32_t baseCs = (ct * info.chunkSamples) / srcS;
                const std::uint32_t baseCsx = (cx * info.chunkXlines) / srcX;
                const std::uint32_t spanCs =
                    ((ct * info.chunkSamples + info.chunkSamples - 1) / srcS) - baseCs + 1;
                const std::uint32_t spanCsx =
                    ((cx * info.chunkXlines + info.chunkXlines - 1) / srcX) - baseCsx + 1;
                std::vector<const float*> rowPtrs(static_cast<std::size_t>(spanCs) * spanCsx, nullptr);
                for(std::uint32_t a = 0; a < spanCs; ++a) {
                    for(std::uint32_t b = 0; b < spanCsx; ++b) {
                        const std::uint32_t cs = baseCs + a;
                        const std::uint32_t csx = baseCsx + b;
                        if(cs >= src.ChunksS() || csx >= src.ChunksX()) {
                            continue;
                        }
                        const float* values = fetchChunk(cs, csi, csx);
                        if(values == nullptr) {
                            return false;
                        }
                        rowPtrs[static_cast<std::size_t>(a) * spanCsx + b] = values;
                    }
                }
                for(std::uint32_t t = 0; t < info.chunkSamples; ++t) {
                    const std::uint64_t vT = static_cast<std::uint64_t>(ct) * info.chunkSamples + t;
                    if(vT >= src.samples) {
                        break;
                    }
                    const std::uint32_t cs = static_cast<std::uint32_t>(vT / srcS);
                    const std::uint32_t s = static_cast<std::uint32_t>(vT % srcS);
                    for(std::uint32_t x = 0; x < info.chunkXlines; ++x) {
                        const std::uint64_t vX = static_cast<std::uint64_t>(cx) * info.chunkXlines + x;
                        if(vX >= src.xlines) {
                            break;
                        }
                        const std::uint32_t csx = static_cast<std::uint32_t>(vX / srcX);
                        const std::uint32_t lx = static_cast<std::uint32_t>(vX % srcX);
                        const float* srcChunk = rowPtrs[
                            static_cast<std::size_t>(cs - baseCs) * spanCsx + (csx - baseCsx)];
                        if(srcChunk == nullptr) {
                            continue;
                        }
                        for(std::uint32_t i = 0; i < info.chunkInlines; ++i) {
                            const std::uint64_t vI = static_cast<std::uint64_t>(ci) * info.chunkInlines + i;
                            if(vI >= src.inlines) {
                                break;
                            }
                            const std::uint32_t li = static_cast<std::uint32_t>(vI % srcI);
                            chunkValues[(static_cast<std::size_t>(t) * info.chunkXlines + x) *
                                            info.chunkInlines + i] =
                                srcChunk[(static_cast<std::size_t>(s) * srcI + li) * srcX + lx];
                        }
                    }
                }
                if(!writer.WriteChunk(ci, cx, ct, chunkValues.data(), errorMessage)) {
                    return false;
                }
            }
        }
    }
    if(outInfo != nullptr) {
        *outInfo = writer.Info();
    }
    if(!writer.Finalize(errorMessage)) {
        return false;
    }
    if(outInfo != nullptr) {
        outInfo->complete = true;
    }
    return true;
}

} // namespace engine
} // namespace seismic
