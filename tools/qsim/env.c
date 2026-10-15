//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * env.c -- openfpgaOS os25 environment for the qsim CPU core.
 * Every rule cites ENV_SPEC.md (section numbers) or the kernel source it ports.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "env.h"
#include "cache.h"
#include "gpuvec.h"
#include "lint.h"

/* ------------------------------------------------------------------------ */
/* constants from the kernel / SDK                                          */

#define EINVAL_ 22
#define ENOENT_ 2
#define EACCES_ 13
#define EBADF_ 9
#define EMFILE_ 24
#define ENOMEM_ 12
#define ENOSYS_ 38
#define EFBIG_ 27
#define ENOTDIR_ 20

#define O_ACCMODE_  3
#define O_TRUNC_    0x200
#define O_APPEND_   0x400
#define O_DIRECTORY_ 0x10000

#define FDK_RO   1
#define FDK_NV   2
#define FDK_DIR  3

static const char *const svc_names[ENV_NSVC] = {
    "video_init", "video_get_surface", "video_flip", "video_wait_flip", "video_vsync",
    "video_set_palette", "video_set_palette_bulk", "video_set_palette_vga4", "video_clear",
    "video_flush_cache", "video_set_display_mode", "video_set_color_mode", "video_acquire_next",
    "video_buffer_addr", "input_poll", "input_get_state", "input_poll_p0", "input_set_deadzone",
    "mixer_init", "mixer_play", "mixer_stop", "mixer_stop_all", "mixer_set_volume", "mixer_set_pan",
    "mixer_voice_active", "mixer_pump", "mixer_set_loop", "mixer_set_rate", "mixer_set_rate_raw",
    "mixer_set_vol_lr", "mixer_set_bidi", "mixer_get_position", "mixer_set_position",
    "mixer_set_voice", "mixer_set_voice_raw", "mixer_set_vol_rate", "mixer_poll_ended",
    "mixer_alloc_samples", "mixer_free_samples", "mixer_set_end_callback", "audio_init",
    "audio_write", "audio_get_free", "timer_set_callback", "timer_stop", "timer_get_us",
    "timer_get_ms", "timer_delay_us", "cache_flush", "cache_clean_range", "cache_inval_range",
    "video_set_vsync_callback", "file_size", "file_size_fd", "mixer_retrigger", "mixer_play_8bit",
    "mixer_set_group", "mixer_set_group_volume", "mixer_set_master_volume", "audio_stream_open",
    "audio_stream_write", "audio_stream_ready", "audio_stream_close", "file_slot_register",
    "smp_bank_preload_base", "smp_bank_preload_size", "mixer_alloc_for_group", "mixer_voice_group",
    "cache_flush_range", "input_get_keyboard_state", "input_read_mouse_state", "mixer_play_h",
    "mixer_play_8bit_h", "mixer_alloc_for_group_h", "mixer_retrigger_h", "mixer_stop_h",
    "mixer_handle_active", "mixer_handle_group", "mixer_handle_voice", "mixer_set_volume_h",
    "mixer_set_pan_h", "mixer_set_loop_h", "mixer_set_rate_h", "mixer_set_rate_raw_h",
    "mixer_set_vol_lr_h", "mixer_set_bidi_h", "mixer_get_position_h", "mixer_set_position_h",
    "mixer_set_voice_h", "mixer_set_voice_raw_h", "mixer_set_vol_rate_h", "mixer_poll_ended_h",
    "video_get_timing", "video_set_refresh_vtotal", "video_set_mode", "video_get_mode",
    "video_get_mode_count", "video_get_mode_info", "video_get_caps", "video_check_mode",
    "config_get", "config_get_int", "config_get_bool", "config_next", "file_set_idle_hook",
    "input_is_docked",
};
const char *env_svc_name(int i) { return (i >= 0 && i < ENV_NSVC) ? svc_names[i] : "?"; }

/* ------------------------------------------------------------------------ */
/* diagnostics                                                              */

static void fatal(qenv *E, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E->done_msg, sizeof E->done_msg, fmt, ap);
    va_end(ap);
    fprintf(E->log, "qsim: FATAL: %s (pc=%08x ra=%08x insns=%" PRIu64 ")\n", E->done_msg,
            E->cpu->pc, E->cpu->x[1], E->cpu->instret);
    E->done = 2;
    qsim_request_stop(E->cpu);
}

/* ------------------------------------------------------------------------ */
/* guest memory                                                             */

static uint8_t *gp(qenv *E, uint32_t addr, uint32_t len)
{
    uint8_t *p = qsim_mem_ptr(E->cpu, addr, len);
    if (p) return p;
    if (addr - ENV_CRAM0_BASE < ENV_CRAM0_SIZE && len <= ENV_CRAM0_SIZE - (addr - ENV_CRAM0_BASE))
        return E->cram0 + (addr - ENV_CRAM0_BASE);
    return NULL;
}

