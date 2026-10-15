//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * env.h -- qsim openfpgaOS (os25 / Analogue Pocket) environment for a
 * production app ELF: loader + auxv/caps, ECALL dispatcher, OS services
 * table, data-slot file system, GPU MMIO (qsim gpu.c), display buffer-role
 * model, audio ring model and the virtual clock.  Spec: ENV_SPEC.md.
 *
 * Nothing here is app-specific.  The facts that are -- the data-slot
 * registry, the app .ini, argv[0], the frame-counter/z-buffer symbols and
 * the console line conventions -- arrive in an app_profile (app.h).
 */
#ifndef QSIM_ENV_H
#define QSIM_ENV_H
#include <stdint.h>
#include <stdio.h>
#include "app.h"
#include "cpu.h"
#include "gpu.h"
#include "qelf.h"

#define ENV_NSVC          106
#define ENV_SVC_BASE      0x103F0000u   /* service trap addresses: base + 4*i */
#define ENV_SVC_TABLE     0x10343D14u   /* where the kernel's g_svc lives */
#define ENV_CAPS_ADDR     0x10343EC8u   /* g_caps */
#define ENV_ARGV0_ADDR    0x10343CD4u   /* app_elf_name */
#define ENV_ARGS_ADDR     0x1035FAB0u   /* app_args_buf */
#define ENV_STACK_TOP     0x13A80000u
#define ENV_MMAP_TOP      0x13A00000u
#define ENV_GPU_BASE      0x4A000000u
#define ENV_SYSREG_BASE   0x40000000u
#define ENV_SYSREG_CYC_PER_INSN 3u
#define ENV_CRAM0_BASE    0x30000000u
#define ENV_CRAM0_SIZE    0x01000000u
#define ENV_MAX_FDS       32
#define ENV_MMAP_SLOTS    4096
#define ENV_NV_CAP        0x40000u
#define ENV_MAX_SLOTS     32

typedef struct env_slot {
    int      used;
    uint32_t id;
    char     name[24];
    int      nv;               /* nonvolatile (CRAM-backed, read/write) */
    int      host_fd;          /* RO host file, -1 if none */
    uint8_t *mem;              /* RO in-memory content or NV CRAM window */
    uint64_t size;             /* RO: datatable size.  NV: cached logical size */
} env_slot;

typedef struct env_fd {
    int      in_use;
    int      kind;             /* 1 RO slot file, 2 NV file, 3 root dir */
    int      slot;             /* index into slots[] */
    int      flags;
    uint64_t offset, size;
    int      dirty;
    int      dir_pos;
} env_fd;

typedef struct env_frame_rec {
    uint64_t seq;              /* GPU flip sequence number */
    int      idx;              /* displayed buffer */
    uint32_t token;
    uint64_t hash_px, hash_z, hash_pal, hash_cmd;
    uint64_t cmd_words;
    uint32_t host_framecount;
} env_frame_rec;

/* memory modes (--mem) */
enum { ENV_MEM_FLAT = 0, ENV_MEM_LINT = 1, ENV_MEM_STRICT = 2 };

/* run end (qenv.done).  ENV_DONE_RESULT is the app profile's result_line
 * (Quake: the timedemo result); summary.json still calls it
 * "timedemo_complete" so that recordings stay comparable across versions. */
enum { ENV_DONE_RESULT = 1, ENV_DONE_ERROR = 2, ENV_DONE_EXIT = 3, ENV_DONE_MARKER = 4 };
#define ENV_DONE_TIMEDEMO ENV_DONE_RESULT

#define ENV_MAX_EXTRA_FILES 8

/* Options that must be set before env_init (it clears everything else). */
typedef struct env_opts {
    int         normalize_exe_date;   /* rewrite the app profile's build_stamp date */
    int         gpu_sched;         /* GPU_SCHED_* (--gpu-sched) */
    int         mem_mode;          /* ENV_MEM_* (--mem) */
    uint64_t    seed_mem;          /* --seed-mem: 0 = zero-filled memory; else PRNG seed for every byte
                                      the device does not initialize (see env_seed_memory) */
    const char *end_marker;        /* --end-on: stop when a guest console line contains this text */
    const char *window_marker;     /* --window-on: measurement window starts at the host frame that
                                      prints a console line containing this text */
    const char *window_end_marker; /* --window-end-on: ... and ends before the host frame that prints this */
    int         n_extra_files;     /* --file NAME=PATH: extra read-only data slots (ids 20+) */
    const char *extra_name[ENV_MAX_EXTRA_FILES];
    const char *extra_path[ENV_MAX_EXTRA_FILES];
} env_opts;

