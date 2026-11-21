// Indirect-branch dispatch: guest address -> lifted function, O(1).
//
// Layout of the table: one uint32_t slot per 4-byte guest instruction over [lo, hi), where lo is the lowest function
// start of any module and hi is past the highest. A slot is 0 when no function starts there, else the host function's
// address minus `anchor` (an address just below the lowest host function), so 4 bytes are enough as long as all lifted
// code lies within 4 GiB, which a single PE image guarantees. Memory: 4 bytes per 4 guest bytes, i.e. the size of the
// guest range covered. The gaps between code ranges (rodata, data, bss, alignment) are reserved but never written, so
// the OS never backs them with RAM; resident memory is about the total .text size of the modules.
//
// The table is filled once by rt_dispatch_init and then made read-only, so any number of threads read it without
// locks or per-thread caches. Host (HLE) functions at addresses outside the modules live in a small hash table that
// is also fixed at init; they are looked up only after the flat table misses.
#include "dispatch.h"
#include "host.h"
#include "cfg.h"
#include "platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TABLE_SPAN_LIMIT (1ull << 30)   // guest bytes covered by the table, and its size in bytes
#define HOST_CAP 8192                   // host function registry, power of two
#define MAX_MODULES 16

// Hot data read by every rt_dispatch: one cache line.
static struct __attribute__((aligned(64))) {
    const uint32_t *slot;
    uintptr_t anchor;
    uint64_t lo;
    uint64_t slots;
} D;

static const RtModule *g_mods[MAX_MODULES];   // sorted by base
static int g_nmods;
static bool g_ready;
static uint64_t g_table_bytes, g_functions;

static struct { uint64_t addr; GuestFn fn; } g_host[HOST_CAP];
static uint64_t g_nhost;

/* Opt-in TPIDR trace. Keep a lock-free, crash-dumpable tail: ordinary log writes may be lost
 * when an access violation terminates the process. Every field is atomic because guest threads
 * can write concurrently with the crash handler. */
#define TPIDR_TRACE_CAP 128u
typedef struct {
    uint64_t seq, thread, pc, old_value, new_value;
    uint32_t op;
} TpidrTraceEntry;
static TpidrTraceEntry tpidr_trace_ring[TPIDR_TRACE_CAP];
static uint64_t tpidr_trace_next;
static int rt_tpidr_trace;

int rt_tpidr_trace_enabled(void) { return rt_tpidr_trace; }

static void tpidr_trace_record(Cpu *c, uint64_t pc, uint64_t old_value, uint64_t new_value, uint32_t op) {
    uint64_t seq = __atomic_add_fetch(&tpidr_trace_next, 1, __ATOMIC_RELAXED);
    TpidrTraceEntry *e = &tpidr_trace_ring[(seq - 1) % TPIDR_TRACE_CAP];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&e->thread, (uint64_t)(uintptr_t)c->thread, __ATOMIC_RELAXED);
    __atomic_store_n(&e->pc, pc, __ATOMIC_RELAXED);
    __atomic_store_n(&e->old_value, old_value, __ATOMIC_RELAXED);
    __atomic_store_n(&e->new_value, new_value, __ATOMIC_RELAXED);
    __atomic_store_n(&e->op, op, __ATOMIC_RELAXED);
    __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
}

void rt_tpidr_trace_dump(void) {
    uint64_t end = __atomic_load_n(&tpidr_trace_next, __ATOMIC_ACQUIRE);
    uint64_t begin = end > TPIDR_TRACE_CAP ? end - TPIDR_TRACE_CAP + 1 : 1;
    uint64_t count = end ? end - begin + 1 : 0;
    rt_logf("FATAL: TPIDR trace: newest %llu of %llu recorded events\n",
            (unsigned long long)count, (unsigned long long)end);
    for (uint64_t seq = begin; seq <= end && seq; seq++) {
        TpidrTraceEntry *e = &tpidr_trace_ring[(seq - 1) % TPIDR_TRACE_CAP];
        uint64_t seen = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
        if (seen != seq) continue;
        uint64_t thread = __atomic_load_n(&e->thread, __ATOMIC_RELAXED);
        uint64_t pc = __atomic_load_n(&e->pc, __ATOMIC_RELAXED);
        uint64_t old_value = __atomic_load_n(&e->old_value, __ATOMIC_RELAXED);
        uint64_t new_value = __atomic_load_n(&e->new_value, __ATOMIC_RELAXED);
        uint32_t op = __atomic_load_n(&e->op, __ATOMIC_RELAXED);
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) continue;
        rt_logf("  tpidr[%llu] %s thread=%#llx pc=%#llx old=%#llx new=%#llx",
                (unsigned long long)seq, op ? "write" : "read",
                (unsigned long long)thread, (unsigned long long)pc,
                (unsigned long long)old_value, (unsigned long long)new_value);
        rt_log_raw("\n", 1);
    }
}

