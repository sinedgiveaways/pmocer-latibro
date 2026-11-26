#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- aliases (ORBITALS_TEX_ALIAS)
// Bytes per block and block size of the image's VkFormat (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
// VkFormat substitutes another one of unknown size.
static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc7) { *bw = *bh = 4; return e->f.astc_bc7 == 2 ? 8 : 16; }   // re-encoded as BC7 or BC1 (vk_astc_bc7)
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

// The new entry e can take src's content texel for texel: colour, one sample, the same shape and storage layout, and
// image formats of one block size.
static int alias_compatible(const Tex *e, const Tex *src) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1) return 0;
    if (p->type != q->type || p->width != q->width || p->height != q->height || p->depth != q->depth) return 0;
    if (e->t.L.size != src->t.L.size || nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    return eb && eb == sb && ew == sw && eh == sh;
}

// src holds content the storage bytes do not: the GPU wrote it and, for a mirrored texture, it was not written back
// and the CPU has not rewritten the storage since (the new entry's upload then reads the newer bytes).
static int alias_has_gpu_content(const Tex *src) {
    if (!src->pub.gpu_owned) return 0;
    if (!src->pub.mirrored) return 1;
    uint32_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

// Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's
// current position, before any later command of the submission uses e. Through a buffer, since bound images share
// their memory and image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition
// cannot clobber src's bytes before they are read. 0 when no replay is recording (e is then not initialised yet).
static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    if (e->pub.drop != src->pub.drop) {   // vk_tex_image.c tex_drop: the levels do not line up; not copied
        static volatile int32_t n_drop;
        if (plat_inc32(&n_drop) <= 8)
            rt_log("vk: alias copy %#llx: levels dropped %u and %u differ: not copied", (unsigned long long)e->pub.addr,
                   e->pub.drop, src->pub.drop);
        vk_exec_inline_end(reopen);
        return 1;
    }
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(e->pub.width, lv), h = mip(e->pub.height, lv), z = mip(e->pub.depth, lv);
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

// Live aliases (entries of one storage under different descriptors) are separate images here and one memory on the
// console: whatever writes one, the others must read. alias_copy at an entry's creation covered the first use only.
// Orbitals' light grids (3D r32ui 20x20x100) are written each frame by a compute shader through a storage image
// descriptor and sampled by the lighting through another one: the sampled image kept the content of its creation
// (all 0xffffffff, every cell empty) and the colony was lit by nothing (18/11/2026). Now each entry carries the order of
// its newest content (wseq) and, when it is used, takes a newer alias's image (alias_sync).

// e got new content (texture lock held): its order, and its address re-resolved by the slots that cached it, so the
// aliases' next use in this submission syncs them.
void alias_wrote(Tex *e) {
    e->wseq = ++tex_wseq;
    e->gpu_src = 1;
    for (Tex *p = nvn_hm_get(&by_addr, &e->pub.addr); p; p = p->next_at)
        if (p != e && !p->released) {
            tex_gen_bump(e->pub.addr);
            return;
        }
}

// e is about to be used (texture lock held): if a live alias holds newer content and has e's shape, its image is copied
// into e's at the replay's current position. Aliases of another shape stay as they are (logged once each).
void alias_sync(Tex *e) {
    if (tex_alias <= 0) return;
    Tex *src = NULL;
    for (Tex *p = nvn_hm_get(&by_addr, &e->pub.addr); p; p = p->next_at) {
        // only GPU content: an alias uploaded from the storage holds bytes e can upload itself
        if (p == e || p->released || !p->gpu_src || p->wseq <= e->wseq || (src && p->wseq <= src->wseq)) continue;
        if (!alias_compatible(e, p)) {
            static uint64_t seen[16];
            static int nseen;
            int k = 0;
            while (k < nseen && seen[k] != e->pub.addr) k++;
            if (k == nseen && nseen < 16) {
                seen[nseen++] = e->pub.addr;
                rt_log("vk: texture %#llx: an alias of another shape got newer content (%ux%ux%u fmt %#x against "
                       "%ux%ux%u fmt %#x): not synced", (unsigned long long)e->pub.addr, p->t.d.width, p->t.d.height,
                       p->t.d.depth, p->t.d.format, e->t.d.width, e->t.d.height, e->t.d.depth, e->t.d.format);
            }
            continue;
        }
        src = p;
    }
    if (!src || !alias_copy(e, src)) return;
    e->wseq = src->wseq;
    e->gpu_src = 1;
    n_alias_synced++;
    if (e->pub.mirrored) {   // the content is the alias's GPU write, newer than the storage bytes
        uint32_t now;
        vk_mem_changed(e->pub.addr, e->t.L.size, 0, &now);
        e->wgen = now;
        e->uploaded = 1;
        e->pub.gpu_owned = 1;
        e->gpu_lv = ~0u;   // every level is the alias's
    }
    static int logged;
    if (!logged++)
        rt_log("vk: texture %#llx %ux%ux%u fmt %#x: took a newer alias's content (first alias sync)",
               (unsigned long long)e->pub.addr, e->t.d.width, e->t.d.height, e->t.d.depth, e->t.d.format);
}

// The entries at a storage address, chained by next_at (ui/glyphs/glyphs.c re-uploads all of them). Texture lock held.
Tex *entries_at(uint64_t addr) { return nvn_hm_get(&by_addr, &addr); }

Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if (!e) {
        if (tex_alias < 0) {
            tex_alias = cfg_bool("ORBITALS_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        if (!(e = create_tex(t, &k, tex_alias))) return NULL;
        perf_count(PERF_CNT_TEX_NEW);
        // released entries of another descriptor at this address are replaced by the new one; a live one holding
        // GPU-written content is the alias source, preferring the one that differs only in swizzle (same_but_swizzle)
        int alias = 0;
        Tex *src = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr), *nx; p; p = nx) {
            nx = p->next_at;
            if (p->released) { destroy_idle(p); continue; }
            alias = 1;
            if (tex_alias && (!src || (same_but_swizzle(&p->key, &k) && !same_but_swizzle(&src->key, &k))) &&
                alias_compatible(e, p) && alias_has_gpu_content(p))
                src = p;
        }
        if (alias) perf_count(PERF_CNT_TEX_ALIAS);
        if (tex_alias) {
            if (src && alias_copy(e, src)) {
                n_alias_copied++;
                e->wseq = src->wseq, e->gpu_src = 1;
                if (e->pub.mirrored) {   // the content is src's GPU render, newer than the storage bytes
                    vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
                    e->uploaded = 1;
                    e->pub.gpu_owned = 1;
                    e->gpu_lv = ~0u;
                }
            } else {
                init_image(e, vk_upload_begin());
                vk_upload_end();
                if (alias) {
                    if (e->pub.mirrored) n_alias_upload++;
                    else n_alias_uninit++;
                }
            }
        }
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        tex_gen_bump(k.addr);
    }
    if (e->released) e->released = 0, nreleased--, rel_unlink(e), tex_gen_bump(k.addr);
    e->t = *t;
    return e;
}

