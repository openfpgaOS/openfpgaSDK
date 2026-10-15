//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * lint.c -- memory coherency model: L1 D-cache vs committed SDRAM (see lint.h).
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "lint.h"

#define SD_BASE 0x10000000u

const char *const lint_class_names[LH_N] = {
    "gpu_read_of_uncommitted_cpu_store",
    "cpu_cached_read_of_stale_line",
    "writeback_reverts_gpu_or_uncached_write",
    "cpu_uncached_read_of_uncommitted_store",
    "cbo_inval_dropped_dirty_line",
    "wc_gpu_read_of_line_stored_since_cbo",
    "wc_cpu_cached_access_to_gpu_written_line",
};

qsim_lint *lint_new(uint8_t *V, uint32_t sdram_size, int strict)
{
    qsim_lint *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->lines = sdram_size >> 6;
    L->V = V;
    L->strict = strict;
    L->map = calloc(L->lines + 2, 1);
    L->dirty_pc = calloc(L->lines + 2, sizeof *L->dirty_pc);
    if (strict) {
        /* +64: the inline hooks may touch one byte past a straddling access */
        L->C = malloc((size_t)sdram_size + 64);
        if (L->C) { memcpy(L->C, V, sdram_size); memset(L->C + sdram_size, 0, 64); }
    } else {
        L->C = V;
    }
    if (!L->map || !L->dirty_pc || !L->C) { lint_free(L); return NULL; }
    return L;
}

void lint_free(qsim_lint *L)
{
    if (!L) return;
    if (L->strict) free(L->C);
    free(L->map); free(L->dirty_pc);
    free(L);
}

/* record the (class, pc) site of a hazard (layout-independent identity) */
static void lint_site_add(qsim_lint *L, int cls, uint32_t pc)
{
    uint32_t h = (pc * 2654435761u + (uint32_t)cls * 0x9E3779B9u) % LINT_MAX_SITES;
    for (uint32_t i = 0; i < LINT_MAX_SITES; i++) {
        uint32_t k = (h + i) % LINT_MAX_SITES;
        if (L->site[k].used) {
            if (L->site[k].pc == pc && L->site[k].cls == (uint8_t)cls) { L->site[k].n++; return; }
            continue;
        }
        if (L->nsites >= LINT_MAX_SITES - 8) { L->sites_dropped++; return; }
        L->site[k].used = 1;
        L->site[k].pc = pc;
        L->site[k].cls = (uint8_t)cls;
        L->site[k].n = 1;
        L->nsites++;
        return;
    }
    L->sites_dropped++;
}

void lint_event(qsim_lint *L, int cls, uint32_t off, uint32_t pc)
{
    L->count[cls]++;
    if (cls < LH_N_AWARE) lint_site_add(L, cls, pc);
    if (L->nex[cls] >= LINT_MAX_EXAMPLES) return;
    uint32_t addr = SD_BASE + off;
    for (uint64_t i = 0; i < L->nex[cls]; i++)          /* distinct lines only */
        if ((L->ex[cls][i].addr >> 6) == (addr >> 6)) return;
    lint_example *e = &L->ex[cls][L->nex[cls]++];
    e->frame = L->frame ? *L->frame : 0;
    e->pc = pc;
    e->addr = addr;
    e->dirty_pc = (off >> 6) < L->lines ? L->dirty_pc[off >> 6] : 0;
}

static inline uint32_t cur_pc(const qsim_lint *L) { return L->pcp ? *L->pcp : 0; }

static inline struct lint_way *find_way(qsim_lint *L, uint32_t line)
{
    struct lint_way *wp = &L->way[(line & 1023u) * 2];
    return wp[0].tag == line + 1 ? wp : wp[1].tag == line + 1 ? wp + 1 : NULL;
}

static inline void set_flags(qsim_lint *L, struct lint_way *wy, uint32_t line, uint32_t flags)
{
    wy->flags = flags;
    L->map[line] = (uint8_t)((L->map[line] & ~(LM_DIRTY | LM_STALE)) | flags | LM_CACHED);
}

