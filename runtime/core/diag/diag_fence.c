// ORBITALS_DIAG_FENCE=1 (default off): a record of the waits and signals that can hold a frame back, for a run that
// stalls with every guest thread idle (Orbitals, 16/11/2026: Unreal's watchdog aborts after 120 s, "GameThread timed
// out waiting for RenderThread"; the abort's thread dump shows guest threads only and no host thread).
//
// What it keeps, all lock-free except the table of open waits:
//   - a ring of the last RING events (diag_fence.h: submits, blocking fence and sync waits, acquires, presents, the
//     completion thread moving "done", event signals), each with the time, the host thread id and, on a guest thread,
//     the guest handle that the abort's thread dump prints ("thread 0x1021");
//   - a count and the time of the last event of each kind;
//   - the blocking waits that have begun and not ended (a wait open at the abort names the thread and the fence);
//   - a line every 5 s of the vsync thread (its own ticks: if the line stops, the vsync thread stopped), with the
//     fence the guest submitted, the back end's queued and done values, and the frames presented.
// At a guest abort (svc.c, after the thread dump) the ring's last events, the counts and the open waits are printed.
// Reading a slot while another thread writes it can show a torn event; the dump is evidence, not a trace.
// ORBITALS_DIAG_FENCE adds one load to the paths it hooks while it is off.
#include "diag_fence.h"

#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "kernel.h"
#include "nvn_api.h"
#include "platform/platform.h"

volatile int diag_fence_state = -1;

#define RING 2048
#define DUMP_LAST 64
#define OPEN_MAX 32
#define BEAT_TICKS 300   // 5 s of the 60 Hz vsync thread

typedef struct { uint64_t ns, obj, a, b; uint32_t guest, tid, kind; } Ev;
typedef struct { uint64_t ns, obj, a, b; uint32_t guest, tid, kind, used; } Open;

static Ev ring[RING];
static uint64_t head, counts[DF_N], last_ns[DF_N], t_start, vs_tick, vs_ns;
static Open open_t[OPEN_MAX];
static PlatMutex open_lock = PLAT_MUTEX_INIT;

static const char *const kind_name[DF_N] = {
    [DF_SUBMIT] = "SUBMIT", [DF_CATCHUP_TIMEOUT] = "CATCHUP_TIMEOUT", [DF_FENCE_WAIT] = "FENCE_WAIT",
    [DF_FENCE_WAIT_END] = "FENCE_WAIT_END", [DF_SYNC_WAIT] = "SYNC_WAIT", [DF_SYNC_WAIT_END] = "SYNC_WAIT_END",
    [DF_ACQUIRE] = "ACQUIRE", [DF_ACQUIRE_END] = "ACQUIRE_END", [DF_ACQUIRE_TIMEOUT] = "ACQUIRE_TIMEOUT",
    [DF_PRESENT] = "PRESENT", [DF_FINISH] = "FINISH", [DF_FINISH_END] = "FINISH_END", [DF_FENCE_DONE] = "FENCE_DONE",
    [DF_EVENT_SIGNAL] = "EVENT_SIGNAL", [DF_EVENT_WRITE] = "EVENT_WRITE",
};

static int end_of(unsigned k) {
    switch (k) {
    case DF_FENCE_WAIT: return DF_FENCE_WAIT_END;
    case DF_SYNC_WAIT: return DF_SYNC_WAIT_END;
    case DF_ACQUIRE: return DF_ACQUIRE_END;
    case DF_FINISH: return DF_FINISH_END;
    default: return -1;
    }
}

static int begin_of(unsigned k) {
    switch (k) {
    case DF_FENCE_WAIT_END: return DF_FENCE_WAIT;
    case DF_SYNC_WAIT_END: return DF_SYNC_WAIT;
    case DF_ACQUIRE_END: return DF_ACQUIRE;
    case DF_FINISH_END: return DF_FINISH;
    default: return -1;
    }
}

