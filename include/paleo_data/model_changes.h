#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace paleo::data {
// One filesystem gate shared by SDK writers and model-directory publishers.
// Thread-affine ownership; nested leases on the same thread are supported.
class ModelWriteLease {
public:
    ModelWriteLease();
    ~ModelWriteLease();
    ModelWriteLease(ModelWriteLease&&) noexcept;
    ModelWriteLease& operator=(ModelWriteLease&&) noexcept;
    ModelWriteLease(const ModelWriteLease&)=delete;
    ModelWriteLease& operator=(const ModelWriteLease&)=delete;
    explicit operator bool() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend ModelWriteLease tryAcquireModelWriteLease(const std::string&,int,std::string&);
};
ModelWriteLease tryAcquireModelWriteLease(const std::string& projectDirectory,int timeoutMs,std::string& error);
struct ModelChangeHead {
    std::string storeId, generation;
    std::int64_t sequence=0;
    bool available=false;
};
struct ModelChangeItem {
    std::int64_t ordinal=0, objectId=0, sourceId=0;
    std::string kind, metadataJson;
    bool deleted=false;
};
struct ModelChangePage {
    ModelChangeHead head;
    std::int64_t sequence=0, nextOrdinal=0;
    std::string operationId;
    std::vector<ModelChangeItem> items;
    bool complete=true;
};
// A cursor names a committed batch and its ordinal, not a mutable OFFSET.
// Sequence zero asks for the first batch after afterSequence.
struct ModelChangeQuery {
    std::string expectedGeneration;
    std::int64_t afterSequence=0, sequence=0, afterOrdinal=0;
    int maxItems=200;
    std::int64_t maxBytes=2*1024*1024;
};
}
