//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * lint.h -- qsim memory coherency model: the os25 VexiiRiscv L1 data cache
 * versus the other readers and writers of the same SDRAM (GPU, uncached
 * alias, scanout).
 *
 * qsim's flat memory makes a CPU store through the cached alias (0x1xxxxxxx)
 * visible to the GPU at once, and a GPU write visible to later CPU cached reads
 * at once.  On the device neither holds:
 *   - A cached store lands in an L1 line, directly or through the store buffer,
 *     which retries it into L1.  It reaches SDRAM only when the line is written
 *     back: on dirty-victim eviction, or on cbo.clean / cbo.flush of a dirty
 *     line.  VexiiRiscv decodes CBM_FLUSH as CLEAN (LsuPlugin.scala:236-238),
 *     and the line stays loaded (LsuL1Plugin.scala:890,951-966).
 *   - cbo.inval drops the line WITHOUT writeback in M-mode.  invalIntoClean is
 *     only set below M-mode (LsuPlugin.scala:221-226).
 *   - A GPU or uncached-alias write to a line that is loaded in L1 leaves the
 *     L1 copy stale.  Later cached reads return the old bytes, and a later
 *     writeback of that line (the whole 64 B) reverts the new bytes.
 *
 * The model keeps its own L1: 1024 sets x 2 ways x 64 B, LRU victim, the same
 * policy as cache.c.  Every CPU cached load/store (both interpreters), cbo,
 * kernel cache service and kernel memcpy/memset of the environment drives it,
 * so the counts are identical in fast and --profile runs.
 *
 * Two uses:
 *   LINT    (strict = 0) memory stays flat.  The model only counts hazards;
 *           execution is never changed.
 *   STRICT  (strict = 1) memory is split.  C is the committed SDRAM image the
 *           GPU, the uncached alias and scanout (frame hashes) read and write.
 *           V (cpu->sdram) is what the CPU sees through the cached alias: the
 *           L1 copy for loaded lines, C for all others.  Invariant: V == C on
 *           every line that is not loaded.
 *           Data moves only on device events:
 *             - dirty eviction and cbo clean/flush of a dirty line: V -> C (64 B);
 *             - eviction of a stale line and cbo.inval: C -> V;
 *             - GPU / uncached writes to a line that is not loaded: C -> V byte.
 *           Hazards therefore become pixel differences.
 *
 * Not modelled: the kernel's own cached-memory traffic (services), which can
 * only commit lines earlier; store-buffer and writeback latency (a writeback
 * commits at the cbo/eviction instant); and the device timing of GPU reads,
 * which comes from the GPU schedule (gpu.h).
 */
#ifndef QSIM_LINT_H
#define QSIM_LINT_H
#include <stdint.h>
#include <stdio.h>

/* per-64-byte-line map bits (SDRAM offset >> 6) */
#define LM_CACHED  0x01u   /* line loaded in the modelled CPU L1 */
#define LM_DIRTY   0x02u   /* loaded and dirty: CPU stores not written back (dmask) */
#define LM_STALE   0x04u   /* loaded, and SDRAM changed under it (smask) */
#define LM_WCDIRTY 0x08u   /* worst case: cached store since the last cbo on the line */
#define LM_GPUW    0x10u   /* worst case: GPU wrote the line since the last cbo on it */

enum {
    /* cache-aware (the gate) */
    LH_GPU_RD_UNCOMMITTED,  /* GPU byte read of a byte the CPU stored into a still-dirty L1 line */
    LH_CPU_RD_STALE,        /* CPU cached load of bytes SDRAM changed under the loaded L1 line */
    LH_WB_REVERT,           /* writeback of a dirty line whose SDRAM bytes changed under it (revert) */
    LH_UC_RD_UNCOMMITTED,   /* CPU uncached-alias load of bytes stored into a still-dirty L1 line */
    LH_INVAL_DIRTY,         /* cbo.inval / cache_inval_range dropped a dirty line (stores lost) */
    LH_N_AWARE,
    /* worst case (advisory): no eviction, only cbo commits */
    LH_WC_GPU_RD_DIRTY = LH_N_AWARE,  /* GPU byte read of a line with a cached store since its last cbo */
    LH_WC_CPU_GPUW,         /* CPU cached access to a line the GPU wrote since its last cbo */
    LH_N
};
extern const char *const lint_class_names[LH_N];

