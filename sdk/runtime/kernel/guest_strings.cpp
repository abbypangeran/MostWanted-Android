#include "kernel/kernel.h"

namespace kernel {

std::string ReadAnsiString(uint32_t str) {
    if (!str) return {};
#ifdef __APPLE__
    uint8_t descriptor[8];
    const auto descriptor_status = mem::Read(str, descriptor, sizeof(descriptor));
    if (descriptor_status != mem::AccessStatus::kOk) GuestMemoryAccessFailure("read", str, descriptor_status);
    uint16_t len;
    uint32_t buf;
    std::memcpy(&len, descriptor, sizeof(len));
    std::memcpy(&buf, descriptor + 4, sizeof(buf));
#else
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
#endif
    if (!buf) return {};
#ifdef __APPLE__
    std::string value(len, '\0');
    if (len) {
        const auto status = mem::Read(buf, value.data(), len);
        if (status != mem::AccessStatus::kOk) GuestMemoryAccessFailure("read", buf, status);
    }
    return value;
#else
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
#endif
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string value;
    if (!va) return value;
    for (size_t i = 0; i < max; ++i) {
#ifdef __APPLE__
        if (i > uint64_t(UINT32_MAX) - va) GuestMemoryAccessFailure("read", UINT32_MAX, mem::AccessStatus::kInvalidAddress);
#endif
        char c = char(R8(va + uint32_t(i)));
        if (!c) break;
        value += c;
    }
    return value;
}

}  // namespace kernel
