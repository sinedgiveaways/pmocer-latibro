// The start screen's background (ui/launcher/launcher_backdrop.cpp).
#pragma once
#include <SDL3/SDL.h>

struct ImDrawList;
void backdrop_load(SDL_Renderer *ren, const char *exe_dir);   // background.png or .jpg (else splash.png), and drapeau.png or .jpg, next to the program, if there are any
// The window's flat colour; picture: the splash over all of it (the main page)
void backdrop_draw(ImDrawList *dl, float w, float h, bool picture);
void backdrop_free(void);
bool backdrop_has_picture(void);   // there is a background picture
struct ImVec2;
// drapeau's picture, rounded, at size px with its top left at p; false (nothing drawn) when there is none.
bool backdrop_mark(ImDrawList *dl, ImVec2 p, float size);
