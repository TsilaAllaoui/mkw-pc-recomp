// DKB sparse texture residency (M15.26). The game streams its course material arrays (8192x8192x68 ASTC 8x8 and BC6H,
// 1.4-5.8 GiB as whole images) and renders its terrain into sparse targets (16384x16384 R8, 8192x8192 RG16 / R16) by
// mapping 64 KiB pages of physical pools into virtual pools: CPU-written tiles for the arrays, GPU-written ones for the
// targets (KartNative docs/reference/rendering.md, "Sparse textures laid out in 64 KB tiles" and the entries after
// it; facts and design only, no code). Fully resident images of those sizes do not fit 8 GiB: they failed to
// allocate (black course surfaces) or spilled into system memory (stutter). Here such a texture is a Vulkan image
// with sparse residency whose blocks are bound to memory of their own, one allocation per guest page shared by every
// image that maps the same physical page in the same format (SpPage), so GPU writes through one alias show in the
// others as on the console, and unmapped tiles read zero. Never physical pool memory: the CPU never wrote it.
//
// Guest layout (nvn_tex.c nvn_tex_sparse_tile): levels at least one 64 KiB tile wide and high are whole tiles, tile
// (row, column) of a level at (row x tiles per row + column) x 64 KiB, each tile a standalone block-linear image of
// the tile's block height; the smaller levels form the mip tail, packed as usual, and each layer is whole tiles.
// A guest tile covers nx x ny Vulkan sparse blocks (BC3 for ASTC: 2x2). The guest tail levels plus Vulkan's own mip
// tail stay bound to per-texture memory and are uploaded as a unit per layer from the tail pages.
//
// Each sync reads the page mappings that changed (vk_mem_vpages_changed). A page of a CPU-visible physical pool is
// deswizzled from its storage when its SpPage is new or the CPU rewrote it; a GPU-only page starts zeroed, as the
// pools do (vk_fix_zero). Binds go to vk_upload_flush, which submits them before the upload batch. DKB leaves the
// storage of its course array and terrain pages zero and fills the tiles with texture copies and renders into the
// image, so a tile is bound in the sync of the submission that writes it, all-zero storage is cleared on the GPU,
// and a write-watch answer re-uploads a page only when its bytes changed (page_sum).
//
// Knobs: DKB_SPARSE_TEX=0 (off: the images are made as before), DKB_SPARSE_MIB (tile memory limit, default 2048 on
// cards with up to 10 GiB, else 4096; tiles past it stay unbound), DKB_SPARSE_STEP (tiles uploaded or cleared per
// submission, default 0: no limit; with one, the rest follow in the next submissions and miss the GPU's writes in
// this one), DKB_SPARSE_SKIP_FMT (NVN formats made as before, for comparison runs).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "dkb_sparse_tex.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

#define PAGE NVN_VPOOL_PAGE
#define MAX_LEVELS 16
#define ZERO_BYTES (1u << 20)   // the zero source of new GPU tiles; a guest tile's blocks fit it (dkb_sparse_create)
#define CHUNK_JOBS 256          // tiles per staging buffer

typedef struct { struct DkbSparse *s; uint64_t slot; } Binder;

typedef struct SpPage {
    uint64_t serial, off;          // the physical page
    int gfmt;                      // NVN format, VkFormat and memory types of the images it is bound into
    VkFormat vfmt;
    uint32_t memtypes;
    VmaAllocation alloc;
    VkDeviceMemory mem;
    VkDeviceSize mem_off, bytes;
    const uint8_t *host;           // CPU storage; NULL: GPU-only page
    uint32_t wgen;                 // write generation when it was last uploaded
    uint64_t sum;                  // page_sum of the storage it was last uploaded from
    uint8_t filled, dirty;         // content uploaded or cleared; CPU storage rewritten since (in dirty_q)
    Binder *b;                     // the tiles it is bound to; b[0] uploads it
    uint32_t nb, cb;
    struct SpPage *hnext;
    struct SpPool *pool;
    struct SpPage *pnext, *pprev;
} SpPage;

typedef struct SpPool {            // CPU-visible physical pool with live pages: one write check per epoch over them
    uint64_t serial;
    uintptr_t lo, hi;
    uint32_t wgen, n;
    SpPage *pages;
    struct SpPool *next;
} SpPool;

struct DkbSparse {
    VkImage image;
    tex_t t;
    TexVkFormat f;
    VkImageAspectFlags aspect;
    uint64_t addr, npages, ppl, tail_page, layer_size;
    uint32_t bpb, bw, bh, tbw, tbh, gtw, gth, vtw, vth, nx, ny, vbpb, vbw, vbh;
    unsigned sgh;
    VkDeviceSize bs;
    uint32_t memtypes;
    uint32_t width, height, levels, layers, gt, vt, ntail;
    uint64_t lvl_page[MAX_LEVELS + 1];
    uint32_t lvl_tx[MAX_LEVELS], lvl_ty[MAX_LEVELS];
    SpPage **slot;                 // per guest page of a tiled level
    const uint8_t **tail_host;     // layers x ntail: CPU storage of the mapped tail pages
    uint8_t *tail_dirty;
    uint32_t *tail_wgen;
    VmaAllocation tail_alloc;
    VkDeviceSize tail_bytes;
    uint64_t vgen;
    uint32_t epoch, usage;
    uint8_t inited, refused;
    uint64_t n_bound, n_up, n_zero_src, n_clear, n_rewrite, n_gpu;   // per-texture counters for the perf trace
    struct DkbSparse *lnext, *lprev;
};

