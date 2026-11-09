// NVN texture storage layout (Tegra X1 block-linear), see nvn_tex.c.
// Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.h @ b75be8cf (zlib, deko3d-derived; see nvn_tex.c).
// nvn_texdesc and nvn_texlayout are defined in nvn_obj.h.
#pragma once
#include <stdint.h>
#include "nvn_obj.h"

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   // Bytes per block (0 = unknown format), block width and height
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
// A sparse 2D or 2D-array block-linear texture: 1 with the shape of its 64 KiB tile in blocks and the tile's block
// height (log2 GOBs); 0 for every other texture. See nvn_tex.c for the layout rules that follow from it.
int nvn_tex_sparse_tile(const nvn_texdesc *d, uint32_t *tw, uint32_t *th, unsigned *gob_h);

/* LINEAR_RENDER_TARGET (0x40) uses pitch storage even without LINEAR (0x10).
   TOTK's CPU-loaded water height/mask textures use that flag alone. */
static inline int nvn_tex_is_linear(const nvn_texdesc *d) { return (d->flags & 0x50) != 0; }
static inline uint32_t nvn_tex_linear_stride(const nvn_texdesc *d) {
    if (d->stride) return d->stride;
    int bw, bh; int bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint32_t align = (d->flags & 0x40) ? 128 : 32;
    return (((d->width + bw - 1) / bw) * bpb + align - 1) & ~(align - 1);
}
