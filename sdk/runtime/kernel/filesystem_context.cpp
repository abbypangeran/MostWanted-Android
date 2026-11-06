#include "kernel/filesystem_context.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace kernel::filesystem {
namespace {

namespace fs = std::filesystem;
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string value) {
    for (char& c : value) c = char(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::string Normalize(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    std::string result;
    for (char c : value) {
        if (c == '\\' && !result.empty() && result.back() == '\\') continue;
        result += c;
    }
    while (result.size() > 1 && result.back() == '\\') result.pop_back();
    return result;
}

bool Within(const fs::path& root, const fs::path& candidate, fs::path& canonical_candidate) {
    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec) return false;
    canonical_candidate = fs::weakly_canonical(candidate, ec);
    if (ec) return false;

    auto root_part = canonical_root.begin();
    auto candidate_part = canonical_candidate.begin();
    for (; root_part != canonical_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == canonical_candidate.end() || *root_part != *candidate_part)
            return false;
    }
    return true;
}

fs::path MatchCase(const fs::path& root, const std::vector<std::string>& parts) {
    fs::path current = root;
    for (const std::string& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            current /= "..";
            current = current.lexically_normal();
            continue;
        }
        fs::path direct = current / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) {
            current = std::move(direct);
            continue;
        }
        bool found = false;
        const std::string wanted = Upper(part);
        for (fs::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
            if (Upper(it->path().filename().string()) == wanted) {
                current = it->path();
                found = true;
                break;
            }
        }
        if (!found) current = std::move(direct);
    }
    return current;
}

bool EnsureDirectory(const fs::path& path, std::vector<fs::path>& created, std::string& error) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (!ec && fs::exists(status)) {
        if (!fs::is_directory(status)) {
            error = "mapped path is not a directory: " + path.string();
            return false;
        }
        return true;
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "cannot inspect mapped directory " + path.string() + ": " + ec.message();
        return false;
    }
    const fs::path parent = path.parent_path();
    if (parent.empty() || parent == path || !EnsureDirectory(parent, created, error)) return false;
    ec.clear();
    const bool created_now = fs::create_directory(path, ec);
    if (ec) {
        error = "cannot create mapped directory " + path.string() + ": " + ec.message();
        return false;
    }
    if (created_now) {
        created.push_back(path);
    } else {
        ec.clear();
        if (!fs::is_directory(path, ec) || ec) {
            error = "mapped path is not a directory: " + path.string();
            return false;
        }
    }
    return true;
}

void Rollback(const std::vector<fs::path>& created, std::string& error) {
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        std::error_code ec;
        fs::remove(*it, ec);
        if (ec) error += "; rollback failed for " + it->string() + ": " + ec.message();
    }
}

}  // namespace

bool Context::MountDefaults(const std::string& game_dir, const std::string& data_dir, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    std::lock_guard lock(mutex_);

    std::error_code ec;
    const fs::path requested_game = fs::absolute(game_dir, ec);
    if (ec) {
        error = "game directory is unavailable: " + game_dir;
        return false;
    }
    const fs::path game_root = fs::weakly_canonical(requested_game, ec);
    if (ec || !fs::is_directory(game_root, ec) || ec) {
        error = "game directory is unavailable: " + game_dir;
        return false;
    }

    std::vector<fs::path> created;
    const fs::path requested_data = fs::absolute(data_dir, ec).lexically_normal();
    if (ec || !EnsureDirectory(requested_data, created, error)) {
        Rollback(created, error);
        return false;
    }
    const fs::path data_root = fs::weakly_canonical(requested_data, ec);
    if (ec || !fs::is_directory(data_root, ec) || ec) {
        error = "data directory is unavailable: " + data_dir;
        Rollback(created, error);
        return false;
    }

    std::map<std::string, fs::path> devices = devices_;
    devices[kCdRom] = game_root;
    const std::array<std::pair<const char*, const char*>, 5> drives = {{{"\\DEVICE\\HARDDISK0\\PARTITION1", "E"},
                                                                        {"\\DEVICE\\HARDDISK0\\PARTITION2", "C"},
                                                                        {"\\DEVICE\\HARDDISK0\\PARTITION3", "X"},
                                                                        {"\\DEVICE\\HARDDISK0\\PARTITION4", "Y"},
                                                                        {"\\DEVICE\\HARDDISK0\\PARTITION5", "Z"}}};
    for (const auto& [device, drive] : drives) {
        const fs::path root = data_root / drive;
        if (!EnsureDirectory(root, created, error)) {
            Rollback(created, error);
            return false;
        }
        fs::path canonical;
        if (!Within(data_root, root, canonical)) {
            error = "mapped directory escapes the data root: " + root.string();
            Rollback(created, error);
            return false;
        }
        devices[device] = canonical;
    }

    std::map<std::string, std::string> links = links_;
    links["\\??\\D:"] = "\\Device\\CdRom0";
    devices_.swap(devices);
    links_.swap(links);
    return true;
}