/* Remove a loaded way from L1.  writeback: dirty victim eviction semantics
 * (V -> C); otherwise the CPU copy is dropped (C -> V). */
static void drop_way(qsim_lint *L, struct lint_way *wy, int writeback, uint32_t pc)
{
    if (!wy->tag) return;
    uint32_t line = wy->tag - 1, lo = line << 6;
    if ((wy->flags & LM_DIRTY) && writeback) {
        if ((wy->flags & LM_STALE) && wy->smask) lint_event(L, LH_WB_REVERT, lo, pc);
        if (L->strict) memcpy(L->C + lo, L->V + lo, 64);
        L->evict_commits++;
    } else if (wy->flags & (LM_DIRTY | LM_STALE)) {
        if (L->strict) memcpy(L->V + lo, L->C + lo, 64);
        if (!(wy->flags & LM_DIRTY)) L->stale_refresh++;
    }
    L->map[line] &= (uint8_t)~(LM_CACHED | LM_DIRTY | LM_STALE);
    memset(wy, 0, sizeof *wy);
}

void lint_access_slow(qsim_lint *L, uint32_t off, unsigned size, int store, uint32_t pc)
{
    uint32_t line = off >> 6;
    if (line >= L->lines) return;
    uint32_t set = line & 1023u;
    struct lint_way *wp = &L->way[set * 2];
    int w = wp[0].tag == line + 1 ? 0 : (wp[1].tag == line + 1 ? 1 : -1);
    uint64_t bytes = lint_bytes(off, size);
    if (w >= 0) {
        struct lint_way *wy = &wp[w];
        if (wy->flags & LM_STALE) {
            if (L->map[line] & LM_GPUW) lint_event(L, LH_WC_CPU_GPUW, off, pc);
            if (!store) {
                if (wy->smask & bytes) lint_event(L, LH_CPU_RD_STALE, off, pc);
            } else {
                /* the CPU's new bytes supersede the SDRAM change */
                wy->smask &= ~bytes;
                if (!wy->smask) set_flags(L, wy, line, wy->flags & ~LM_STALE);
            }
        }
    } else {
        /* miss: the victim way is written back if dirty; the line is refilled
         * from SDRAM (V == C there by the invariant, so no data moves) */
        if (L->map[line] & LM_GPUW) lint_event(L, LH_WC_CPU_GPUW, off, pc);
        w = L->mru[set] ^ 1;
        drop_way(L, &wp[w], 1, pc);
        wp[w].tag = line + 1;
        set_flags(L, &wp[w], line, 0);
    }
    if (store) {
        struct lint_way *wy = &wp[w];
        if (!(wy->flags & LM_DIRTY)) {
            L->dirty_pc[line] = pc;
            set_flags(L, wy, line, wy->flags | LM_DIRTY);
            L->map[line] |= LM_WCDIRTY;
        }
        wy->dmask |= bytes;
    }
    L->mru[set] = (uint8_t)w;
}

void lint_uc_rd_slow(qsim_lint *L, uint32_t off, unsigned size, uint32_t pc)
{
    for (uint32_t o = off; o < off + size; o = (o | 63u) + 1u) {
        uint32_t line = o >> 6;
        if (line >= L->lines) break;
        struct lint_way *wy = find_way(L, line);
        if (wy && (wy->flags & LM_DIRTY) && (wy->dmask & lint_bytes(o, off + size - o)))
            lint_event(L, LH_UC_RD_UNCOMMITTED, o, pc);
    }
}

void lint_uc_wr_slow(qsim_lint *L, uint32_t off, unsigned size)
{
    for (uint32_t o = off; o < off + size; o = (o | 63u) + 1u) {
        uint32_t line = o >> 6;
        if (line >= L->lines) break;
        unsigned n = off + size - o;
        if (n > 64u - (o & 63u)) n = 64u - (o & 63u);
        struct lint_way *wy = find_way(L, line);
        if (wy) {
            wy->smask |= lint_bytes(o, n);
            set_flags(L, wy, line, wy->flags | LM_STALE);
        } else if (L->strict) {
            memcpy(L->V + o, L->C + o, n);
        }
    }
}

