// The start screen's pictures (launcher_image.h): background behind the start screen and drapeau as the mark beside its
// title and as the windows' icon. Both are built into the executable (images/, royalty-free pictures chosen by the
// owner, 27/11/2026); a background.png/.jpg or drapeau.png/.jpg the player puts beside the executable comes first.
// Decoding: stb_image (runtime/third_party/stb, public domain / MIT, nothings/stb @ 6b5799b), PNG and JPEG only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 14)
#include "stb_image.h"   // runtime/third_party/stb is an include directory (build.py adds each folder with headers)

#include "ui/launcher/launcher_image.h"

extern "C" {
#include "platform/platform.h"
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
// Relative to this file: #embed does not search the -I directories (ui/menu/menu_look.cpp)
static const unsigned char builtin_background[] = {
#embed "images/background.png"
};
static const unsigned char builtin_drapeau[] = {
#embed "images/drapeau.jpg"
};
#pragma clang diagnostic pop
static_assert(sizeof builtin_background > 100000, "images/background.png not embedded");
static_assert(sizeof builtin_drapeau > 10000, "images/drapeau.jpg not embedded");

static SDL_Surface *surface_of(const unsigned char *buf, int n) {
    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(buf, n, &w, &h, &ch, 4);
    if (!px) return nullptr;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        for (int y = 0; y < h; y++) memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch, px + (size_t)y * w * 4, (size_t)w * 4);
    }
    stbi_image_free(px);
    return s;
}

extern "C" SDL_Surface *launcher_image_builtin(const char *name) {
    if (!strcmp(name, "background")) return surface_of(builtin_background, (int)sizeof builtin_background);
    if (!strcmp(name, "drapeau")) return surface_of(builtin_drapeau, (int)sizeof builtin_drapeau);
    return nullptr;
}

extern "C" SDL_Surface *launcher_image_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64L << 20)) {   // an empty file, or a picture of more than 64 MiB
        fclose(f);
        return nullptr;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)n) {
        free(buf);
        return nullptr;
    }
    SDL_Surface *s = surface_of(buf, (int)n);
    free(buf);
    return s;
}

extern "C" SDL_Surface *launcher_image_find(const char *dir, const char *name) {
    static const char *const ext[] = {"png", "jpg"};
    for (const char *e : ext) {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s.%s", dir, name, e);
        if (!plat_file_exists(path)) continue;
        if (SDL_Surface *s = launcher_image_load(path)) return s;
    }
    return nullptr;
}

extern "C" void app_icon_custom(SDL_Window *w) {
    char dir[1024];
    if (!w || !plat_exe_path(dir, sizeof dir)) return;
    char *cut = plat_last_sep(dir);
    if (cut) *cut = 0;
    SDL_Surface *s = launcher_image_find(dir, "drapeau");
    if (!s) s = launcher_image_builtin("drapeau");
    if (!s) return;
    SDL_SetWindowIcon(w, s);
    SDL_DestroySurface(s);
}
