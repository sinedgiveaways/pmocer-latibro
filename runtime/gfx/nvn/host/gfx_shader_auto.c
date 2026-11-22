// Automatic translation of the programs a run dumps: tools/prepare/dumped_shaders.py in a child process, on a thread
// of its own, a few seconds after the last dump; when it is done, gfx_shader_gen() moves on and the replay looks the
// programs it had found without SPIR-V up again (vk_exec_draw.c cached_shader), so they draw in the same run.
//
// Why: Orbitals' programs are in Unreal containers the setup does not read, so a fresh installation has no SPIR-V. The
// player had to run dumped_shaders.py by hand after every run, and an installation that lost its spirv/ drew a black
// screen with sound (21/11/2026). The programs and their translation stay on the player's machine: they are the game's
// and are not distributed (AGENTS.md).
//
// ORBITALS_SHADERS_AUTO=0 turns it off. The child gets a quarter of the processors (at least one) so the game keeps the
// rest; its output is kept in logs/shaders-auto.log and its summary lines go into the run's log.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define QUIET_MS 3000   // after the last dump: the boot and a level's load dump in bursts

static volatile uint32_t g_gen;            // translations finished; cached misses older than this look again
static volatile uint32_t g_dumped;         // programs met without SPIR-V so far (each once)
static volatile uint64_t g_last_dump_ms;
static char g_dump_dir[1100];
static PlatOnce g_once = PLAT_ONCE_INIT;
static int g_on;

uint32_t gfx_shader_gen(void) { return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE); }

// The package's Python and dumped_shaders.py beside the executable (as the launcher's setup finds prepare.py), else a
// source checkout's (the executable in build/). 0 when neither is there.
static int find_tool(char *python, char *script, size_t cap) {
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *slash = strrchr(exe, '/'), *bslash = strrchr(exe, '\\');
    if (bslash > slash) slash = bslash;
    if (!slash) return 0;
    *slash = 0;
#ifdef _WIN32
    snprintf(python, cap, "%s/setup/python/python.exe", exe);
#else
    snprintf(python, cap, "%s/setup/python/bin/python3", exe);
#endif
    snprintf(script, cap, "%s/setup/src/tools/prepare/dumped_shaders.py", exe);
    if (plat_file_exists(python) && plat_file_exists(script)) return 1;
#ifdef _WIN32
    snprintf(python, cap, "%s/../.venv/Scripts/python.exe", exe);
#else
    snprintf(python, cap, "%s/../.venv/bin/python3", exe);
#endif
    snprintf(script, cap, "%s/../tools/prepare/dumped_shaders.py", exe);
    return plat_file_exists(python) && plat_file_exists(script);
}

// One pass of dumped_shaders.py over the dump folder; 1 when it ran to its end.
static int translate_once(const char *python, const char *script) {
    char jobs[16], log_path[1200];
    uint32_t cpus = plat_cpu_count();
    snprintf(jobs, sizeof jobs, "%u", cpus >= 8 ? cpus / 4 : 1);
    const char *game = cfg_game_dir();
    const char *args[] = {python, "-I", script, "--dumped", g_dump_dir, "--game", game, "--jobs", jobs, NULL};
    uint64_t t0 = plat_now_ms();
    SDL_Process *p = SDL_CreateProcess(args, true);
    if (!p) {
        rt_log("gfx_shader: automatic translation could not start %s: %s", python, SDL_GetError());
        return 0;
    }
    size_t n = 0;
    int code = -1;
    char *out = SDL_ReadProcess(p, &n, &code);   // until it exits
    SDL_DestroyProcess(p);
    snprintf(log_path, sizeof log_path, "%s/logs/shaders-auto.log", game);
    if (out) plat_file_write(log_path, out, n);
    // its summary: "N programs: M valid SPIR-V", "done in", "failed:"
    for (char *line = out, *nl; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        size_t k = strlen(line);
        if (k && line[k - 1] == '\r') line[k - 1] = 0;
        if (strstr(line, "valid SPIR-V") || strstr(line, "done in") || strstr(line, "failed") ||
            strstr(line, "unsupported"))
            rt_log("gfx_shader: automatic translation: %s", line);
        if (!nl) break;
    }
    SDL_free(out);
    rt_log("gfx_shader: automatic translation finished in %.1f s (exit %d)%s", (plat_now_ms() - t0) / 1000.0, code,
           code ? "; logs/shaders-auto.log has its output" : "");
    return code == 0;
}

static void worker(void *arg) {
    (void)arg;
    char python[1200], script[1200];
    if (!find_tool(python, script, sizeof python)) {
        rt_log("gfx_shader: automatic translation off: no setup/python and setup/src/tools/prepare/dumped_shaders.py "
               "beside the executable");
        return;
    }
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint32_t seen = 0;
    for (;;) {
        plat_sleep_ms(500);
        uint32_t d = __atomic_load_n(&g_dumped, __ATOMIC_ACQUIRE);
        if (d == seen || plat_now_ms() - __atomic_load_n(&g_last_dump_ms, __ATOMIC_RELAXED) < QUIET_MS) continue;
        seen = d;
        rt_log("gfx_shader: %u programs met without SPIR-V so far: translating the dumped ones in the background", d);
        if (translate_once(python, script)) __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELEASE);
    }
}

static void start(void *arg) {
    (void)arg;
    const char *e = cfg_get("ORBITALS_SHADERS_AUTO");
    g_on = !(e && !strcmp(e, "0"));
    if (!g_on) {
        rt_log("gfx_shader: automatic translation off (ORBITALS_SHADERS_AUTO=0): run dumped_shaders.py by hand");
        return;
    }
    if (!plat_thread_spawn(worker, NULL, "shader translate", 256 << 10)) {
        g_on = 0;
        rt_log("gfx_shader: automatic translation off: its thread did not start");
    }
}

// Programs already counted this run (the first 8 bytes of their key): a program the translator refuses is looked up
// again after each translation and must not start another one.
#define SEEN_N 8192
static uint64_t g_seen[SEEN_N];
static PlatMutex g_seen_lock = PLAT_MUTEX_INIT;

static int first_time(const uint8_t key[32]) {
    uint64_t k;
    memcpy(&k, key, 8);
    if (!k) k = 1;
    plat_mutex_lock(&g_seen_lock);
    uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 51), n = 0;
    while (g_seen[i] && g_seen[i] != k && n++ < SEEN_N) i = (i + 1) & (SEEN_N - 1);
    int fresh = n < SEEN_N && !g_seen[i];
    if (fresh) g_seen[i] = k;
    plat_mutex_unlock(&g_seen_lock);
    return fresh;
}

void gfx_shader_auto_dumped(const char *dump_dir, const uint8_t key[32]) {
    if (!dump_dir || !first_time(key)) return;
    if (!g_dump_dir[0]) snprintf(g_dump_dir, sizeof g_dump_dir, "%s", dump_dir);
    __atomic_store_n(&g_last_dump_ms, plat_now_ms(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_dumped, 1, __ATOMIC_RELEASE);
    plat_once(&g_once, start, NULL);
}
