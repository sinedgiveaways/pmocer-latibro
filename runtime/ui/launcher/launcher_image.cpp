// The player's own pictures (launcher_image.h): background.png (or .jpg) behind the start screen and drapeau.png (or
// .jpg) as the mark beside its title and as the windows' icon, all read from the folder of the executable like
// splash.png. None of it ships with the port: the pictures are the player's own, never in git.
// Decoding: stb_image (runtime/third_party/stb, public domain / MIT, nothings/stb @ 6b5799b), PNG and JPEG only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 14)
#include "stb/stb_image.h"

#include "ui/launcher/launcher_image.h"

extern "C" {
#include "platform/platform.h"
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
    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(buf, (int)n, &w, &h, &ch, 4);
    free(buf);
    if (!px) return nullptr;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        for (int y = 0; y < h; y++) memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch, px + (size_t)y * w * 4, (size_t)w * 4);
    }
    stbi_image_free(px);
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
    if (!s) return;
    SDL_SetWindowIcon(w, s);
    SDL_DestroySurface(s);
}