// vk_dump: upload bookkeeping of an image vk_texture_get / vk_texture_by_id returned. pages_written: whether its
// storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;   // pub is the first member
    uint32_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

// What the guest memory behind a mirrored texture holds now, as 32-bit words (layout-independent): how many are all
// ones, zero and other, and the first other one. Compared with the image's own statistics in a frame dump it tells a
// texture the CPU never filled from one whose new contents the upload missed. 0 when not mirrored or larger than 16 MiB.
int vk_texture_guest_words(const VkTexImage *img, uint32_t *ones, uint32_t *zeros, uint32_t *other, uint32_t *first) {
    lock();
    const Tex *e = (const Tex *)img;
    const uint8_t *m = e->pub.mirrored ? (const uint8_t *)e->t.mem : NULL;
    uint64_t n = m ? e->t.L.size / 4 : 0;
    *ones = *zeros = *other = *first = 0;
    if (n > (16u << 20) / 4) n = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t w;
        memcpy(&w, m + 4 * i, 4);
        if (w == 0xffffffffu) ++*ones;
        else if (!w) ++*zeros;
        else if (!(*other)++) *first = w;
    }
    unlock();
    return n != 0;
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e), e->seen = submit_epoch;
    if (e && vk_guard_on()) vk_guard_note(&e->use);
    unlock();
    return e ? &e->pub : NULL;
}

// Small render targets and pitch textures the CPU reads without NVN's invalidate (vk_texture_write_back_small);
// small_pending: one may be dirty.
int small_pending;

// Without creating an entry (entry_of would: a capture asked about every registered texture and made images for
// thousands nobody used).
int vk_texture_capture_state(const tex_t *t, uint32_t recent) {
    if (!t || !t->mem) return 0;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    int r = 0;
    if (e && !e->released && e->pub.image) {
        if (e->pub.gpu_owned) r |= VK_TEXCAP_GPU;
        if (vk_sparse_is(e->pub.image)) r |= VK_TEXCAP_SPARSE;
        if (e->checked && (uint32_t)(submit_epoch - e->checked) <= recent) r |= VK_TEXCAP_RECENT;
    }
    unlock();
    return r;
}

// levels: the NVN levels written, a bit each (all ones: any, as a render, clear or storage image may write).
static void gpu_wrote(const tex_t *t, uint32_t levels, uintptr_t caller) {
    if (!t || !t->mem) return;
    if (tracing((uintptr_t)t->mem)) {   // with the replay function that wrote it (copies, clears, render targets)
        // the caller as an offset in the executable (addr2line -f -e build/orbitals-recomp)
        rt_log("vk: texture %#llx %ux%ux%u fmt %#x written on the GPU (levels %#x), caller build/orbitals-recomp+%#llx",
               (unsigned long long)trace_addr, t->d.width, t->d.height, t->d.depth, t->d.format, levels,
               (unsigned long long)(caller - plat_exe_base()));
    }
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);   // a partial render keeps what the storage held before
        e->gpu_lv = e->pub.gpu_owned ? e->gpu_lv | levels : levels;
        e->pub.gpu_owned = 1;
        e->gpu_caller = caller - plat_exe_base();
        alias_wrote(e);
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);   // CPU writes up to here are older than this render
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            if (wb_at_fence(&e->t.d, e->t.L.size)) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

void vk_texture_gpu_written(const tex_t *t) { gpu_wrote(t, ~0u, (uintptr_t)__builtin_return_address(0)); }

void vk_texture_gpu_written_level(const tex_t *t, uint32_t level) {
    gpu_wrote(t, level < 32 ? 1u << level : ~0u, (uintptr_t)__builtin_return_address(0));
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    if (e && !e->released) {
        e->released = 1;
        nreleased++;
        rel_append(e);
        tex_gen_bump(k.addr);
        sweep_released();
    }
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    unlock();
}

