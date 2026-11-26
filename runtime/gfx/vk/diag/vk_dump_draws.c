// Part of the frame dump (vk_dump.c): ORBITALS_DUMP_DRAWS, a copy of the first colour target after each draw of a selected
// run of passes. ORBITALS_DUMP_DRAWS=<from>-<to> or =<16 hex chars of an fs hash> (diagnosis, with ORBITALS_DUMP or an ORBITALS_CTL
// dump): from the first draw of a pass in that range, or the first draw with that fragment shader, every draw until the
// targets change also copies the first colour target (draw_<n>_t0_....png, one line per draw in passes.txt). A range or
// a shader, because pass numbers move between frames (an fs that draws once a frame just before the race's particles,
// drift7). Copies split and resume rendering with LOAD, so they observe the just-recorded draw; the passes those splits
// end copy no targets (the snapshots stand for them). ORBITALS_DUMP_DRAWS_RECT=x,y,w,h copies only that rectangle of the
// target, so the DRAW_CAP snapshots of a long run (the race's particles, ~120 draws) fit in INFLIGHT_CAP.
// Each selected draw also copies its buffer textures (up to 64 KiB each; Niagara's particle and ribbon data): the GPU's
// bytes to drawbuf_<n>_<stage>_c<slot>_<offset>_f<nvn format>_<texels>.bin, the first dwords to drawbufs.txt
// (vk_dump_buf.c buf_copy_named). Colony fire of 25/11/2026: an arc ribbon writes 40000 where the scene holds a few units;
// the buffers show whether its vertex shader was fed garbage. The draw's arguments go to a "draw args" line in passes.txt,
// its index buffer to drawbuf_<n>_ib_i<bytes per index>.bin and its indirect arguments to drawbuf_<n>_ind.bin.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"

#define DRAW_CAP 256                   // draw snapshots per dump
static int draw_lo = -1, draw_hi = -1; // ORBITALS_DUMP_DRAWS=<from>-<to>: passes copied after every draw (-1: none)
static uint32_t draw_rect[4];          // ORBITALS_DUMP_DRAWS_RECT: x, y, w, h of the snapshots (w 0: the whole target)
static int draw_n;                     // snapshots in the selected run
static uint64_t draw_fs[16];           // or the pass that draws with one of these fragment shaders (none: ndraw_fs 0)
static int ndraw_fs;
static int draw_run_on;                // latched through replay's splits of the selected pass
static uint64_t draw_targets;          // targets of that pass, to stop at the next pass
static _Thread_local int draw_pending; // this thread recorded a draw selected for a snapshot

void draws_init(void) {
    const char *dp = cfg_get("ORBITALS_DUMP_DRAWS");
    if (dp && strlen(dp) >= 16 && (dp[16] == 0 || dp[16] == ',')) {
        // fragment shaders, not a pass range: "H" or "H,H,..." (16 hex digits each, up to 16): the run starts at the first
        // draw by any of them, so a list of every material of the translucent pass starts it at the pass's first draw
        for (const char *q = dp; *q && ndraw_fs < 16;) {
            char *end;
            uint64_t h = strtoull(q, &end, 16);
            if (end == q) break;
            draw_fs[ndraw_fs++] = h;
            if (*end != ',') break;
            q = end + 1;
        }
    } else if (dp && sscanf(dp, "%d-%d", &draw_lo, &draw_hi) == 2 && draw_hi - draw_lo > 64) draw_lo = draw_hi = -1;
    const char *dr = cfg_get("ORBITALS_DUMP_DRAWS_RECT");
    if (dr && sscanf(dr, "%u,%u,%u,%u", &draw_rect[0], &draw_rect[1], &draw_rect[2], &draw_rect[3]) != 4) draw_rect[2] = 0;
    if (!draw_rect[3]) draw_rect[2] = 0;
}

void draws_reset(void) { draw_n = draw_run_on = 0; }

