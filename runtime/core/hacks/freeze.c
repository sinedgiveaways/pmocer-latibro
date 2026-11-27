// Freeze: stops the game on a frame to look at it, and steps one frame at a time, from the control pipe only
// (tools/run/ctl.sh NAME pause | step [N] | resume; a development tool, no setting: the F3/F4 keys and
// ORBITALS_FREEZE_KEYS were removed on 27/11/2026). The game thread waits in its present (nvn_api_obj.c nvnQueuePresentTexture), after the frame
// went to the back end, so the window holds that frame; the game's other threads (audio, loading) go on.
#include <stdatomic.h>

#include "hacks.h"
#include "host.h"
#include "platform/platform.h"

static _Atomic int frozen;
static _Atomic int64_t steps;   // frames still to let through while frozen

void freeze_set(int on, int64_t n) {
    atomic_store(&steps, n);
    atomic_store(&frozen, on);
    rt_log("freeze: %s%s", on ? "frozen" : "running", on && n ? " after the next frames" : "");
}

void freeze_present(void) {
    if (!atomic_load(&frozen)) return;
    if (atomic_load(&steps) > 0) {
        atomic_fetch_sub(&steps, 1);
        return;
    }
    while (atomic_load(&frozen) && atomic_load(&steps) <= 0) {
        plat_sleep_ms(10);
    }
    if (atomic_load(&steps) > 0) atomic_fetch_sub(&steps, 1);
}
