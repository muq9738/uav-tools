#pragma once
#include "native_tiles.hpp"
#include <mutex>

namespace nt {
// Private checkpoint format, independent of protocolVersion (the public CLI protocol).
std::string sha256(const std::string& bytes);
std::string fileSha256(const fs::path& file);
Json fileIdentity(const fs::path& file);
Json checkpointConverterIdentity();
void verifyInputIdentity(const Json& inputs,const std::function<void()>& checkCancelled={});
bool pathContains(const fs::path& parent,const fs::path& child);

class Checkpoint {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Checkpoint(const fs::path& directory,const Json& identity,bool resume);
    ~Checkpoint();
    const fs::path& directory() const;
    fs::path work() const;
    std::unique_lock<std::mutex> lockEntry(const std::string& key);
    Json entry(const std::string& key) const;
    void save(const std::string& key,Json value,const std::vector<std::string>& assets);
    std::uint64_t filesCompleted() const;
    bool wasResumed(const std::string& key) const;
    void verifyAssets(bool finalized=false,const Json& derivedAssets=Json::object()) const;
    void preserve();
};
}