uint64_t rt_tpidr_read(Cpu *c, uint64_t pc) {
    uint64_t value = c->tpidr;
    tpidr_trace_record(c, pc, value, value, 0);
    return value;
}

void rt_tpidr_write(Cpu *c, uint64_t pc, uint64_t value) {
    uint64_t old = c->tpidr;
    c->tpidr = value;
    tpidr_trace_record(c, pc, old, value, 1);
}

/* Always-on, per-call-site snapshot for the SDK's GetTlsValue. The crash under
 * investigation is sdk+0x16b730; its entry ABI receives TlsSlot in x0 and uses
 * x27 as the TLS table anchor. This bounded ring records only that function,
 * not every guest instruction, and is filtered by guest-thread at dump time. */
#define TLS_CALL_TRACE_CAP 32u
#define TLS_CANDIDATES 3u
uint64_t g_get_tls_value_pc __attribute__((visibility("hidden")));
typedef struct {
    uint64_t seq, thread, pc, lr, x0, x27, table, tpidr, tpidrro;
    uint64_t x27_region_end, table_region_end;
    int32_t x27_region_state, table_region_state;
    uint32_t slot_valid, slots[TLS_CANDIDATES];
} TlsCallTraceEntry;
static TlsCallTraceEntry tls_call_trace[TLS_CALL_TRACE_CAP];
static uint64_t tls_call_trace_next;

void rt_tls_call_trace_entry(Cpu *c, uint64_t pc) {
    if (!c || !g_get_tls_value_pc || pc != g_get_tls_value_pc) return;
    uint64_t seq = __atomic_add_fetch(&tls_call_trace_next, 1, __ATOMIC_RELAXED);
    TlsCallTraceEntry *e = &tls_call_trace[(seq - 1) % TLS_CALL_TRACE_CAP];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
    uint64_t thread = (uint64_t)(uintptr_t)c->thread;
    uint64_t x27 = c->x[27], table = 0, x27_end = 0, table_end = 0;
    int x27_state = PLAT_MEM_FREE, table_state = PLAT_MEM_FREE;
    if (x27) x27_state = plat_mem_state(x27, x27 + 1, &x27_end);
    int have_table = x27 && plat_mem_read(x27, &table, sizeof table);
    if (have_table && table) table_state = plat_mem_state(table, table + 1, &table_end);
    uint32_t valid = 0, slots[TLS_CANDIDATES] = {0};
    for (uint32_t i = 0; i < TLS_CANDIDATES; i++) {
        uint64_t at = table + (uint64_t)i * 0x38 + 0x5c;
        if (have_table && at >= table && plat_mem_read(at, &slots[i], sizeof slots[i]))
            valid |= 1u << i;
    }
    __atomic_store_n(&e->thread, thread, __ATOMIC_RELAXED);
    __atomic_store_n(&e->pc, pc, __ATOMIC_RELAXED);
    __atomic_store_n(&e->lr, c->x[30], __ATOMIC_RELAXED);
    __atomic_store_n(&e->x0, c->x[0], __ATOMIC_RELAXED);
    __atomic_store_n(&e->x27, x27, __ATOMIC_RELAXED);
    __atomic_store_n(&e->table, have_table ? table : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&e->tpidr, c->tpidr, __ATOMIC_RELAXED);
    __atomic_store_n(&e->tpidrro, c->tpidrro, __ATOMIC_RELAXED);
    __atomic_store_n(&e->x27_region_end, x27_end, __ATOMIC_RELAXED);
    __atomic_store_n(&e->table_region_end, table_end, __ATOMIC_RELAXED);
    __atomic_store_n(&e->x27_region_state, x27_state, __ATOMIC_RELAXED);
    __atomic_store_n(&e->table_region_state, table_state, __ATOMIC_RELAXED);
    __atomic_store_n(&e->slot_valid, valid, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < TLS_CANDIDATES; i++)
        __atomic_store_n(&e->slots[i], slots[i], __ATOMIC_RELAXED);
    __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
}