// 24, not 12: the burn ribbon (M_VFX_GrowingPlant_Burn) binds 7 uniform blocks and 8 buffer textures, and with 12 its
// last three tables were never copied (dump_feu12/13), so its vertex shader could not be replayed offline from one dump
#define PEND_BUFS 24
static _Thread_local struct { uint64_t addr, bytes; char tag[48]; } pend_buf[PEND_BUFS];   // the selected draw's buffer textures
static _Thread_local int pend_nbuf;

// The selected draw's arguments (vk_dump_draw_args). With the buffers above they let a script rebuild the draw's
// triangles offline (tools: glslvm on the translated vertex shader): run on vertex ids 0..8191 the burn ribbon's shader
// gives small flames for most ids and points thousands of pixels away for others (dump_feu13), so which ids the index
// buffer holds decides whether the huge yellow triangle comes from the indices or from the shader.
static _Thread_local struct {
    const char *kind;
    int64_t count, inst, base;
    uint64_t ibuf, ind;
    uint32_t isize, indbytes;
} pend_args;

void vk_dump_draw_args(const char *kind, int64_t count, int64_t inst, int64_t base, uint64_t ibuf, uint32_t isize,
                       uint64_t ind, uint32_t indbytes) {
    if (!draw_pending) return;
    pend_args.kind = kind, pend_args.count = count, pend_args.inst = inst, pend_args.base = base;
    pend_args.ibuf = ibuf, pend_args.isize = isize, pend_args.ind = ind, pend_args.indbytes = indbytes;
}

// Copies [addr, addr + want) or, want 0, as much of up to `cap` bytes from addr as one backed span holds
static const char *copy_upto(VkExec *ex, uint64_t addr, uint64_t want, uint64_t cap, const char *name, uint64_t *got) {
    VkMemSpan s;
    *got = 0;
    if (!addr || !vk_mem_resolve(addr, 1, &s)) return "not backed";
    uint64_t n = want ? want : cap;
    if (n > s.avail) n = s.avail;
    if (n > cap) n = cap;
    *got = n;
    return buf_copy_named(ex, addr, n, name);
}

int draws_pending(void) { return draw_pending; }

void draws_note_buf(uint64_t addr, uint64_t bytes, const char *tag) {
    if (!draw_pending || pend_nbuf >= PEND_BUFS) return;
    pend_buf[pend_nbuf].addr = addr, pend_buf[pend_nbuf].bytes = bytes;
    snprintf(pend_buf[pend_nbuf++].tag, sizeof pend_buf[0].tag, "%s", tag);
}

void draws_select(VkExec *ex, const VkPipeState *s, int pass) {
    draw_pending = 0;
    pend_nbuf = 0;
    pend_args.kind = NULL;
    if (frame_now() == dump_frame && !draw_run_on) {
        int match = pass >= draw_lo && pass <= draw_hi;
        if (ndraw_fs) {
            uint64_t fh = shader_hash(s->fs);
            for (int k = 0; k < ndraw_fs; k++) match |= draw_fs[k] == fh;
        }
        if (match) draw_run_on = 1, draw_targets = targets_sig(ex);   // draw_n goes on: a later run must not reuse names
    }
    if (draw_run_on && frame_now() == dump_frame) draw_pending = 1;
}

const char *draws_pass_end(uint64_t sig, const char *skip) {
    if (draw_run_on && sig != draw_targets) draw_run_on = 0;
    if (draw_run_on && !skip) return "draw snapshots instead";   // a split of the run: its copies stand for it
    return skip;
}

