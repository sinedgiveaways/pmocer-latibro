// Temporary Crash B instrumentation for the 1.2.1 main module.
// It is inert unless ORBITALS_CRASH_B_PROBE=1 is present in the process environment.
// The generated call spills its register locals before entering here, so Cpu contains the live guest state.
#include "host.h"
#include "platform.h"

static const uint64_t probe_pcs[] = {
    0x700174E180ull, 0x700174E20Cull, 0x700174E4A8ull,
    0x700175A01Cull, 0x700175A030ull
};
static unsigned probe_hits[sizeof probe_pcs / sizeof probe_pcs[0]];
static int probe_enabled = -2;

static int crash_b_enabled(void) {
    int state = __atomic_load_n(&probe_enabled, __ATOMIC_ACQUIRE);
    if (state != -2) return state;
    char value[16] = {0};
    int enabled = plat_getenv("ORBITALS_CRASH_B_PROBE", value, sizeof value) != 0 &&
                  value[0] != '0';
    int expected = -2;
    __atomic_compare_exchange_n(&probe_enabled, &expected, enabled ? 1 : 0, 0,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return __atomic_load_n(&probe_enabled, __ATOMIC_ACQUIRE);
}

static int probe_index(uint64_t pc) {
    for (unsigned i = 0; i < sizeof probe_pcs / sizeof probe_pcs[0]; i++)
        if (probe_pcs[i] == pc) return (int)i;
    return -1;
}

static void dump_words(const char *tag, uint64_t addr, size_t bytes) {
    if (!addr) {
        rt_logf("  [probe] %s: null\n", tag);
        return;
    }
    for (size_t off = 0; off < bytes; off += 8) {
        uint64_t value = 0;
        if (addr > UINT64_MAX - off || !plat_mem_read(addr + off, &value, sizeof value)) {
            rt_logf("  [probe] %s+%03zx @%#llx: unreadable\n", tag, off,
                    (unsigned long long)(addr + off));
            break;
        }
        rt_logf("  [probe] %s+%03zx @%#llx = %016llx\n", tag, off,
                (unsigned long long)(addr + off), (unsigned long long)value);
    }
}

void rt_crash_b_probe(Cpu *c, uint64_t pc) {
    if (!c || !crash_b_enabled()) return;
    int index = probe_index(pc);
    if (index < 0) return;
    unsigned hit = __atomic_fetch_add(&probe_hits[index], 1, __ATOMIC_RELAXED);
    // Producer sites are expected to be relatively sparse; the consumer may iterate many records.
    unsigned limit = index >= 3 ? 256u : 64u;
    if (hit >= limit) return;

    rt_logf("\n[CRASH_B_PROBE] pc=%#llx hit=%u thread=%p sp=%#llx\n",
            (unsigned long long)pc, hit, c->thread, (unsigned long long)c->sp);
    for (int i = 0; i < 28; i += 4)
        rt_logf("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",
                i, (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1],
                i + 2, (unsigned long long)c->x[i + 2], i + 3, (unsigned long long)c->x[i + 3]);
    rt_logf("  x28=%016llx x29=%016llx x30=%016llx\n",
            (unsigned long long)c->x[28], (unsigned long long)c->x[29],
            (unsigned long long)c->x[30]);

    if (index >= 3) {
        uint64_t slot = c->x[21] <= UINT64_MAX - 0x158 ? c->x[21] + 0x158 : 0;
        uint64_t array = 0;
        if (slot && plat_mem_read(slot, &array, sizeof array)) {
            rt_logf("  [probe] x21+0x158 slot=%#llx array=%#llx index(x20)=%#llx record(x27)=%#llx\n",
                    (unsigned long long)slot, (unsigned long long)array,
                    (unsigned long long)c->x[20], (unsigned long long)c->x[27]);
            dump_words("array-base-first-0x40", array, 0x40);
        } else {
            rt_logf("  [probe] x21+0x158 slot=%#llx unreadable; x20=%#llx x27=%#llx\n",
                    (unsigned long long)slot, (unsigned long long)c->x[20],
                    (unsigned long long)c->x[27]);
        }
        dump_words("record-x27", c->x[27], 0xE8);
    } else {
        // At the producer's copy site x24 is the source and x1 is the destination argument.
        dump_words("producer-source-x24", c->x[24], 0x20);
        dump_words("producer-destination-x1", c->x[1], 0x20);
    }
}
