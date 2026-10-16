#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char **argv) {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Event event;
    Uint32 pixels[64 * 64];
    Uint32 start;
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return 10;
    window = SDL_CreateWindow("SDL2 sobre BlesKernOS",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 480, 300,
        SDL_WINDOW_SHOWN);
    if (!window) return 11;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, 64, 64);
    if (!renderer || !texture) return 12;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        pixels[y * 64 + x] = 0xFF000000U | ((Uint32)(x * 4) << 16) |
                             ((Uint32)(y * 4) << 8) | 0x50U;
    SDL_UpdateTexture(texture, 0, pixels, 64 * 4);
    SDL_SetRenderDrawColor(renderer, 30, 38, 60, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 50, 150, 230, 255);
    SDL_RenderFillRect(renderer, &(SDL_Rect){40, 40, 400, 80});
    SDL_SetRenderDrawColor(renderer, 230, 170, 40, 255);
    SDL_RenderFillRect(renderer, &(SDL_Rect){100, 160, 280, 90});
    SDL_RenderCopy(renderer, texture, 0, &(SDL_Rect){208, 48, 64, 64});
    SDL_RenderPresent(renderer);
    puts("SDL2 POSIX x86 OK");
    start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1500U) {
        while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) break;
        SDL_Delay(5);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return 0;
}