#define LINT_MAX_EXAMPLES 8
/* hazard SITES: (class, pc) pairs with a count each.  Counts move with code and
 * data layout (the same buggy loop touches a different number of lines when the
 * stack moves), but the set of sites does not, so the gate compares sites. */
#define LINT_MAX_SITES 512

typedef struct {
    uint64_t frame;       /* flips executed so far when the event happened */
    uint32_t pc;          /* CPU pc at the event (for GPU classes: the CPU pc that let the GPU run) */
    uint32_t addr;        /* cached-alias address of the byte */
    uint32_t dirty_pc;    /* pc of the store that first dirtied the line (0 = unknown / kernel) */
} lint_example;

typedef struct qsim_lint {
    uint8_t  *V;          /* CPU cached view (cpu->sdram) */
    uint8_t  *C;          /* committed SDRAM (== V unless strict) */
    int       strict;
    uint32_t  lines;
    uint8_t  *map;        /* lines + 2 entries: LM_* for every line (GPU / uncached hooks) */
    uint32_t *dirty_pc;   /* pc of the store that dirtied the line */
    /* L1 ways: everything the CPU fast path needs, 48 KB (host-cache friendly) */
    struct lint_way {
        uint32_t tag;     /* line + 1, 0 = empty */
        uint32_t flags;   /* LM_DIRTY | LM_STALE (mirrors map) */
        uint64_t dmask;   /* bytes stored into the L1 copy and not written back */
        uint64_t smask;   /* bytes changed in SDRAM under the loaded copy */
    } way[1024 * 2];
    uint8_t   mru[1024];
    uint64_t  count[LH_N];
    uint64_t  nex[LH_N];
    lint_example ex[LH_N][LINT_MAX_EXAMPLES];
    struct { uint32_t pc; uint8_t cls, used; uint64_t n; } site[LINT_MAX_SITES];   /* open addressing */
    uint32_t  nsites, sites_dropped;
    uint64_t *frame;      /* environment flip counter (may be NULL) */
    const uint32_t *pcp;  /* live CPU pc (cpu->pc; exact on MMIO/hook paths) */
    uint32_t  env_pc;     /* pc attributed to environment (kernel) stores */
    /* statistics */
    uint64_t  evict_commits, cbo_commits, stale_refresh, env_store_ranges, inval_drops;
} qsim_lint;

/* V: CPU view (64 MB, cpu->sdram).  strict: allocate C and copy V into it. */
qsim_lint *lint_new(uint8_t *V, uint32_t sdram_size, int strict);
void       lint_free(qsim_lint *L);

/* out-of-line parts */
void lint_access_slow(qsim_lint *L, uint32_t off, unsigned size, int store, uint32_t pc);
void lint_event(qsim_lint *L, int cls, uint32_t off, uint32_t pc);
void lint_gpu_rd_slow(qsim_lint *L, uint32_t off, uint8_t f);
void lint_gpu_wr_slow(qsim_lint *L, uint32_t off, uint8_t f);
void lint_uc_rd_slow(qsim_lint *L, uint32_t off, unsigned size, uint32_t pc);
void lint_uc_wr_slow(qsim_lint *L, uint32_t off, unsigned size);
/* cbo on the line holding SDRAM offset off: op 0 inval, 1 clean, 2 flush */
void lint_cbo(qsim_lint *L, unsigned op, uint32_t off, uint32_t pc);
/* kernel helpers (environment side) */
void lint_cbo_range(qsim_lint *L, unsigned op, uint32_t off, uint32_t len, uint32_t pc);
/* The kernel stored [off, off+len) through the cached alias; V already holds
 * the new bytes.  Must be called right after the write. */
