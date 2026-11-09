#pragma once

#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "cpu/x86_context.h"

namespace dispatch {

bool Init(std::string& error);
void Reset();
uint32_t KernelThunkAddress(uint32_t ordinal);
uint32_t HostThunk(X86Func* fn);
#ifdef MOSTWANTED_CHECKED_KERNEL_ABI_TESTS
bool SetKernel255FixtureBridge(bool enabled);
bool SetKernel187FixtureBridge(bool enabled);
#endif

enum class TargetKind {
    kGeneratedFunction, kContinuation, kKernelThunk, kHostThunk, kNull, kUninitialized, kUnmapped, kUnknown
};
struct ResolvedTarget {
    TargetKind kind;
    X86Func* function;
};

ResolvedTarget ResolveTarget(uint32_t guest);
const char* TargetKindName(TargetKind kind);

class CallLease {
public:
    CallLease();
    ~CallLease();
    CallLease(const CallLease&) = delete;
    CallLease& operator=(const CallLease&) = delete;
    ResolvedTarget Resolve(uint32_t guest) const;
    uint64_t Generation() const;
    uint32_t CodeBase() const;
    uint32_t CodeEnd() const;

private:
    std::shared_lock<std::shared_mutex> lock_;
    bool outer_ = false;
};

struct Metadata {
    uint32_t code_base;
    uint32_t code_end;
    uint32_t entry_point;
    std::span<const X86FuncMapping> functions;
    std::span<const uint32_t> continuations;
};

bool Init(const Metadata& metadata, std::string& error);
bool IsContinuation(uint32_t guest);
#ifdef MOSTWANTED_GUEST_CALLBACK_TESTS
bool ResetWouldBlockForTest();
#endif

class Table {
public:
    bool Initialize(const Metadata& metadata, std::string& error);
    void Reset();
    bool Ready() const { return !functions_.empty(); }
    X86Func* Find(uint32_t guest) const;
    bool IsContinuation(uint32_t guest) const;
    uint32_t CodeBase() const { return code_base_; }
    uint32_t CodeEnd() const { return code_end_; }

private:
    uint32_t code_base_ = 0;
    uint32_t code_end_ = 0;
    std::vector<X86Func*> functions_;
    std::vector<bool> continuations_;
};

}  // namespace dispatch