void lint_gpu_rd_slow(qsim_lint *L, uint32_t off, uint8_t f)
{
    uint32_t line = off >> 6;
    if (f & LM_DIRTY) {
        struct lint_way *wy = find_way(L, line);
        if (wy && ((wy->dmask >> (off & 63u)) & 1u)) lint_event(L, LH_GPU_RD_UNCOMMITTED, off, cur_pc(L));
    }
    if (f & LM_WCDIRTY) lint_event(L, LH_WC_GPU_RD_DIRTY, off, cur_pc(L));
}

void lint_gpu_wr_slow(qsim_lint *L, uint32_t off, uint8_t f)
{
    uint32_t line = off >> 6;
    if (f & LM_CACHED) {
        struct lint_way *wy = find_way(L, line);
        wy->smask |= 1ull << (off & 63u);
        set_flags(L, wy, line, wy->flags | LM_STALE);
    } else if (L->strict) {
        L->V[off] = L->C[off];
    }
    L->map[line] |= LM_GPUW;
}

void lint_cbo(qsim_lint *L, unsigned op, uint32_t off, uint32_t pc)
{
    uint32_t line = off >> 6;
    if (line >= L->lines) return;
    L->map[line] &= (uint8_t)~(LM_WCDIRTY | LM_GPUW);
    struct lint_way *wy = find_way(L, line);
    if (!wy) return;                       /* askCbm needs WAYS_HIT */
    if (op == 0) {                         /* inval: drop without writeback */
        if (wy->flags & LM_DIRTY) { lint_event(L, LH_INVAL_DIRTY, off, pc); L->inval_drops++; }
        drop_way(L, wy, 0, pc);
        return;
    }
    /* clean / flush: writeback if dirty; the line stays loaded.  After a
     * writeback SDRAM equals the L1 copy, so the line is no longer stale. */
    if (wy->flags & LM_DIRTY) {
        uint32_t lo = line << 6;
        if ((wy->flags & LM_STALE) && wy->smask) lint_event(L, LH_WB_REVERT, lo, pc);
        if (L->strict) memcpy(L->C + lo, L->V + lo, 64);
        L->cbo_commits++;
        wy->dmask = wy->smask = 0;
        set_flags(L, wy, line, 0);
    }
}

void lint_cbo_range(qsim_lint *L, unsigned op, uint32_t off, uint32_t len, uint32_t pc)
{
    if (!len) return;
    uint64_t end = (uint64_t)off + len;
    if (end > (uint64_t)L->lines << 6) end = (uint64_t)L->lines << 6;
    for (uint64_t a = off & ~63u; a < end; a += 64) lint_cbo(L, op, (uint32_t)a, pc);
}

void lint_env_store(qsim_lint *L, uint32_t off, uint32_t len)
{
    if (!len || (off >> 6) >= L->lines) return;
    uint64_t end = (uint64_t)off + len;
    if (end > (uint64_t)L->lines << 6) end = (uint64_t)L->lines << 6;
    L->env_store_ranges++;
    /* Line by line, in address order: a line evicted while the range is
     * processed is already complete in V, so its writeback is correct; lines
     * not yet processed are never read. */
    for (uint64_t a = off; a < end; a = (a | 63u) + 1u) {
        uint64_t n = end - a;
        if (n > 64u - (a & 63u)) n = 64u - (a & 63u);
        lint_access_slow(L, (uint32_t)a, (unsigned)n, 1, L->env_pc);
    }
}

void lint_evict_all(qsim_lint *L, uint32_t evict_off)
{
    for (uint32_t i = 0; i < L->lines; i++) L->map[i] &= (uint8_t)~(LM_WCDIRTY | LM_GPUW);
    for (uint32_t a = 0; a < 1024u * 2u * 64u; a += 64)
        if (((evict_off + a) >> 6) < L->lines) lint_access_slow(L, evict_off + a, 1, 0, L->env_pc);
}

