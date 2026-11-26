#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"

// ---- ORBITALS_DUMP_BUF=<addr>:<size>[:<off>:<stride>:<w>:<h>]: after every pass and dispatch that is logged, the GPU
// buffer range is copied to a host buffer; when it differs from the previous copy it goes to buf_<pass>.bin, and
// bufs.txt gets the changed bytes' extent plus, with the grid arguments, one map per dword of the w x h records of
// `stride` bytes at `off` (each record a letter: its value's index in the table under the map).

uint64_t bd_addr, bd_size, bd_off, bd_stride;
uint32_t bd_w, bd_h;
static uint8_t *bd_prev;
FILE *bufs_f;

typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; int pass; } BufJob;

static void buf_grid(const uint8_t *m) {
    for (uint32_t dw = 0; dw < bd_stride / 4; dw++) {
        uint32_t vals[52];
        int nv = 0, over = 0;
        fprintf(bufs_f, "  dword %u:\n", dw);
        for (uint32_t y = 0; y < bd_h; y++) {
            char line[260];
            uint32_t x;
            for (x = 0; x < bd_w && x < 256; x++) {
                uint64_t o = bd_off + ((uint64_t)y * bd_w + x) * bd_stride + dw * 4;
                if (o + 4 > bd_size) { line[x] = ' '; continue; }
                uint32_t v;
                memcpy(&v, m + o, 4);
                int k = 0;
                while (k < nv && vals[k] != v) k++;
                if (k == nv) {
                    if (nv < 52) vals[nv++] = v;
                    else { over = 1; line[x] = '?'; continue; }
                }
                line[x] = (char)(k < 26 ? 'a' + k : 'A' + k - 26);
            }
            line[x] = 0;
            fprintf(bufs_f, "    %2u %s\n", y, line);
        }
        for (int k = 0; k < nv; k++) fprintf(bufs_f, "%s%c=%08x", k % 8 ? " " : "    ", k < 26 ? 'a' + k : 'A' + k - 26, vals[k]);
        fprintf(bufs_f, "%s\n", over ? " (more: ?)" : "");
    }
}

static void buf_done(void *arg) {
    BufJob *j = arg;
    vmaInvalidateAllocation(dvma(), j->alloc, 0, VK_WHOLE_SIZE);
    plat_mutex_lock(&img_lock);
    if (bufs_f) {
        uint64_t lo = bd_size, hi = 0;
        for (uint64_t i = 0; i < bd_size; i++)
            if (!bd_prev || j->map[i] != bd_prev[i]) { if (i < lo) lo = i; hi = i + 1; }
        if (hi) {
            char path[600];
            snprintf(path, sizeof path, "%s/buf_%04d.bin", dump_dir, j->pass);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(j->map, 1, bd_size, f), fclose(f);
            fprintf(bufs_f, "after pass %04d: bytes %#llx..%#llx changed\n", j->pass, (unsigned long long)lo,
                    (unsigned long long)hi);
            if (bd_stride) buf_grid(j->map);
            fflush(bufs_f);
            if (!bd_prev) bd_prev = malloc(bd_size);
            if (bd_prev) memcpy(bd_prev, j->map, bd_size);
        }
    }
    plat_mutex_unlock(&img_lock);
    vmaDestroyBuffer(dvma(), j->buf, j->alloc);
    free(j);
}

void buf_snap(VkExec *ex, int pass) {
    if (!bd_size) return;
    VkMemSpan s;
    if (!vk_mem_resolve(bd_addr, bd_size, &s) || s.avail < bd_size) return;
    VK_MEM_PICK(s);   // the GPU's view of the buffer
    BufJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bd_size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(dvma(), &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return;
    }
    j->map = info.pMappedData, j->pass = pass;
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, bd_size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    vk_completion_add(ex->fence, buf_done, j);
}


// ---- buf_copy_named: the buffer textures of a draw selected by ORBITALS_DUMP_DRAWS (vk_dump_draws.c). The GPU's bytes
// of the range go to <name>.bin, and the first 16 dwords, as hex and as float, to drawbufs.txt: enough to see whether the
// particle data a ribbon's vertex shader reads is sane (a colour of 40000 against a few units) or garbage.

typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; uint64_t size; char name[120]; } NamedJob;

static void named_done(void *arg) {
    NamedJob *j = arg;
    vmaInvalidateAllocation(dvma(), j->alloc, 0, VK_WHOLE_SIZE);
    char path[600];
    snprintf(path, sizeof path, "%s/%s.bin", dump_dir, j->name);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(j->map, 1, j->size, f), fclose(f);
    snprintf(path, sizeof path, "%s/drawbufs.txt", dump_dir);
    plat_mutex_lock(&img_lock);
    if ((f = fopen(path, "a"))) {
        fprintf(f, "%s (%llu bytes):", j->name, (unsigned long long)j->size);
        for (uint64_t i = 0; i < 16 && (i + 1) * 4 <= j->size; i++) {
            uint32_t v;
            float fv;
            memcpy(&v, j->map + i * 4, 4);
            memcpy(&fv, &v, 4);
            fprintf(f, " %08x/%g", v, (double)fv);
        }
        fputc('\n', f);
        fclose(f);
    }
    plat_mutex_unlock(&img_lock);
    vmaDestroyBuffer(dvma(), j->buf, j->alloc);
    free(j);
}

