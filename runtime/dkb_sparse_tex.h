// DKB sparse texture residency (M15.26, dkb_sparse_tex.c). Called by vk_texture.c under the texture lock.
#pragma once
#include "tex.h"

typedef struct DkbSparse DkbSparse;

// create_tex: for a sparse NVN texture in a virtual pool that the device can make sparse-resident, *img gets an image
// made from ci with sparse residency (ci gets the flags and usage it was made with) and the result is its residency
// state; NULL leaves ci alone and the caller makes the image as before. The caller records the image's first
// transition (init_image) before its first dkb_sparse_sync.
DkbSparse *dkb_sparse_create(const tex_t *t, const TexVkFormat *f, VkImageCreateInfo *ci, VkImage *img);
// Binds the tiles the game mapped since the last call, unbinds the ones it unmapped and uploads the CPU-written ones;
// at most once per submission (dkb_sparse_new_epoch).
void dkb_sparse_sync(DkbSparse *s);
void dkb_sparse_gpu_written(DkbSparse *s);   // a copy, render or store into the image was recorded (perf counter)
void dkb_sparse_destroy(DkbSparse *s);   // after vk_defer_image of its image
void dkb_sparse_new_epoch(uint32_t epoch);
void dkb_sparse_perf(uint64_t frame);    // TOTK_TRACE=perf line
void dkb_sparse_shutdown(void);          // vk_texture_shutdown, after every texture went
