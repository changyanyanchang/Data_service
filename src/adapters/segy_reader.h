#pragma once
#include "paleo_data/seismic.h"
#include <functional>
#include <memory>

// Internal SEG-Y access: no SQL, no GUI and no source-file writes.
namespace paleo::data::segy_detail {
using Cancel = std::function<bool()>;

struct BatchReadOptions {
    std::string ioMode="auto"; // auto / buffered / mapped / legacy.
    int bufferMiB=8;          // 每个读取器的有界读取/映射窗口，1–32 MiB。
    bool collectTimings=false;
};
struct TraceWindowBatch {
    std::vector<long long> traceIndexes;
    std::vector<int> inlineNumbers,crosslineNumbers;
    std::vector<double> x,y;
    std::vector<float> amplitudes; // 输入道序优先：trace * sampleCount + sample。
    int sampleCount=0;
};

class Reader {
public:
    Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool open(const std::string& path, std::string& error, const Cancel& cancel = {}, bool geometry = true);
    const SeismicFileInfo& info() const;
    const std::vector<int>& inlines() const;
    const std::vector<int>& crosslines() const;
    long long traceIndex(int inlineNumber, int crosslineNumber) const;
    bool readTrace(long long traceIndex, int firstSample, int sampleCount,
                   SeismicTraceResult& result, std::string& error, const Cancel& cancel = {},
                   SeismicReadStatistics* statistics=nullptr, bool collectTimings=false);
    bool readTraceBatch(const std::vector<long long>& ordinals, int firstSample, int sampleCount,
                        const BatchReadOptions& options, TraceWindowBatch& result,
                        SeismicReadStatistics& statistics, std::string& error, const Cancel& cancel = {});
    bool hasCoordinateMap() const;
    double maxCoordinateResidual() const;
    // 拟合统计：参与拟合的道头数与RMS残差（坐标单位）。未拟合时为 0 / NaN。
    int coordinateSampleCount() const;
    double coordinateRmsResidual() const;
    bool mapXY(double x, double y, double& il, double& xl) const;
    bool mapLine(double il, double xl, double& x, double& y) const;
    bool covers(double x, double y) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Used by the file parser and public inspection API. The large amplitude payload stays on disk.
bool inspectSegyFile(const std::string& path, SeismicFileInfo& result, std::string& error,
                     const Cancel& cancel = {});
}
