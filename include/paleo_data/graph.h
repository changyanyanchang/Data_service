#pragma once
#include <string>
#include <vector>

namespace paleo::data {
// A directed, user-confirmed relation between two registered source files.
struct SourceRelation {
    long long id=0;
    long long sourceId=0;
    long long targetSourceId=0;
    std::string relation;
    std::string note;
};
// IDs are stable within a project: file:<sourceId>, well:<wellId>, or <objectKind>:<objectId>.
struct FileGraphNode {
    std::string id;
    std::string kind; // file, well, log_set, interval_track, sample_table, volume, horizon, map, table, or well-object kind
    long long refId=0; // Source ID for files; database object ID otherwise.
    long long sourceId=0; // Owning source for object nodes; equal to refId for files.
    std::string name, path, storedPath, sha256, status;
    std::string category; // Original SourceIndexEntry.kind for files; empty for other nodes.
    long long bytes=0;
};
struct FileGraphEdge {
    std::string id;
    std::string from, to, relation;
    std::string basis; // stored: projected from a business relation; manual: source_relations row.
    long long relationId=0; // SourceRelation.id for manual edges, otherwise zero.
    std::string note;
};
struct FileGraphOptions {
    bool includeObjects=true;
    bool includeInactiveSources=false;
    long long maxNodes=20000;
    long long maxEdges=100000;
};
// A renderer-neutral snapshot. No coordinates or layout are assigned by the data service.
struct FileGraph {
    std::string projectCrs, projectCrsUnit;
    std::vector<FileGraphNode> nodes;
    std::vector<FileGraphEdge> edges;
};
}
