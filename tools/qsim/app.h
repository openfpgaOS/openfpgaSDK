//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * app.h -- qsim app profile: the handful of facts that are specific to the
 * openfpgaOS app under test, kept out of the simulator itself.
 *
 * Everything qsim models (CPU, caches, FPU, GPU, kernel services, ECALLs,
 * coherency lint) is app-agnostic.  What is not, is:
 *
 *   - the data-slot registry the kernel publishes (ids, names, contents);
 *   - the app's `<name>.ini` config text (config_get / config_next);
 *   - the guest argv[0] and its default arguments;
 *   - which ELF symbols hold the frame counter and the z-buffer;
 *   - which console lines mean "workload finished" or "fatal error", and
 *     which carry workload-parity facts worth comparing between two runs;
 *   - the build-stamp string to normalize so two builds of the same source
 *     made at different times render identical console text.
 *
 * All of that comes from one plain-text profile (`qsim --app <file>`), which
 * lives next to the app's workloads under tools/qsim/apps/<app>/.  See
 * apps/quake/quake.app for the worked example and README.md for the syntax.
 * Without --app, qsim uses a minimal built-in profile: no data files, the
 * three slots every openfpgaOS app sees (os.bin, app.ini, app.elf), and no
 * console conventions.
 */
#ifndef QSIM_APP_H
#define QSIM_APP_H
#include <stdint.h>
#include <regex.h>

#define APP_MAX_SLOTS   24
#define APP_MAX_PARITY   8
#define APP_NAME_MAX    24              /* of_file_slot_register() limit + NUL */

/* app_slot_def.kind */
enum {
    APP_SLOT_STUB = 0,     /* reports a size; reads return zeros (os.bin, the app ELF) */
    APP_SLOT_CONFIG,       /* the app's .ini: profile text with $ARGS expanded */
    APP_SLOT_DATA,         /* read-only host file from --data-dir (case-insensitive) */
    APP_SLOT_NV            /* writable nonvolatile slot, starts empty */
};

typedef struct app_slot_def {
    uint32_t id;
    int      kind;
    char     name[APP_NAME_MAX];        /* name the app opens (fopen by basename) */
    char     arg[256];                  /* DATA/CONFIG: file name; STUB: unused */
    uint64_t size;                      /* STUB: the size the registry reports */
} app_slot_def;

typedef struct app_parity {
    char     name[24];
    char    *re;                        /* POSIX ERE, one capture group */
    regex_t  rx;
    int      compiled;
} app_parity;

typedef struct app_profile {
    char  path[4096];                   /* profile file (empty: built-in default) */
    char  dir[4096];                    /* profile directory: relative file= paths resolve here */
    char  name[64];                     /* recorded in summary.json ("app") */
    char  elf_name[APP_NAME_MAX];       /* guest argv[0]; also the app-ELF slot name */
    char *default_args;                 /* guest argv after argv[0] when --args is absent */
    char  build_stamp[32];              /* prefix of "<stamp> HH:MM:SS Mon DD YYYY" to normalize */
    char  frame_symbol[64];             /* ELF symbol: host frame counter (frames.csv) */
    char  zbuffer_symbol[64];           /* ELF symbol: z-buffer storage (hash_z) */

    char    *result_re;                 /* console line: workload complete; group 1 = window frames */
    regex_t  result_rx;
    int      result_ok;
    char    *error_re;                  /* console line: the app reported a fatal error */
    regex_t  error_rx;
    int      error_ok;

    app_parity   parity[APP_MAX_PARITY];
    int          nparity;
    app_slot_def slot[APP_MAX_SLOTS];
    int          nslots;                /* registry order = probe order */
} app_profile;

/* Built-in profile: three stub slots, no data files, no console conventions. */
void app_default(app_profile *P);

/* Parse <path>.  Returns 0, or -1 after printing the reason to stderr. */
int  app_load(app_profile *P, const char *path);

void app_free(app_profile *P);

/* Expand $ARGS in the config text.  Returns a malloc'd string (caller frees),
 * or NULL when the profile declares no config slot. */
char *app_config_text(const app_profile *P, const char *args);

/* Match a console line.  *nframes gets capture group 1 of result_re when it is
 * a number (the measurement-window frame count), else 0. */
int  app_match_result(const app_profile *P, const char *line, uint64_t *nframes);
int  app_match_error(const app_profile *P, const char *line);
/* Capture group 1 of parity rule i into out; returns 1 on a match. */
int  app_match_parity(const app_profile *P, int i, const char *line, char *out, size_t outsz);

#endif
