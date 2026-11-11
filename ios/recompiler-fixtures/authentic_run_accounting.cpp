// Phase 2D-20: static/preparation memory accounting for the authentic image.
// This tool does not initialize the dispatch table, start a worker or execute a
// guest instruction. It loads the exact image, prepares the parent and child
// Thread blocks the experiment would use, cancels them, and prints the guest
// memory counters at each stage. It is the before/after evidence for the
// virtual=1280 observation.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "kernel/crypto.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xbe/image_loader.h"
#include "xbe/xbe.h"

namespace kernel {
uint8_t* g_base = nullptr;
}

namespace {

constexpr char kExpectedSha1[] = "f636a16af04171089f879a4bed646e185c734233";
constexpr uint32_t kImageEntry = 0x002B3916;
constexpr uint32_t kParentStart = 0x002B3916;
constexpr uint32_t kChildStart = 0x002B38A2;
constexpr uint32_t kChildSystem = 0x002B5C0F;
constexpr uint32_t kOutputPage = 0x00600000;

std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) std::array<uint8_t, 92> context{};
    uint8_t digest[20];
    crypto::ShaInit(context.data());
    crypto::ShaUpdate(context.data(), data.data(), data.size());
    crypto::ShaFinal(context.data(), digest);
    char text[41];
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

void Print(const char* label) {
    const mem::Stats stats = mem::Query();
    std::printf("%s total_pages=%u available=%u virtual=%u system=%u contiguous=%u\n", label, stats.total_pages,
                stats.available_pages, stats.virtual_committed, stats.system_used, stats.contiguous_used);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : AUTHENTIC_RUN_XBE;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("cannot open %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (Sha1Hex(bytes) != kExpectedSha1) {
        std::printf("image SHA-1 is not the reviewed image\n");
        return 2;
    }
    xbe::Image image;
    std::string error;
    if (!image.Parse(bytes, error)) {
        std::printf("image parse failed: %s\n", error.c_str());
        return 2;
    }
    if (!mem::Init()) {
        std::printf("guest memory did not initialize\n");
        return 2;
    }
    kernel::g_base = mem::Base();
    Print("BASELINE");
    if (!xbe::PrepareImage(image, kImageEntry, kExpectedSha1, error)) {
        std::printf("image preparation failed: %s\n", error.c_str());
        return 2;
    }
    Print("AFTER_IMAGE");
    if (!mem::Commit(kOutputPage, mem::kPage)) {
        std::printf("output page commit failed\n");
        return 2;
    }
    Print("AFTER_OUTPUT_PAGE");

    kernel::ThreadRef parent;
    kernel::ThreadRef child;
    const uint32_t parent_status =
        kernel::PrepareSystemThreadEx(0, 0x10000, 0x94, 0, kParentStart, 0, false, 0, parent);
    const uint32_t child_status = kernel::PrepareSystemThreadEx(kOutputPage, 0x10000, 0x94, kOutputPage + 4,
                                                                kChildStart, 0, false, kChildSystem, child);
    std::printf("PREPARED parent=%08X child=%08X\n", parent_status, child_status);
    Print("AFTER_THREADS");
    const bool cancelled = parent && child && kernel::CancelPreparedThread(parent) &&
                           kernel::CancelPreparedThread(child);
    std::printf("CANCELLED=%d\n", cancelled ? 1 : 0);
    Print("AFTER_CANCEL");
    mem::Shutdown();
    Print("AFTER_SHUTDOWN");
    return cancelled ? 0 : 1;
}
