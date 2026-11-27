// Development helpers the console does not have, with no setting for the player (the turbo, the fixed seed and the
// freeze keys were removed on 27/11/2026: of no use for Orbitals).
#pragma once
#include <stdint.h>

uint64_t rt_ticks_real(void);              // main.c: the host's clock in guest ticks (rt_ticks)

// ---- freeze (freeze.c): ctl pause | step [N] | resume
void freeze_set(int on, int64_t steps);  // on: stop after `steps` more frames
void freeze_present(void);               // the game's present: waits while frozen
