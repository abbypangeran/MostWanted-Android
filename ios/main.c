#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_main.h>
#include <unistd.h>

typedef struct AppState {
    SDL_Window *window;
    SDL_MetalView metal_view;
    bool first_frame;
} AppState;

static AppState app;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("Darwin host page size=%ld", sysconf(_SC_PAGESIZE));
    app.window = SDL_CreateWindow("Most Wanted iOS host", 960, 540,
                                  SDL_WINDOW_FULLSCREEN | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    *appstate = &app;
    app.metal_view = SDL_Metal_CreateView(app.window);
    if (!app.metal_view) {
        SDL_Log("SDL_Metal_CreateView failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    void *metal_layer = SDL_Metal_GetLayer(app.metal_view);
    if (!metal_layer) {
        SDL_Log("SDL_Metal_GetLayer failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("SDL initialized; video driver=%s; window=%u", SDL_GetCurrentVideoDriver(),
            SDL_GetWindowID(app.window));
    SDL_Log("Metal surface initialized; layer=%p", metal_layer);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
        SDL_Log("SDL app terminating");
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        SDL_Log("SDL lifecycle: will enter background");
        break;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        SDL_Log("SDL lifecycle: entered background");
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        SDL_Log("SDL lifecycle: will enter foreground");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        SDL_Log("SDL lifecycle: entered foreground");
        break;
    case SDL_EVENT_LOW_MEMORY:
        SDL_Log("SDL lifecycle: low memory");
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        SDL_Log("SDL window shown: %u", event->window.windowID);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        SDL_Log("SDL window pixel size changed: %dx%d", event->window.data1, event->window.data2);
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState *state = appstate;
    if (!state->first_frame) {
        int width, height;
        SDL_GetWindowSizeInPixels(state->window, &width, &height);
        SDL_Log("SDL first iterate; pixel size=%dx%d", width, height);
        state->first_frame = true;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    AppState *state = appstate;
    (void)result;
    SDL_Log("SDL app quit");
    if (state && state->metal_view) SDL_Metal_DestroyView(state->metal_view);
    if (state && state->window) SDL_DestroyWindow(state->window);
}
