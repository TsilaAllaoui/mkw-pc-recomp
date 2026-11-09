// M15.12 (DKB): NVN sparse texture tile layouts.
//
// Observed after M15.11: "NVN nvnTextureGetSparseTileLayout is not implemented". DKB sizes the tile tables of its
// sparse textures from NVNtextureSparseTileLayout {int numTiledLevels, tileWidth, tileHeight, tileDepth,
// reserved[4]}. Tiles are 64 KiB, shaped by the format's bytes per block (128 x 128 texels at 4 bytes; the shapes of
// Hookshot's MK World host, host/nvn_native.c); the levels at least one tile in size are tiled, the rest are the mip
// tail. An unknown format stops with a named fatal rather than an invented shape.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"

static void dkb_sparse_tile_layout(const nvn_texdesc *d, int32_t *out, const char *who) {
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    int tw, th;
    switch (bpb) {
    case 1: tw = 256, th = 256; break;
    case 2: tw = 256, th = 128; break;
    case 4: tw = 128, th = 128; break;
    case 8: tw = 128, th = 64; break;
    case 16: tw = 64, th = 64; break;
    default: rt_fatal("DKB-NVN-SPARSE-FORMAT: %s of format %#x (%d bytes per block)", who, d->format, bpb);
    }
    tw *= bw, th *= bh;
    int levels = 0;
    for (uint32_t l = 0, w = d->width, h = d->height; l < d->levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        if (w >= (uint32_t)tw && h >= (uint32_t)th) levels++;
    memset(out, 0, 32);
    out[0] = levels, out[1] = tw, out[2] = th, out[3] = 1;
    static volatile int32_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 4)
        rt_log("nvn: sparse tile layout (%s): format %#x %ux%u, %u levels -> %d tiled levels of %dx%d texels",
               who, d->format, d->width, d->height, d->levels, levels, tw, th);
}

void nvnp_nvnTextureGetSparseTileLayout(Cpu *c) {
    const tex_t *t = (const tex_t *)arg_p(c, 0);
    if (t->magic != NVN_TEX_MAGIC) rt_fatal("nvnTextureGetSparseTileLayout: texture %p is not initialized", (void *)t);
    dkb_sparse_tile_layout(&t->d, (int32_t *)arg_p(c, 1), "texture");
}

void nvnp_nvnTextureBuilderGetSparseTileLayout(Cpu *c) {
    const tex_builder_t *b = (const tex_builder_t *)arg_p(c, 0);
    dkb_sparse_tile_layout(&b->d, (int32_t *)arg_p(c, 1), "builder");
}