bool Context::CreateSymbolicLink(const std::string& link, const std::string& target) {
    const std::string normalized_link = Upper(Normalize(link));
    const std::string normalized_target = Normalize(target);
    if (normalized_link.empty() || normalized_target.empty()) return false;
    std::lock_guard lock(mutex_);
    links_[normalized_link] = normalized_target;
    return true;
}

bool Context::DeleteSymbolicLink(const std::string& link) {
    std::lock_guard lock(mutex_);
    return links_.erase(Upper(Normalize(link))) != 0;
}

std::string Context::QuerySymbolicLink(const std::string& link) const {
    std::lock_guard lock(mutex_);
    const auto it = links_.find(Upper(Normalize(link)));
    return it == links_.end() ? std::string() : it->second;
}

bool Context::ResolvePath(const std::string& xbox_path, ResolvedPath& result, std::string& error) const {
    error.clear();
    std::lock_guard lock(mutex_);
    std::string path = Normalize(xbox_path);
    if (path.size() >= 2 && path[1] == ':') path = "\\??\\" + path;

    for (int depth = 0; depth < 8; ++depth) {
        const std::string upper = Upper(path);
        const std::pair<const std::string, std::string>* best = nullptr;
        for (const auto& link : links_) {
            if (upper.compare(0, link.first.size(), link.first) == 0 &&
                (upper.size() == link.first.size() || upper[link.first.size()] == '\\') &&
                (!best || link.first.size() > best->first.size()))
                best = &link;
        }
        if (!best) break;
        path = best->second + path.substr(best->first.size());
        if (depth == 7) {
            error = "symbolic-link depth exceeded";
            return false;
        }
    }

    const std::string upper = Upper(path);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (const auto& mapping : devices_)
        if (upper.compare(0, mapping.first.size(), mapping.first) == 0 &&
            (upper.size() == mapping.first.size() || upper[mapping.first.size()] == '\\') &&
            (!best || mapping.first.size() > best->first.size()))
            best = &mapping;
    if (!best) {
        error = "no device mapping for path";
        return false;
    }

    std::vector<std::string> parts;
    const std::string rest = path.substr(best->first.size());
    size_t start = 0;
    while (start < rest.size()) {
        size_t end = rest.find('\\', start);
        if (end == std::string::npos) end = rest.size();
        if (end > start) parts.push_back(rest.substr(start, end - start));
        start = end + 1;
    }
    for (const std::string& part : parts) {
        if (part.find(':') != std::string::npos) {
            error = "colon in path component";
            return false;
        }
    }

    const fs::path candidate = MatchCase(best->second, parts);
    fs::path canonical;
    // shortcut: a host symlink can change after this check and before open; use descriptor-relative opens if concurrent untrusted writers matter.
    if (!Within(best->second, candidate, canonical)) {
        error = "resolved path escapes mapped device root";
        return false;
    }
    result = {best->first, best->second, candidate, std::move(parts)};
    return true;
}

void Context::Clear() {
    std::lock_guard lock(mutex_);
    links_.clear();
    devices_.clear();
}

}  // namespace kernel::filesystem