static void init(void) {
    const char *e = cfg_get("ORBITALS_DIAG_FENCE");
    int on = e && *e && strcmp(e, "0"), expect = -1;
    t_start = plat_now_ns();
    if (__atomic_compare_exchange_n(&diag_fence_state, &expect, on, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && on)
        rt_log("diag-fence: on (ring of %d events, a line every %d s, the ring and the open waits at an abort; "
               "ORBITALS_DIAG_FENCE)", RING, BEAT_TICKS / 60);
}

void diag_fence_note(unsigned kind, uint64_t obj, uint64_t a, uint64_t b) {
    if (diag_fence_state < 0) init();
    if (diag_fence_state <= 0 || kind >= DF_N) return;
    uint64_t now = plat_now_ns();
    uint32_t tid = plat_thread_id(), guest = kern_current_handle();
    uint64_t i = __atomic_fetch_add(&head, 1, __ATOMIC_RELAXED);
    ring[i % RING] = (Ev){now, obj, a, b, guest, tid, kind};
    __atomic_fetch_add(&counts[kind], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&last_ns[kind], now, __ATOMIC_RELAXED);
    int opens = end_of(kind) >= 0, closes = begin_of(kind);
    if (!opens && closes < 0) return;
    plat_mutex_lock(&open_lock);
    if (opens) {
        for (int k = 0; k < OPEN_MAX; k++)
            if (!open_t[k].used) {
                open_t[k] = (Open){now, obj, a, b, guest, tid, kind, 1};
                break;
            }
    } else {
        for (int k = 0; k < OPEN_MAX; k++)
            if (open_t[k].used && open_t[k].tid == tid && (int)open_t[k].kind == closes) {
                open_t[k].used = 0;
                break;
            }
    }
    plat_mutex_unlock(&open_lock);
}

// The counters in one line: where the guest's work and the GPU's stand.
static void state_line(char *out, size_t cap, uint64_t now) {
    const GfxBackend *be = gfx_backend();
    uint64_t last = 0;
    for (int k = 0; k < DF_N; k++) {
        uint64_t t = __atomic_load_n(&last_ns[k], __ATOMIC_RELAXED);
        if (t > last) last = t;
    }
    uint64_t vt = __atomic_load_n(&vs_ns, __ATOMIC_RELAXED);
#define C(k) (unsigned long long)__atomic_load_n(&counts[k], __ATOMIC_RELAXED)
    snprintf(out, cap,
             "t=%.0fs vsync tick %llu (%.1fs ago) | submits %llu presents %llu acquires %llu fence waits %llu | "
             "fence submitted %llu queued %llu done %llu | frames %llu | last event %.1fs ago",
             (double)(now - t_start) / 1e9, (unsigned long long)__atomic_load_n(&vs_tick, __ATOMIC_RELAXED),
             vt ? (double)(now - vt) / 1e9 : -1.0, C(DF_SUBMIT), C(DF_PRESENT), C(DF_ACQUIRE), C(DF_FENCE_WAIT),
             (unsigned long long)nvn_fence_last(), (unsigned long long)be->fence_queued(),
             (unsigned long long)be->fence_done(),
             (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
             last ? (double)(now - last) / 1e9 : -1.0);
#undef C
}

static void open_line(char *out, size_t cap, const Open *o, uint64_t now) {
    char who[48];
    if (o->guest) snprintf(who, sizeof who, "host thread %u (guest %#x)", o->tid, o->guest);
    else snprintf(who, sizeof who, "host thread %u", o->tid);
    snprintf(out, cap, "%s: %s for %.1fs, obj %#llx a %llu b %llu", who, kind_name[o->kind] ? kind_name[o->kind] : "?",
             (double)(now - o->ns) / 1e9, (unsigned long long)o->obj, (unsigned long long)o->a,
             (unsigned long long)o->b);
}

void diag_fence_heartbeat(uint64_t tick) {
    if (diag_fence_state < 0) init();
    if (diag_fence_state <= 0) return;
    uint64_t now = plat_now_ns();
    __atomic_store_n(&vs_tick, tick, __ATOMIC_RELAXED);
    __atomic_store_n(&vs_ns, now, __ATOMIC_RELAXED);
    if (tick % BEAT_TICKS) return;
    char line[512];
    state_line(line, sizeof line, now);
    rt_log("diag-fence: %s", line);
    if (!plat_mutex_trylock(&open_lock)) return;
    for (int k = 0; k < OPEN_MAX; k++)
        if (open_t[k].used && now - open_t[k].ns > 2000000000ull) {
            open_line(line, sizeof line, &open_t[k], now);
            rt_log("diag-fence: open wait: %s", line);
        }
    plat_mutex_unlock(&open_lock);
}

void diag_fence_dump(void) {
    if (diag_fence_state <= 0) return;
    uint64_t now = plat_now_ns(), n = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    char line[512];
    state_line(line, sizeof line, now);
    rt_logf("  diag-fence at the abort: %s\n", line);
    rt_logf("  diag-fence counts of %llu events:", (unsigned long long)n);
    for (int k = 0; k < DF_N; k++)
        if (counts[k]) rt_logf(" %s %llu,", kind_name[k], (unsigned long long)counts[k]);
    rt_logf("\n");
    int shown = 0;
    if (plat_mutex_trylock(&open_lock)) {
        for (int k = 0; k < OPEN_MAX; k++)
            if (open_t[k].used) {
                open_line(line, sizeof line, &open_t[k], now);
                rt_logf("  diag-fence open wait: %s\n", line);
                shown++;
            }
        plat_mutex_unlock(&open_lock);
        if (!shown) rt_logf("  diag-fence: no blocking wait of the NVN layer or the back end is open\n");
    } else {
        rt_logf("  diag-fence: the table of open waits is locked; not read\n");
    }
    uint64_t from = n > DUMP_LAST ? n - DUMP_LAST : 0;
    rt_logf("  diag-fence: the last %llu events, oldest first (host tid, guest handle as in the thread dump):\n",
            (unsigned long long)(n - from));
    for (uint64_t i = from; i < n; i++) {
        Ev e = ring[i % RING];
        rt_logf("    -%9.3fs tid %-6u guest %#-6x %-16s obj %#llx a %llu b %llu\n", (double)(now - e.ns) / 1e9, e.tid,
                e.guest, e.kind < DF_N && kind_name[e.kind] ? kind_name[e.kind] : "?", (unsigned long long)e.obj,
                (unsigned long long)e.a, (unsigned long long)e.b);
    }
}