const char *buf_copy_named(VkExec *ex, uint64_t addr, uint64_t size, const char *name) {
    VkMemSpan s;
    if (!size || !vk_mem_resolve(addr, size, &s) || s.avail < size) return "not backed";
    VK_MEM_PICK(s);   // the GPU's view of the buffer
    NamedJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(dvma(), &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return "host buffer allocation failed";
    }
    j->map = info.pMappedData, j->size = size;
    snprintf(j->name, sizeof j->name, "%s", name);
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    vk_completion_add(ex->fence, named_done, j);
    return NULL;
}


// ---- ORBITALS_SHADOW_WATCH_CS (vk_exec_bind.c): the texel buffers of the watched compute program's dispatches, as the
// GPU had them right before and right after each one, kept in memory for the last CSW_RING copies and written to
// <game>/logs/cswatch_f<frame>_d<dispatch>_<before|after>_<address>.bin when an indbad line (vk_ind_read.c) names a
// frame. Niagara's DrawIndirectArgsGen turns the instance counts and its task list into the particle draws' arguments;
// a spark draw still asked for ~2*10^9 instances with the buffer images typed right (logs of 26/11/2026): which of its
// inputs was wrong in that frame, the counts or the tasks, or its output alone?
#define CSW_RING 96
#define CSW_BUFS 8
typedef struct {
    VkBuffer buf;
    VmaAllocation alloc;
    const uint8_t *map;
    uint64_t addr, size, frame, disp;
    int after, done, used;
} CswCopy;
static CswCopy csw[CSW_RING];
static int csw_next;
static uint64_t csw_disp;
static PlatMutex csw_lock = PLAT_MUTEX_INIT;
static _Thread_local struct { uint64_t addr, size; } csw_note[CSW_BUFS];
static _Thread_local int csw_n;

void vk_cswatch_reset(void) { csw_n = 0; }

void vk_cswatch_note(uint64_t addr, uint64_t size) {
    for (int i = 0; i < csw_n; i++)
        if (csw_note[i].addr == addr) return;
    if (csw_n < CSW_BUFS && size && size <= (1u << 20)) csw_note[csw_n].addr = addr, csw_note[csw_n++].size = size;
}

// The count buffer (the watched program's first storage texel buffer) and every binding over it since: a ring of the
// dispatches and draws whose writable buffers overlap it, dumped when a count slot turns into float bits. In logs of
// 26/11/2026 slot 0x38 held 0xbf800079 (-1.0f, plus 121 particles) for frames before an emitter took it over.
static uint64_t csw_cnt_lo, csw_cnt_hi;
#define CSW_WR 4096
static struct { uint64_t frame, disp, addr, size; uint8_t key[8]; uint8_t kind; } csw_wr[CSW_WR];
static uint64_t csw_wr_n;
static uint32_t *csw_prev;   // the counts after the watched program's last dispatch
static uint64_t csw_prev_frame, csw_prev_disp;
static int csw_reports;

void vk_cswatch_counts(uint64_t lo, uint64_t hi) { csw_cnt_lo = lo, csw_cnt_hi = hi; }

void vk_cswatch_bindnote(uint64_t addr, uint64_t size, const uint8_t *key, int kind) {
    if (!csw_cnt_hi || addr >= csw_cnt_hi || addr + size <= csw_cnt_lo) return;
    plat_mutex_lock(&csw_lock);
    uint64_t i = csw_wr_n++ % CSW_WR;
    csw_wr[i].frame = vk_exec_presented, csw_wr[i].disp = csw_disp, csw_wr[i].addr = addr, csw_wr[i].size = size;
    csw_wr[i].kind = (uint8_t)kind;
    if (key) memcpy(csw_wr[i].key, key, 8);
    else memset(csw_wr[i].key, 0, 8);
    plat_mutex_unlock(&csw_lock);
}

static int floaty(uint32_t v) { return v >= 0x10000000u && v < 0xfff00000u; }   // no count this big; -1..-1M excluded

