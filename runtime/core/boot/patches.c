// Code patch sets (recomp/patches.py, gen.py --patches). The generator lifts both the original and the replacement
// instructions of every patched site, and rt_patch_on[set] picks one when the site runs, so a set is switched at start
// without touching code, and also while the game runs (rt_patch_switch). The sets, their environment switches and
// defaults, and the patch page come from the game code's tables (gamelib.h, from the generated patches.h); without
// them (C generated without --patches) there are no sets and every site runs the original.
//
// The patch page is the first 4 KiB page after main's image, inside the module window memory.c reserved: replacement
// code reaches it with one adrp, as MacNativeGames' data page after main (games/totk/runtime/game_patches.c). Its
// layout belongs to the sets that use it; a set that calls a host function registers it in rt_patch_hosts.
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gamelib.h"
#include "host.h"

#define ORBITALS_VCALL_PROBE_PAGE_SLOT 0x100
#define ORBITALS_VCALL_PROBE_LIMIT 32

#define ORBITALS_RECORD_SOURCE_PROBE_PAGE_SLOT 0x108
#define ORBITALS_RECORD_SOURCE_PROBE_LIMIT 32

// At main+0x1750524 the caller loads X25 = [X22+8]. It later adds 0x3810
// and passes that pointer through main+0x17508e0 to f_7001759b20, where
// LDUR W8,[X25,#0x34] faults in the 24/11 capture. The patch passes the
// live values in x0/x1 so this probe does not rely on Cpu's stale register
// snapshot. It only observes; it does not change the guest registers.
static void orbitals_record_source_probe(Cpu *c) {
    static uint64_t calls;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t source = c->x[0];  // live X25, copied by the patch
    uint64_t owner = c->x[1];   // live X22, copied by the patch
    uint64_t owner_source = 0;
    int owner_ok = owner && plat_mem_read(owner + 8, &owner_source, sizeof owner_source);
    uint64_t computed = source + 0x3810;
    uint32_t field = 0;
    int field_ok = computed <= UINT64_MAX - 0x34 &&
                   plat_mem_read(computed + 0x34, &field, sizeof field);

    if (n <= ORBITALS_RECORD_SOURCE_PROBE_LIMIT) {
        rt_log("record-source probe #%llu at main+0x1750524: X22=%#llx [X22+8]=%#llx (%s) X25=%#llx X25+0x3810=%#llx field+0x34=%#x (%s)",
               (unsigned long long)n, (unsigned long long)owner,
               (unsigned long long)owner_source, owner_ok ? "readable" : "unreadable",
               (unsigned long long)source, (unsigned long long)computed,
               field, field_ok ? "readable" : "unmapped");
        rt_log("  register cross-check: Cpu.X22=%#llx Cpu.X25=%#llx X30=%#llx SP=%#llx",
               (unsigned long long)c->x[22], (unsigned long long)c->x[25],
               (unsigned long long)c->x[30], (unsigned long long)c->sp);
    }
}

static void orbitals_vcall_probe(Cpu *c) {
    static uint64_t calls;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t target = c->x[16];             // saved from x8 by the patch sequence
    uint64_t receiver = c->x[0];
    uint64_t vptr = LD64(receiver);         // the original call already dereferenced this vptr
    uint64_t slot_target = LD64(vptr + 8);  // the original call already loaded this slot
    uint64_t parent = c->x[21];
    uint64_t object = LD64(parent + 8);     // loaded by the caller immediately before the probe site
    uint64_t callback = LD64(object + 0x30);
    uint64_t alternate = LD64(object + 0x58);

    if (n <= ORBITALS_VCALL_PROBE_LIMIT) {
        char callsite[96], target_name[96], slot_name[96];
        ldr_describe(c->x[30] - 4, callsite, sizeof callsite);
        ldr_describe(target, target_name, sizeof target_name);
        ldr_describe(slot_target, slot_name, sizeof slot_name);
        rt_log("vcall probe #%llu at %s: receiver=%#llx vptr=%#llx slot+8=%#llx (%s) saved_target=%#llx (%s)",
               (unsigned long long)n, callsite, (unsigned long long)receiver, (unsigned long long)vptr,
               (unsigned long long)slot_target, slot_name, (unsigned long long)target, target_name);
        rt_log("  args x1=%#llx x2=%#llx x21=%#llx x23=%#llx sp=%#llx lr=%#llx parent=%#llx object=%#llx +30=%#llx +58=%#llx",
               (unsigned long long)c->x[1], (unsigned long long)c->x[2],
               (unsigned long long)parent, (unsigned long long)c->x[23], (unsigned long long)c->sp,
               (unsigned long long)c->x[30], (unsigned long long)parent, (unsigned long long)object,
               (unsigned long long)callback, (unsigned long long)alternate);
    }

    // The patch saves the original vtable target in x16, then the unchanged BLR calls this probe.
    c->x[8] = target;
    rt_call(c, target, 0);

    if (n <= ORBITALS_VCALL_PROBE_LIMIT)
        rt_log("  vcall probe #%llu returned x0=%#llx", (unsigned long long)n, (unsigned long long)c->x[0]);
}

void rt_patches_init(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets) return;
    if (!plat_mem_commit(g->patch_page, 0x1000))
        rt_fatal("patches: cannot commit the patch page at %#llx", (unsigned long long)g->patch_page);
    char list[512];
    size_t n = 0;
    list[0] = 0;
    for (int i = 0; i < g->npatch_sets; i++) {
        const RtPatchSet *s = &g->patch_sets[i];
        const char *e = cfg_get(s->env);
        // a set with a row in the settings table takes its default from there; the generated default serves the rest
        g->patch_on[i] = (uint8_t)(cfg_meta(s->env) ? cfg_bool(s->env) : e && *e ? strcmp(e, "0") != 0 : s->on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", s->name, g->patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, (unsigned long long)g->patch_page);
}

// The host entry points on the patch page, before the dispatch table is built (hostfn.c).
// The generated replacement at main+0x1d761cc loads this address from patch-page slot +0x100.
void rt_patch_hosts(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets || !g->patch_page) return;
    uint64_t probe = hostfn_register("orbitals_vcall_probe", orbitals_vcall_probe);
    ST64(g->patch_page + ORBITALS_VCALL_PROBE_PAGE_SLOT, probe);
}

void rt_patch_switch(int set, int on) {
    const RtGame *g = rt_game();
    if (set >= 0 && set < g->npatch_sets) __atomic_store_n(&g->patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}
