#pragma once
#include <string>
#include <functional>
namespace paleo::data {
enum class DataObjectKind { TimeDepth, IntervalTrack, SeismicVolume };
// Metadata only. No relation points, interval rows or seismic samples are read.
struct DataObjectDescriptor {
    DataObjectKind kind=DataObjectKind::TimeDepth;
    long long id=0,sourceId=0,wellId=0,parentId=0;
    std::string name,revision;
    bool revisionKnown=false; // A change token detects stale data, not historical values.
};
// Explicit metadata-only upgrade; ordinary read-only services never write.
bool upgradeObjectRevisionSchema(const std::string& projectDirectory,std::string& error,
                                 const std::function<bool()>& cancelled={});
}