void rt_tls_call_trace_dump(Cpu *c) {
    if (!c) return;
    uint64_t thread = (uint64_t)(uintptr_t)c->thread;
    uint64_t end = __atomic_load_n(&tls_call_trace_next, __ATOMIC_ACQUIRE);
    uint64_t begin = end > TLS_CALL_TRACE_CAP ? end - TLS_CALL_TRACE_CAP + 1 : 1;
    rt_logf("FATAL: GetTlsValue entry trace for thread=%#llx (latest %llu entries):\\n",
            (unsigned long long)thread, (unsigned long long)(end - begin + (end ? 1 : 0)));
    for (uint64_t seq = begin; seq <= end && seq; seq++) {
        TlsCallTraceEntry *e = &tls_call_trace[(seq - 1) % TLS_CALL_TRACE_CAP];
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) continue;
        uint64_t owner = __atomic_load_n(&e->thread, __ATOMIC_RELAXED);
        if (owner != thread || __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) continue;
        uint64_t pc = __atomic_load_n(&e->pc, __ATOMIC_RELAXED);
        uint64_t lr = __atomic_load_n(&e->lr, __ATOMIC_RELAXED);
        uint64_t x0 = __atomic_load_n(&e->x0, __ATOMIC_RELAXED);
        uint64_t x27 = __atomic_load_n(&e->x27, __ATOMIC_RELAXED);
        uint64_t table = __atomic_load_n(&e->table, __ATOMIC_RELAXED);
        uint64_t tpidr = __atomic_load_n(&e->tpidr, __ATOMIC_RELAXED);
        uint64_t tpidrro = __atomic_load_n(&e->tpidrro, __ATOMIC_RELAXED);
        uint64_t xe = __atomic_load_n(&e->x27_region_end, __ATOMIC_RELAXED);
        uint64_t te = __atomic_load_n(&e->table_region_end, __ATOMIC_RELAXED);
        int xs = __atomic_load_n(&e->x27_region_state, __ATOMIC_RELAXED);
        int ts = __atomic_load_n(&e->table_region_state, __ATOMIC_RELAXED);
        uint32_t valid = __atomic_load_n(&e->slot_valid, __ATOMIC_RELAXED);
        uint32_t slots[TLS_CANDIDATES];
        for (uint32_t i = 0; i < TLS_CANDIDATES; i++)
            slots[i] = __atomic_load_n(&e->slots[i], __ATOMIC_RELAXED);
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) continue;
        rt_logf("  GetTlsValue[%llu]: pc=%#llx lr=%#llx TlsSlot(x0)=%#llx x27=%#llx [x27]=%#llx TPIDR=%#llx TLR=%#llx\\n",
                (unsigned long long)seq, (unsigned long long)pc, (unsigned long long)lr,
                (unsigned long long)x0, (unsigned long long)x27, (unsigned long long)table,
                (unsigned long long)tpidr, (unsigned long long)tpidrro);
        rt_logf("    x27 region: state=%d end=%#llx; table region: state=%d end=%#llx\\n",
                xs, (unsigned long long)xe, ts, (unsigned long long)te);
        rt_logf("    table candidates (+i*0x38+0x5c):");
        for (uint32_t i = 0; i < TLS_CANDIDATES; i++)
            rt_logf(" [%u]=%s%08x", i, valid & (1u << i) ? "" : "unreadable/", slots[i]);
        rt_log_raw("\\n", 1);
    }
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_log_raw("FATAL: ", 7);
    rt_vlogf(fmt, ap);
    rt_log_raw("\n", 1);
    va_end(ap);
    // whether the system was out of memory then: a fatal from a failed allocation reads the same otherwise
    uint64_t res, prv;
    float used, total, proc;
    plat_process_memory(&res, &prv);
    plat_mem_info(&used, &total, &proc);
    rt_log("FATAL: memory: process %.0f MiB resident, %.0f MiB private; system %.0f of %.0f MiB in use", res / 1048576.0,
           prv / 1048576.0, used, total);
    fflush(stdout);
    fflush(stderr);
    _Exit(70);
}

static const RtModule *module_at(uint64_t pc) {
    for (int i = 0; i < g_nmods; i++)
        if (pc - g_mods[i]->base < g_mods[i]->size) return g_mods[i];
    return NULL;
}

const char *rt_guest_name(uint64_t pc, char *buf, int size) {
    const RtModule *m = module_at(pc);
    if (m) snprintf(buf, size, "%#llx (%s+%#llx)", (unsigned long long)pc, m->name, (unsigned long long)(pc - m->base));
    else snprintf(buf, size, "%#llx", (unsigned long long)pc);
    return buf;
}

static uint64_t host_hash(uint64_t addr) { return (addr >> 2) * 0x9E3779B97F4A7C15ull >> 51; }   // 13 bits

