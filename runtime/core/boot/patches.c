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

#define ORBITALS_READBACK_PROBE_PAGE_SLOT 0x110
#define ORBITALS_READBACK_PROBE_LIMIT 24

// vk_exec_bind.c, vk_mem_wback.c, vk_mem_capture.c (gfx/vk/device/vk.h)
int vk_unsized_written_find(int vram, uint64_t addr, uint64_t size, uint64_t *baddr, uint64_t *bsize, uint64_t *submit);
int vk_mem_gpu_may_write(uint64_t addr, uint64_t size);
int vk_mem_shadow_state(uint64_t addr);
extern uint64_t vk_replay_submit;
int nvn_copy_b2b_find(int over, uint64_t addr, uint64_t size, uint64_t size_lo, uint64_t size_hi, uint64_t *src,
                      uint64_t *dst, uint64_t *n, uint64_t *seq, uint64_t *total);   // gfx/nvn/api/nvn_api_cmd.c

// At main+0x1750524 the caller loads X25 = [X22+8]. It later adds 0x3810
// and passes that pointer through main+0x17508e0 to f_7001759b20, where
// LDUR W8,[X25,#0x34] faults in the 24/11 capture. The patch passes the
// live values in x0/x1 so this probe does not rely on Cpu's stale register
// snapshot. It only observes; it does not change the guest registers.

// Entry probes called by FN_ENTER for the two functions on the failing Free path.
// Unlike the old indirect-call hooks, these are attached to function entries that
// are present in the captured guest backtrace and do not depend on guessed call sites.
volatile int g_orbitals_entry_probe;

static void orbitals_log_read64(const char *name, uint64_t address) {
    uint64_t value = 0;
    int ok = address <= UINT64_MAX - sizeof value &&
             plat_mem_read(address, &value, sizeof value);
    rt_log("  %s[%#llx] = %#llx (%s)", name, (unsigned long long)address,
           (unsigned long long)value, ok ? "readable" : "unmapped");
}

static void orbitals_log_read32(const char *name, uint64_t address) {
    uint32_t value = 0;
    int ok = address <= UINT64_MAX - sizeof value &&
             plat_mem_read(address, &value, sizeof value);
    rt_log("  %s[%#llx] = %#x (%s)", name, (unsigned long long)address,
           value, ok ? "readable" : "unmapped");
}

void rt_patch_record_entry(uint64_t lr, uint64_t x0) {
    static uint64_t calls;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n > 64) return;
    char from[96];
    ldr_describe(lr, from, sizeof from);
    rt_log("record-entry #%llu main+0x1759b20: x0=%#llx lr=%s",
           (unsigned long long)n, (unsigned long long)x0, from);
    orbitals_log_read64("x0", x0);
    orbitals_log_read64("x0+8", x0 + 8);
    orbitals_log_read64("x0+0x30", x0 + 0x30);
    orbitals_log_read32("x0+0x34", x0 + 0x34);
    orbitals_log_read32("x0+0x5c", x0 + 0x5c);
}

void rt_patch_free_entry(uint64_t lr, uint64_t x0) {
    static uint64_t calls;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n > 128) return;
    char from[96];
    ldr_describe(lr, from, sizeof from);
    rt_log("free-entry #%llu main+0xbf040: ptr(x0)=%#llx lr=%s",
           (unsigned long long)n, (unsigned long long)x0, from);
    orbitals_log_read64("free-ptr", x0);
    orbitals_log_read64("free-ptr+8", x0 + 8);
    orbitals_log_read64("free-ptr+0x10", x0 + 0x10);
}

static void orbitals_record_source_probe(Cpu *c) {
    static uint64_t calls;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t target = c->x[16];           // original vtable target saved by the patch
    uint64_t source = c->x[25];           // live X25, loaded from [X22+8]
    uint64_t owner = c->x[22];            // live X22 from the caller
    uint64_t owner_source = 0;
    int owner_ok = owner && plat_mem_read(owner + 8, &owner_source, sizeof owner_source);
    uint64_t computed = source + 0x3810;
    uint32_t field = 0;
    int field_ok = computed <= UINT64_MAX - 0x34 &&
                   plat_mem_read(computed + 0x34, &field, sizeof field);

    if (n <= ORBITALS_RECORD_SOURCE_PROBE_LIMIT) {
        char callsite[96], target_name[96];
        ldr_describe(c->x[30] - 4, callsite, sizeof callsite);
        ldr_describe(target, target_name, sizeof target_name);
        rt_log("record-source probe #%llu at %s: X22=%#llx [X22+8]=%#llx (%s) X25=%#llx X25+0x3810=%#llx field+0x34=%#x (%s) target=%#llx (%s)",
               (unsigned long long)n, callsite, (unsigned long long)owner,
               (unsigned long long)owner_source, owner_ok ? "readable" : "unreadable",
               (unsigned long long)source, (unsigned long long)computed,
               field, field_ok ? "readable" : "unmapped",
               (unsigned long long)target, target_name);
    }

    // The unchanged BLR calls this probe. Restore and invoke the original target.
    c->x[8] = target;
    rt_call(c, target, 0);
    if (n <= ORBITALS_RECORD_SOURCE_PROBE_LIMIT)
        rt_log("  record-source probe #%llu returned x0=%#llx",
               (unsigned long long)n, (unsigned long long)c->x[0]);
}