static int sp_on = -1, sp_aliased;
static uint64_t sp_limit, sp_bytes;
static uint32_t sp_step, sp_epoch = 1, sp_used;
static SpPage **ht;
static uint32_t ht_cap, ht_n;
static SpPool *pools;
static SpPage **dirty_q;
static uint32_t ndirty, cdirty;
static VkBuffer zero_buf;
static VmaAllocation zero_alloc;
static uint64_t n_live_tex, n_live_pages;
static DkbSparse *live;
static int skip_fmt[16], n_skip_fmt = -1;
static struct {
    uint64_t created, refused_tex, tiles_new, tiles_shared, tiles_unbound, uploads, upload_bytes, zeroed, rewrites,
        rewrites_same, over_limit, deferred, tails, ns;
} st;

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t mip(uint32_t v, unsigned lv) { v >>= lv; return v ? v : 1; }
static uint32_t cdiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void grow(void **arr, uint32_t *cap, size_t elem, uint32_t need) {
    if (need <= *cap) return;
    uint32_t n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    void *p = realloc(*arr, (size_t)n * elem);
    if (!p) rt_fatal("vk: out of memory (sparse textures)");
    *arr = p, *cap = n;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static uint64_t knob_u64(const char *name, uint64_t def) {
    const char *s = cfg_get(name);
    return s && *s ? strtoull(s, NULL, 10) : def;
}

// DKB_SPARSE_SKIP_FMT=0x50,0x26: NVN formats left to the previous image creation (comparison runs).
static int format_skipped(int fmt) {
    if (n_skip_fmt < 0) {
        n_skip_fmt = 0;
        const char *s = cfg_get("DKB_SPARSE_SKIP_FMT");
        for (char *e; s && *s && n_skip_fmt < 16; s = *e ? e + 1 : e) {
            long v = strtol(s, &e, 0);
            if (e == s) break;
            skip_fmt[n_skip_fmt++] = (int)v;
        }
    }
    for (int i = 0; i < n_skip_fmt; i++)
        if (skip_fmt[i] == fmt) return 1;
    return 0;
}

static inline uint64_t rotl64(uint64_t x, int r) { return x << r | x >> (64 - r); }

// Content hash of a CPU page; 0 for all-zero storage.
static uint64_t page_sum(const uint8_t *p) {
    const uint64_t *w = (const uint64_t *)p, P = 0x9E3779B185EBCA87ull;
    uint64_t a = 0, b = 0, c = 0, d = 0;
    for (size_t i = 0; i < PAGE / 8; i += 4) {
        a = rotl64(a ^ w[i], 31) * P, b = rotl64(b ^ w[i + 1], 31) * P;
        c = rotl64(c ^ w[i + 2], 31) * P, d = rotl64(d ^ w[i + 3], 31) * P;
    }
    return a ^ rotl64(b, 17) ^ rotl64(c, 34) ^ rotl64(d, 51);
}

static int sparse_init(VkCtx *c) {
    if (sp_on >= 0) return sp_on;
    VkPhysicalDeviceFeatures ft;
    vkGetPhysicalDeviceFeatures(c->phys, &ft);
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    int queue = c->gfx_family < nq && (qp[c->gfx_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
    uint64_t used = 0, budget = 0;
    int exact = 0;
    vk_mem_vram(&used, &budget, &exact);
    sp_limit = knob_u64("DKB_SPARSE_MIB", budget && budget <= (10ull << 30) ? 2048 : 4096) << 20;
    sp_step = (uint32_t)knob_u64("DKB_SPARSE_STEP", 0);
    int want = knob_u64("DKB_SPARSE_TEX", 1) != 0;
    sp_aliased = ft.sparseResidencyAliased;
    sp_on = want && ft.sparseBinding && ft.sparseResidencyImage2D && queue;
    rt_log("DKB-SPARSE-RESIDENCY: %s (DKB_SPARSE_TEX=%d): sparse binding %d, 2D residency %d, aliased %d, "
           "non-resident strict %d, graphics queue binds %d; tile memory up to %llu MiB (DKB_SPARSE_MIB), %u tiles per "
           "submission (DKB_SPARSE_STEP, 0: no limit)", sp_on ? "on" : "off", want, ft.sparseBinding, ft.sparseResidencyImage2D,
           ft.sparseResidencyAliased, c->props.sparseProperties.residencyNonResidentStrict, queue,
           (unsigned long long)(sp_limit >> 20), sp_step);
    return sp_on;
}

// ---------------------------------------------------------------- shared tile memory
static uint32_t page_hash(uint64_t serial, uint64_t off, int gfmt, VkFormat vf, uint32_t mt) {
    uint64_t h = serial * 0x9E3779B97F4A7C15ull ^ (off >> 16) * 0xC2B2AE3D27D4EB4Full ^
                 ((uint64_t)(uint32_t)gfmt << 32 | (uint32_t)vf) * 0x165667B19E3779F9ull ^ mt;
    h ^= h >> 29;
    return (uint32_t)(h * 0xBF58476D1CE4E5B9ull >> 32);
}

static void ht_insert(SpPage *p) {
    if (ht_n + 1 > ht_cap / 2) {
        uint32_t nc = ht_cap ? ht_cap * 2 : 4096;
        SpPage **n = calloc(nc, sizeof *n);
        if (!n) rt_fatal("vk: out of memory (sparse page table)");
        for (uint32_t i = 0; i < ht_cap; i++)
            for (SpPage *q = ht[i], *nx; q; q = nx) {
                nx = q->hnext;
                uint32_t k = page_hash(q->serial, q->off, q->gfmt, q->vfmt, q->memtypes) & (nc - 1);
                q->hnext = n[k], n[k] = q;
            }
        free(ht);
        ht = n, ht_cap = nc;
    }
    uint32_t k = page_hash(p->serial, p->off, p->gfmt, p->vfmt, p->memtypes) & (ht_cap - 1);
    p->hnext = ht[k], ht[k] = p;
    ht_n++;
}

static SpPage *ht_find(uint64_t serial, uint64_t off, int gfmt, VkFormat vf, uint32_t mt) {
    if (!ht_cap) return NULL;
    for (SpPage *p = ht[page_hash(serial, off, gfmt, vf, mt) & (ht_cap - 1)]; p; p = p->hnext)
        if (p->serial == serial && p->off == off && p->gfmt == gfmt && p->vfmt == vf && p->memtypes == mt) return p;
    return NULL;
}

static void ht_remove(SpPage *p) {
    SpPage **pp = &ht[page_hash(p->serial, p->off, p->gfmt, p->vfmt, p->memtypes) & (ht_cap - 1)];
    while (*pp && *pp != p) pp = &(*pp)->hnext;
    if (*pp) *pp = p->hnext, ht_n--;
}

static void pool_add(SpPage *p) {
    SpPool *o = pools;
    while (o && o->serial != p->serial) o = o->next;
    if (!o) {
        if (!(o = calloc(1, sizeof *o))) rt_fatal("vk: out of memory (sparse pools)");
        o->serial = p->serial, o->lo = (uintptr_t)p->host, o->hi = (uintptr_t)p->host + PAGE;
        o->next = pools, pools = o;
    }
    if ((uintptr_t)p->host < o->lo) o->lo = (uintptr_t)p->host;
    if ((uintptr_t)p->host + PAGE > o->hi) o->hi = (uintptr_t)p->host + PAGE;
    p->pool = o, p->pprev = NULL, p->pnext = o->pages;
    if (o->pages) o->pages->pprev = p;
    o->pages = p, o->n++;
}

static void pool_del(SpPage *p) {
    SpPool *o = p->pool;
    if (!o) return;
    if (p->pprev) p->pprev->pnext = p->pnext;
    else o->pages = p->pnext;
    if (p->pnext) p->pnext->pprev = p->pprev;
    if (!--o->n) {
        SpPool **pp = &pools;
        while (*pp != o) pp = &(*pp)->next;
        *pp = o->next;
        free(o);
    }
    p->pool = NULL;
}

static void binder_add(SpPage *p, DkbSparse *s, uint64_t slot) {
    grow((void **)&p->b, &p->cb, sizeof *p->b, p->nb + 1);
    p->b[p->nb++] = (Binder){s, slot};
}

static void page_release(SpPage *p, const DkbSparse *s, uint64_t slot) {
    for (uint32_t i = 0; i < p->nb; i++)
        if (p->b[i].s == s && p->b[i].slot == slot) {
            memmove(p->b + i, p->b + i + 1, (p->nb - i - 1) * sizeof *p->b);
            p->nb--;
            break;
        }
    if (p->nb) return;
    if (p->dirty)
        for (uint32_t i = 0; i < ndirty; i++)
            if (dirty_q[i] == p) { dirty_q[i] = dirty_q[--ndirty]; break; }
    if (sp_aliased) ht_remove(p);
    pool_del(p);
    vk_defer_memory(p->alloc);
    sp_bytes -= p->bytes, n_live_pages--;
    free(p->b);
    free(p);
}

// The page for a mapped guest page of s at slot; *fill: its tile needs an upload (CPU) or a clear (GPU). NULL: over
// the tile memory limit or out of device memory (the tile stays unbound).
static SpPage *page_acquire(DkbSparse *s, const VkMemVPage *m, uint64_t slot, int *fill) {
    VkCtx *c = vk_ctx();
    *fill = 0;
    SpPage *p = sp_aliased ? ht_find(m->serial, m->off, s->t.d.format, s->f.vk, s->memtypes) : NULL;
    if (p) {
        binder_add(p, s, slot);
        uint32_t now;
        if (!p->filled || (p->host && !p->dirty && vk_mem_changed_snap((uintptr_t)p->host, PAGE, p->wgen, sp_epoch, &now) == 1))
            *fill = 1;
        st.tiles_shared++;
        return p;
    }
    VkDeviceSize bytes = (VkDeviceSize)s->nx * s->ny * s->bs;
    if (sp_bytes + bytes > sp_limit) { st.over_limit++; return NULL; }
    VkMemoryRequirements req = {.size = bytes, .alignment = s->bs, .memoryTypeBits = s->memtypes};
    VmaAllocationCreateInfo ai = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocation a;
    VmaAllocationInfo info;
    if (vmaAllocateMemory(c->vma, &req, &ai, &a, &info) != VK_SUCCESS) { st.over_limit++; return NULL; }
    if (!(p = calloc(1, sizeof *p))) rt_fatal("vk: out of memory (sparse pages)");
    p->serial = m->serial, p->off = m->off, p->gfmt = s->t.d.format, p->vfmt = s->f.vk, p->memtypes = s->memtypes;
    p->alloc = a, p->mem = info.deviceMemory, p->mem_off = info.offset, p->bytes = bytes, p->host = m->host;
    if (sp_aliased) ht_insert(p);
    if (p->host) pool_add(p);
    binder_add(p, s, slot);
    sp_bytes += bytes, n_live_pages++;
    st.tiles_new++;
    *fill = 1;
    return p;
}

// ---------------------------------------------------------------- geometry
// 1: slot p is a tile of level *lv, layer *ly at texel (*x, *y), *w x *h texels; 2: tail page *x of layer *ly;
// 0: padding.
static int page_kind(const DkbSparse *s, uint64_t p, uint32_t *lv, uint32_t *ly, uint32_t *x, uint32_t *y, uint32_t *w,
                     uint32_t *h) {
    *ly = (uint32_t)(p / s->ppl);
    uint64_t q = p % s->ppl;
    if (q >= s->tail_page) {
        if (q - s->tail_page >= s->ntail) return 0;
        *x = (uint32_t)(q - s->tail_page);
        return 2;
    }
    uint32_t l = 0;
    while (l + 1 < s->gt && q >= s->lvl_page[l + 1]) l++;
    uint64_t i = q - s->lvl_page[l];
    uint32_t tx = (uint32_t)(i % s->lvl_tx[l]), ty = (uint32_t)(i / s->lvl_tx[l]);
    if (ty >= s->lvl_ty[l]) return 0;
    uint32_t lw = mip(s->width, l), lh = mip(s->height, l);
    *lv = l, *x = tx * s->gtw, *y = ty * s->gth;
    *w = umin(s->gtw, lw - *x), *h = umin(s->gth, lh - *y);
    return 1;
}

static VkSparseImageMemoryBind tile_bind(const DkbSparse *s, uint64_t slot, const SpPage *p) {
    uint32_t lv = 0, ly = 0, x = 0, y = 0, w = 0, h = 0;
    page_kind(s, slot, &lv, &ly, &x, &y, &w, &h);
    return (VkSparseImageMemoryBind){.subresource = {s->aspect, lv, ly}, .offset = {(int32_t)x, (int32_t)y, 0},
                                     .extent = {w, h, 1}, .memory = p ? p->mem : VK_NULL_HANDLE,
                                     .memoryOffset = p ? p->mem_off : 0};
}

static int format_ok(VkCtx *c, const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(c->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    if (ci->extent.width > q->maxExtent.width || ci->extent.height > q->maxExtent.height ||
        ci->mipLevels > q->maxMipLevels || ci->arrayLayers > q->maxArrayLayers)
        return 0;
    uint32_t n = 0;
    vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, ci->format, ci->imageType, ci->samples, ci->usage, ci->tiling,
                                                    &n, NULL);
    return n != 0;
}

// ---------------------------------------------------------------- creation
DkbSparse *dkb_sparse_create(const tex_t *t, const TexVkFormat *f, VkImageCreateInfo *ci, VkImage *img) {
    static volatile int32_t n_log, n_why;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    uint32_t tbw, tbh;
    unsigned sgh;
    if (!c || !(d->flags & NVN_TEXTURE_FLAGS_SPARSE) || !nvn_gpu_only((uintptr_t)t->mem) ||
        !nvn_tex_sparse_tile(d, &tbw, &tbh, &sgh) || !sparse_init(c))
        return NULL;
    const nvn_texlayout *L = &t->L;
    const char *why = NULL;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint64_t addr = (uintptr_t)t->mem, stride = (L->layer_size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (format_skipped(d->format))
        why = "excluded by DKB_SPARSE_SKIP_FMT";
    else if (ci->imageType != VK_IMAGE_TYPE_2D || ci->samples != VK_SAMPLE_COUNT_1_BIT || ci->mipLevels > MAX_LEVELS)
        why = "image shape";
    else if (f->expand_rgb || f->stencil || (f->astc_bw ? !(f->astc_bc && vk_astc_gpu()) : f->vk != f->ideal))
        why = "format conversion";
    else if (addr & (PAGE - 1) || (ci->arrayLayers > 1 && stride != L->layer_size) || L->tileH != sgh || !stride)
        why = "layout";
    else if (cdiv(ci->extent.width, (uint32_t)bw) < tbw || cdiv(ci->extent.height, (uint32_t)bh) < tbh)
        why = "smaller than a tile";
    uint64_t npages = stride / PAGE * ci->arrayLayers, gen;
    if (!why && !vk_mem_vpages_changed(addr, npages, UINT64_MAX, NULL, NULL, &gen)) why = "not inside one virtual pool";
    if (why) {
        if (plat_inc32(&n_why) <= 16)
            rt_log("DKB-SPARSE-RESIDENCY: texture %#llx fmt %#x %ux%ux%u stays fully resident: %s",
                   (unsigned long long)addr, d->format, ci->extent.width, ci->extent.height, ci->arrayLayers, why);
        st.refused_tex++;
        return NULL;
    }
    DkbSparse *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("vk: out of memory (sparse texture)");
    s->t = *t, s->f = *f, s->addr = addr, s->npages = npages, s->ppl = stride / PAGE, s->layer_size = stride;
    s->bpb = (uint32_t)bpb, s->bw = (uint32_t)bw, s->bh = (uint32_t)bh, s->tbw = tbw, s->tbh = tbh, s->sgh = sgh;
    s->gtw = tbw * (uint32_t)bw, s->gth = tbh * (uint32_t)bh;
    s->width = ci->extent.width, s->height = ci->extent.height, s->levels = ci->mipLevels, s->layers = ci->arrayLayers;
    s->aspect = f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (f->astc_bw) s->vbpb = 16, s->vbw = s->vbh = 4;
    else s->vbpb = (uint32_t)bpb, s->vbw = (uint32_t)bw, s->vbh = (uint32_t)bh;
    s->gt = s->levels;
    for (uint32_t lv = 0; lv < s->levels; lv++) {
        uint32_t lwb = cdiv(mip(s->width, lv), s->bw), lhb = cdiv(mip(s->height, lv), s->bh);
        if (lwb < tbw || lhb < tbh) { s->gt = lv; break; }
        uint64_t off = nvn_tex_level_offset(d, lv);
        if (off % PAGE) why = "level offset";
        s->lvl_page[lv] = off / PAGE, s->lvl_tx[lv] = cdiv(lwb, tbw), s->lvl_ty[lv] = cdiv(lhb, tbh);
    }
    s->tail_page = nvn_tex_level_offset(d, s->gt) / PAGE;
    s->lvl_page[s->gt] = s->tail_page;
    s->ntail = s->gt < s->levels && s->tail_page < s->ppl ? (uint32_t)(s->ppl - s->tail_page) : 0;
    if (nvn_tex_level_offset(d, s->gt) % PAGE || s->tail_page > s->ppl) why = "level offset";

    VkImageCreateInfo sci = *ci;
    sci.flags = (ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) | VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | (sp_aliased ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0);
    VkImageUsageFlags attach = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageUsageFlags tries[3] = {ci->usage, ci->usage & ~VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT,
                                  ci->usage & attach ? 0 : ci->usage & ~(VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
                                                                         VK_IMAGE_USAGE_STORAGE_BIT)};
    int ok = 0;
    for (int i = 0; !why && !ok && i < 3; i++)
        if (tries[i]) sci.usage = tries[i], ok = format_ok(c, &sci);
    VkImage im = VK_NULL_HANDLE;
    if (!why && !ok) why = "the device has no sparse residency for this format and usage";
    if (!why && vkCreateImage(c->device, &sci, NULL, &im) != VK_SUCCESS) why = "vkCreateImage failed";
    VkSparseImageMemoryRequirements sr[4];
    const VkSparseImageMemoryRequirements *r = NULL;
    VkMemoryRequirements req = {0};
    if (!why) {
        vkGetImageMemoryRequirements(c->device, im, &req);
        uint32_t n = 4;
        vkGetImageSparseMemoryRequirements(c->device, im, &n, sr);
        for (uint32_t i = 0; i < n; i++) {
            if (sr[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT) why = "metadata aspect";
            if (sr[i].formatProperties.aspectMask & s->aspect) r = &sr[i];
        }
        if (!r && !why) why = "no sparse requirements for the aspect";
    }
    if (!why) {
        s->bs = req.alignment, s->memtypes = req.memoryTypeBits;
        s->vtw = r->formatProperties.imageGranularity.width, s->vth = r->formatProperties.imageGranularity.height;
        s->vt = r->imageMipTailFirstLod < s->levels ? r->imageMipTailFirstLod : s->levels;
        if (!s->vtw || !s->vth || s->gtw % s->vtw || s->gth % s->vth || r->formatProperties.imageGranularity.depth != 1)
            why = "Vulkan block shape";
        else if (s->vt < s->gt) why = "Vulkan mip tail starts before the guest's";
        else {
            s->nx = s->gtw / s->vtw, s->ny = s->gth / s->vth;
            if ((uint64_t)s->nx * s->ny * s->bs > ZERO_BYTES) why = "guest tile too large";
        }
    }
    // Per-texture memory: Vulkan blocks of the levels in [gt, vt) and Vulkan's mip tail, layer by layer.
    VkDeviceSize per_layer = 0, tail_size = 0;
    int single = 0;
    if (!why) {
        for (uint32_t lv = s->gt; lv < s->vt; lv++)
            per_layer += (VkDeviceSize)cdiv(mip(s->width, lv), s->vtw) * cdiv(mip(s->height, lv), s->vth) * s->bs;
        single = (r->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
        if (s->vt < s->levels) tail_size = r->imageMipTailSize;
        s->tail_bytes = per_layer * s->layers + (single ? tail_size : tail_size * s->layers);
        if (s->tail_bytes) {
            VkMemoryRequirements tr = {.size = s->tail_bytes, .alignment = s->bs, .memoryTypeBits = s->memtypes};
            VmaAllocationCreateInfo ai = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
            if (vmaAllocateMemory(c->vma, &tr, &ai, &s->tail_alloc, NULL) != VK_SUCCESS) why = "mip tail memory";
        }
    }
    if (!why) {
        s->slot = calloc(npages, sizeof *s->slot);
        s->tail_host = s->ntail ? calloc((size_t)s->layers * s->ntail, sizeof *s->tail_host) : NULL;
        s->tail_dirty = calloc(s->layers, 1);
        s->tail_wgen = calloc(s->layers, sizeof *s->tail_wgen);
        if (!s->slot || (s->ntail && !s->tail_host) || !s->tail_dirty || !s->tail_wgen)
            rt_fatal("vk: out of memory (sparse texture tables)");
    }
    if (why) {
        if (im) vkDestroyImage(c->device, im, NULL);
        if (s->tail_alloc) vmaFreeMemory(c->vma, s->tail_alloc);
        if (plat_inc32(&n_why) <= 16)
            rt_log("DKB-SPARSE-RESIDENCY: texture %#llx fmt %#x %ux%ux%u stays fully resident: %s",
                   (unsigned long long)addr, d->format, ci->extent.width, ci->extent.height, ci->arrayLayers, why);
        st.refused_tex++;
        free(s);
        return NULL;
    }
    s->image = im;
    if (s->tail_bytes) {   // bound for good; cleared at the first sync, after the caller's first transition
        VmaAllocationInfo info;
        vmaGetAllocationInfo(c->vma, s->tail_alloc, &info);
        VkDeviceSize mo = info.offset;
        VkSparseImageMemoryBind *ib = calloc((size_t)s->layers * (s->vt - s->gt) + 1, sizeof *ib);
        VkSparseMemoryBind *ob = calloc(s->layers + 1, sizeof *ob);
        if (!ib || !ob) rt_fatal("vk: out of memory (sparse tail binds)");
        int ni = 0, no = 0;
        for (uint32_t ly = 0; ly < s->layers; ly++) {
            for (uint32_t lv = s->gt; lv < s->vt; lv++) {
                uint32_t lw = mip(s->width, lv), lh = mip(s->height, lv);
                ib[ni++] = (VkSparseImageMemoryBind){.subresource = {s->aspect, lv, ly}, .extent = {lw, lh, 1},
                                                     .memory = info.deviceMemory, .memoryOffset = mo};
                mo += (VkDeviceSize)cdiv(lw, s->vtw) * cdiv(lh, s->vth) * s->bs;
            }
            if (tail_size && (!single || ly == 0)) {
                ob[no++] = (VkSparseMemoryBind){.resourceOffset = r->imageMipTailOffset + (single ? 0 : ly * r->imageMipTailStride),
                                                .size = tail_size, .memory = info.deviceMemory, .memoryOffset = mo};
                mo += tail_size;
            }
        }
        if (ni) vk_sparse_bind(im, ib, ni);
        if (no) vk_sparse_bind_opaque(im, ob, no);
        free(ib), free(ob);
        sp_bytes += s->tail_bytes;
    }
    *ci = sci, *img = im;
    s->usage = (uint32_t)sci.usage;
    s->lnext = live;
    if (live) live->lprev = s;
    live = s;
    n_live_tex++, st.created++;
    if (plat_inc32(&n_log) <= 24)
        rt_log("DKB-SPARSE-RESIDENCY: texture %#llx fmt %#x %ux%ux%u levels %u: guest tile %ux%u, Vulkan block %ux%u "
               "(%ux%u per page, %llu KiB), tail from level %u (Vulkan %u), %.2f MiB bound for the tails, usage %#x",
               (unsigned long long)addr, d->format, s->width, s->height, s->layers, s->levels, s->gtw, s->gth, s->vtw,
               s->vth, s->nx, s->ny, (unsigned long long)(s->bs >> 10), s->gt, s->vt, s->tail_bytes / 1048576.0,
               (unsigned)sci.usage);
    return s;
}

// ---------------------------------------------------------------- uploads
typedef struct { SpPage *p; uint64_t slot; } Job;

static VkBuffer zero_source(VkCommandBuffer cb) {
    if (zero_buf) return zero_buf;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &zero_buf, &zero_alloc, NULL) != VK_SUCCESS) {
        zero_buf = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    vkCmdFillBuffer(cb, zero_buf, 0, VK_WHOLE_SIZE, 0);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    return zero_buf;
}

// Zeroes [x, x + w) x [y, y + h) of level lv, layer ly in bands that fit the zero source.
static int zero_rect(const DkbSparse *s, VkBufferImageCopy *rg, int n, uint32_t lv, uint32_t ly, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h) {
    uint64_t row = (uint64_t)cdiv(w, s->vbw) * s->vbpb;
    uint32_t band = (uint32_t)(ZERO_BYTES / row) * s->vbh;
    for (uint32_t y0 = 0; y0 < h; y0 += band) {
        if (rg) rg[n] = (VkBufferImageCopy){.imageSubresource = {s->aspect, lv, ly, 1},
                                            .imageOffset = {(int32_t)x, (int32_t)(y + y0), 0},
                                            .imageExtent = {w, umin(band, h - y0), 1}};
        n++;
    }
    return n;
}

// Uploads (CPU pages) and clears (GPU pages) into the tiles of jobs; then the CPU storage of the pages counts as seen.
static void record_tiles(DkbSparse *s, const Job *jobs, uint32_t n) {
    if (!n) return;
    int astc = s->f.astc_bw != 0;
    uint32_t tile_bytes = s->tbw * s->bpb * s->tbh, row_bytes = s->tbw * s->bpb;
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = astc ? calloc(n, sizeof *ar) : NULL;
    VkBufferImageCopy *zr = calloc(n, sizeof *zr);
    uint8_t *tmp = astc ? malloc(tile_bytes) : NULL;
    if (!rg || !zr || (astc && (!ar || !tmp))) rt_fatal("vk: out of memory (sparse uploads)");
    uint32_t nc = 0, nz = 0;
    for (uint32_t i = 0; i < n; i++) nc += jobs[i].p->host != NULL;
    VkBuffer sb = VK_NULL_HANDLE;
    uint8_t *stg = nc ? vk_staging((VkDeviceSize)nc * tile_bytes, &sb, NULL) : NULL;
    VkDeviceSize at = 0;
    uint32_t ncopy = 0;
    for (uint32_t i = 0; i < n; i++) {
        SpPage *p = jobs[i].p;
        uint32_t lv, ly, x, y, w, h;
        if (page_kind(s, jobs[i].slot, &lv, &ly, &x, &y, &w, &h) != 1) continue;
        if (!p->host) {
            nz = (uint32_t)zero_rect(s, zr, (int)nz, lv, ly, x, y, w, h);
            p->filled = 1, st.zeroed++, s->n_clear++;
            continue;
        }
        vk_mem_changed_snap((uintptr_t)p->host, PAGE, 0, sp_epoch, &p->wgen);   // writes from here on count
        s->n_up++;
        if (!(p->sum = page_sum(p->host))) {   // storage the CPU never wrote: DKB fills such tiles with GPU copies
            nz = (uint32_t)zero_rect(s, zr, (int)nz, lv, ly, x, y, w, h);
            p->filled = 1, p->dirty = 0, st.zeroed++, s->n_zero_src++;
            continue;
        }
        uint8_t *dst = stg + at;
        if (astc) {   // tight rows of the w x h region
            uint32_t rb = cdiv(w, s->bw) * 16, rows = cdiv(h, s->bh);
            uint8_t *lin = rb == row_bytes ? dst : tmp;
            tex_bl_copy((uint8_t *)(uintptr_t)p->host, lin, row_bytes, row_bytes, rows, 1, s->sgh, 0, 1);
            if (lin != dst)
                for (uint32_t r = 0; r < rows; r++) memcpy(dst + (size_t)r * rb, tmp + (size_t)r * row_bytes, rb);
            ar[ncopy++] = (VkAstcRegion){.src = at, .w = w, .h = h, .slices = 1, .level = lv, .layer = ly,
                                         .offset = {(int32_t)x, (int32_t)y, 0}};
        } else {
            tex_bl_copy((uint8_t *)(uintptr_t)p->host, dst, row_bytes, row_bytes, s->tbh, 1, s->sgh, 0, 1);
            rg[ncopy++] = (VkBufferImageCopy){.bufferOffset = at, .bufferRowLength = s->gtw, .bufferImageHeight = s->gth,
                                              .imageSubresource = {s->aspect, lv, ly, 1},
                                              .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {w, h, 1}};
        }
        at += tile_bytes;
        p->filled = 1, p->dirty = 0;
        st.uploads++, st.upload_bytes += PAGE;
    }
    if (ncopy) {
        if (astc) {
            if (!vk_astc_record(cb, sb, at, ar, (int)ncopy, s->f.astc_bw, s->f.astc_bh, s->f.astc_srgb, s->f.astc_bc,
                                s->image, s->aspect, VK_NULL_HANDLE))
                for (uint32_t i = 0; i < n; i++) jobs[i].p->filled = 0;   // not decoded: tried again when remapped
        } else {
            vkCmdCopyBufferToImage(cb, sb, s->image, VK_IMAGE_LAYOUT_GENERAL, ncopy, rg);
        }
    }
    if (nz) {
        VkBuffer zb = zero_source(cb);
        if (zb) vkCmdCopyBufferToImage(cb, zb, s->image, VK_IMAGE_LAYOUT_GENERAL, nz, zr);
    }
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    free(rg), free(ar), free(zr), free(tmp);
}

// First sync: every tail level cleared (CPU tails are then uploaded over it).
static void clear_tails(DkbSparse *s) {
    if (s->gt >= s->levels) return;
    int n = 0;
    for (uint32_t ly = 0; ly < s->layers; ly++)
        for (uint32_t lv = s->gt; lv < s->levels; lv++)
            n = zero_rect(s, NULL, n, lv, ly, 0, 0, mip(s->width, lv), mip(s->height, lv));
    VkBufferImageCopy *zr = calloc((size_t)n, sizeof *zr);
    if (!zr) rt_fatal("vk: out of memory (sparse tails)");
    n = 0;
    for (uint32_t ly = 0; ly < s->layers; ly++)
        for (uint32_t lv = s->gt; lv < s->levels; lv++)
            n = zero_rect(s, zr, n, lv, ly, 0, 0, mip(s->width, lv), mip(s->height, lv));
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer zb = zero_source(cb);
    if (zb) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cb, zb, s->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)n, zr);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
    vk_upload_end();
    free(zr);
}

// The tail levels of the layers marked in tail_dirty, deswizzled from their tail pages (unmapped ones read zero).
static void upload_tails(DkbSparse *s) {
    if (!s->ntail) return;
    uint32_t nl = 0;
    for (uint32_t ly = 0; ly < s->layers; ly++) {
        if (!s->tail_dirty[ly]) continue;
        int any = 0;
        for (uint32_t i = 0; i < s->ntail; i++) any |= s->tail_host[(size_t)ly * s->ntail + i] != NULL;
        if (!any) s->tail_dirty[ly] = 0;   // GPU-only tail: its content is what the GPU rendered
        else nl++;
    }
    if (!nl) return;
    int astc = s->f.astc_bw != 0;
    size_t tb_size = (size_t)s->ntail * PAGE;
    uint8_t *tb = malloc(tb_size);
    uint32_t nlev = s->levels - s->gt, k = 0;
    VkBufferImageCopy *rg = calloc((size_t)nl * nlev, sizeof *rg);
    VkAstcRegion *ar = astc ? calloc((size_t)nl * nlev, sizeof *ar) : NULL;
    if (!tb || !rg || (astc && !ar)) rt_fatal("vk: out of memory (sparse tail upload)");
    VkDeviceSize total = 0;
    for (uint32_t lv = s->gt; lv < s->levels; lv++) {
        uint32_t lw = mip(s->width, lv), lh = mip(s->height, lv);
        total += ((VkDeviceSize)cdiv(lw, s->bw) * s->bpb * cdiv(lh, s->bh) + 15) & ~15ull;
    }
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb;
    uint8_t *stg = vk_staging(total * nl, &sb, NULL);
    VkDeviceSize at = 0;
    for (uint32_t ly = 0; ly < s->layers; ly++) {
        if (!s->tail_dirty[ly]) continue;
        s->tail_dirty[ly] = 0;
        uint32_t gmax = 0;
        for (uint32_t i = 0; i < s->ntail; i++) {
            const uint8_t *h = s->tail_host[(size_t)ly * s->ntail + i];
            if (h) {
                uint32_t g;
                vk_mem_changed_snap((uintptr_t)h, PAGE, 0, sp_epoch, &g);
                if (g > gmax) gmax = g;
                memcpy(tb + (size_t)i * PAGE, h, PAGE);
            } else {
                memset(tb + (size_t)i * PAGE, 0, PAGE);
            }
        }
        s->tail_wgen[ly] = gmax;
        uint64_t tail0 = (uint64_t)ly * s->layer_size + s->tail_page * PAGE;
        for (uint32_t lv = s->gt; lv < s->levels; lv++) {
            uint32_t rb, rows, sl, bpb;
            uint64_t base;
            unsigned th, td;
            if (nvn_deswizzle_geom(&s->t.d, lv, ly, &rb, &rows, &sl, &base, &th, &td, &bpb) || base < tail0) continue;
            uint64_t rel = base - tail0, bytes = (uint64_t)((rb + 63) / 64) * ((rows + (8u << th) - 1) / (8u << th)) * (512ull << th);
            uint32_t lw = mip(s->width, lv), lh = mip(s->height, lv);
            VkDeviceSize sz = ((VkDeviceSize)rb * rows + 15) & ~15ull;
            if (rel + bytes > tb_size) { at += sz; continue; }
            tex_bl_copy(tb + rel, stg + at, rb, rb, rows, 1, th, td, 1);
            if (astc)
                ar[k++] = (VkAstcRegion){.src = at, .w = lw, .h = lh, .slices = 1, .level = lv, .layer = ly};
            else
                rg[k++] = (VkBufferImageCopy){.bufferOffset = at, .bufferRowLength = rb / s->bpb * s->bw,
                                              .bufferImageHeight = rows * s->bh, .imageSubresource = {s->aspect, lv, ly, 1},
                                              .imageExtent = {lw, lh, 1}};
            at += sz;
        }
        st.tails++;
    }
    if (k) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (astc)
            vk_astc_record(cb, sb, total * nl, ar, (int)k, s->f.astc_bw, s->f.astc_bh, s->f.astc_srgb, s->f.astc_bc, s->image,
                           s->aspect, VK_NULL_HANDLE);
        else
            vkCmdCopyBufferToImage(cb, sb, s->image, VK_IMAGE_LAYOUT_GENERAL, k, rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
    vk_upload_end();
    free(tb), free(rg), free(ar);
}

// ---------------------------------------------------------------- sync
typedef struct { uint64_t k; VkMemVPage m; } Change;
static Change *chg;
static uint32_t nchg, cchg;

static void on_page(void *ctx, uint64_t k, const VkMemVPage *m) {
    (void)ctx;
    grow((void **)&chg, &cchg, sizeof *chg, nchg + 1);
    chg[nchg++] = (Change){k, *m};
}

void dkb_sparse_sync(DkbSparse *s) {
    if (!s || s->epoch == sp_epoch) return;
    s->epoch = sp_epoch;
    if (!s->inited) {
        clear_tails(s);
        s->inited = 1;
    }
    uint64_t t0 = plat_now_ns(), now;
    // Tiles refused for memory are retried by a full scan now and then, not by holding vgen back (a rescan per sync).
    uint64_t since = s->refused && !(sp_epoch & 63) ? 0 : s->vgen;
    if (!since) s->refused = 0;
    nchg = 0;
    if (!vk_mem_vpages_changed(s->addr, s->npages, since, on_page, NULL, &now)) return;   // the pool is going away
    VkSparseImageMemoryBind *binds = NULL;
    Job *jobs = NULL;
    uint32_t nbind = 0, cbind = 0, njob = 0, cjob = 0;
    int complete = 1;
    for (uint32_t i = 0; i < nchg; i++) {
        const Change *ch = &chg[i];
        uint32_t lv, ly, x, y, w, h;
        int kind = page_kind(s, ch->k, &lv, &ly, &x, &y, &w, &h);
        if (kind == 2) {
            const uint8_t **th = &s->tail_host[(size_t)ly * s->ntail + x], *nh = ch->m.serial ? ch->m.host : NULL;
            if (*th != nh) *th = nh, s->tail_dirty[ly] = 1;
            continue;
        }
        if (kind != 1) continue;
        SpPage *old = s->slot[ch->k];
        if (old ? old->serial == ch->m.serial && old->off == ch->m.off : !ch->m.serial) continue;
        SpPage *np = NULL;
        int fill = 0;
        if (ch->m.serial) {
            if (sp_step && sp_used >= sp_step) { complete = 0, st.deferred++; continue; }   // keeps the old tile a few submissions
            if (!(np = page_acquire(s, &ch->m, ch->k, &fill))) s->refused = 1;
            if (!np && !old) continue;
        }
        if (old) page_release(old, s, ch->k), s->n_bound--;
        s->slot[ch->k] = np;
        if (np) s->n_bound++;
        else st.tiles_unbound++;
        grow((void **)&binds, &cbind, sizeof *binds, nbind + 1);
        binds[nbind++] = tile_bind(s, ch->k, np);
        if (fill) {
            grow((void **)&jobs, &cjob, sizeof *jobs, njob + 1);
            jobs[njob++] = (Job){np, ch->k};
            sp_used++;
        }
    }
    if (complete) s->vgen = now;
    for (uint32_t i = 0; i < ndirty;) {   // CPU storage the game rewrote in place, uploaded by the first binder
        SpPage *p = dirty_q[i];
        if (!p->nb || p->b[0].s != s) { i++; continue; }
        dirty_q[i] = dirty_q[--ndirty];
        grow((void **)&jobs, &cjob, sizeof *jobs, njob + 1);
        jobs[njob++] = (Job){p, p->b[0].slot};
        s->n_rewrite++;
    }
    if (s->ntail && !(sp_epoch & 7))   // CPU tails rewritten in place
        for (uint32_t ly = 0; ly < s->layers; ly++)
            for (uint32_t i = 0; i < s->ntail && !s->tail_dirty[ly]; i++) {
                const uint8_t *h = s->tail_host[(size_t)ly * s->ntail + i];
                uint32_t g;
                if (h && vk_mem_changed_snap((uintptr_t)h, PAGE, s->tail_wgen[ly], sp_epoch, &g) == 1) s->tail_dirty[ly] = 1;
            }
    if (nbind) vk_sparse_bind(s->image, binds, (int)nbind);
    for (uint32_t i = 0; i < njob; i += CHUNK_JOBS) record_tiles(s, jobs + i, umin(CHUNK_JOBS, njob - i));
    upload_tails(s);
    free(binds), free(jobs);
    st.ns += plat_now_ns() - t0;
}

void dkb_sparse_gpu_written(DkbSparse *s) {
    if (s) s->n_gpu++;
}

void dkb_sparse_destroy(DkbSparse *s) {
    if (!s) return;
    for (uint64_t p = 0; p < s->npages; p++)
        if (s->slot[p]) page_release(s->slot[p], s, p);
    if (s->tail_alloc) vk_defer_memory(s->tail_alloc), sp_bytes -= s->tail_bytes;
    free(s->slot), free(s->tail_host), free(s->tail_dirty), free(s->tail_wgen);
    if (s->lprev) s->lprev->lnext = s->lnext;
    else live = s->lnext;
    if (s->lnext) s->lnext->lprev = s->lprev;
    free(s);
    n_live_tex--;
}

void dkb_sparse_new_epoch(uint32_t epoch) {
    sp_epoch = epoch, sp_used = 0;
    if (epoch & 3) return;
    for (SpPool *o = pools; o; o = o->next) {   // pages the CPU rewrote while mapped
        uint32_t now;
        int ch = vk_mem_changed_snap(o->lo, o->hi - o->lo, o->wgen, epoch, &now);
        o->wgen = now;
        if (ch != 1) continue;
        for (SpPage *p = o->pages; p; p = p->pnext) {
            uint32_t g;
            if (!p->filled || p->dirty || vk_mem_changed_snap((uintptr_t)p->host, PAGE, p->wgen, epoch, &g) != 1) continue;
            // Write-watch answers are page-conservative; an upload of unchanged bytes would erase the GPU's copies
            // and renders into the tile.
            if (page_sum(p->host) == p->sum) { p->wgen = g, st.rewrites_same++; continue; }
            p->dirty = 1;
            grow((void **)&dirty_q, &cdirty, sizeof *dirty_q, ndirty + 1);
            dirty_q[ndirty++] = p;
            st.rewrites++;
        }
    }
}

void dkb_sparse_shutdown(void) {
    if (zero_buf) vk_defer_buffer(zero_buf, zero_alloc);
    zero_buf = VK_NULL_HANDLE, zero_alloc = NULL;
    free(ht), free(dirty_q), free(chg);
    ht = NULL, dirty_q = NULL, chg = NULL;
    ht_cap = ht_n = ndirty = cdirty = nchg = cchg = 0;
}

void dkb_sparse_perf(uint64_t frame) {
    if (sp_on <= 0) return;
    uint64_t binds, submits;
    vk_sparse_stats(&binds, &submits);
    rt_log("perf: frame %llu: sparse textures %llu live (%llu made, %llu fully resident), tile memory %.1f/%llu MiB in "
           "%llu pages; tiles new %llu, shared %llu, unbound %llu, over the limit %llu, deferred %llu; uploads %llu "
           "(%.1f MiB), cleared %llu, rewritten %llu (unchanged, kept %llu), tails %llu; %llu binds in %llu submissions; "
           "%.2f ms",
           (unsigned long long)frame, (unsigned long long)n_live_tex, (unsigned long long)st.created,
           (unsigned long long)st.refused_tex, sp_bytes / 1048576.0, (unsigned long long)(sp_limit >> 20),
           (unsigned long long)n_live_pages, (unsigned long long)st.tiles_new, (unsigned long long)st.tiles_shared,
           (unsigned long long)st.tiles_unbound, (unsigned long long)st.over_limit, (unsigned long long)st.deferred,
           (unsigned long long)st.uploads, st.upload_bytes / 1048576.0, (unsigned long long)st.zeroed,
           (unsigned long long)st.rewrites, (unsigned long long)st.rewrites_same, (unsigned long long)st.tails,
           (unsigned long long)binds,
           (unsigned long long)submits, st.ns / 1e6);
    memset(&st, 0, sizeof st);
    if (frame % 600) return;
    for (DkbSparse *s = live; s; s = s->lnext)
        rt_log("perf: frame %llu: sparse %#llx fmt %#x %ux%ux%u usage %#x: %llu of %llu tile pages bound; since "
               "created: uploaded %llu (%llu with all-zero storage), cleared %llu, rewritten %llu, GPU writes %llu",
               (unsigned long long)frame, (unsigned long long)s->addr, s->t.d.format, s->width, s->height, s->layers,
               s->usage, (unsigned long long)s->n_bound, (unsigned long long)(s->lvl_page[s->gt] * s->layers),
               (unsigned long long)s->n_up, (unsigned long long)s->n_zero_src, (unsigned long long)s->n_clear,
               (unsigned long long)s->n_rewrite, (unsigned long long)s->n_gpu);
}
