// The start screen (ui/launcher/launcher.cpp), before the game: its own window, gone again when it returns.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { LAUNCHER_QUIT = 0, LAUNCHER_PLAY = 1 };
int launcher_run(void);   // after cfg_init, before mem_init; LAUNCHER_PLAY: start the game, LAUNCHER_QUIT: end
// The profile folder the game will use: the Profiles page lists and makes its players (core/base/players.h).
void launcher_set_profile(const char *profile);

#ifdef __cplusplus
}
#endif
