#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

extern int RunMemoryTests(void);
extern int RunXbeImageLoaderTests(const char *real_xbe_path);
extern int RunStartupDataTests(void);
extern int RunCheckedFileTransferTests(void);
extern int RunCheckedGpuTransferTests(void);
extern int RunClockWorkerTests(void);
extern int RunDispatchTableTests(void);
extern int RunKernelThunkTests(void);
extern int RunFilesystemSetupTests(void);
extern int RunGuestMemoryPrototypeTests(void);
#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
extern int RunCheckedRecompilerFixtureTests(void);
#endif

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)appstate;
    (void)argc;
    (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("memory tests: SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    int failures = RunMemoryTests();
    failures += RunGuestMemoryPrototypeTests();
    failures += RunStartupDataTests();
    failures += RunCheckedFileTransferTests();
    failures += RunCheckedGpuTransferTests();
    failures += RunClockWorkerTests();
    failures += RunDispatchTableTests();
    failures += RunKernelThunkTests();
    failures += RunFilesystemSetupTests();
#ifdef MOSTWANTED_CHECKED_RECOMPILER_FIXTURE
    failures += RunCheckedRecompilerFixtureTests();
#endif
    failures += RunXbeImageLoaderTests(NULL);
    return failures ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate;
    (void)event;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    (void)appstate;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
    SDL_Log("memory tests: finished");
}