// The caller closes rendering after the actual draw before copying; image-to-buffer copies inside dynamic rendering
// are invalid and otherwise produce a plausible-looking but unchanged PNG.
void vk_dump_draw_after(VkExec *ex) {
    plat_mutex_lock(&rec_lock);
    if (!draw_pending || !draw_run_on || frame_now() != dump_frame || draw_n >= DRAW_CAP) {
        draw_pending = 0;
        plat_mutex_unlock(&rec_lock);
        return;
    }
    draw_pending = 0;
    int n = draw_n++;
    plat_mutex_unlock(&rec_lock);
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return;
    for (int i = 0; i < pend_nbuf; i++) {   // the draw's buffer textures: what its vertex shader read
        char bn[120];
        snprintf(bn, sizeof bn, "drawbuf_%03d_%s", n, pend_buf[i].tag);
        const char *why = buf_copy_named(ex, pend_buf[i].addr, pend_buf[i].bytes, bn);
        if (why) {
            plat_mutex_lock(&rec_lock);
            fprintf(passes_f, "    draw buffer %s skipped: %s\n", bn, why);
            plat_mutex_unlock(&rec_lock);
        }
    }
    pend_nbuf = 0;
    if (pend_args.kind) {   // the draw's arguments, index buffer and indirect arguments (GPU's bytes, after the draw)
        char bn[120], line[400];
        uint64_t ibytes = 0, abytes = 0;
        const char *iwhy = NULL, *awhy = NULL;
        if (pend_args.ibuf) {
            snprintf(bn, sizeof bn, "drawbuf_%03d_ib_i%u", n, pend_args.isize);
            uint64_t want = pend_args.ind ? 0 : (uint64_t)(pend_args.count > 0 ? pend_args.count : 0) * pend_args.isize;
            iwhy = copy_upto(ex, pend_args.ibuf, want, 256 << 10, bn, &ibytes);
        }
        if (pend_args.ind) {
            snprintf(bn, sizeof bn, "drawbuf_%03d_ind", n);
            awhy = copy_upto(ex, pend_args.ind, pend_args.indbytes, 4096, bn, &abytes);
        }
        snprintf(line, sizeof line, "    draw args %s count %lld instances %lld base %lld ibuf %#llx (%u-byte, %llu bytes copied%s%s) "
                 "ind %#llx (%llu bytes copied%s%s)\n", pend_args.kind, (long long)pend_args.count, (long long)pend_args.inst,
                 (long long)pend_args.base, (unsigned long long)pend_args.ibuf, pend_args.isize, (unsigned long long)ibytes,
                 iwhy ? ": " : "", iwhy ? iwhy : "", (unsigned long long)pend_args.ind, (unsigned long long)abytes,
                 awhy ? ": " : "", awhy ? awhy : "");
        plat_mutex_lock(&rec_lock);
        fputs(line, passes_f);
        plat_mutex_unlock(&rec_lock);
        pend_args.kind = NULL;
    }
    const VkExecTarget *t = &ex->color[0];
    if (!t->image || t->samples > 1 || t->is3d) {
        vk_exec_inline_end(reopen);
        return;
    }
    uint32_t x = 0, y = 0, w = t->width, h = t->height;
    if (draw_rect[2] && draw_rect[0] < t->width && draw_rect[1] < t->height) {   // clamped to the target
        x = draw_rect[0], y = draw_rect[1];
        w = draw_rect[2] < t->width - x ? draw_rect[2] : t->width - x;
        h = draw_rect[3] < t->height - y ? draw_rect[3] : t->height - y;
    }
    char fn[16], name[160];
    snprintf(name, sizeof name, "draw_%03d_t0_%ux%u_%d_%s", n, w, h, (int)t->format, fmt_name(t->format, fn));
    const char *why = copy_region(ex, t->image, t->format, t->aspect, t->level, t->base_layer,
                                  (VkOffset3D){(int32_t)x, (int32_t)y, 0}, (VkExtent3D){w, h, 1}, w, h, 1, name);
    plat_mutex_lock(&rec_lock);
    fprintf(passes_f, "    draw snapshot %s.png at %u,%u of %ux%u%s%s\n", name, x, y, t->width, t->height,
            why ? " skipped: " : "", why ? why : "");
    plat_mutex_unlock(&rec_lock);
    vk_exec_inline_end(reopen);
}
