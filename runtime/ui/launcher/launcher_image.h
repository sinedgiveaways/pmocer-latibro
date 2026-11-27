// The player's own pictures for the start screen and the windows (ui/launcher/launcher_image.cpp).
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif
// A PNG or JPEG file as an RGBA surface (stb_image), or NULL when it is missing or unreadable.
SDL_Surface *launcher_image_load(const char *path);
// <dir>/<name>.png or .jpg (name without extension, .png first), or NULL.
SDL_Surface *launcher_image_find(const char *dir, const char *name);
// The window's icon from drapeau.png or drapeau.jpg beside the executable; with none, the icon app_icon_set gave stays.
void app_icon_custom(SDL_Window *w);
#ifdef __cplusplus
}
#endif
