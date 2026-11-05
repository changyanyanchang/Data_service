#pragma once
#include <cstdint>
#include <string>

namespace paleo::data {
class DataService;
// Prepared on a read-only worker connection; immutable once handed to the owner.
// Commit checks the source row and candidate metadata again, without rehashing.
class PreparedSourceRelocation {
public:
    long long sourceId() const { return sourceId_; }
    const std::string& fingerprint() const { return fingerprint_; }
    const std::string& originalLocation() const { return original_; }
    const std::string& candidateLocation() const { return candidate_; }
    long long verifiedBytes() const { return verifiedBytes_; }
private:
    friend class DataService;
    PreparedSourceRelocation() = default;
    long long sourceId_=0, size_=0, mtime_=0, change_=0, birth_=0, verifiedBytes_=0;
    std::string root_, stored_, original_, candidate_, fingerprint_;
};
}