static GuestFn host_lookup(uint64_t addr) {
    for (uint64_t h = host_hash(addr), n = 0; n < HOST_CAP; h = (h + 1) & (HOST_CAP - 1), n++) {
        if (g_host[h].addr == addr) return g_host[h].fn;
        if (!g_host[h].addr) return NULL;
    }
    return NULL;
}

void rt_register_host(uint64_t addr, GuestFn fn) {
    if (g_ready) rt_fatal("rt_register_host(%#llx) after rt_dispatch_init", (unsigned long long)addr);
    if (!addr || (addr & 3) || !fn) rt_fatal("rt_register_host: bad entry %#llx", (unsigned long long)addr);
    if (g_nhost >= HOST_CAP / 2) rt_fatal("rt_register_host: more than %d host functions", HOST_CAP / 2);
    uint64_t h = host_hash(addr);
    while (g_host[h].addr && g_host[h].addr != addr) h = (h + 1) & (HOST_CAP - 1);
    if (g_host[h].addr) rt_fatal("rt_register_host: %#llx registered twice", (unsigned long long)addr);
    g_host[h].addr = addr;
    g_host[h].fn = fn;
    g_nhost++;
}

static void *table_alloc(uint64_t bytes) {
    return plat_mem_alloc(bytes);   // zero pages, backed on first write
}

static void table_seal(void *p, uint64_t bytes) {
    if (!plat_mem_protect((uint64_t)(uintptr_t)p, bytes, PLAT_PROT_READ)) rt_fatal("dispatch: sealing the table failed");
}

void rt_dispatch_init(const RtModule *mods, int count) {
    const char *trace_tpidr = cfg_get("ORBITALS_TRACE_TPIDR");
    rt_tpidr_trace = trace_tpidr && *trace_tpidr && strcmp(trace_tpidr, "0");
    if (rt_tpidr_trace) rt_log("tpidr: tracing guest TPIDR_EL0 reads and writes (ORBITALS_TRACE_TPIDR)");
    if (g_ready) rt_fatal("rt_dispatch_init called twice");
    if (count <= 0 || count > MAX_MODULES) rt_fatal("rt_dispatch_init: %d modules", count);

    // Pin the investigated SDK entry point by module-relative address, independent of ASLR.
    for (int i = 0; i < count; i++) {
        if (mods[i].name && !strcmp(mods[i].name, "sdk")) {
            g_get_tls_value_pc = mods[i].base + 0x16b730;
            rt_logf("crash trace: GetTlsValue entry=%#llx (sdk+0x16b730)\\n",
                    (unsigned long long)g_get_tls_value_pc);
            break;
        }
    }
    if (!g_get_tls_value_pc)
        rt_log("crash trace: SDK module not found; GetTlsValue entry hook is unavailable");

    // Modules by base, checked for overlap.
    for (int i = 0; i < count; i++) {
        int j = g_nmods++;
        while (j > 0 && g_mods[j - 1]->base > mods[i].base) { g_mods[j] = g_mods[j - 1]; j--; }
        g_mods[j] = &mods[i];
    }
    for (int i = 1; i < g_nmods; i++)
        if (g_mods[i - 1]->base + g_mods[i - 1]->size > g_mods[i]->base)
            rt_fatal("dispatch: modules %s and %s overlap", g_mods[i - 1]->name, g_mods[i]->name);

    // Function tables: sorted, aligned, inside their module. Range of guest starts and of host functions.
    uint64_t lo = UINT64_MAX, hi = 0;
    uintptr_t host_lo = UINTPTR_MAX, host_hi = 0;
    for (int i = 0; i < g_nmods; i++) {
        const RtModule *m = g_mods[i];
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            uintptr_t f = (uintptr_t)m->fns[k];
            if ((a & 3) || a - m->base >= m->size)
                rt_fatal("dispatch: %s_fn_addrs[%llu] = %#llx is misaligned or outside the module", m->name,
                         (unsigned long long)k, (unsigned long long)a);
            if (k && a <= m->addrs[k - 1])
                rt_fatal("dispatch: %s_fn_addrs is not strictly ascending at [%llu] = %#llx", m->name,
                         (unsigned long long)k, (unsigned long long)a);
            if (!f) rt_fatal("dispatch: %s_fn_ptrs[%llu] is NULL", m->name, (unsigned long long)k);
            if (f < host_lo) host_lo = f;
            if (f > host_hi) host_hi = f;
        }
        if (m->count) {
            if (m->addrs[0] < lo) lo = m->addrs[0];
            if (m->addrs[m->count - 1] + 4 > hi) hi = m->addrs[m->count - 1] + 4;
        }
    }
    if (hi <= lo) rt_fatal("dispatch: no functions in any module");
    if (hi - lo > TABLE_SPAN_LIMIT)
        rt_fatal("dispatch: modules span %#llx guest bytes, over the %#llx the flat table allows",
                 (unsigned long long)(hi - lo), TABLE_SPAN_LIMIT);
    uintptr_t anchor = (host_lo & ~(uintptr_t)15) - 16;
    if (host_hi - anchor > UINT32_MAX)
        rt_fatal("dispatch: lifted code spans %#llx host bytes; slots hold 32-bit offsets (4 GiB)",
                 (unsigned long long)(host_hi - anchor));

    for (uint64_t h = 0; h < HOST_CAP; h++)
        if (g_host[h].addr && module_at(g_host[h].addr))
            rt_fatal("dispatch: host function at %#llx lies inside module %s", (unsigned long long)g_host[h].addr,
                     module_at(g_host[h].addr)->name);

    uint64_t slots = (hi - lo) >> 2, bytes = (slots * 4 + 0xFFFF) & ~0xFFFFull;
    uint32_t *slot = table_alloc(bytes);
    if (!slot) rt_fatal("dispatch: cannot allocate a %llu-byte table", (unsigned long long)bytes);
    for (int i = 0; i < g_nmods; i++) {
        const RtModule *m = g_mods[i];
        for (uint64_t k = 0; k < m->count; k++)
            slot[(m->addrs[k] - lo) >> 2] = (uint32_t)((uintptr_t)m->fns[k] - anchor);
        g_functions += m->count;
    }
    table_seal(slot, bytes);

    g_table_bytes = bytes;
    D.slot = slot;
    D.anchor = anchor;
    D.lo = lo;
    D.slots = slots;
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
}

