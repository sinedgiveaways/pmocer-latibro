// The start screen's pictures and the windows' icon (ui/launcher/launcher_image.cpp).
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif
// A PNG or JPEG file as an RGBA surface (stb_image), or NULL when it is missing or unreadable.
SDL_Surface *launcher_image_load(const char *path);
// <dir>/<name>.png or .jpg (name without extension, .png first), or NULL.
SDL_Surface *launcher_image_find(const char *dir, const char *name);
// The picture built into the executable: "background" or "drapeau"; NULL for another name.
SDL_Surface *launcher_image_builtin(const char *name);
// The window's icon from drapeau.png/.jpg beside the executable, else the built-in drapeau.
void app_icon_custom(SDL_Window *w);
#ifdef __cplusplus
}
#endif