static void csw_check(const CswCopy *c) {   // csw_lock held, completion thread, fence order
    if (c->addr != csw_cnt_lo || c->size != csw_cnt_hi - csw_cnt_lo) return;
    const uint32_t *w = (const uint32_t *)c->map;
    uint32_t n = (uint32_t)(c->size / 4);
    if (csw_prev && csw_reports < 8) {
        char slots[300];
        int o = 0, bad = 0;
        for (uint32_t k = 0; k < n; k++)
            if (floaty(w[k]) && !floaty(csw_prev[k])) {
                bad++;
                if (o < (int)sizeof slots - 40)
                    o += snprintf(slots + o, sizeof slots - (size_t)o, " %#x: %#x -> %#x", k, csw_prev[k], w[k]);
            }
        if (bad) {
            csw_reports++;
            rt_log("cswatch: %d count slots turned into float bits between dispatch %llu (frame %llu, after) and %llu "
                   "(frame %llu, %s):%s", bad, (unsigned long long)csw_prev_disp, (unsigned long long)csw_prev_frame,
                   (unsigned long long)c->disp, (unsigned long long)c->frame, c->after ? "after" : "before", slots);
            uint64_t from = csw_wr_n > CSW_WR ? csw_wr_n - CSW_WR : 0;
            for (uint64_t i = from; i < csw_wr_n; i++) {
                const typeof(csw_wr[0]) *e = &csw_wr[i % CSW_WR];
                if (e->frame + 1 < csw_prev_frame || e->frame > c->frame) continue;
                if (e->disp < csw_prev_disp || e->disp > c->disp) continue;
                rt_log("cswatch:   frame %llu after watched dispatch %llu: %s %02x%02x%02x%02x%02x%02x%02x%02x writes "
                       "[%#llx, %#llx)", (unsigned long long)e->frame, (unsigned long long)e->disp,
                       e->kind == 0 ? "cs texel" : e->kind == 1 ? "cs ssbo" : e->kind == 2 ? "gfx texel" : "gfx ssbo",
                       e->key[0], e->key[1], e->key[2], e->key[3], e->key[4], e->key[5], e->key[6], e->key[7],
                       (unsigned long long)e->addr, (unsigned long long)(e->addr + e->size));
            }
        }
    }
    if (!csw_prev) csw_prev = malloc(c->size);
    if (csw_prev) memcpy(csw_prev, w, c->size), csw_prev_frame = c->frame, csw_prev_disp = c->disp;
}

static void csw_done(void *arg) {   // completion thread
    CswCopy *c = arg;
    plat_mutex_lock(&csw_lock);
    if (c->buf) vmaInvalidateAllocation(dvma(), c->alloc, 0, VK_WHOLE_SIZE), c->done = 1, csw_check(c);
    plat_mutex_unlock(&csw_lock);
}

void vk_cswatch_copy(VkExec *ex, int after) {
    if (!csw_n) return;
    plat_mutex_lock(&csw_lock);
    if (!after) csw_disp++;
    uint64_t disp = csw_disp;
    plat_mutex_unlock(&csw_lock);
    for (int i = 0; i < csw_n; i++) {
        VkMemSpan s;
        if (!vk_mem_resolve(csw_note[i].addr, csw_note[i].size, &s) || s.avail < csw_note[i].size) continue;
        VK_MEM_PICK(s);   // the GPU's view (the VRAM mirror when there is one)
        plat_mutex_lock(&csw_lock);
        CswCopy *c = &csw[csw_next];
        if (c->used && !c->done) {   // its copy not back yet: the ring is too short for this rate, skip
            plat_mutex_unlock(&csw_lock);
            continue;
        }
        csw_next = (csw_next + 1) % CSW_RING;
        if (c->buf) vmaDestroyBuffer(dvma(), c->buf, c->alloc);
        memset(c, 0, sizeof *c);
        plat_mutex_unlock(&csw_lock);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = csw_note[i].size,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VkBuffer buf;
        VmaAllocation alloc;
        if (vmaCreateBuffer(dvma(), &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS || !info.pMappedData) continue;
        VkMemoryBarrier2 mb[2] = {
            {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
             .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
            {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
        vkCmdPipelineBarrier2(ex->cb, &di);
        VkBufferCopy cp = {s.offset, 0, csw_note[i].size};
        vkCmdCopyBuffer(ex->cb, s.buffer, buf, 1, &cp);
        di.pMemoryBarriers = &mb[1];
        vkCmdPipelineBarrier2(ex->cb, &di);
        ex->recorded += 3;
        plat_mutex_lock(&csw_lock);
        *c = (CswCopy){buf, alloc, info.pMappedData, csw_note[i].addr, csw_note[i].size, vk_exec_presented, disp, after, 0, 1};
        plat_mutex_unlock(&csw_lock);
        vk_completion_add(ex->fence, csw_done, c);
    }
}

void vk_cswatch_flush(uint64_t frame) {
    const char *dir = cfg_get("ORBITALS_GAME_DIR");
    int n = 0;
    plat_mutex_lock(&csw_lock);
    for (int i = 0; i < CSW_RING; i++) {
        CswCopy *c = &csw[i];
        if (!c->used || !c->done || c->frame + 2 < frame || c->frame > frame) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/logs/cswatch_f%llu_d%llu_%s_%llx.bin", dir ? dir : ".",
                 (unsigned long long)c->frame, (unsigned long long)c->disp, c->after ? "after" : "before",
                 (unsigned long long)c->addr);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(c->map, 1, c->size, f), fclose(f), n++;
        c->done = 2;   // written once
        c->used = 1;
    }
    plat_mutex_unlock(&csw_lock);
    rt_log("vk: cswatch: %d copies of the watched dispatches' buffers (frames %llu-%llu) written to logs/cswatch_*.bin", n,
           (unsigned long long)(frame > 2 ? frame - 2 : 0), (unsigned long long)frame);
}
