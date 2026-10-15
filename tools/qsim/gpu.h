//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * gpu.h -- qsim software raster model of the openfpgaOS os25 gpu_core.
 *
 * Consumes the GPU command-word stream exactly as the os25 bitstream's
 * gpu_core.v decodes it and writes framebuffer / z-buffer bytes into the
 * shared SDRAM through a memory accessor.
 *
 * Scheduling (gpu_config_t.sched).  Published commands enter a queue that
 * stands for the ring BRAM between ring_rdptr and ring_wrptr.  The CPU can only
 * learn about GPU progress through GPU_FENCE_REACHED, GPU_RING_RDPTR,
 * GPU_STATUS and the kernel's fence wait (video_acquire_next).  A schedule
 * decides how far the GPU has got at each of those observations:
 *   GPU_SCHED_EAGER  (default) infinitely fast GPU: the queue drains inside
 *                    the publish, so every fence is reached at once.
 *   GPU_SCHED_LAZY   as late as legally possible.  Commands run only when the
 *                    CPU observes progress, and then only as far as needed:
 *                      - a FENCE_REACHED read runs up to and including the next
 *                        queued FENCE/FLIP (nothing if none is queued);
 *                      - a RING_RDPTR read runs one command, so ring-space polls
 *                        free just enough words;
 *                      - gpu_wait_fence(token) (kernel waits) runs until
 *                        fence_reached >= token.
 *                    A CPU read of FB/z/texture memory that the software did not
 *                    synchronize therefore sees pre-GPU contents.
 *   GPU_SCHED_DRAIN  in between: every observation runs everything queued (the
 *                    fidelity review's QSIM_LAZY_GPU prototype).
 * gpu_drain() runs everything in any schedule (end of run).
 * STATUS is side-effect free: busy/ring_empty follow the queue.  MMIO register
 * writes (TEX_FLUSH, PALOOKUP_BASE, translucency LUT) take effect at the write,
 * before still-queued commands, as in the RTL.  RDPTR and cpu_ring_full follow
 * the queue as in the RTL.
 *
 * Ground truth: openfpgaOS src/fpga/common/gpu_core.v (+ gpu_tex_cache.v)
 * built with the os25 parameter set (see README.md "GPU raster model").
 * All arithmetic is the RTL's integer arithmetic (26-bit address wrap,
 * 32-bit attribute wrap, 16-bit clamp compares, LUT + Newton-Raphson
 * reciprocal, Q29 z restore/saturation, perspective 16-pixel segments).
 *
 * Addresses: the GPU works in 26-bit SDRAM byte offsets (the SDRAM slave
 * decodes [25:2]).  Every draw command carries absolute addresses; a CPU
 * pointer p maps to GPU address (p & 0x03FFFFFF).  The accessor decides how
 * a GPU address maps to host storage (mask + base array, or callbacks).
 */
#ifndef QSIM_GPU_H
#define QSIM_GPU_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_ADDR_MASK 0x03FFFFFFu

/* Memory accessor.  If base != NULL the model reads/writes
 * base[addr & mask] directly (fast path); otherwise it calls read8/write8
 * with (addr & mask).  mask defaults to GPU_ADDR_MASK when 0. */
typedef struct gpu_mem_if {
    void     *ctx;
    uint8_t  *base;
    uint32_t  mask;
    uint8_t (*read8)(void *ctx, uint32_t addr);
    void    (*write8)(void *ctx, uint32_t addr, uint8_t v);
} gpu_mem_if_t;

/* Variant / modelling options.  gpu_default_config() = os25 bitstream. */
typedef struct gpu_config {
    uint8_t compact_span;   /* INCLUDE_COMPACT_SPAN: 0x48 4+7N lane form     (os25 1) */
    uint8_t column_list;    /* INCLUDE_COLUMN_LIST: 0x4C                     (os25 1) */
    uint8_t transluc;       /* INCLUDE_TRANSLUC: SPAN_TRANSLUC blend LUT      (os25 1) */
    uint8_t palette;        /* INCLUDE_PALETTE: SPAN_COLORMAP lookup          (os25 1) */
    uint8_t param_q29;      /* INCLUDE_PARAM_SPAN_Q29                        (os25 1) */
    uint8_t cpu_ring;       /* INCLUDE_CPU_RING: MMIO 0x04 append + CTRL bit3 (os25 1) */
    uint8_t command_dma;    /* INCLUDE_COMMAND_DMA: 0x0C/0x1C/0x2C doorbell   (os25 0) */
    /* Model gpu_tex_cache: 1024 direct-mapped 16-byte lines shared by texel
     * and colormap reads, filled from SDRAM on first touch and only
     * invalidated by GPU_TEX_FLUSH.  1 = hardware-faithful (stale texels if
     * the CPU rewrites a cached line without a flush); 0 = always read live
     * SDRAM.  Default 1. */
    uint8_t tex_cache;
    /* 0x58 PARAM_SPAN_CONT: os25 gpu_core has no decoder for it (drained as
     * a no-op, caps bit 28 not advertised).  1 = emulate the SDK's intended
     * header-reuse semantics instead (NOT hardware behaviour).  Default 0. */
    uint8_t emulate_span_cont;
    uint8_t sched;          /* GPU_SCHED_* (default EAGER) */
} gpu_config_t;

enum { GPU_SCHED_EAGER = 0, GPU_SCHED_LAZY = 1, GPU_SCHED_DRAIN = 2 };
/* CPU observation points of GPU progress (barrier statistics) */
enum { GPU_OBS_FENCE = 0, GPU_OBS_RDPTR = 1, GPU_OBS_WAIT = 2, GPU_OBS_N = 3 };

