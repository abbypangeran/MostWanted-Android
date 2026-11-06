#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "app/app.h"
#include "kernel/clock_worker.h"
#include "kernel/kernel.h"
#include "kernel/kernel_thunks.h"
#include "kernel/startup_data.h"
#include "cpu/dispatch_table.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "xbe/xbe.h"
#include "xbe/image_loader.h"

extern const char kImageSha1[];

namespace xnet {
void CloseSockets();  // xdk/xnet.cpp
}

namespace kernel {

uint8_t* g_base = nullptr;
std::string g_launch_file;

namespace {
startup_data::DisplayState g_display_state;
bool g_mark = false;
std::string g_picture;  // the last picture the title showed, raw path string with its terminator

std::string PersistFile() { return g_launch_file.substr(0, g_launch_file.find_last_of("/\\") + 1) + "display.bin"; }

void LoadPreviousRunData() {
    std::string error;
    const startup_data::Result launch = startup_data::ImportLaunchData(g_launch_file, g_launch_data_page_va, error);
    if (launch == startup_data::Result::kImported) {
        LOGI("boot: launch data from the previous run imported");
        if (!error.empty()) LOGW("boot: %s", error.c_str());
    } else if (launch == startup_data::Result::kFailed) {
        LOGW("boot: launch data import failed; source preserved: %s", error.c_str());
    }
    if (launch != startup_data::Result::kImported) return;

    error.clear();
    const startup_data::Result display = startup_data::ImportPersistedDisplay(PersistFile(), g_display_state, error);
    if (display == startup_data::Result::kImported) {
        LOGI("boot: display kept from the previous run%s", g_display_state.marked ? ", marked" : "");
        if (!error.empty()) LOGW("boot: %s", error.c_str());
    } else if (display == startup_data::Result::kFailed) {
        LOGW("boot: persisted display import failed; source preserved: %s", error.c_str());
    }
}

bool ResolveKernelThunk(uint32_t ordinal, uint32_t& address, bool& implemented, void*) {
    if (ordinal >= 400) return false;
    const uint32_t data = DataAddress(ordinal);
    address = data ? data : dispatch::KernelThunkAddress(ordinal);
    implemented = HasExport(ordinal);
    return address != 0;
}
}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    if (FILE* f = std::fopen(PersistFile().c_str(), "wb")) {
        uint32_t head[7] = {startup_data::kPersistMagic, common, format, size, bytes, g_mark,
                             uint32_t(g_picture.size())};
        std::fwrite(head, sizeof head, 1, f);
        std::fwrite(pixels, 1, bytes, f);
        std::fwrite(g_picture.data(), 1, g_picture.size(), f);
        std::fclose(f);
    }
}

void MarkPersistedDisplay() { g_mark = true; }
bool KeptDisplayMarked() { return g_display_state.marked; }
void SetShownPicture(std::string path) { g_picture = std::move(path); }
const std::string& KeptPicture() { return g_display_state.picture; }

uint32_t SavedDataAddress() { return g_display_state.saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_display_state.saved_data = surface; }
const xbe::Image* g_image = nullptr;
static xbe::Image g_xbe;

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir, const std::string& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();
    if (!g_base) {
        LOGE("boot: raw guest access is blocked until the recompiler and host callers use checked memory");
        mem::Shutdown();
        return false;
    }

    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    const xbe::Image& img = g_xbe;
    if (img.entry != X86EntryPoint) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_name.c_str(), img.entry,
             X86EntryPoint);
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());
    for (auto& l : img.libraries) LOGI("boot: library %-8s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);

    if (!xbe::PrepareImage(img, X86EntryPoint, kImageSha1, err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    if (app::kGame.loaded) app::kGame.loaded();

    bool filesystem_mounted = false;
    auto rollback_image = [&] {
        dispatch::Reset();
        if (filesystem_mounted) {
            ClearFilesystemMappings();
            filesystem_mounted = false;
        }
        g_image = nullptr;
        uint32_t image_base = img.base, image_size = 0;
        if (mem::FreeVirtual(image_base, image_size, mem::kMemRelease) != 0)
            LOGE("boot: could not roll back the prepared XBE image");
    };

    if (!InitKernelData()) {
        LOGE("boot: kernel data initialization failed");
        rollback_image();
        return false;
    }
    if (!dispatch::Init(err)) {
        LOGE("boot: dispatch initialization failed: %s", err.c_str());
        rollback_image();
        return false;
    }
    std::vector<uint32_t> missing;
    if (!PatchKernelThunks(img, ResolveKernelThunk, nullptr, missing, err)) {
        LOGE("boot: kernel thunk patching failed: %s", err.c_str());
        rollback_image();
        return false;
    }
    for (uint32_t ordinal : missing)
        LOGW("boot: kernel import %u (%s) has no implementation", ordinal, ExportName(ordinal));
    LOGI("boot: kernel imports resolved, %zu missing", missing.size());

    if (!MountDefaults(game_dir, data_dir)) {
        LOGE("boot: filesystem setup failed");
        rollback_image();
        return false;
    }
    filesystem_mounted = true;

    // a quick reboot into this title left optional launch and display data behind
    g_launch_file = data_dir + "/launch.bin";
    LoadPreviousRunData();

    uint32_t tls = 0;
    if (img.tls_addr) tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15) & ~15u) + 4;
    if (!StartKernelClock()) {
        LOGE("boot: kernel clock worker could not start");
        rollback_image();
        return false;
    }
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000), tls, 0, img.entry, 0, false);
    if (!t) {
        LOGE("boot: guest thread initialization failed");
        StopKernelClockWorker();
        rollback_image();
        return false;
    }
    StartThread(t);
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(uint8_t(c)));
    for (auto& [xbe, exe] : app::kGame.images) {
        size_t n = std::strlen(xbe);
        if (xbe_path.size() >= n && xbe_path.compare(xbe_path.size() - n, n, xbe) == 0 &&
            (xbe_path.size() == n || xbe_path[xbe_path.size() - n - 1] == '\\' || xbe_path[xbe_path.size() - n - 1] == ';'))
            return exe;
    }
    return nullptr;
}

std::wstring HandoverEvent(unsigned long process) {
    return L"Local\\" + std::wstring(app::kGame.name) + L"Handover-" + std::to_wstring(process);
}

void Relaunch(const char* exe) {
    logx::Flush();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    // the command line keeps its arguments (--instance) behind the new executable
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + path + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    if (line.find(L"--relaunched") == std::wstring::npos) line += L" --relaunched";  // no boot splash again
    // this process lives on through the handover: the new one binds the same ports
    xnet::CloseSockets();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        // this window stays up until the new instance shows its first frames (HandoverEvent), so no gap shows
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, HandoverEvent(pi.dwProcessId).c_str());
        AllowSetForegroundWindow(pi.dwProcessId);
        g_relaunching = true;
        ResumeThread(pi.hThread);
        HANDLE wait[] = {ready, pi.hProcess};
        if (ready) WaitForMultipleObjects(2, wait, FALSE, 15000);
        if (ready) ::CloseHandle(ready);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        LOGE("relaunch: cannot start %s.exe (%lu)", exe, GetLastError());
    }
    StopKernelClockWorker();
    std::_Exit(0);
#else
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    std::string path = std::string(self, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + exe;
    StopKernelClockWorker();
    execl(path.c_str(), path.c_str(), nullptr);
    std::_Exit(0);
#endif
}

void Shutdown(int code) {
    StopKernelClockWorker();
    LOGR("Exit (%d)", code);
    logx::Flush();
    std::_Exit(code);
}

}  // namespace kernel
