#pragma once
#include "model.h"
#include <functional>

namespace paleo::data {
struct CurveCleaningRule {
    std::string curveType, unit;
    std::vector<double> nullValues;
    bool checkMinimum = false;
    double minimum = 0;
    bool checkMaximum = false;
    double maximum = 0;
    bool replaceInvalidWithMissing = false;
};
struct ParseOptions {
    std::string wellNameOverride;
    std::string coordinateSystem, coordinateUnit, depthUnit;
    std::string coordinateOrder; // "xy" / "yx"；空表示未知，不猜测
    std::vector<CurveCleaningRule> cleaningRules;
    // 服务重导入副本时自动填写原始身份路径，仅供井名/目录语义和相对附件定位。
    // 文件内容仍从 parseFile(path, ...) 的 path 读取；普通调用者可留空。
    std::string sourceContextPath;
    bool comprehensiveWellMetreDepth=false; // Host-confirmed exporter contract; generic tables remain unknown.
};
struct ParseIssue {
    std::string code, message, sheet, originalText;
    int row = 0;
    bool blocking = false;
};
struct SourceCell {
    int row = 0, column = 0; // 1-based physical position
    std::string text;
};
struct SourceTable {
    std::string sheetName;
    int rowCount = 0, columnCount = 0;
    std::vector<SourceCell> cells; // sparse, preserves original row/column and headers
};
struct CoordinateSystemDeclaration {
    std::string coordinateSystem, coordinateUnit;
};
struct ParsedFile {
    std::string path, kind, status; // parsed / review / registered / ignored / failed
    std::string message, documentText;
    std::vector<double> declaredNullValues; // 文件明确声明的空值；优先于默认及配置空值
    std::vector<CoordinateSystemDeclaration> coordinateSystems; // Explicit source declarations only; never inferred from numeric ranges.
    std::vector<WellDataset> wells;
    std::vector<TimeDepthTable> unassignedTimeDepth; // 已解析但井名未确认，保留完整数值，不创建占位井
    std::vector<SeismicHorizon> horizons;
    std::vector<SeismicVolume> volumes; // metadata only; traces always empty
    std::vector<GeologicalMap> maps;
    std::vector<SourceTable> sourceTables;
    std::vector<ParseIssue> issues;
};
using CancelCheck = std::function<bool()>;
// Parsing layer: no SQL and no UI, all strings UTF-8. Original file is never changed.
ParsedFile parseFile(const std::string& path, const ParseOptions& options = {}, const CancelCheck& cancelled = {});
ParsedFile parseTextFile(const std::string& path, const ParseOptions& options, const CancelCheck& cancelled);
ParsedFile parseAuxiliaryFile(const std::string& path, const ParseOptions& options, const CancelCheck& cancelled);
std::string canonicalWellName(const std::string& text);
}