static uint32_t rd32(qenv *E, uint32_t addr)
{
    uint8_t *p = gp(E, addr, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/* The kernel wrote [addr, addr+len) of guest memory (through gp()): decode
 * invalidation, and for the memory coherency model a kernel memcpy/memset
 * through the cached alias (hal: plain C stores), or an uncached-alias store. */
static void env_wrote(qenv *E, uint32_t addr, uint32_t len)
{
    qsim_invalidate(E->cpu, addr, len);
    qsim_lint *L = E->lint;
    if (!L || !len) return;
    uint32_t size = E->cpu->sdram_size, off = addr - E->cpu->sdram_base;
    if (off < size) {
        lint_env_store(L, off, len > size - off ? size - off : len);
        return;
    }
    off = addr - E->cpu->sdram_alias_base;
    if (off < size) {
        if (len > size - off) len = size - off;
        if (L->strict) memcpy(L->C + off, L->V + off, len);
        E->env_uc_writes++;
    }
}

static void wr32(qenv *E, uint32_t addr, uint32_t v)
{
    uint8_t *p = gp(E, addr, 4);
    if (p) { memcpy(p, &v, 4); env_wrote(E, addr, 4); }
}

/* copy a NUL-terminated guest string; returns 0 or -1 */
static int gstr(qenv *E, uint32_t addr, char *out, size_t max)
{
    if (!addr) return -1;
    for (size_t i = 0; i < max; i++) {
        uint8_t *p = gp(E, addr + (uint32_t)i, 1);
        if (!p) return -1;
        out[i] = (char)*p;
        if (!*p) return 0;
    }
    out[max - 1] = 0;
    return 0;
}

static void gzero(qenv *E, uint32_t addr, uint32_t len)
{
    uint8_t *p = gp(E, addr, len);
    if (!p) return;
    memset(p, 0, len);
    env_wrote(E, addr, len);
}

static int ascii_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int stricmp_(const char *a, const char *b)
{
    while (*a && *b) {
        int d = ascii_lc((unsigned char)*a) - ascii_lc((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return ascii_lc((unsigned char)*a) - ascii_lc((unsigned char)*b);
}

/* ------------------------------------------------------------------------ */
/* ELF loader (kernel/loader.c semantics: vaddr, app vmap v1)              */

static int load_elf(qenv *E, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(E->log, "qsim: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    fclose(f);
    int rc = -1;
    if (n < 52 || memcmp(b, "\177ELF", 4) || b[4] != 1 || b[5] != 1) goto out;
    uint32_t entry, phoff; uint16_t phentsize, phnum;
    memcpy(&entry, b + 24, 4); memcpy(&phoff, b + 28, 4);
    memcpy(&phentsize, b + 42, 2); memcpy(&phnum, b + 44, 2);
    uint32_t top = 0;
    for (unsigned i = 0; i < phnum; i++) {
        uint32_t ph[8];
        if ((size_t)phoff + (size_t)i * phentsize + 32 > (size_t)n) goto out;
        memcpy(ph, b + phoff + i * phentsize, 32);
        if (ph[0] != 1 || ph[5] == 0) continue;
        uint32_t va = ph[2], fsz = ph[4], msz = ph[5];
        int in_bram = va >= 0x4000 && va + msz <= 0x7800;
        int in_sd = va >= 0x10400000u && va + msz <= ENV_MMAP_TOP;
        if (!in_bram && !in_sd) {
            fprintf(E->log, "qsim: PT_LOAD %08x+%x outside the app vmap (kernel error -7)\n", va, msz);
            goto out;
        }
        uint8_t *dst = gp(E, va, msz);
        if (!dst || (size_t)ph[1] + fsz > (size_t)n) goto out;
        memcpy(dst, b + ph[1], fsz);
        memset(dst + fsz, 0, msz - fsz);
        qsim_invalidate(E->cpu, va, msz);
        if (in_sd && va + msz > top) top = va + msz;
    }
    E->entry = entry;
    E->bss_end = (top + 15) & ~15u;
    /* Normalize the app's build stamp -- "<build_stamp> HH:MM:SS Mon DD YYYY",
     * i.e. a printf of __TIME__ and __DATE__ (Quake: "Exe: ", host.c:984 and
     * host_cmd.c:1321) -- in the loaded image, so that two builds of the same
     * source made at different times print (and render on the console)
     * identical text.  The prefix comes from the app profile; no prefix means
     * no rewrite. */
    const char *stamp = E->app ? E->app->build_stamp : "";
    uint32_t stamp_len = (uint32_t)strlen(stamp);
    if (E->normalize_exe_date && stamp_len) {
        for (unsigned i = 0; i < phnum; i++) {
            uint32_t ph[8];
            memcpy(ph, b + phoff + i * phentsize, 32);
            if (ph[0] != 1 || ph[4] < stamp_len + 21) continue;
            uint8_t *m = gp(E, ph[2], ph[4]);
            if (!m) continue;
            for (uint32_t k = 0; k + stamp_len + 20 <= ph[4]; k++) {
                const uint8_t *q = m + k + stamp_len;
                if (memcmp(m + k, stamp, stamp_len)) continue;
                int ok = isdigit(q[0]) && isdigit(q[1]) && q[2] == ':' && isdigit(q[3]) && isdigit(q[4]) &&
                         q[5] == ':' && isdigit(q[6]) && isdigit(q[7]) && q[8] == ' ' && isupper(q[9]) &&
                         islower(q[10]) && islower(q[11]) && q[12] == ' ' && (q[13] == ' ' || isdigit(q[13])) &&
                         isdigit(q[14]) && q[15] == ' ' && isdigit(q[16]) && isdigit(q[17]) && isdigit(q[18]) &&
                         isdigit(q[19]);
                if (!ok) continue;
                memcpy(m + k + stamp_len, "00:00:00 Jan 01 2000", 20);
                E->exe_dates_normalized++;
            }
            qsim_invalidate(E->cpu, ph[2], ph[4]);
        }
    }
    rc = 0;
out:
    free(b);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* data slots (ENV_SPEC 6.1)                                                */

static int host_open_ci(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int fd = -1;
    while ((e = readdir(d))) {
        if (!stricmp_(e->d_name, name)) {
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            fd = open(p, O_RDONLY);
            break;
        }
    }
    closedir(d);
    return fd;
}

static env_slot *add_slot(qenv *E, uint32_t id, const char *name, int nv)
{
    env_slot *s = &E->slots[E->nslots++];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->id = id;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->nv = nv;
    s->host_fd = -1;
    if (nv) s->mem = calloc(1, ENV_NV_CAP);
    return s;
}

static int slot_index_by_id(qenv *E, uint32_t id)
{
    for (int i = 0; i < E->nslots; i++) if (E->slots[i].id == id) return i;
    return -1;
}

/* file_slot_lookup: strip '/', full-name match, then basename match */
static int slot_lookup(qenv *E, const char *path)
{
    while (*path == '/') path++;
    for (int i = 0; i < E->nslots; i++) if (!stricmp_(E->slots[i].name, path)) return i;
    const char *b = strrchr(path, '/');
    if (b) {
        b++;
        for (int i = 0; i < E->nslots; i++) if (!stricmp_(E->slots[i].name, b)) return i;
    }
    return -1;
}

static int alloc_fd(qenv *E)
{
    for (int i = 3; i < ENV_MAX_FDS; i++)
        if (!E->fds[i].in_use) { memset(&E->fds[i], 0, sizeof E->fds[i]); E->fds[i].in_use = 1; return i; }
    return -EMFILE_;
}

static int want_write(int flags) { return (flags & O_ACCMODE_) != 0 || (flags & O_TRUNC_); }

/* nv_infer_text_size (kernel syscall.c:1425) */
static uint64_t nv_infer_text(env_slot *s)
{
    uint32_t last = 0;
    int seen = 0;
    for (uint32_t i = 0; i < ENV_NV_CAP; i++) {
        uint8_t c = s->mem[i];
        if (c == 0 || c == 0xff) return seen ? last : 0;
        seen = 1;
        last = i + 1;
    }
    return last;
}

static long open_nv(qenv *E, int fd, int si, int flags)
{
    env_fd *f = &E->fds[fd];
    env_slot *s = &E->slots[si];
    int is_save = s->id >= 10 && s->id <= 19;
    f->kind = FDK_NV;
    f->slot = si;
    f->flags = flags;
    f->size = s->size;
    if (f->size == 0 && is_save && (flags & O_ACCMODE_) == 0) f->size = ENV_NV_CAP;
    if (f->size == 0 && !is_save && (flags & O_ACCMODE_) == 0) f->size = nv_infer_text(s);
    if (flags & O_TRUNC_) { f->size = 0; f->dirty = 1; s->size = 0; }
    if (flags & O_APPEND_) f->offset = f->size;
    return fd;
}

static long sys_openat(qenv *E, uint32_t path_addr, int flags)
{
    char path[256];
    if (!path_addr || gstr(E, path_addr, path, sizeof path)) return -EINVAL_;
    if (!path[0]) return -ENOENT_;
    int fd = alloc_fd(E);
    if (fd < 0) return fd;
    env_fd *f = &E->fds[fd];
    if (E->verbose) fprintf(E->log, "qsim: openat(\"%s\", 0x%x)\n", path, flags);
    if (flags & O_DIRECTORY_) { f->kind = FDK_DIR; return fd; }
    if (!strncmp(path, "slot:", 5)) {
        const char *p = path + 5;
        if (*p < '0' || *p > '9') { f->in_use = 0; return -EINVAL_; }
        int id = atoi(p);
        int si = slot_index_by_id(E, (uint32_t)id);
        if (id == 8 || (id >= 10 && id <= 19)) {
            if (si < 0) { f->in_use = 0; return -EINVAL_; }
            return open_nv(E, fd, si, flags);
        }
        if (si < 0 || E->slots[si].size == 0) { f->in_use = 0; return -ENOENT_; }
        if (want_write(flags)) { f->in_use = 0; return -EACCES_; }
        f->kind = FDK_RO; f->slot = si; f->size = E->slots[si].size; f->flags = flags;
        return fd;
    }
    if (!strncmp(path, "save:", 5) || !strncmp(path, "save_", 5)) {
        const char *p = path + 5;
        if (*p < '0' || *p > '9') { f->in_use = 0; return -EINVAL_; }
        int si = slot_index_by_id(E, 10u + (uint32_t)atoi(p));
        if (si < 0) { f->in_use = 0; return -EINVAL_; }
        return open_nv(E, fd, si, flags);
    }
    int si = slot_lookup(E, path);
    if (si >= 0) {
        env_slot *s = &E->slots[si];
        if (s->nv) return open_nv(E, fd, si, flags);
        if (want_write(flags)) { f->in_use = 0; return -EACCES_; }
        if (s->size == 0) { f->in_use = 0; return -ENOENT_; }
        f->kind = FDK_RO; f->slot = si; f->size = s->size; f->flags = flags;
        return fd;
    }
    f->in_use = 0;
    return -ENOENT_;
}

static long sys_close(qenv *E, long fd)
{
    if (fd < 3 || fd >= ENV_MAX_FDS || !E->fds[fd].in_use) return -EBADF_;
    env_fd *f = &E->fds[fd];
    if (f->kind == FDK_NV && f->dirty) E->slots[f->slot].size = f->size;
    f->in_use = 0;
    return 0;
}

static long sys_read(qenv *E, long fd, uint32_t buf, uint32_t count)
{
    if ((int32_t)count <= 0) return 0;
    if (fd < 0 || fd >= ENV_MAX_FDS || !E->fds[fd].in_use) return -EBADF_;
    env_fd *f = &E->fds[fd];
    if (f->kind == FDK_DIR) return -EBADF_;
    env_slot *s = &E->slots[f->slot];
    if (f->kind == FDK_NV && (f->flags & O_ACCMODE_) == 1) return -EBADF_;
    if (f->offset >= f->size) return 0;
    uint32_t n = count;
    if (n > f->size - f->offset) n = (uint32_t)(f->size - f->offset);
    uint8_t *dst = gp(E, buf, n);
    if (!dst) return -14;
    if (f->kind == FDK_NV) {
        memcpy(dst, s->mem + f->offset, n);
    } else if (s->host_fd >= 0) {
        ssize_t r = pread(s->host_fd, dst, n, (off_t)f->offset);
        if (r != (ssize_t)n) return -5;
    } else if (s->mem) {
        memcpy(dst, s->mem + f->offset, n);
    } else {
        memset(dst, 0, n);
    }
    env_wrote(E, buf, n);
    f->offset += n;
    return n;
}

static long sys_write(qenv *E, long fd, uint32_t buf, uint32_t count);

static long sys_llseek(qenv *E, long fd, uint32_t hi, uint32_t lo, uint32_t res, uint32_t whence)
{
    if (fd < 0 || fd >= ENV_MAX_FDS || !E->fds[fd].in_use) return -EBADF_;
    env_fd *f = &E->fds[fd];
    int64_t off = (int64_t)(((uint64_t)hi << 32) | lo), no;
    switch (whence) {
    case 0: no = off; break;
    case 1: no = (int64_t)f->offset + off; break;
    case 2: no = (int64_t)f->size + off; break;
    default: return -EINVAL_;
    }
    if (no < 0 || no > 0xFFFFFFFFll) return -EINVAL_;
    f->offset = (uint64_t)no;
    if (res) { wr32(E, res, (uint32_t)no); wr32(E, res + 4, (uint32_t)((uint64_t)no >> 32)); }
    return 0;
}

static long sys_getdents64(qenv *E, long fd, uint32_t buf, uint32_t count)
{
    if (fd < 0 || fd >= ENV_MAX_FDS || !E->fds[fd].in_use) return -EBADF_;
    env_fd *f = &E->fds[fd];
    if (f->kind != FDK_DIR) return -ENOTDIR_;
    uint32_t written = 0;
    while (f->dir_pos < E->nslots) {
        env_slot *s = &E->slots[f->dir_pos];
        uint32_t nl = (uint32_t)strlen(s->name);
        uint32_t reclen = (8 + 8 + 2 + 1 + nl + 1 + 7) & ~7u;
        if (written + reclen > count) break;
        uint8_t *d = gp(E, buf + written, reclen);
        if (!d) return -14;
        uint64_t ino = s->id + 1, doff = (uint64_t)f->dir_pos + 1;
        memcpy(d, &ino, 8);
        memcpy(d + 8, &doff, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(d + 16, &rl, 2);
        d[18] = 8;  /* DT_REG */
        memcpy(d + 19, s->name, nl + 1);
        env_wrote(E, buf + written, reclen);
        written += reclen;
        f->dir_pos++;
    }
    return written;
}

static void console_bytes(qenv *E, const uint8_t *p, uint32_t n);

static long sys_write(qenv *E, long fd, uint32_t buf, uint32_t count)
{
    if ((int32_t)count <= 0) return 0;
    if (fd == 1 || fd == 2) {
        uint8_t *p = gp(E, buf, count);
        if (!p) return -14;
        console_bytes(E, p, count);
        return count;
    }
    if (fd < 0 || fd >= ENV_MAX_FDS || !E->fds[fd].in_use) return -EBADF_;
    env_fd *f = &E->fds[fd];
    if (f->kind != FDK_NV) return -EACCES_;
    if ((f->flags & O_ACCMODE_) == 0) return -EBADF_;
    if (f->flags & O_APPEND_) f->offset = f->size;
    if (f->offset >= ENV_NV_CAP) return -EFBIG_;
    uint32_t n = count;
    if (n > ENV_NV_CAP - f->offset) n = (uint32_t)(ENV_NV_CAP - f->offset);
    uint8_t *src = gp(E, buf, n);
    if (!src) return -14;
    memcpy(E->slots[f->slot].mem + f->offset, src, n);
    f->offset += n;
    if (f->offset > f->size) f->size = f->offset;
    f->dirty = 1;
    E->slots[f->slot].size = f->size;   /* save_size_cache_store */
    return n;
}

static long sys_statx(qenv *E, long dirfd, uint32_t path, uint32_t sx)
{
    uint64_t size = 0;
    if (dirfd >= 0 && dirfd < ENV_MAX_FDS && E->fds[dirfd].in_use) {
        size = E->fds[dirfd].size;
    } else if (path) {
        long fd = sys_openat(E, path, 0);
        if (fd < 0) return -ENOENT_;
        size = E->fds[fd].size;
        E->fds[fd].in_use = 0;           /* no close side effects */
    } else {
        return -EBADF_;
    }
    gzero(E, sx, 256);
    wr32(E, sx, 0x7FF);
    wr32(E, sx + 40, (uint32_t)size);
    wr32(E, sx + 44, (uint32_t)(size >> 32));
    return 0;
}

/* ------------------------------------------------------------------------ */
/* brk / mmap2 / munmap: verbatim port of kernel/syscall.c:1200,1921-2230   */

static void mm_coalesce_bottom(qenv *E)
{
    int changed;
    do {
        changed = 0;
        for (int i = 0; i < ENV_MMAP_SLOTS; i++) {
            if (!E->mmap_slots[i].base || !E->mmap_slots[i].free) continue;
            if (E->mmap_slots[i].base == E->mmap_bottom) {
                E->mmap_bottom = E->mmap_slots[i].base + E->mmap_slots[i].len;
                E->mmap_slots[i].base = E->mmap_slots[i].len = 0;
                E->mmap_slots[i].free = 0;
                E->mmap_used--;
                changed = 1;
            }
        }
    } while (changed);
}

static void mm_merge_free(qenv *E)
{
    int changed;
    do {
        changed = 0;
        for (int i = 0; i < ENV_MMAP_SLOTS; i++) {
            if (!E->mmap_slots[i].base || !E->mmap_slots[i].free) continue;
            for (int j = 0; j < ENV_MMAP_SLOTS; j++) {
                if (i == j || !E->mmap_slots[j].base || !E->mmap_slots[j].free) continue;
                if (E->mmap_slots[i].base + E->mmap_slots[i].len == E->mmap_slots[j].base) {
                    E->mmap_slots[i].len += E->mmap_slots[j].len;
                    E->mmap_slots[j].base = E->mmap_slots[j].len = 0;
                    E->mmap_slots[j].free = 0;
                    E->mmap_used--;
                    changed = 1;
                }
            }
        }
    } while (changed);
}

static int mm_alloc_slot(qenv *E)
{
    for (int i = 0; i < ENV_MMAP_SLOTS; i++) if (!E->mmap_slots[i].base) return i;
    return -1;
}

static int mm_find_reusable(qenv *E, uint32_t want)
{
    int best = -1;
    for (int i = 0; i < ENV_MMAP_SLOTS; i++) {
        if (!E->mmap_slots[i].base || !E->mmap_slots[i].free || E->mmap_slots[i].len < want) continue;
        if (best < 0 || E->mmap_slots[i].len < E->mmap_slots[best].len) best = i;
    }
    if (best < 0) return -1;
    uint32_t excess = E->mmap_slots[best].len - want;
    if (excess >= 4096) {
        int extra = mm_alloc_slot(E);
        if (extra >= 0) {
            E->mmap_slots[extra].base = E->mmap_slots[best].base + want;
            E->mmap_slots[extra].len = excess;
            E->mmap_slots[extra].free = 1;
            E->mmap_used++;
            E->mmap_slots[best].len = want;
        }
    }
    return best;
}

static long sys_mmap2(qenv *E, long length)
{
    if (length <= 0) return -EINVAL_;
    uint32_t ulen = (uint32_t)length;
    uint32_t la = (ulen + 4095u) & ~4095u;
    if (la < ulen) return -ENOMEM_;
    mm_coalesce_bottom(E);
    int r = mm_find_reusable(E, la);
    if (r >= 0) {
        E->mmap_slots[r].free = 0;
        gzero(E, E->mmap_slots[r].base, E->mmap_slots[r].len);
        return (long)E->mmap_slots[r].base;
    }
    uint32_t mb = E->mmap_bottom, cb = E->cur_brk;
    if (la > mb) return -ENOMEM_;
    uint32_t base = mb - la;
    if (base < cb) return -ENOMEM_;
    if (base < 0x10000000u) return -ENOMEM_;
    int slot = mm_alloc_slot(E);
    if (slot < 0) {
        mm_merge_free(E);
        int r2 = mm_find_reusable(E, la);
        if (r2 >= 0) {
            E->mmap_slots[r2].free = 0;
            gzero(E, E->mmap_slots[r2].base, E->mmap_slots[r2].len);
            return (long)E->mmap_slots[r2].base;
        }
        slot = mm_alloc_slot(E);
    }
    if (slot < 0) return -ENOMEM_;
    gzero(E, base, la);
    E->mmap_bottom = base;
    E->mmap_slots[slot].base = base;
    E->mmap_slots[slot].len = la;
    E->mmap_slots[slot].free = 0;
    E->mmap_used++;
    return (long)base;
}

static long sys_munmap(qenv *E, uint32_t addr, long length)
{
    if (!addr || length <= 0) return 0;
    for (int i = 0; i < ENV_MMAP_SLOTS; i++) {
        if (E->mmap_slots[i].base != addr || E->mmap_slots[i].free) continue;
        if (addr == E->mmap_bottom) {
            E->mmap_bottom = addr + E->mmap_slots[i].len;
            E->mmap_slots[i].base = E->mmap_slots[i].len = 0;
            E->mmap_slots[i].free = 0;
            E->mmap_used--;
            mm_coalesce_bottom(E);
        } else {
            E->mmap_slots[i].free = 1;
        }
        return 0;
    }
    return 0;
}

static long sys_brk(qenv *E, uint32_t addr)
{
    if (addr == 0) return (long)E->cur_brk;
    if (addr < E->brk_base) return (long)E->cur_brk;
    if (addr > E->mmap_bottom) return (long)E->cur_brk;
    if (addr > E->cur_brk) gzero(E, E->cur_brk, addr - E->cur_brk);
    E->cur_brk = addr;
    return (long)E->cur_brk;
}

/* ------------------------------------------------------------------------ */
/* virtual clock (ENV_SPEC 7.4)                                             */

static uint64_t clock_read(qenv *E, uint32_t pc)
{
    int k = -1;
    for (int i = 0; i < E->nclk; i++) if (E->clk_pc[i] == pc) { k = i; break; }
    if (k < 0) {
        if (E->nclk < 64) { k = E->nclk++; E->clk_pc[k] = pc; E->clk_flag[k] = 0; }
        else k = 63;
    }
    if (E->clk_flag[k]) {
        E->vt_us += E->vt_quantum;
        E->clk_advances++;
        memset(E->clk_flag, 0, sizeof E->clk_flag);
    }
    E->clk_flag[k] = 1;
    return E->vt_us;
}

/* ------------------------------------------------------------------------ */
/* console                                                                  */

static void console_line(qenv *E, const char *s)
{
    /* The app profile's result_line -- the console line its benchmark
     * workload prints when it is done (Quake: "<N> frames <T> seconds <F>
     * fps" from CL_FinishTimeDemo).  Capture group 1 is the number of host
     * frames in the measurement window. */
    uint64_t nf = 0;
    if (app_match_result(E->app, s, &nf)) {
        snprintf(E->timedemo_line, sizeof E->timedemo_line, "%.255s", s);
        E->result_frames = nf;
        if (!E->done) { E->done = ENV_DONE_RESULT; snprintf(E->done_msg, sizeof E->done_msg, "result line: %.240s", s); }
        qsim_request_stop(E->cpu);
    }
    if (app_match_error(E->app, s)) E->sys_error_seen = 1;
    /* Workload-parity facts (app profile "parity"): the first match wins, so
     * a value printed once at start-up is what summary.json reports. */
    for (int i = 0; i < E->app->nparity; i++)
        if (!E->parity_seen[i] && app_match_parity(E->app, i, s, E->parity_val[i], sizeof E->parity_val[i]))
            E->parity_seen[i] = 1;
    if (E->opt.window_marker && !E->window_seen && strstr(s, E->opt.window_marker)) {
        E->window_seen = 1;
        E->window_hfc = E->host_framecount_addr ? rd32(E, E->host_framecount_addr) : 0;
        E->window_flips = E->flips;
    }
    if (E->opt.window_end_marker && E->window_seen && !E->window_end_seen && strstr(s, E->opt.window_end_marker)) {
        E->window_end_seen = 1;
        E->window_end_hfc = E->host_framecount_addr ? rd32(E, E->host_framecount_addr) : 0;
    }
    if (E->opt.end_marker && strstr(s, E->opt.end_marker)) {
        if (!E->done) { E->done = ENV_DONE_MARKER; snprintf(E->done_msg, sizeof E->done_msg, "end marker: %.240s", s); }
        qsim_request_stop(E->cpu);
    }
}

static void console_bytes(qenv *E, const uint8_t *p, uint32_t n)
{
    if (E->console) fwrite(p, 1, n, E->console);
    if (E->echo) fwrite(p, 1, n, stderr);
    for (uint32_t i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\n' || E->line_len >= (int)sizeof E->line - 1) {
            E->line[E->line_len] = 0;
            console_line(E, E->line);
            E->line_len = 0;
        } else if (c != '\r') {
            E->line[E->line_len++] = c;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* ECALL dispatcher (ENV_SPEC 2)                                            */

static long linux_syscall(qenv *E, uint32_t nr, uint32_t *a)
{
    switch (nr) {
    case 25: return -ENOSYS_;                          /* fcntl */
    case 29: return 0;                                  /* ioctl */
    case 35: return -ENOSYS_;                           /* unlinkat */
    case 56: return sys_openat(E, a[1], (int)a[2]);     /* openat */
    case 57: return sys_close(E, (int32_t)a[0]);
    case 61: return sys_getdents64(E, (int32_t)a[0], a[1], a[2]);
    case 62: return sys_llseek(E, (int32_t)a[0], a[1], a[2], a[3], a[4]);
    case 63: return sys_read(E, (int32_t)a[0], a[1], a[2]);
    case 64: return sys_write(E, (int32_t)a[0], a[1], a[2]);
    case 65:                                             /* readv */
    case 66: {                                           /* writev */
        long total = 0;
        for (uint32_t i = 0; i < a[2]; i++) {
            uint32_t base = rd32(E, a[1] + 8 * i), len = rd32(E, a[1] + 8 * i + 4);
            if (nr == 65) {
                if (len == 0) continue;
                long rc = sys_read(E, (int32_t)a[0], base, len);
                if (rc < 0) return total > 0 ? total : rc;
                if (rc == 0) break;
                total += rc;
                if ((uint32_t)rc < len) break;
            } else {
                long rc = sys_write(E, (int32_t)a[0], base, len);
                if (rc < 0) return total > 0 ? total : rc;
                total += rc;
            }
        }
        return total;
    }
    case 93: case 94:
        E->done = 3;
        snprintf(E->done_msg, sizeof E->done_msg, "guest exit(%d)", (int32_t)a[0]);
        qsim_request_stop(E->cpu);
        return 0;
    case 96: return 1;                                  /* set_tid_address */
    case 214: return sys_brk(E, a[0]);
    case 215: return sys_munmap(E, a[0], (int32_t)a[1]);
    case 216: return -ENOSYS_;                          /* mremap */
    case 222: return sys_mmap2(E, (int32_t)a[1]);
    case 226: return 0;                                 /* mprotect */
    case 233: return 0;                                 /* madvise */
    case 291: return sys_statx(E, (int32_t)a[0], a[1], a[4]);
    case 414: return -ENOSYS_;
    case 422: return 0;                                 /* futex */
    case 172: case 178: return 1;                       /* getpid/gettid */
    case 134: case 135: return 0;                       /* rt_sigaction/procmask */
    case 130: return 0;                                 /* tkill */
    }
    fprintf(E->log, "qsim: unhandled linux syscall %u at pc %08x\n", nr, E->cpu->pc);
    return -ENOSYS_;
}

static int vendor_call(qenv *E, uint32_t eid, uint32_t fid, uint32_t *a, int32_t *err, uint32_t *val)
{
    *err = 0; *val = 0;
    switch (eid) {
    case 0xC0DE0006:                                    /* TIMER */
        if (fid == 2) { *val = (uint32_t)clock_read(E, E->cpu->pc); return 0; }
        if (fid == 3) { *val = (uint32_t)(clock_read(E, E->cpu->pc) / 1000u); return 0; }
        break;
    case 0xC0DE000E:                                    /* FILE */
        if (fid == 8) { *val = 65536; return 0; }       /* ASYNC_MAX_READ */
        if (fid == 6) {                                  /* DMA_STAGE_ALLOC(size, align) */
            uint32_t size = a[0], align = a[1] ? a[1] : 1;
            uint32_t off = (E->dma_next + align - 1) & ~(align - 1);
            if (off + size > 0x100000u) { *err = -12; return 0; }
            E->dma_next = off + size;
            *val = ENV_CRAM0_BASE + 0x500000u + off;
            return 0;
        }
        if (fid == 1 || fid == 2) return 0;             /* ASYNC_POLL / ASYNC_BUSY: nothing pending */
        break;
    case 0xC0DE0004:                                    /* ANALOGIZER */
        if (fid == 1) return 0;                          /* not enabled */
        if (fid == 0) { gzero(E, a[0], 6); return 0; }
        break;
    }
    fatal(E, "unhandled vendor ecall eid=%08x fid=%u", eid, fid);
    return -1;
}

static int on_trap(void *ctx, qsim_cpu *cpu, uint32_t cause, uint32_t tval)
{
    qenv *E = ctx;
    if (cause == QSIM_CAUSE_ECALL_M) {
        if (E->lint) E->lint->env_pc = cpu->pc;
        uint32_t a[6];
        for (int i = 0; i < 6; i++) a[i] = cpu->x[10 + i];
        uint32_t nr = cpu->x[17], fid = cpu->x[16];
        if (nr < 0xC0DE0000u) {
            if (nr < 512) E->ecalls_linux[nr]++;
            long r = linux_syscall(E, nr, a);
            cpu->x[10] = (uint32_t)r;
        } else {
            if ((nr & 0xFFFFFF00u) == 0xC0DE0000u && (nr & 0xff) < 16) E->ecalls_vendor[nr & 0xff][fid & 15]++;
            int32_t err; uint32_t val;
            vendor_call(E, nr, fid, a, &err, &val);
            cpu->x[10] = (uint32_t)err;
            cpu->x[11] = val;
        }
        cpu->pc += 4;
        return E->done == 2 ? QSIM_TRAP_STOP : QSIM_TRAP_RESUME;
    }
    const char *what = cause == QSIM_CAUSE_BREAKPOINT ? "ebreak (__builtin_trap)" :
                       cause == QSIM_CAUSE_ILLEGAL ? "illegal instruction" :
                       cause == QSIM_CAUSE_LOAD_FAULT ? "load access fault" :
                       cause == QSIM_CAUSE_STORE_FAULT ? "store access fault" :
                       cause == QSIM_CAUSE_FETCH_FAULT ? "fetch fault" : "trap";
    const qsim_sym *s = qsim_syms_func(&E->syms, cpu->pc);
    const qsim_sym *rs = qsim_syms_func(&E->syms, cpu->x[1]);
    fatal(E, "%s cause=%u tval=%08x in %s (ra in %s)", what, cause, tval, s ? s->name : "?",
          rs ? rs->name : "?");
    return QSIM_TRAP_STOP;
}

/* ------------------------------------------------------------------------ */
/* video: port of targets/pocket/video.c with instant vblank (ENV_SPEC 3.3) */

static const uint32_t fb_addr[3] = { 0x10000000u, 0x10100000u, 0x10200000u };

static void v_swapctrl_write(qenv *E, uint32_t v) { if (v & 1) E->hw_display = (int)((v >> 1) & 3); }

static void v_sync(qenv *E)
{
    int hw_display = E->hw_display, hw_pending = 0;
    if ((unsigned)hw_display < 3) E->buf_display = hw_display;
    if (hw_pending) E->swap_kicked = 1;
    if (E->buf_ready == E->buf_display) {
        E->buf_ready = -1;
        E->swap_kicked = 0;
    } else if (E->buf_ready >= 0 && E->swap_kicked && !hw_pending) {
        E->buf_display = E->buf_ready;
        E->buf_ready = -1;
        E->swap_kicked = 0;
    }
}

static int v_pick(qenv *E)
{
    for (int i = 0; i < 3; i++) if (i != E->buf_display && i != E->buf_ready) return i;
    return E->buf_draw;
}

static void v_reset_roles(qenv *E)
{
    int hd = E->hw_display;
    E->buf_display = (unsigned)hd < 3 ? hd : 0;
    E->buf_draw = E->buf_display == 0 ? 1 : 0;
    E->buf_ready = -1;
    E->swap_kicked = 0;
}

static void v_force_present(qenv *E)
{
    v_sync(E);
    int idx = v_pick(E);
    v_swapctrl_write(E, ((uint32_t)idx << 1) | 1u);
    E->swap_kicked = 1;
    E->buf_ready = idx;
    v_sync(E);                      /* of_video_wait_flip */
    v_reset_roles(E);
}

static void v_fill_all(qenv *E, uint32_t bytes)
{
    if (bytes == 0 || bytes > 0x100000u) bytes = 0x100000u;
    for (int i = 0; i < 3; i++) {
        gzero(E, fb_addr[i], bytes);      /* video_fill_all_buffers_bytes: memset + of_cache_clean_range */
        if (E->lint) lint_cbo_range(E->lint, 1, fb_addr[i] - E->cpu->sdram_base, bytes, E->lint->env_pc);
    }
}

/* video_normalize_mode; mode struct {u16 w, u16 h, u16 stride, u8 color, u8 rsvd} */
static int v_normalize(uint16_t w, uint16_t h, uint16_t stride_in, uint8_t cm,
                       uint16_t *stride_out, uint32_t *frame_bytes)
{
    if (w == 0 || h == 0 || w > 800 || h > 600 || cm > 5) return -1;
    uint32_t line;
    switch (cm) {
    case 1: line = ((uint32_t)w + 1) >> 1; break;      /* 4BIT */
    case 2: line = ((uint32_t)w + 3) >> 2; break;      /* 2BIT */
    case 3: case 4: case 5: line = (uint32_t)w * 2; break;
    default: line = w; break;
    }
    line = (line + 1) & ~1u;
    uint32_t stride = stride_in ? ((stride_in + 1u) & ~1u) : line;
    if (stride < line || stride > 2048) return -1;
    uint32_t fb = stride * h;
    if (fb == 0 || fb > 0x100000u) return -1;
    *stride_out = (uint16_t)stride;
    *frame_bytes = fb;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* frame events                                                             */

static void write_ppm(qenv *E, const char *path, int idx)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t w = E->mode_w, h = E->mode_h, st = E->mode_stride ? E->mode_stride : w;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t *fb = E->scan + ((uint32_t)idx << 20);
    uint8_t *row = malloc(w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = E->palette[fb[y * st + x]];
            row[x * 3] = (uint8_t)(c >> 16); row[x * 3 + 1] = (uint8_t)(c >> 8); row[x * 3 + 2] = (uint8_t)c;
        }
        fwrite(row, 1, w * 3, f);
    }
    free(row);
    fclose(f);
}

static void frame_event(qenv *E, int idx, uint32_t token)
{
    E->hw_display = idx & 3;                           /* instant vblank */
    if (E->vec_f && E->vec_frame == E->flips) E->vec_finish = idx & 3;
    uint64_t seq = E->flips++;
    if (E->lint && E->check_invariant) {
        uint64_t bad = lint_check_invariant(E->lint, E->log);
        if (bad) fprintf(E->log, "qsim: strict memory invariant broken at flip %" PRIu64 ": %" PRIu64 " lines\n", seq, bad);
        E->invariant_errors += bad;
    }
    if (idx > 2) return;
    uint32_t w = E->mode_w, h = E->mode_h, st = E->mode_stride ? E->mode_stride : w;
    uint8_t *fb = E->scan + ((uint32_t)idx << 20);
    uint64_t hp = FNV64_INIT;
    for (uint32_t y = 0; y < h; y++) hp = fnv1a64(hp, fb + y * st, w);
    uint64_t hz = E->zclears ? E->zhash : 0;     /* 0 = no z clear this frame */
    E->zhash = FNV64_INIT;
    E->zclears = 0;
    uint64_t hpal = fnv1a64(FNV64_INIT, (const uint8_t *)E->palette, sizeof E->palette);
    if (E->npend < 8) {
        env_frame_rec *r = &E->pend[E->npend++];
        r->seq = seq; r->idx = idx; r->token = token;
        r->hash_px = hp; r->hash_z = hz; r->hash_pal = hpal;
        r->hash_cmd = E->cmd_hash; r->cmd_words = E->cmd_words;
        r->host_framecount = E->host_framecount_addr ? rd32(E, E->host_framecount_addr) : 0;
        for (int k = 0; k < E->nflip_pub; k++) {
            if (E->flip_pub[k].token != token) continue;
            r->host_framecount = E->flip_pub[k].hfc;           /* value when the FLIP was published */
            E->nflip_pub -= k + 1;
            memmove(E->flip_pub, E->flip_pub + k + 1, (size_t)E->nflip_pub * sizeof E->flip_pub[0]);
            break;
        }
    }
    E->cmd_hash = FNV64_INIT;
    E->cmd_words = 0;
    if (E->ppm_dir && E->ppm_every > 0 && seq >= E->ppm_start && (seq - E->ppm_start) % (uint64_t)E->ppm_every == 0) {
        char p[4096];
        snprintf(p, sizeof p, "%s/frame_%05" PRIu64 ".ppm", E->ppm_dir, seq);
        write_ppm(E, p, idx);
    }
    qsim_request_stop(E->cpu);
}

static void on_gpu_flip(void *ctx, int idx, uint32_t token) { frame_event(ctx, idx, token); }

static void on_gpu_publish_flip(void *ctx, uint32_t hdr, const uint32_t *p)
{
    qenv *E = ctx;
    if ((hdr & 0x1FFF) < 2 || !E->host_framecount_addr) return;
    if (E->nflip_pub == (int)(sizeof E->flip_pub / sizeof E->flip_pub[0])) {
        E->nflip_pub--;
        memmove(E->flip_pub, E->flip_pub + 1, (size_t)E->nflip_pub * sizeof E->flip_pub[0]);
    }
    E->flip_pub[E->nflip_pub].token = p[1];
    E->flip_pub[E->nflip_pub].hfc = rd32(E, E->host_framecount_addr);
    E->nflip_pub++;
}

/* Every command the GPU executes (header + payload), before it runs.
 * - hash_cmd / gpu_cmd_words: all executed words since the previous flip,
 *   including the FLIP itself (raw words; addresses included as-is).
 * - hash_z: R_RenderView queues the z-buffer CLEAR_RECT for the next frame
 *   before VID_Update's FLIP (r_main.c:1217), so the z-buffer is always clear
 *   at the flip.  The frame's final depth is captured here, just before a
 *   z clear executes: CLEAR_RECT whose target is zbuffer_storage (symbol) or,
 *   without the symbol, a 2*width-byte-wide rect with stride 2*width. */
static void env_gpu_cmd(qenv *E, uint32_t hdr, const uint32_t *p)
{
    uint32_t n = hdr & 0x1FFF, op = hdr >> 24;
    uint8_t b[4];
    b[0] = (uint8_t)hdr; b[1] = (uint8_t)(hdr >> 8); b[2] = (uint8_t)(hdr >> 16); b[3] = (uint8_t)(hdr >> 24);
    E->cmd_hash = fnv1a64(E->cmd_hash, b, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = p[i];
        b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
        E->cmd_hash = fnv1a64(E->cmd_hash, b, 4);
    }
    E->cmd_words += 1u + n;
    E->cmd_words_total += 1u + n;
    if (op == 0x11 && n >= 3) {
        uint32_t addr = p[0] & 0x03FFFFFFu, w = p[1] >> 16, h = p[1] & 0xFFFF, stride = p[2] >> 16;
        int is_z = E->zbuffer_addr ? addr == (E->zbuffer_addr & 0x03FFFFFFu)
                                   : (w == 2u * E->mode_w && stride == w);
        if (is_z && w && h && stride && (uint64_t)addr + (uint64_t)(h - 1) * stride + w <= E->cpu->sdram_size) {
            for (uint32_t y = 0; y < h; y++) E->zhash = fnv1a64(E->zhash, E->scan + addr + y * stride, w);
            E->zclears++;
        }
    }
}

static void on_gpu_cmd(void *ctx, uint32_t hdr, const uint32_t *p) { env_gpu_cmd(ctx, hdr, p); }

/* minimal command walker used only with --no-gpu-model: FENCE/FLIP */
static void ng_publish(qenv *E)
{
    uint32_t i = 0;
    while (i < E->ng_staged) {
        uint32_t h = E->ng_words[i], n = h & 0x1FFF, op = h >> 24;
        if (i + 1 + n > E->ng_staged) break;
        env_gpu_cmd(E, h, E->ng_words + i + 1);
        if (op == 0x02 && n >= 1) E->ng_fence = E->ng_words[i + 1];
        if (op == 0x42 && n >= 2) { frame_event(E, (int)(E->ng_words[i + 1] & 3), E->ng_words[i + 2]); E->ng_fence = E->ng_words[i + 2]; }
        i += 1 + n;
    }
    E->ng_staged = 0;
}

/* ------------------------------------------------------------------------ */
/* MMIO                                                                     */

/* ------------------------------------------------------------------------ */
/* GPU test-vector capture (--gpuvec): records everything the GPU sees during
 * one presented frame in the gpuvec.h format, so the frame can be replayed
 * through the os25 gpu_core Verilator model (rtlsim/) and through gputest:
 * MEM1 records for all nonzero SDRAM at the frame's first GPU_TEX_FLUSH,
 * then, before every publish, MEM1 records for the 4 KB chunks the CPU
 * changed since the previous GPU batch, the batch as CMDS, FLSH/PALB for
 * those MMIO writes, and at the flip DUMP(displayed FB) + FENC + HASH.     */

#define VEC_CHUNK 4096u

static void vec_u32(qenv *E, uint32_t v) { fwrite(&v, 4, 1, E->vec_f); }

static void vec_mem(qenv *E, uint32_t off, uint32_t len)
{
    static const uint8_t pad[4];
    vec_u32(E, GV_TAG('M', 'E', 'M', '1'));
    vec_u32(E, off);
    vec_u32(E, len);
    fwrite(E->cpu->sdram + off, 1, len, E->vec_f);
    fwrite(pad, 1, (4 - (len & 3)) & 3, E->vec_f);
}

static void vec_diff(qenv *E)
{
    uint8_t *sd = E->cpu->sdram;
    for (uint32_t o = 0; o < E->cpu->sdram_size; o += VEC_CHUNK) {
        if (memcmp(sd + o, E->vec_shadow + o, VEC_CHUNK)) {
            vec_mem(E, o, VEC_CHUNK);
            memcpy(E->vec_shadow + o, sd + o, VEC_CHUNK);
            E->vec_mem_chunks++;
        }
    }
}

static int vec_wanted(qenv *E, uint64_t frame)
{
    for (int i = 0; i < E->vec_nframes; i++) if (E->vec_frames[i] == frame) return 1;
    return 0;
}

static void vec_begin(qenv *E)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/frame_%05" PRIu64 ".vec", E->vec_dir, E->flips);
    E->vec_f = fopen(p, "wb");
    if (!E->vec_f) { fprintf(E->log, "qsim: cannot write %s\n", p); E->vec_nframes = 0; return; }
    if (!E->vec_shadow) E->vec_shadow = malloc(E->cpu->sdram_size);
    static const uint8_t zero[VEC_CHUNK];
    uint8_t *sd = E->cpu->sdram;
    for (uint32_t o = 0; o < E->cpu->sdram_size; o += VEC_CHUNK)
        if (memcmp(sd + o, zero, VEC_CHUNK)) vec_mem(E, o, VEC_CHUNK);
    memcpy(E->vec_shadow, sd, E->cpu->sdram_size);
    vec_u32(E, GV_TAG('P', 'A', 'L', 'B'));
    vec_u32(E, gpu_mmio_read(E->gpu, 0x30));
    E->vec_n = 0;
    E->vec_frame = E->flips;
    E->vec_finish = -1;
}

static void vec_mmio_write(qenv *E, uint32_t off, uint32_t val)
{
    if (!E->vec_f) {
        if (off == 0x28 && vec_wanted(E, E->flips)) vec_begin(E);
        if (!E->vec_f) { gpu_mmio_write(E->gpu, off, val); return; }
    }
    switch (off) {
    case 0x04:
        if (E->vec_n < 4096) E->vec_words[E->vec_n++] = val;
        break;
    case 0x28:
        vec_diff(E);
        vec_u32(E, GV_TAG('F', 'L', 'S', 'H'));
        break;
    case 0x30:
        vec_u32(E, GV_TAG('P', 'A', 'L', 'B'));
        vec_u32(E, val);
        break;
    case 0x00:
        if (val & 4) E->vec_n = 0;
        else if ((val & 8) && E->vec_n) {
            vec_diff(E);
            vec_u32(E, GV_TAG('C', 'M', 'D', 'S'));
            vec_u32(E, E->vec_n);
            fwrite(E->vec_words, 4, E->vec_n, E->vec_f);
            E->vec_n = 0;
            gpu_mmio_write(E->gpu, off, val);
            memcpy(E->vec_shadow, E->cpu->sdram, E->cpu->sdram_size);   /* GPU writes are the replayer's own */
            if (E->vec_finish >= 0) {
                uint32_t st = E->mode_stride ? E->mode_stride : E->mode_w;
                vec_u32(E, GV_TAG('D', 'U', 'M', 'P'));
                vec_u32(E, (uint32_t)E->vec_finish << 20);
                vec_u32(E, st * E->mode_h);
                vec_u32(E, GV_TAG('F', 'E', 'N', 'C'));
                vec_u32(E, GV_TAG('H', 'A', 'S', 'H'));
                vec_u32(E, GV_TAG('E', 'N', 'D', '.'));
                fclose(E->vec_f);
                E->vec_f = NULL;
                E->vec_captured++;
            }
            return;
        }
        break;
    default:
        break;
    }
    gpu_mmio_write(E->gpu, off, val);
}

static int on_mmio_read(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t *val)
{
    qenv *E = ctx;
    if (addr - ENV_GPU_BASE < 0x40) {
        uint32_t off = addr - ENV_GPU_BASE, v;
        if (E->gpu) v = gpu_mmio_read(E->gpu, off & ~3u);
        else {
            switch (off & ~3u) {
            case 0x04: case 0x10: v = (E->ng_wrptr << 2) & 0xFFFF; break;
            case 0x14: v = 0x82u | (E->ng_mode ? 0x100u : 0); break;
            case 0x18: v = E->ng_fence; break;
            default: v = 0; break;
            }
        }
        *val = size == 4 ? v : (v >> ((off & 3) * 8));
        return 0;
    }
    if (addr - ENV_SYSREG_BASE < 0x10) {
        /* sysreg (axi_periph_slave.v:1779-1782).  The app reads SYS_CYCLE_LO
         * directly in CL_ParseServerMessage (cl_parse.c:862, a 50 000-cycle
         * busy wait per svc command during signon).  [CHOICE] the counter is
         * instret * ENV_SYSREG_CYC_PER_INSN: deterministic, identical in fast
         * and --profile runs, and close to the device's ~9-10 cycles per
         * 3-instruction wait iteration. */
        uint64_t cyc = cpu->instret_live * ENV_SYSREG_CYC_PER_INSN;
        uint32_t off = addr - ENV_SYSREG_BASE, v;
        switch (off & ~3u) {
        case 0x00: v = 3; break;                     /* dataslot_allcomplete, 1 */
        case 0x04: v = (uint32_t)cyc; break;
        case 0x08: v = (uint32_t)(cyc >> 32); break;
        default:   v = E->term_fb_ctrl & 3; break;   /* TERM_FB_CTRL */
        }
        *val = size == 4 ? v : (v >> ((off & 3) * 8));
        E->sysreg_reads++;
        return 0;
    }
    if (addr - ENV_CRAM0_BASE < ENV_CRAM0_SIZE) {
        uint32_t v = 0;
        memcpy(&v, E->cram0 + (addr - ENV_CRAM0_BASE), size);
        *val = v;
        return 0;
    }
    E->unknown_mmio++;
    (void)cpu;
    return 1;          /* load access fault: on_trap reports the exact pc */
}

static int on_mmio_write(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t val)
{
    qenv *E = ctx;
    if (addr - ENV_GPU_BASE < 0x40) {
        uint32_t off = addr - ENV_GPU_BASE;
        if (size != 4) { fatal(E, "GPU MMIO write size %u at +%02x", size, off); return 0; }
        if (E->gpu) {
            if (E->vec_nframes) vec_mmio_write(E, off, val);
            else gpu_mmio_write(E->gpu, off, val);
        } else {
            if (off == 0x00) {
                if ((val & 0x10) && E->ng_staged == 0) E->ng_mode = 1;
                if ((val & 8) && E->ng_mode && !(val & 4)) { E->ng_wrptr = (E->ng_wrptr + E->ng_staged) & 0xFFF; ng_publish(E); }
                if (val & 4) { E->ng_staged = 0; E->ng_wrptr = 0; E->ng_mode = 0; }
            } else if (off == 0x04 && E->ng_mode && E->ng_staged < 4095) {
                E->ng_words[E->ng_staged++] = val;
            }
        }
        return 0;
    }
    if (addr == ENV_SYSREG_BASE + 0x0C && size == 4) {
        /* TERM_FB_CTRL (axi_periph_slave.v sysreg word 3): Host_EndGame /
         * Host_Error write 0 to reveal the terminal (host.c:110,161). */
        E->term_fb_ctrl = val;
        E->term_fb_writes++;
        return 0;
    }
    if (addr - ENV_CRAM0_BASE < ENV_CRAM0_SIZE) {
        memcpy(E->cram0 + (addr - ENV_CRAM0_BASE), &val, size);
        return 0;
    }
    E->unknown_mmio++;
    (void)cpu; (void)val;
    return 1;          /* store access fault: on_trap reports the exact pc */
}

/* ------------------------------------------------------------------------ */
/* OS services (ENV_SPEC 3.2)                                               */

static void audio_advance(qenv *E)
{
    uint64_t now = E->vt_us;
    if (E->audio_active && now > E->audio_last_t) {
        uint64_t num = (now - E->audio_last_t) * 48000u + E->audio_rd_frac;
        E->audio_rd += num / 1000000u;
        E->audio_rd_frac = num % 1000000u;
        if (E->audio_rd > E->audio_wr) { E->audio_rd = E->audio_wr; E->audio_rd_frac = 0; }
    }
    E->audio_last_t = now;
}

static uint32_t audio_room(qenv *E)
{
    if (!E->audio_active) return 131072u;
    uint32_t gap = (uint32_t)((E->audio_wr - E->audio_rd) & 0x1FFFF);
    return 131071u - gap;
}

static const char *cfg_lookup(qenv *E, const char *sect, const char *key)
{
    const char *v = NULL;
    for (int i = 0; i < E->ncfg; i++)
        if (!stricmp_(E->cfg[i].sect, sect) && !stricmp_(E->cfg[i].key, key)) v = E->cfg[i].val;
    return v;
}

static int32_t cfg_copy(qenv *E, uint32_t out, uint32_t len, const char *v)
{
    if (!out || !len) return -22;
    size_t n = strlen(v) + 1;
    if (n > len) return -28;
    uint8_t *p = gp(E, out, (uint32_t)n);
    if (!p) return -22;
    memcpy(p, v, n);
    env_wrote(E, out, (uint32_t)n);
    return 0;
}

static int on_hook(void *ctx, qsim_cpu *cpu, uint32_t pc)
{
    qenv *E = ctx;
    uint32_t *x = cpu->x;
    int i = (int)((pc - ENV_SVC_BASE) / 4u);
    uint32_t a0 = x[10], a1 = x[11], a2 = x[12], a3 = x[13];
    uint32_t r0 = x[10], r1 = x[11];
    if (i < 0 || i >= ENV_NSVC) { fatal(E, "hook at %08x", pc); return QSIM_TRAP_STOP; }
    E->svc_calls[i]++;
    E->svc_total++;
    if (E->lint) E->lint->env_pc = pc;
    switch (i) {
    case 0: {                                        /* video_init */
        uint16_t st; uint32_t fb;
        v_normalize(320, 240, 320, 0, &st, &fb);
        E->mode_w = 320; E->mode_h = 240; E->mode_stride = st; E->mode_color = 0; E->frame_bytes = fb;
        v_reset_roles(E);
        E->display_mode = 1;
        v_fill_all(E, E->frame_bytes);
        break;
    }
    case 1: r0 = fb_addr[E->buf_draw]; break;       /* video_get_surface */
    case 2:                                          /* video_flip */
        v_sync(E);
        if ((unsigned)E->buf_draw >= 3 || E->buf_draw == E->buf_display || E->buf_draw == E->buf_ready)
            E->buf_draw = v_pick(E);
        v_swapctrl_write(E, ((uint32_t)E->buf_draw << 1) | 1u);
        E->swap_kicked = 1;
        E->buf_ready = E->buf_draw;
        E->buf_draw = v_pick(E);
        E->kernel_flips++;
        r0 = fb_addr[E->buf_draw];
        break;
    case 3: v_sync(E); break;                        /* video_wait_flip */
    case 6: {                                        /* video_set_palette_bulk */
        int n = (int32_t)a1;
        if (n > 256) n = 256;
        for (int k = 0; k < n; k++) E->palette[k] = rd32(E, a0 + 4u * (uint32_t)k) & 0xFFFFFF;
        E->palette_sets++;
        break;
    }
    case 10:                                         /* video_set_display_mode */
        E->display_mode = (int32_t)a0;
        /* The app dropped back to text mode after printing its error_line
         * (Quake: Sys_Error -> video_set_display_mode(0)): the run is over. */
        if (a0 == 0 && E->sys_error_seen) {
            E->done = ENV_DONE_ERROR;
            snprintf(E->done_msg, sizeof E->done_msg, "guest reported a fatal error (see console.log)");
            qsim_request_stop(cpu);
        }
        break;
    case 11: {                                       /* video_set_color_mode */
        uint16_t st; uint32_t fb, old = E->frame_bytes;
        if (v_normalize(E->mode_w, E->mode_h, 0, (uint8_t)a0, &st, &fb) < 0) break;
        E->mode_color = (uint8_t)a0; E->mode_stride = st; E->frame_bytes = fb;
        v_reset_roles(E);
        uint32_t bytes = fb > old ? fb : old;
        if (bytes < 76800) bytes = 76800;
        v_fill_all(E, bytes);
        v_force_present(E);
        E->kernel_flips++;
        break;
    }
    case 12: {                                       /* video_acquire_next */
        int just = (int32_t)a0;
        v_sync(E);
        if (just < 0) { r0 = (uint32_t)E->buf_draw; break; }
        if (E->gpu) gpu_wait_fence(E->gpu, a1);       /* bounded GPU_FENCE_REACHED poll (video.c:1229-1236) */
        uint32_t fence = E->gpu ? gpu_fence_reached(E->gpu) : E->ng_fence;
        int fence_ok = (int32_t)(fence - a1) >= 0;
        v_sync(E);
        if (!fence_ok) {
            fprintf(E->log, "qsim: warning: acquire_next fence %u not reached (%u)\n", a1, fence);
            v_swapctrl_write(E, (((uint32_t)just & 3) << 1) | 1u);
        }
        E->buf_ready = just;
        E->swap_kicked = 1;
        v_sync(E);
        E->buf_draw = v_pick(E);
        r0 = (uint32_t)E->buf_draw;
        break;
    }
    case 13: r0 = a0 < 3 ? fb_addr[a0] : 0; break;  /* video_buffer_addr */
    case 14: break;                                  /* input_poll */
    case 15: gzero(E, a1, 24); break;                /* input_get_state */
    case 16: gzero(E, a0, 24); break;                /* input_poll_p0 */
    case 17: break;
    case 18: case 21: case 57: case 58: break;       /* mixer_init/stop_all/group vol/master vol */
    case 40:                                          /* audio_init */
        audio_advance(E);
        E->audio_active = 0;
        E->audio_wr = E->audio_rd = 0;
        E->audio_rd_frac = 0;
        break;
    case 41: {                                       /* audio_write(samples, pairs) */
        int32_t n = (int32_t)a1;
        audio_advance(E);
        if (!a0 || n <= 0) { r0 = 0; break; }
        if (!E->audio_active) {
            E->audio_active = 1;
            E->audio_wr = E->audio_rd = 0;
            E->audio_rd_frac = 0;
        }
        uint32_t room = audio_room(E);
        if ((uint32_t)n > room) n = (int32_t)room;
        E->audio_wr += (uint32_t)n;
        r0 = (uint32_t)n;
        break;
    }
    case 42: audio_advance(E); r0 = audio_room(E); break;   /* audio_get_free */
    case 48:                                         /* cache_flush: hal/cache.c dcache_evict_all */
        if (E->lint) lint_evict_all(E->lint, E->cpu->sdram_size - 1024u * 2u * 64u);
        break;
    case 49: case 50: case 68:                       /* cache_clean_range / inval_range / flush_range */
        if (E->lint) {
            /* hal/cache.c: per-line cbo over [a0 & ~63, a0 + a1) clamped to the
             * cacheable window 0x10000000..0x14000000 */
            uint64_t lo = a0 & ~63u, hi = (uint64_t)a0 + a1, wlo = E->cpu->sdram_base,
                     whi = wlo + E->cpu->sdram_size;
            if (a1 == 0) break;
            if (lo < wlo) lo = wlo;
            if (hi > whi) hi = whi;
            if (lo < hi)
                lint_cbo_range(E->lint, i == 50 ? 0u : i == 49 ? 1u : 2u, (uint32_t)(lo - wlo),
                               (uint32_t)(hi - lo), E->lint->env_pc);
        }
        break;
    case 69: gzero(E, a0, 112); break;               /* input_get_keyboard_state */
    case 70: gzero(E, a0, 20); break;                /* input_read_mouse_state */
    case 73: {                                       /* mixer_alloc_for_group_h -> uint64 */
        uint64_t seq = ++E->mixer_seq;
        uint64_t h = (seq << 8) | (seq % 32);
        r0 = (uint32_t)h; r1 = (uint32_t)(h >> 32);
        break;
    }
    case 75: case 79: case 80: case 81: case 82: case 83: case 84: case 85: case 87: case 88:
    case 89: case 90:
        break;                                        /* handle setters: no observable effect */
    case 86: r0 = 0; break;                          /* mixer_get_position_h */
    case 76: r0 = 1; break;                          /* mixer_handle_active */
    case 94: {                                       /* video_set_mode(const mode*) */
        uint8_t *m = gp(E, a0, 8);
        uint16_t w, h, s, st; uint32_t fb, old = E->frame_bytes;
        if (!m) { r0 = (uint32_t)-1; break; }
        memcpy(&w, m, 2); memcpy(&h, m + 2, 2); memcpy(&s, m + 4, 2);
        if (v_normalize(w, h, s, m[6], &st, &fb) < 0) { r0 = (uint32_t)-1; break; }
        E->mode_w = w; E->mode_h = h; E->mode_stride = st; E->mode_color = m[6]; E->frame_bytes = fb;
        E->display_mode = 1;
        v_reset_roles(E);
        uint32_t bytes = fb > old ? fb : old;
        if (bytes < 76800) bytes = 76800;
        v_fill_all(E, bytes);
        v_force_present(E);
        E->kernel_flips++;
        r0 = 0;
        break;
    }
    case 95: {                                       /* video_get_mode */
        uint8_t *m = gp(E, a0, 8);
        if (m) {
            memcpy(m, &E->mode_w, 2); memcpy(m + 2, &E->mode_h, 2); memcpy(m + 4, &E->mode_stride, 2);
            m[6] = E->mode_color; m[7] = 0;
            env_wrote(E, a0, 8);
        }
        break;
    }
    case 99: {                                       /* video_check_mode(in, out) */
        uint8_t *m = gp(E, a0, 8);
        uint16_t w, h, s, st; uint32_t fb;
        if (!m) { r0 = (uint32_t)-1; break; }
        memcpy(&w, m, 2); memcpy(&h, m + 2, 2); memcpy(&s, m + 4, 2);
        if (v_normalize(w, h, s, m[6], &st, &fb) < 0) { r0 = (uint32_t)-1; break; }
        uint8_t *o = a1 ? gp(E, a1, 8) : NULL;
        if (o) {
            uint8_t tmp[8];
            memcpy(tmp, m, 8);
            memcpy(tmp + 4, &st, 2);
            tmp[7] = 0;
            memcpy(o, tmp, 8);
            env_wrote(E, a1, 8);
        }
        r0 = 0;
        break;
    }
    case 100: {                                      /* config_get(sect, key, out, len) */
        char sect[64], key[128];
        if (gstr(E, a0, sect, sizeof sect) || gstr(E, a1, key, sizeof key)) { r0 = (uint32_t)-22; break; }
        const char *v = cfg_lookup(E, sect, key);
        if (E->verbose) fprintf(E->log, "qsim: config_get [%s] %s -> %s\n", sect, key, v ? v : "(none)");
        r0 = v ? (uint32_t)cfg_copy(E, a2, a3, v) : (uint32_t)-2;
        break;
    }
    case 103: {                                      /* config_next(sect, *cursor, key, klen, val, vlen) */
        char sect[64];
        if (gstr(E, a0, sect, sizeof sect) || !a1) { r0 = (uint32_t)-22; break; }
        uint32_t cur = rd32(E, a1);
        r0 = (uint32_t)-2;
        while (cur < (uint32_t)E->ncfg) {
            int k = (int)cur++;
            if (stricmp_(E->cfg[k].sect, sect)) continue;
            int32_t rc = cfg_copy(E, a2, a3, E->cfg[k].key);
            if (rc == 0) rc = cfg_copy(E, x[14], x[15], E->cfg[k].val);
            if (rc == 0) wr32(E, a1, cur);
            r0 = (uint32_t)rc;
            break;
        }
        break;
    }
    case 105: r0 = 0; break;                         /* input_is_docked */
    default:
        fatal(E, "unimplemented OS service %d (%s) called from %08x", i, svc_names[i], x[1]);
        return QSIM_TRAP_STOP;
    }
    x[10] = r0;
    x[11] = r1;
    cpu->pc = x[1] & ~1u;
    return E->done == 2 ? QSIM_TRAP_STOP : QSIM_TRAP_RESUME;
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */

static void parse_ini(qenv *E, const char *text)
{
    char sect[32] = "";
    const char *p = text;
    int cap = 64;
    E->cfg = calloc((size_t)cap, sizeof *E->cfg);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char ln[512];
        if (n >= sizeof ln) n = sizeof ln - 1;
        memcpy(ln, p, n); ln[n] = 0;
        p += n + (e ? 1 : 0);
        char *s = ln;
        while (isspace((unsigned char)*s)) s++;
        char *t = s + strlen(s);
        while (t > s && isspace((unsigned char)t[-1])) *--t = 0;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *c = strchr(s, ']');
            if (c) { *c = 0; snprintf(sect, sizeof sect, "%s", s + 1); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *v = eq + 1;
        t = k + strlen(k);
        while (t > k && isspace((unsigned char)t[-1])) *--t = 0;
        while (isspace((unsigned char)*v)) v++;
        if (E->ncfg == cap) { cap *= 2; E->cfg = realloc(E->cfg, (size_t)cap * sizeof *E->cfg); }
        snprintf(E->cfg[E->ncfg].sect, sizeof E->cfg[0].sect, "%s", sect);
        snprintf(E->cfg[E->ncfg].key, sizeof E->cfg[0].key, "%s", k);
        snprintf(E->cfg[E->ncfg].val, sizeof E->cfg[0].val, "%s", v);
        E->ncfg++;
    }
}

/* --seed-mem: fill every byte of guest RAM that the device does not initialize
 * with a deterministic PRNG pattern instead of zero.  On an os25 Pocket the app
 * stack (loader.c builds the SysV frame at the top and zeroes nothing else),
 * SDRAM outside the loaded image, BRAM outside PT_LOADs and CRAM0 hold whatever
 * was there before.  What the device does zero is re-zeroed after this fill by
 * the same code paths as without seeding: the loader's p_filesz..p_memsz fill
 * (.bss), brk/mmap zeroing (kernel syscall.c port), the video init buffer fills
 * and the kernel structures the environment writes.  A frame whose hashes
 * change with the seed read memory nobody initialized.  splitmix64, one 64-bit
 * word per 8 bytes, little endian. */
static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void seed_fill(uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t st = seed;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v = splitmix64(&st); memcpy(p + i, &v, 8); }
    for (uint64_t v = splitmix64(&st); i < n; i++, v >>= 8) p[i] = (uint8_t)v;
}

static void env_seed_memory(qenv *E)
{
    qsim_cpu *cpu = E->cpu;
    seed_fill(cpu->sdram, cpu->sdram_size, E->opt.seed_mem);
    seed_fill(cpu->bram, cpu->bram_size, E->opt.seed_mem ^ 0x4252414D00000000ull);   /* "BRAM" */
    seed_fill(E->cram0, ENV_CRAM0_SIZE, E->opt.seed_mem ^ 0x4352414D00000000ull);    /* "CRAM" */
    qsim_invalidate(cpu, cpu->sdram_base, cpu->sdram_size);
    qsim_invalidate(cpu, 0, cpu->bram_size);
}

int env_init(qenv *E, const char *elf_path, const app_profile *app, const char *data_dir,
             int argc, char **argv, int use_gpu_model, uint32_t heap_size_override,
             FILE *console_log)
{
    int normalize = E->normalize_exe_date, gpu_sched = E->gpu_sched, mem_mode = E->mem_mode;
    env_opts opt = E->opt;
    const char *vec_dir = E->vec_dir;
    int vec_nframes = E->vec_nframes;
    uint64_t vec_frames[256];
    memcpy(vec_frames, E->vec_frames, sizeof vec_frames);
    memset(E, 0, sizeof *E);
    E->normalize_exe_date = normalize;
    E->gpu_sched = gpu_sched;
    E->mem_mode = mem_mode;
    E->opt = opt;
    E->vec_dir = vec_dir;
    E->vec_nframes = vec_nframes;
    memcpy(E->vec_frames, vec_frames, sizeof vec_frames);
    E->log = stderr;
    E->elf_path = elf_path;
    E->app = app;
    E->data_dir = data_dir;
    E->argc = argc;
    E->argv = argv;
    E->console = console_log;
    E->cmd_hash = FNV64_INIT;
    E->zhash = FNV64_INIT;
    E->vt_us = 5000000u;           /* [CHOICE] 5 s since boot at app entry */
    E->vt_quantum = 200000u;       /* ENV_SPEC 7.4 */
    E->hw_display = 0; E->buf_display = 0; E->buf_draw = 1; E->buf_ready = -1;
    E->mode_w = 320; E->mode_h = 240; E->mode_stride = 320; E->frame_bytes = 76800;
    E->cram0 = calloc(1, ENV_CRAM0_SIZE);

    qsim_cpu *cpu = qsim_cpu_new(NULL);
    if (!cpu || !E->cram0) return -1;
    E->cpu = cpu;
    cpu->env.ctx = E;
    cpu->env.mmio_read = on_mmio_read;
    cpu->env.mmio_write = on_mmio_write;
    cpu->env.trap = on_trap;
    cpu->env.hook = on_hook;
    /* os25 LSU traps misaligned loads/stores and the kernel emulates them
     * transparently; qsim performs the access and the model charges it. */
    cpu->trap_misaligned = 0;

    if (E->opt.seed_mem) env_seed_memory(E);
    if (load_elf(E, elf_path)) return -1;
    if (qsim_elf_symbols(elf_path, &E->syms)) fprintf(E->log, "qsim: warning: no symbol table\n");
    const qsim_sym *s;
    if (app->frame_symbol[0] && (s = qsim_syms_lookup(&E->syms, app->frame_symbol)))
        E->host_framecount_addr = s->addr;
    if (app->zbuffer_symbol[0] && (s = qsim_syms_lookup(&E->syms, app->zbuffer_symbol)))
        E->zbuffer_addr = s->addr;

    /* memory model (loader.c steps 6-9) */
    E->brk_base = E->cur_brk = E->bss_end;
    E->mmap_bottom = ENV_MMAP_TOP;

    /* services table (ENV_SPEC 3.1) */
    wr32(E, ENV_SVC_TABLE, 0x4F535643u);
    wr32(E, ENV_SVC_TABLE + 4, 2);
    wr32(E, ENV_SVC_TABLE + 8, ENV_NSVC);
    for (int i = 0; i < ENV_NSVC; i++) {
        if (i == 64 || i == 65) { wr32(E, ENV_SVC_TABLE + 12 + 4u * (uint32_t)i, 0); continue; }
        uint32_t a = ENV_SVC_BASE + 4u * (uint32_t)i;
        wr32(E, ENV_SVC_TABLE + 12 + 4u * (uint32_t)i, a);
        qsim_add_hook(cpu, a);
    }

    /* caps (ENV_SPEC 4.2) */
    uint32_t heap_size = heap_size_override ? heap_size_override : ENV_MMAP_TOP - E->bss_end;
    const uint32_t caps[25] = {
        0x43415053u, 4, E->bss_end, heap_size, 0x10000000u, 76800, 320, 240, 320,
        0x13A80000u, 0x00080000u, 0x01A7E35Bu, 32, 48000, 1, 0, 0x04000000u, 0x01000000u,
        0x00000900u, 100000000u, 0x10000000u, 0x50000000u, ENV_GPU_BASE, 0, 1,
    };
    for (int i = 0; i < 25; i++) wr32(E, ENV_CAPS_ADDR + 4u * (uint32_t)i, caps[i]);

    /* argv strings (kernel .bss) + SysV stack (ENV_SPEC 1.4) */
    uint32_t argp[64];
    if (argc > 32) argc = E->argc = 32;
    {
        uint32_t a = ENV_ARGV0_ADDR;
        for (int i = 0; i < argc; i++) {
            size_t n = strlen(argv[i]) + 1;
            if (i == 1) a = ENV_ARGS_ADDR;
            uint8_t *p = gp(E, a, (uint32_t)n);
            memcpy(p, argv[i], n);
            argp[i] = a;
            a += (uint32_t)n;
        }
    }
    uint32_t T = ENV_STACK_TOP;
    const uint32_t aux[] = { 6, 4096, 16, 0, 25, 0, 0xC0DE0001u, ENV_CAPS_ADDR, 0xC0DE0002u, ENV_SVC_TABLE, 0, 0 };
    uint32_t sp = T - 0x34;      /* envp terminator slot */
    for (int i = 0; i < 12; i++) wr32(E, T - 0x30 + 4u * (uint32_t)i, aux[i]);
    wr32(E, sp, 0);                                  /* envp terminator */
    wr32(E, sp - 4, 0);                              /* argv terminator */
    uint32_t argv0 = T - 0x38 - 4u * (uint32_t)argc;
    for (int i = 0; i < argc; i++) wr32(E, argv0 + 4u * (uint32_t)i, argp[i]);
    sp = argv0 - 4;
    wr32(E, sp, (uint32_t)argc);
    qsim_cpu_reset(cpu, E->entry);
    cpu->x[2] = sp;
    cpu->x[10] = E->entry;
    cpu->x[11] = sp;
    cpu->mstatus |= (1u << 13) | (1u << 3);          /* FS=initial, MIE */

    /* Data slots (ENV_SPEC 6.1), in the app profile's order -- which is the
     * order the kernel publishes them and the app probes them. */
    {
        char args[512] = "";
        for (int i = 1; i < argc; i++) {
            strncat(args, argv[i], sizeof args - strlen(args) - 2);
            if (i + 1 < argc) strncat(args, " ", sizeof args - strlen(args) - 1);
        }
        E->ini_text = app_config_text(app, args);     /* NULL when the app has no config slot */
        if (E->ini_text) parse_ini(E, E->ini_text);
    }
    env_slot *sl;
    for (int i = 0; i < app->nslots; i++) {
        const app_slot_def *d = &app->slot[i];
        switch (d->kind) {
        case APP_SLOT_STUB:
            sl = add_slot(E, d->id, d->name, 0);
            sl->size = d->size;                       /* constant, not a real file size */
            break;
        case APP_SLOT_CONFIG:
            if (!E->ini_text) return -1;
            sl = add_slot(E, d->id, d->name, 0);
            sl->mem = (uint8_t *)E->ini_text;
            sl->size = strlen(E->ini_text);
            break;
        case APP_SLOT_DATA: {
            if (!data_dir) {
                fprintf(E->log, "qsim: app %s needs --data-dir (slot %u %s)\n", app->name, d->id, d->name);
                return -1;
            }
            sl = add_slot(E, d->id, d->name, 0);
            sl->host_fd = host_open_ci(data_dir, d->arg);
            if (sl->host_fd < 0) { fprintf(E->log, "qsim: %s/%s not found\n", data_dir, d->arg); return -1; }
            struct stat st;
            fstat(sl->host_fd, &st);
            sl->size = (uint64_t)st.st_size;
            break;
        }
        case APP_SLOT_NV:
            add_slot(E, d->id, d->name, 1);           /* empty pinned NV file */
            break;
        }
    }
    /* --file NAME=PATH: extra read-only data slots, ids 20.. in option order
     * (e.g. a console script for "+exec play1.cfg"; sys_of.c opens files by
     * basename, which slot_lookup matches).  They appear in the root listing
     * (CDAudio_DumpRoot prints it), so only workloads that use them add them. */
    for (int i = 0; i < E->opt.n_extra_files && E->nslots < ENV_MAX_SLOTS; i++) {
        if (strlen(E->opt.extra_name[i]) >= sizeof sl->name) {
            fprintf(E->log, "qsim: --file name too long: %s\n", E->opt.extra_name[i]);
            return -1;
        }
        sl = add_slot(E, 20u + (uint32_t)i, E->opt.extra_name[i], 0);
        sl->host_fd = open(E->opt.extra_path[i], O_RDONLY);
        if (sl->host_fd < 0) { fprintf(E->log, "qsim: cannot open --file %s\n", E->opt.extra_path[i]); return -1; }
        struct stat st;
        fstat(sl->host_fd, &st);
        sl->size = (uint64_t)st.st_size;
    }

    /* Memory coherency model.  The loader ends with of_cache_flush()
     * (kernel/loader.c:399), so at app entry L1 is empty and every byte the
     * loader/argv/caps setup wrote is committed: C starts as a copy of V. */
    E->scan = cpu->sdram;
    if (E->mem_mode != ENV_MEM_FLAT) {
        E->lint = lint_new(cpu->sdram, cpu->sdram_size, E->mem_mode == ENV_MEM_STRICT);
        if (!E->lint) return -1;
        E->lint->frame = &E->flips;
        E->lint->pcp = &cpu->pc;
        cpu->lint = E->lint;
        if (E->lint->strict) { cpu->sdram_uc = E->lint->C; E->scan = E->lint->C; }
    }

    /* GPU (reads and writes committed SDRAM) */
    if (use_gpu_model) {
        gpu_config_t gc;
        gpu_default_config(&gc);
        gc.sched = (uint8_t)E->gpu_sched;
        gpu_mem_if_t mem = { .base = E->scan, .mask = 0x03FFFFFFu };
        E->gpu = gpu_create(&gc, &mem);
        if (!E->gpu) return -1;
        gpu_set_flip_callback(E->gpu, on_gpu_flip, E);
        gpu_set_cmd_callback(E->gpu, on_gpu_cmd, E);
        gpu_set_publish_callback(E->gpu, on_gpu_publish_flip, E);
        gpu_set_lint(E->gpu, E->lint);
    }
    return 0;
}

void env_free(qenv *E)
{
    for (int i = 0; i < E->nslots; i++) {
        if (E->slots[i].host_fd >= 0) close(E->slots[i].host_fd);
        if (E->slots[i].nv) free(E->slots[i].mem);
    }
    free(E->ini_text);
    free(E->cfg);
    free(E->cram0);
    if (E->vec_f) fclose(E->vec_f);
    free(E->vec_shadow);
    if (E->gpu) gpu_destroy(E->gpu);
    if (E->lint) { E->cpu->lint = NULL; E->cpu->sdram_uc = NULL; lint_free(E->lint); }
    qsim_syms_free(&E->syms);
    qsim_cpu_free(E->cpu);
}
