extern "C" int RunMemoryTests();
extern "C" int RunXbeImageLoaderTests(const char* real_xbe_path);
extern "C" int RunStartupDataTests();
extern "C" int RunCheckedFileTransferTests();
extern "C" int RunClockWorkerTests();
extern "C" int RunDispatchTableTests();
extern "C" int RunKernelThunkTests();
extern "C" int RunFilesystemSetupTests();

int main(int argc, char** argv) {
    int failures = RunMemoryTests();
    failures += RunStartupDataTests();
    failures += RunCheckedFileTransferTests();
    failures += RunClockWorkerTests();
    failures += RunDispatchTableTests();
    failures += RunKernelThunkTests();
    failures += RunFilesystemSetupTests();
    failures += RunXbeImageLoaderTests(argc > 1 ? argv[1] : nullptr);
    return failures ? 1 : 0;
}
