/* Modtopia core: patches against the player's own game files (SPEC §10; the format is in mt_patch.c). MIT. */
#ifndef MT_PATCH_H
#define MT_PATCH_H

#include <stddef.h>
#include <stdint.h>

/* Reads n bytes of a source from off into buf: how many were read (n, or fewer only at its end), -1 on an error */
typedef int64_t (*MtSrcRead)(void *ctx, uint64_t off, void *buf, uint64_t n);

typedef struct MtPatchHead {
    uint64_t src_size, dst_size;
    uint8_t src_sha[32], dst_sha[32];
    uint32_t block;
} MtPatchHead;

typedef struct MtPatchStats { uint64_t added, copied, filled, size; int ops; } MtPatchStats;

/* 0 and the header, or -1 when p is not a MTPATCH1 patch */
int mt_patch_head(const uint8_t *p, size_t n, MtPatchHead *h);
/* Checks a patch's own structure (its operations fit its sizes), without the source: 0, or -1 with *why (free it) */
int mt_patch_check(const uint8_t *p, size_t n, char **why);
/* Applies a patch to a source of src_size bytes read through rd: checks the source's size and sha256, refuses a patch whose added bytes include
   more than 16 KiB of the source (the game's own data), writes out_path through a temporary file, and checks the result's sha256.
   0, or -1 with *why (free it); *refused is set to 1 when it was the legal check */
int mt_patch_apply(const uint8_t *p, size_t n, uint64_t src_size, MtSrcRead rd, void *ctx, const char *out_path, char **why, int *refused);
/* Makes a patch that turns the file src_path into dst_path: 0, or -1 with *why (free it). st may be NULL */
int mt_patch_make(const char *src_path, const char *dst_path, const char *out_path, char **why, MtPatchStats *st);
/* The matching block for a source of this size (32 bytes up to 64 MiB, larger for larger files: at most 2M blocks) */
uint32_t mt_patch_block(uint64_t size);

#endif