uint64_t lint_total_aware(const qsim_lint *L)
{
    uint64_t t = 0;
    for (int i = 0; i < LH_N_AWARE; i++) t += L->count[i];
    return t;
}

uint64_t lint_total_wc(const qsim_lint *L)
{
    uint64_t t = 0;
    for (int i = LH_N_AWARE; i < LH_N; i++) t += L->count[i];
    return t;
}

void lint_print(const qsim_lint *L, FILE *o)
{
    for (int i = 0; i < LH_N; i++) {
        fprintf(o, "  %-44s %12" PRIu64 "%s\n", lint_class_names[i], L->count[i],
                i >= LH_N_AWARE ? "   (worst case, advisory)" : "");
        for (uint64_t k = 0; k < L->nex[i]; k++)
            fprintf(o, "      e.g. frame %" PRIu64 " addr %08x pc %08x dirtied by pc %08x\n", L->ex[i][k].frame,
                    L->ex[i][k].addr, L->ex[i][k].pc, L->ex[i][k].dirty_pc);
    }
    fprintf(o, "  commits: eviction %" PRIu64 ", cbo %" PRIu64 "; stale refreshes %" PRIu64
               "; kernel store ranges %" PRIu64 "\n",
            L->evict_commits, L->cbo_commits, L->stale_refresh, L->env_store_ranges);
}

void lint_json(const qsim_lint *L, FILE *o, const char *ind)
{
    fprintf(o, "%s\"hazards\": %" PRIu64 ",\n%s\"hazards_worst_case\": %" PRIu64 ",\n%s\"classes\": {",
            ind, lint_total_aware(L), ind, lint_total_wc(L), ind);
    for (int i = 0; i < LH_N; i++)
        fprintf(o, "%s\"%s\": %" PRIu64, i ? ", " : "", lint_class_names[i], L->count[i]);
    fprintf(o, "},\n%s\"examples\": [", ind);
    int first = 1;
    for (int i = 0; i < LH_N; i++)
        for (uint64_t k = 0; k < L->nex[i]; k++) {
            fprintf(o, "%s\n%s  {\"class\": \"%s\", \"frame\": %" PRIu64 ", \"addr\": \"0x%08x\", \"pc\": \"0x%08x\", "
                       "\"dirtied_by_pc\": \"0x%08x\"}",
                    first ? "" : ",", ind, lint_class_names[i], L->ex[i][k].frame, L->ex[i][k].addr, L->ex[i][k].pc,
                    L->ex[i][k].dirty_pc);
            first = 0;
        }
    fprintf(o, "%s],\n%s\"commits\": {\"eviction\": %" PRIu64 ", \"cbo\": %" PRIu64 ", \"stale_refresh\": %" PRIu64
               ", \"kernel_store_ranges\": %" PRIu64 "}",
            first ? "" : "\n", ind, L->evict_commits, L->cbo_commits, L->stale_refresh, L->env_store_ranges);
}

uint64_t lint_check_invariant(qsim_lint *L, FILE *log)
{
    /* strict mode self-check: V == C on every line that is not loaded, and on
     * loaded lines that are neither dirty nor stale; dirty/stale lines may differ */
    if (!L->strict) return 0;
    uint64_t bad = 0;
    for (uint32_t line = 0; line < L->lines; line++) {
        uint32_t lo = line << 6;
        if (!memcmp(L->V + lo, L->C + lo, 64)) continue;
        struct lint_way *wy = find_way(L, line);
        if (wy && (wy->flags & (LM_DIRTY | LM_STALE))) continue;
        if (bad < 8 && log)
            fprintf(log, "lint: invariant: line %08x differs (loaded=%d flags=%x)\n", 0x10000000u + lo, wy != NULL,
                    wy ? wy->flags : 0);
        bad++;
    }
    return bad;
}
