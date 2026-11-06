#include "kernel/filesystem_context.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

struct TemporaryRoot {
    fs::path path;
    ~TemporaryRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    bool Create() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("mostwanted-filesystem-" + std::to_string(stamp));
        std::error_code ec;
        return fs::create_directories(path / "game", ec) && !ec;
    }
};

bool Resolve(kernel::filesystem::Context& context, const std::string& xbox, fs::path& host) {
    kernel::filesystem::ResolvedPath result;
    std::string error;
    if (!context.ResolvePath(xbox, result, error)) return false;
    host = result.host;
    return true;
}

bool SamePath(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    const fs::path canonical_left = fs::weakly_canonical(left, ec);
    if (ec) return false;
    const fs::path canonical_right = fs::weakly_canonical(right, ec);
    return !ec && canonical_left == canonical_right;
}

void CheckDefaultMappingsAndBounds() {
    TemporaryRoot temp;
    Check(temp.Create(), "temporary filesystem test root creates");
    const fs::path game = temp.path / "game";
    const fs::path data = temp.path / "data";
    std::ofstream(game / "Default.Xbe") << "synthetic";
    kernel::filesystem::Context context;
    std::string error;
    Check(context.MountDefaults(game.string(), data.string(), error), "default device and directory setup succeeds");
    Check(context.QuerySymbolicLink("\\??\\d:") == "\\Device\\CdRom0", "default D drive link is installed case-insensitively");

    fs::path host;
    Check(Resolve(context, "D:\\default.xbe", host) && SamePath(host, game / "Default.Xbe"),
          "CD-ROM path resolves with case-insensitive host lookup");
    const std::array<std::pair<const char*, const char*>, 5> drives = {{{"1", "E"}, {"2", "C"}, {"3", "X"},
                                                                        {"4", "Y"}, {"5", "Z"}}};
    bool mappings_valid = true;
    for (const auto& [partition, drive] : drives) {
        const std::string xbox = "\\Device\\Harddisk0\\Partition" + std::string(partition) + "\\save.dat";
        mappings_valid &= Resolve(context, xbox, host) && SamePath(host, data / drive / "save.dat");
    }
    Check(mappings_valid, "all five hard-disk partitions map beneath the caller-provided data root");
    Check(fs::is_directory(data / "E") && fs::is_directory(data / "C") && fs::is_directory(data / "X") &&
              fs::is_directory(data / "Y") && fs::is_directory(data / "Z"),
          "all five default drive directories are prepared");

    Check(context.CreateSymbolicLink("\\??\\S:", "\\Device\\Harddisk0\\Partition1") &&
              Resolve(context, "S:\\save.dat", host) && SamePath(host, data / "E" / "save.dat"),
          "symbolic links resolve through the isolated context");
    Check(!Resolve(context, "\\Device\\Harddisk0\\Partition1\\..\\outside", host),
          "parent traversal cannot escape a mapped drive root");

    const fs::path outside = temp.path / "outside";
    fs::create_directories(outside);
    std::error_code ec;
    fs::create_directory_symlink(outside, data / "E" / "escape", ec);
    Check(!ec && !Resolve(context, "\\Device\\Harddisk0\\Partition1\\escape\\secret", host),
          "symlink resolution cannot escape a mapped drive root");

    context.Clear();
    Check(context.QuerySymbolicLink("\\??\\D:").empty() &&
              !Resolve(context, "D:\\default.xbe", host),
          "clearing a scoped context removes every link and device mapping");
    Check(context.MountDefaults(game.string(), data.string(), error) &&
              Resolve(context, "D:\\default.xbe", host),
          "a cleared context can be prepared again without stale mappings");
}

void CheckFailedSetupRollsBack() {
    TemporaryRoot temp;
    Check(temp.Create(), "rollback test root creates");
    const fs::path game = temp.path / "game";
    const fs::path data = temp.path / "rollback-data";
    fs::create_directories(data);
    std::ofstream(data / "X") << "block directory creation";

    kernel::filesystem::Context context;
    std::string error;
    const fs::path working_data = temp.path / "working-data";
    Check(context.MountDefaults(game.string(), working_data.string(), error),
          "initial mappings publish before a later setup attempt");
    Check(!context.MountDefaults(game.string(), data.string(), error) && !error.empty(),
          "a conflicting file makes default-drive setup fail");
    Check(!fs::exists(data / "E") && !fs::exists(data / "C") && fs::is_regular_file(data / "X"),
          "failed setup removes only directories it created and preserves the blocker");
    fs::path host;
    Check(context.QuerySymbolicLink("\\??\\D:") == "\\Device\\CdRom0" &&
              Resolve(context, "D:", host) && SamePath(host, game),
          "failed reinitialization preserves the prior complete mapping set");

    context.Clear();
    Check(context.QuerySymbolicLink("\\??\\D:").empty(), "failed context teardown remains empty");
}

}  // namespace

extern "C" int RunFilesystemSetupTests() {
    CheckDefaultMappingsAndBounds();
    CheckFailedSetupRollsBack();
    return failures ? 1 : 0;
}
