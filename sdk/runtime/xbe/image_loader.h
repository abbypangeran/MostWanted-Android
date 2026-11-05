#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "kernel/memory.h"
#include "xbe/xbe.h"

namespace xbe {

using CheckedWrite = mem::AccessStatus (*)(uint32_t va, const void* source, size_t size);

enum class SectionStatus { kOk, kInvalidSection, kNoMemory, kAccessFailure };

// checked_write is injectable only so the failure rollback path can be tested deterministically.
bool PrepareImage(const Image& image, uint32_t expected_entry, std::string_view expected_sha1, std::string& error,
                  CheckedWrite checked_write = nullptr);
SectionStatus LoadSection(const Image& image, uint32_t section_header, std::string& error,
                          CheckedWrite checked_write = nullptr);
SectionStatus UnloadSection(const Image& image, uint32_t section_header, std::string& error,
                            CheckedWrite checked_write = nullptr);

}  // namespace xbe
