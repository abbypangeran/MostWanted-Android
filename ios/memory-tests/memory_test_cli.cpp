extern "C" int RunMemoryTests();
extern "C" int RunXbeImageLoaderTests(const char* real_xbe_path);
extern "C" int RunStartupDataTests();

int main(int argc, char** argv) {
    int failures = RunMemoryTests();
    failures += RunStartupDataTests();
    failures += RunXbeImageLoaderTests(argc > 1 ? argv[1] : nullptr);
    return failures ? 1 : 0;
}
