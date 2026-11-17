#pragma once
#include "metadata_read.h"
#include "model_changes.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace paleo::data {
inline constexpr char NumericTableContract[]="numeric_table.v1";
inline constexpr int NumericTableMaxColumns=15,NumericTableMaxRows=8192; // Plus the source_row axis.
enum class NumericTableDtype { Float64, Boolean };
struct NumericTableColumn {
    std::string id,label,unit; // unit="1" explicitly declares dimensionless values.
    int sourceColumn=0; // One-based physical source column; never inferred from a label.
    NumericTableDtype dtype=NumericTableDtype::Float64;
    bool missingIsNull=true;
    std::vector<std::string> nullTexts; // Exact source text, checked before numeric conversion.
    bool operator==(const NumericTableColumn&)const=default;
};
struct NumericTableProjection {
    std::int64_t tableId=0,sourceId=0,firstSourceRow=1,rowCount=0;
    std::string sourceSha256;
    std::vector<NumericTableColumn> columns;
    bool operator==(const NumericTableProjection&)const=default;
};
// An observed SQLite read snapshot, not a lease promising historical values.
struct NumericTableReadCut {
    ModelChangeHead model;
    std::int64_t tableId=0,sourceId=0;
    std::string sourceSha256,projectionRevision;
};
struct NumericTableDescriptor {
    NumericTableProjection projection;
    std::string revision;
    NumericTableReadCut cut;
};
struct NumericTableReadOptions {
    MetadataReadOptions metadata;
    std::int64_t maxReadBytes=8*1024*1024,maxOutputBytes=2*1024*1024,maxWorkingBytes=4*1024*1024;
    std::int64_t maxCellBytes=256;
    std::function<bool()> cancelled;
};
struct NumericTableReadStats {
    std::int64_t metadataBytesRead=0,cellTextBytesRead=0,cellsRead=0,rowsReturned=0;
    std::int64_t outputBytes=0,reservedBytes=0;
    // Counts SQLite payload delivered to C++, not physical disk I/O or process RSS.
};
struct NumericTableWindowQuery {
    NumericTableReadCut expected;
    std::vector<std::string> columnIds;
    std::int64_t firstRow=0,rowCount=0; // Zero-based offset in the declared projection.
};
struct NumericTableColumnValues {
    NumericTableColumn column;
    std::vector<double> numbers;
    std::vector<unsigned char> booleans;
    std::vector<unsigned char> nullMask; // 1=null, 0=valid; null payload is canonical zero.
};
struct NumericTableWindow {
    NumericTableDescriptor descriptor;
    std::int64_t firstRow=0;
    std::vector<std::int64_t> sourceRows; // One-based physical rows, stable and never compacted.
    std::vector<NumericTableColumnValues> columns;
};
}