void lint_env_store(qsim_lint *L, uint32_t off, uint32_t len);
/* hal/cache.c of_cache_flush: conflict reads over DCACHE_TOTAL bytes at EVICT_BASE */
void lint_evict_all(qsim_lint *L, uint32_t evict_off);
uint64_t lint_total_aware(const qsim_lint *L);
uint64_t lint_total_wc(const qsim_lint *L);
void lint_print(const qsim_lint *L, FILE *o);
void lint_json(const qsim_lint *L, FILE *o, const char *ind);
/* strict mode self-check (debug): lines whose V and C differ without being dirty or stale */
uint64_t lint_check_invariant(qsim_lint *L, FILE *log);

/* ---- hot inline hooks ---------------------------------------------------- */

static inline uint64_t lint_bytes(uint32_t off, unsigned size)
{
    unsigned o = off & 63u, n = size > 64u - o ? 64u - o : size;
    return (n >= 64 ? ~0ull : ((1ull << n) - 1ull)) << o;
}

/* CPU cached-alias load/store at SDRAM offset off (size bytes), before the
 * data access.  The data access itself uses V. */
static inline void lint_cpu_access(qsim_lint *L, uint32_t off, unsigned size, int store, uint32_t pc)
{
    uint32_t line = off >> 6;
    uint32_t set = line & 1023u;
    struct lint_way *wp = &L->way[set * 2];
    uint32_t w = wp[0].tag == line + 1 ? 0 : (wp[1].tag == line + 1 ? 1 : 2);
    if (__builtin_expect(w < 2 && !(wp[w].flags & LM_STALE) && (!store || (wp[w].flags & LM_DIRTY)), 1)) {
        L->mru[set] = (uint8_t)w;
        if (store) wp[w].dmask |= lint_bytes(off, size);
    } else {
        lint_access_slow(L, off, size, store, pc);
    }
    if (__builtin_expect(((off + size - 1) >> 6) != line, 0)) {
        uint32_t o2 = (line + 1) << 6;
        if ((o2 >> 6) < L->lines) lint_access_slow(L, o2, off + size - o2, store, pc);
    }
}

/* CPU uncached-alias load at SDRAM offset off, before the data access (C). */
static inline void lint_uc_rd(qsim_lint *L, uint32_t off, unsigned size, uint32_t pc)
{
    if (__builtin_expect((L->map[off >> 6] | L->map[(off + size - 1) >> 6]) & LM_DIRTY, 0))
        lint_uc_rd_slow(L, off, size, pc);
}

/* CPU uncached-alias store at SDRAM offset off, AFTER writing C. */
static inline void lint_uc_wr(qsim_lint *L, uint32_t off, unsigned size)
{
    if (__builtin_expect((L->map[off >> 6] | L->map[(off + size - 1) >> 6]) & LM_CACHED, 0))
        lint_uc_wr_slow(L, off, size);
    else if (L->strict)
        for (unsigned i = 0; i < size; i++) L->V[off + i] = L->C[off + i];
}

/* GPU byte read at SDRAM offset off, before reading C */
static inline void lint_gpu_rd(qsim_lint *L, uint32_t off)
{
    uint8_t f = L->map[off >> 6];
    if (__builtin_expect((f & (LM_DIRTY | LM_WCDIRTY)) != 0, 0)) lint_gpu_rd_slow(L, off, f);
}

/* GPU byte write at SDRAM offset off, AFTER writing C */
static inline void lint_gpu_wr(qsim_lint *L, uint32_t off)
{
    uint8_t f = L->map[off >> 6];
    if (__builtin_expect((f & (LM_CACHED | LM_GPUW)) != LM_GPUW, 0)) lint_gpu_wr_slow(L, off, f);
    else if (L->strict) L->V[off] = L->C[off];
}

#endif