typedef struct qenv {
    qsim_cpu *cpu;
    gpu_t    *gpu;             /* NULL with --no-gpu-model */
    struct qsim_lint *lint;    /* memory coherency model (--mem lint|strict), NULL = flat */
    int       gpu_sched;       /* GPU_SCHED_* (--gpu-sched), set before env_init */
    int       mem_mode;        /* ENV_MEM_* (--mem), set before env_init */
    env_opts  opt;             /* pre-init options (gpu_sched/mem_mode/normalize_exe_date mirror these) */
    /* --window-on: host_framecount when the marker line was printed (valid if window_seen) */
    int       window_seen;
    uint32_t  window_hfc;
    uint64_t  window_flips;    /* GPU flips executed when the marker was printed (informational) */
    int       window_end_seen;
    uint32_t  window_end_hfc;
    uint8_t  *scan;            /* memory scanout / GPU z snapshots read: committed SDRAM */
    /* host_framecount at FLIP publish, by fence token (lazy schedules run the
     * FLIP later; frames.csv reports the value at publish in every schedule) */
    struct { uint32_t token, hfc; } flip_pub[64];
    int       nflip_pub;
    uint64_t  env_uc_writes;
    int       check_invariant;   /* --check-strict-invariant: verify the split-memory invariant at every flip */
    uint64_t  invariant_errors;   /* kernel stores through the uncached alias (strict: not modelled as stale) */
    qsim_syms syms;
    FILE     *log;             /* diagnostics (stderr) */
    int       verbose;

    /* workload */
    int       argc;
    char    **argv;
    const char *elf_path;
    const app_profile *app;    /* app profile (app.h); never NULL after env_init */
    const char *data_dir;      /* --data-dir: host directory holding the app data files */
    uint32_t  entry, bss_end, heap_size_override;
    uint32_t  host_framecount_addr, zbuffer_addr;

    /* memory model */
    uint32_t  brk_base, cur_brk, mmap_bottom;
    struct { uint32_t base, len; uint8_t free; } mmap_slots[ENV_MMAP_SLOTS];
    int       mmap_used;
    uint8_t  *cram0;
    uint32_t  dma_next;

    /* files */
    env_slot  slots[ENV_MAX_SLOTS];
    int       nslots;          /* registry order */
    env_fd    fds[ENV_MAX_FDS];
    char     *ini_text;

    /* config (ini) */
    struct { char sect[32], key[64], val[256]; } *cfg;
    int       ncfg;

    /* virtual clock */
    uint64_t  vt_us;
    uint64_t  vt_quantum;
    uint32_t  clk_pc[64];
    uint8_t   clk_flag[64];
    int       nclk;
    uint64_t  clk_advances;

    /* video (kernel role model, instant vblank) */
    int       hw_display, buf_display, buf_draw, buf_ready, swap_kicked;
    uint16_t  mode_w, mode_h, mode_stride;
    uint8_t   mode_color;
    uint32_t  frame_bytes;
    int       display_mode;
    uint32_t  palette[256];
    uint64_t  kernel_flips, palette_sets;

    /* audio ring */
    int       audio_active;
    uint64_t  audio_wr, audio_rd, audio_rd_frac, audio_last_t;
    uint64_t  mixer_seq;

    /* GPU bookkeeping */
    uint64_t  cmd_hash, cmd_words;       /* since the last presented frame */
    uint64_t  cmd_words_total;
    uint64_t  zhash, zclears;            /* pre-clear z snapshots since the last flip */
    uint64_t  flips;                     /* GPU CMD_FLIP count */
    env_frame_rec pend[8];
    int       npend;
    /* minimal ring model for --no-gpu-model */
    uint32_t  ng_words[4096];
    uint32_t  ng_staged, ng_wrptr, ng_fence, ng_mode;

    /* console */
    FILE     *console;
    char      line[1024];
    int       line_len;
    int       echo;

    /* run state */
    int       done;                      /* ENV_DONE_*: 1 timedemo finished, 2 error, 3 exit, 4 end marker */
    char      done_msg[256];
    char      timedemo_line[256];        /* the app profile's result_line, verbatim */
    uint64_t  result_frames;             /* its capture group 1: measurement-window frames */
    char      parity_val[APP_MAX_PARITY][64];  /* app profile "parity" captures */
    int       parity_seen[APP_MAX_PARITY];
    uint64_t  svc_calls[ENV_NSVC];
    uint64_t  svc_total;                 /* hook "instructions" counted by qsim_run */
    int       normalize_exe_date, exe_dates_normalized;
    uint64_t  ecalls_linux[512], ecalls_vendor[16][16];
    uint64_t  unknown_mmio;
    uint64_t  sysreg_reads, term_fb_writes;
    uint32_t  term_fb_ctrl;
    int       sys_error_seen;            /* app profile error_line matched */

    /* GPU test-vector capture (--gpuvec) */
    const char *vec_dir;
    uint64_t  vec_frames[256];
    int       vec_nframes, vec_captured, vec_finish;
    uint64_t  vec_frame, vec_mem_chunks;
    FILE     *vec_f;
    uint8_t  *vec_shadow;
    uint32_t  vec_words[4096];
    uint32_t  vec_n;

    /* PPM snapshots */
    const char *ppm_dir;
    int       ppm_every;
    uint64_t  ppm_start;
} qenv;

int  env_init(qenv *E, const char *elf_path, const app_profile *app, const char *data_dir,
              int argc, char **argv, int use_gpu_model, uint32_t heap_size_override,
              FILE *console_log);
void env_free(qenv *E);
const char *env_svc_name(int i);

/* 64-bit FNV-1a */
static inline uint64_t fnv1a64(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV64_INIT 0xcbf29ce484222325ull

#endif