GuestFn rt_dispatch_lookup(uint64_t pc) {
    uint64_t i = __builtin_rotateright64(pc - D.lo, 2);   // a misaligned pc lands far above D.slots
    if (i < D.slots && D.slot[i]) return (GuestFn)(D.anchor + D.slot[i]);
    return g_ready ? host_lookup(pc) : NULL;
}

__attribute__((noinline, noreturn, cold)) static void dispatch_fail(Cpu *c) {
    char at[64], lr[64];
    uint64_t pc = c->pc;
    if (!g_ready) rt_fatal("rt_dispatch to %#llx before rt_dispatch_init", (unsigned long long)pc);
    const char *why = (pc & 3) ? "misaligned address"
                    : module_at(pc) ? "not a function start"
                    : "outside every module, no host function registered";
    rt_fatal("rt_dispatch: no function at guest pc %s: %s (lr %s)", rt_guest_name(pc, at, sizeof at), why,
             rt_guest_name(c->x[30], lr, sizeof lr));
}

// Host functions, and the fatal error. A separate tail call keeps the fast path below free of a stack frame.
__attribute__((noinline)) static void dispatch_slow(Cpu *c) {
    GuestFn f = g_ready ? host_lookup(c->pc) : NULL;
    if (!f) rt_sj_resume(c);   // a guest longjmp back into a live setjmp owner does not return (sjlj.c)
    if (!f) dispatch_fail(c);
    __attribute__((musttail)) return f(c);
}

// A pending preempt request at an indirect call (a safe point: the guest state is all in Cpu). Out of line, so
// rt_dispatch keeps no stack frame.
static __attribute__((noinline)) void dispatch_preempt(Cpu *c) {
    kern_checkpoint();
    __attribute__((musttail)) return rt_dispatch(c);
}

// Jump to the guest address in c->pc. Always a tail call, so chains of indirect jumps keep the host stack flat. A
// pending preempt request costs two loads and a branch to check.
void rt_dispatch(Cpu *c) {
    if (__builtin_expect(c->thread && kern_preempt_pending(c->thread), 0))
        __attribute__((musttail)) return dispatch_preempt(c);
    uint64_t i = __builtin_rotateright64(c->pc - D.lo, 2);
    uint32_t off = i < D.slots ? D.slot[i] : 0;
    if (__builtin_expect(!off, 0)) __attribute__((musttail)) return dispatch_slow(c);
    GuestFn f = (GuestFn)(D.anchor + off);
    __attribute__((musttail)) return f(c);
}

RtDispatchStats rt_dispatch_stats(void) {
    return (RtDispatchStats){D.lo, D.lo + D.slots * 4, D.slots, g_table_bytes, g_functions, g_nhost};
}