// The record walk f_7001759b20 reads a GPU readback buffer: main+0x1759cac calls main+0x1583dc0, which locks it
// (a tail call through the RHI's vtable slot +0x100, size 0x7ffc), then reads u32 count (clamped to 0xfff) and
// {u32 record index, u32 target count} pairs from it. In the 25/11 dump the buffer held unrelated data: count 0xfff,
// an index that passed the checks with a target of 0xa063e4a0, and the record's inline array popped to -2, whose
// garbage entries fed the 4 GB memcpy (ucrtbase+0xee451). The probe calls the lock, then logs, for that caller only,
// what came back and what the runtime knows of the range: a shader store kept in VRAM over it (ORBITALS_WB_UNSIZED)
// would mean the GPU's count never reaches the CPU.
static void orbitals_readback_probe(Cpu *c) {
    uint64_t target = c->x[16];   // the RHI lock, saved by the patch
    uint64_t lr = c->x[30];       // a tail call: the helper's caller
    rt_call(c, target, 0);
    if (lr != 0x7001759cb0ull) return;
    static uint64_t calls, odd;
    uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t buf = c->x[0];
    uint32_t w[9] = {0};
    int ok = buf && plat_mem_read(buf, w, sizeof w);
    int bad = ok && w[0] > 0xfff;
    if (bad) __atomic_add_fetch(&odd, 1, __ATOMIC_RELAXED);
    if (n > ORBITALS_READBACK_PROBE_LIMIT && !(bad && odd <= ORBITALS_READBACK_PROBE_LIMIT)) return;
    uint64_t ba = 0, bs = 0, bsub = 0;
    int unsized = buf ? vk_unsized_written_find(1, buf, 0x7ffc, &ba, &bs, &bsub) : 0;
    uint64_t sa = 0, ss = 0, ssub = 0;
    int sized = buf ? vk_unsized_written_find(0, buf, 0x7ffc, &sa, &ss, &ssub) : 0;
    rt_log("readback probe #%llu: buffer %#llx (%s) count %u%s pairs {%u,%u} {%u,%u} {%u,%u} {%u,%u}; shadow state %d, GPU "
           "write pending %d; unsized VRAM-kept stores over it %d (newest %#llx+%#llx at submit %llu, now %llu)",
           (unsigned long long)n, (unsigned long long)buf, ok ? "readable" : "unreadable", w[0],
           bad ? " (over 0xfff: not what the GPU wrote)" : "", w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8],
           buf ? vk_mem_shadow_state(buf) : -1, buf ? vk_mem_gpu_may_write(buf, 0x7ffc) : -1, unsized,
           (unsigned long long)ba, (unsigned long long)bs, (unsigned long long)bsub,
           (unsigned long long)__atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED));
    // buffer copies (nvnCommandBufferCopyBufferToBuffer, the readback's usual filler) into it, and of about its size
    uint64_t s1 = 0, d1 = 0, n1 = 0, q1 = 0, s2 = 0, d2 = 0, n2 = 0, q2 = 0, total = 0;
    int into = buf ? nvn_copy_b2b_find(1, buf, 0x7ffc, 0, 0, &s1, &d1, &n1, &q1, &total) : 0;
    int near = nvn_copy_b2b_find(0, 0, 0, 0x7000, 0x8000, &s2, &d2, &n2, &q2, &total);
    rt_log("  readback probe #%llu: copies recorded %llu; into the buffer %d (newest #%llu %#llx -> %#llx, %#llx bytes); "
           "of 0x7000-0x8000 bytes %d (newest #%llu %#llx -> %#llx, %#llx bytes); other shader stores over it %d (newest "
           "%#llx+%#llx at submit %llu)",
           (unsigned long long)n, (unsigned long long)total, into, (unsigned long long)q1, (unsigned long long)s1,
           (unsigned long long)d1, (unsigned long long)n1, near, (unsigned long long)q2, (unsigned long long)s2,
           (unsigned long long)d2, (unsigned long long)n2, sized, (unsigned long long)sa, (unsigned long long)ss,
           (unsigned long long)ssub);
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
    const char *trace_entries = cfg_get("ORBITALS_TRACE_RECORD_SOURCE");
    g_orbitals_entry_probe = trace_entries && *trace_entries && strcmp(trace_entries, "0") != 0;
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
// The vcall probe at main+0x1d761cc uses slot +0x100; the record-source probe at
// main+0x17505b8/+0x17505e0 uses slot +0x108; the readback probe at main+0x1583df0 slot +0x110.
void rt_patch_hosts(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets || !g->patch_page) return;
    uint64_t probe = hostfn_register("orbitals_vcall_probe", orbitals_vcall_probe);
    ST64(g->patch_page + ORBITALS_VCALL_PROBE_PAGE_SLOT, probe);
    uint64_t record_probe = hostfn_register("orbitals_record_source_probe", orbitals_record_source_probe);
    ST64(g->patch_page + ORBITALS_RECORD_SOURCE_PROBE_PAGE_SLOT, record_probe);
    uint64_t readback_probe = hostfn_register("orbitals_readback_probe", orbitals_readback_probe);
    ST64(g->patch_page + ORBITALS_READBACK_PROBE_PAGE_SLOT, readback_probe);
}

void rt_patch_switch(int set, int on) {
    const RtGame *g = rt_game();
    if (set >= 0 && set < g->npatch_sets) __atomic_store_n(&g->patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}