typedef struct gpu_stats {
    uint64_t batches;             /* published batches (gpu_execute calls with n>0) */
    uint64_t words;               /* command words consumed */
    uint64_t cmd_count[256];      /* commands seen, by opcode */
    uint64_t cmd_words[256];      /* payload words, by opcode */
    uint64_t cmd_nop[256];        /* drained with no effect: unsupported / wrong size / header rejected */
    uint64_t cmd_pixels[256];     /* source pixels generated, by opcode */
    uint64_t records;             /* span records (lanes) with count != 0 */
    uint64_t spans_affine;
    uint64_t spans_persp;         /* perspective spans (PSS path) */
    uint64_t spans_persp_constz;  /* ... of which converted to affine (constant 1/z) */
    uint64_t spans_q29;           /* perspective spans in Q29 mode */
    uint64_t persp_passes;        /* PSS passes (anchor + per segment) incl. speculative */
    uint64_t recip_computes;      /* reciprocals (LUT+NR) evaluated */
    uint64_t pixels;              /* source pixels (texel fetches issued) */
    uint64_t texels_fetched;      /* port-A texture byte reads */
    uint64_t cmap_lookups;        /* port-B palookup byte reads */
    uint64_t tex_cache_hits;      /* both ports, when tex_cache modelled */
    uint64_t tex_cache_fills;     /* line fills (16-byte SDRAM bursts) */
    uint64_t skip_zero_discards;
    uint64_t z_tests, z_test_fail;
    uint64_t z_writes;            /* 16-bit z values written */
    uint64_t pixels_written;      /* colour bytes written by span pixels */
    uint64_t transluc_blends;
    uint64_t clear_rects, clear_bytes;
    uint64_t fences, flips, tex_flushes;
    uint64_t ring_overflows;
    /* CPU/GPU synchronisation (all schedules).  An observation is a
     * FENCE_REACHED read, a RDPTR read or an environment wait.  It is a
     * barrier when the GPU executed commands since the previous observation or
     * still has queued work.  barrier_px / barrier_words: GPU pixels / words
     * executed between consecutive barriers, the most such a wait can cover.
     * In LAZY/DRAIN, obs_px / obs_words count what ran AT observations, the
     * work the CPU actually waited for in that schedule. */
    uint64_t published_words, publishes;
    uint64_t observations[GPU_OBS_N], barriers[GPU_OBS_N];
    uint64_t barrier_px, barrier_words;
    uint64_t obs_px, obs_words;
    uint64_t queue_max_words;     /* largest ring occupancy seen (words) */
    /* Branch coverage of RTL corner cases (see gpu_cov_names[] in gpu.c). */
    uint64_t cov[40];
} gpu_stats_t;

extern const char *const gpu_cov_names[40];

typedef struct gpu gpu_t;

typedef void (*gpu_flip_cb_t)(void *ctx, int buffer_index, uint32_t token);
struct qsim_lint;

void   gpu_default_config(gpu_config_t *cfg);
gpu_t *gpu_create(const gpu_config_t *cfg, const gpu_mem_if_t *mem);
void   gpu_destroy(gpu_t *g);
void   gpu_reset(gpu_t *g);                 /* hardware reset (reset_n) */
void   gpu_set_flip_callback(gpu_t *g, gpu_flip_cb_t cb, void *ctx);
/* Integration hook: called before every command executes (header word and
 * its complete payload, header[12:0] words).  Used by the qsim environment
 * for per-frame command hashing and pre-clear z-buffer snapshots. */
typedef void (*gpu_cmd_cb_t)(void *ctx, uint32_t hdr, const uint32_t *payload);
void   gpu_set_cmd_callback(gpu_t *g, gpu_cmd_cb_t cb, void *ctx);
/* Called at publish time (CPU order) for every complete CMD_FLIP newly
 * published, before any of it executes. */
void   gpu_set_publish_callback(gpu_t *g, gpu_cmd_cb_t cb, void *ctx);
/* Memory coherency model (lint.h): every GPU SDRAM byte read is reported
 * before and every write after it happens (the accessor's base must be the
 * model's committed image); NULL = off. */
void   gpu_set_lint(gpu_t *g, struct qsim_lint *lint);

/* Schedule control (see the top of this file). */
void     gpu_drain(gpu_t *g, int obs_kind);            /* run everything queued; counts an observation if obs_kind >= 0 */
void     gpu_wait_fence(gpu_t *g, uint32_t token);     /* kernel fence wait: fence_reached >= token (signed), per schedule */
uint32_t gpu_queued_words(const gpu_t *g);
const char *gpu_sched_name(int sched);

/* Execute a published batch of complete command words (header + payload).
 * A command split across batches is buffered until its payload arrives. */
void   gpu_execute(gpu_t *g, const uint32_t *words, uint32_t n);

/* MMIO window (byte offsets as in of_gpu.h: 0x00 CTRL .. 0x3C).  Handles the
 * CPU-ring transport (0x04 append, CTRL bit3 publish -> gpu_execute),
 * GPU_TRANSLUC_ADDR/DATA, GPU_TEX_FLUSH, GPU_PALOOKUP_BASE, soft/ring reset,
 * and (if configured) the doorbell DMA. */
void     gpu_mmio_write(gpu_t *g, uint32_t offset, uint32_t value);
uint32_t gpu_mmio_read(gpu_t *g, uint32_t offset);

uint32_t gpu_fence_reached(const gpu_t *g);
const gpu_stats_t *gpu_get_stats(const gpu_t *g);
void   gpu_reset_stats(gpu_t *g);
void   gpu_print_stats(const gpu_t *g, FILE *f);

#ifdef __cplusplus
}
#endif
#endif
