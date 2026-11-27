# runtime/core/hacks

Development helpers the console does not have, with no setting for the player. The turbo (`ORBITALS_TURBO`,
`ORBITALS_TURBO_SPEED`, F2), the fixed seed (`ORBITALS_SEED`) and the freeze keys (`ORBITALS_FREEZE_KEYS`, F3/F4) were
removed on 27/11/2026 with the "Hacks" tab: they were of no use for Orbitals.

| File | What it is |
|---|---|
| `hacks.h` | The helpers' interface |
| `freeze.c` | Freeze from the control pipe only (`ctl.sh NAME pause`, `step [N]`, `resume`): the game thread waits in its present |
