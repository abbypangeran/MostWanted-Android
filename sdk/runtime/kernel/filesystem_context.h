#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace kernel::filesystem {

struct ResolvedPath {
    std::string device;
    std::filesystem::path root;
    std::filesystem::path host;
    std::vector<std::string> parts;
};

class Context {
public:
    bool MountDefaults(const std::string& game_dir, const std::string& data_dir, std::string& error);
    bool CreateSymbolicLink(const std::string& link, const std::string& target);
    bool DeleteSymbolicLink(const std::string& link);
    std::string QuerySymbolicLink(const std::string& link) const;
    bool ResolvePath(const std::string& xbox_path, ResolvedPath& result, std::string& error) const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> links_;
    std::map<std::string, std::filesystem::path> devices_;
};

}  // namespace kernel::filesystem
