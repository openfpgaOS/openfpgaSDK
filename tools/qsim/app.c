//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * app.c -- parser for the qsim app profile (app.h).
 *
 * The format is deliberately dumb: `key = value` lines, `#` comments, blank
 * lines ignored, values optionally double-quoted so leading/trailing spaces
 * survive.  Repeatable keys (`slot`, `parity`) append.  Anything a profile
 * does not set keeps the built-in default, so a new app usually needs six
 * lines (name, elf_name, and its data slots).
 */
#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static void copyn(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    /* a double-quoted value keeps its interior spaces verbatim */
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = 0; s++; }
    return s;
}

/* first whitespace-separated token of *s, advancing *s past it */
static char *token(char **s)
{
    char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    char *t = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *s = p;
    return t;
}

static int compile_re(regex_t *rx, const char *re, const char *what, const char *path)
{
    int e = regcomp(rx, re, REG_EXTENDED);
    if (e) {
        char msg[256];
        regerror(e, rx, msg, sizeof msg);
        fprintf(stderr, "qsim: %s: %s: bad regular expression \"%s\": %s\n", path, what, re, msg);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

void app_default(app_profile *P)
{
    memset(P, 0, sizeof *P);
    copyn(P->name, sizeof P->name, "app");
    copyn(P->elf_name, sizeof P->elf_name, "app.elf");
    copyn(P->frame_symbol, sizeof P->frame_symbol, "host_framecount");
    /* The three slots the kernel always publishes.  The sizes are the os25
     * values; nothing reads their contents, only the registry listing. */
    P->slot[0] = (app_slot_def){ .id = 1, .kind = APP_SLOT_STUB, .size = 135400 };
    copyn(P->slot[0].name, APP_NAME_MAX, "os.bin");
    P->slot[1] = (app_slot_def){ .id = 3, .kind = APP_SLOT_STUB, .size = 876096 };
    copyn(P->slot[1].name, APP_NAME_MAX, "app.elf");
    P->nslots = 2;
}

void app_free(app_profile *P)
{
    free(P->default_args);
    free(P->result_re);
    free(P->error_re);
    if (P->result_ok) regfree(&P->result_rx);
    if (P->error_ok) regfree(&P->error_rx);
    for (int i = 0; i < P->nparity; i++) {
        free(P->parity[i].re);
        if (P->parity[i].compiled) regfree(&P->parity[i].rx);
    }
    memset(P, 0, sizeof *P);
}

static int add_slot(app_profile *P, char *v, const char *path, int lineno)
{
    if (P->nslots >= APP_MAX_SLOTS) {
        fprintf(stderr, "qsim: %s:%d: more than %d slots\n", path, lineno, APP_MAX_SLOTS);
        return -1;
    }
    char *ids = token(&v), *name = token(&v), *spec = trim(v);
    if (!*ids || !*name || !*spec) {
        fprintf(stderr, "qsim: %s:%d: slot = <id> <name> <stub:N|config:FILE|data:FILE|nv>\n", path, lineno);
        return -1;
    }
    if (strlen(name) >= APP_NAME_MAX) {
        fprintf(stderr, "qsim: %s:%d: slot name \"%s\" is longer than %d bytes (registry limit)\n",
                path, lineno, name, APP_NAME_MAX - 1);
        return -1;
    }
    app_slot_def *s = &P->slot[P->nslots];
    memset(s, 0, sizeof *s);
    s->id = (uint32_t)strtoul(ids, NULL, 0);
    copyn(s->name, APP_NAME_MAX, name);
    if (!strncmp(spec, "stub:", 5)) {
        s->kind = APP_SLOT_STUB;
        s->size = strtoull(spec + 5, NULL, 0);
    } else if (!strncmp(spec, "config:", 7)) {
        s->kind = APP_SLOT_CONFIG;
        copyn(s->arg, sizeof s->arg, spec + 7);
    } else if (!strncmp(spec, "data:", 5)) {
        s->kind = APP_SLOT_DATA;
        copyn(s->arg, sizeof s->arg, spec + 5);
    } else if (!strcmp(spec, "nv")) {
        s->kind = APP_SLOT_NV;
    } else {
        fprintf(stderr, "qsim: %s:%d: unknown slot kind \"%s\"\n", path, lineno, spec);
        return -1;
    }
    P->nslots++;
    return 0;
}

static int add_parity(app_profile *P, char *v, const char *path, int lineno)
{
    if (P->nparity >= APP_MAX_PARITY) {
        fprintf(stderr, "qsim: %s:%d: more than %d parity rules\n", path, lineno, APP_MAX_PARITY);
        return -1;
    }
    char *name = token(&v), *re = trim(v);
    if (!*name || !*re) {
        fprintf(stderr, "qsim: %s:%d: parity = <name> <regex with one capture group>\n", path, lineno);
        return -1;
    }
    app_parity *q = &P->parity[P->nparity];
    copyn(q->name, sizeof q->name, name);
    q->re = xstrdup(re);
    if (compile_re(&q->rx, q->re, "parity", path)) return -1;
    q->compiled = 1;
    P->nparity++;
    return 0;
}

int app_load(app_profile *P, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "qsim: cannot open app profile %s\n", path); return -1; }
    app_default(P);
    P->nslots = 0;                       /* a profile replaces the default registry */
    P->elf_name[0] = 0;
    copyn(P->path, sizeof P->path, path);
    copyn(P->dir, sizeof P->dir, path);
    char *sl = strrchr(P->dir, '/');
    if (sl) *sl = 0; else copyn(P->dir, sizeof P->dir, ".");

    char line[4096];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "qsim: %s:%d: not a \"key = value\" line\n", path, lineno); rc = -1; break; }
        *eq = 0;
        char *k = trim(s), *v = eq + 1;
        char *tv = trim(v);
        if (!strcmp(k, "name")) copyn(P->name, sizeof P->name, tv);
        else if (!strcmp(k, "elf_name")) {
            if (strlen(tv) >= APP_NAME_MAX) {
                fprintf(stderr, "qsim: %s:%d: elf_name longer than %d bytes\n", path, lineno, APP_NAME_MAX - 1);
                rc = -1; break;
            }
            copyn(P->elf_name, APP_NAME_MAX, tv);
        }
        else if (!strcmp(k, "default_args")) { free(P->default_args); P->default_args = xstrdup(tv); }
        else if (!strcmp(k, "build_stamp")) copyn(P->build_stamp, sizeof P->build_stamp, tv);
        else if (!strcmp(k, "frame_symbol")) copyn(P->frame_symbol, sizeof P->frame_symbol, tv);
        else if (!strcmp(k, "zbuffer_symbol")) copyn(P->zbuffer_symbol, sizeof P->zbuffer_symbol, tv);
        else if (!strcmp(k, "result_line")) {
            free(P->result_re); P->result_re = xstrdup(tv);
            if (P->result_ok) { regfree(&P->result_rx); P->result_ok = 0; }
            if (compile_re(&P->result_rx, P->result_re, "result_line", path)) { rc = -1; break; }
            P->result_ok = 1;
        }
        else if (!strcmp(k, "error_line")) {
            free(P->error_re); P->error_re = xstrdup(tv);
            if (P->error_ok) { regfree(&P->error_rx); P->error_ok = 0; }
            if (compile_re(&P->error_rx, P->error_re, "error_line", path)) { rc = -1; break; }
            P->error_ok = 1;
        }
        else if (!strcmp(k, "parity")) { if (add_parity(P, v, path, lineno)) { rc = -1; break; } }
        else if (!strcmp(k, "slot")) { if (add_slot(P, v, path, lineno)) { rc = -1; break; } }
        else { fprintf(stderr, "qsim: %s:%d: unknown key \"%s\"\n", path, lineno, k); rc = -1; break; }
    }
    fclose(f);
    if (rc == 0 && !P->elf_name[0]) {
        fprintf(stderr, "qsim: %s: elf_name is required (the guest argv[0])\n", path);
        rc = -1;
    }
    if (rc) app_free(P);
    return rc;
}

/* ------------------------------------------------------------------------ */

char *app_config_text(const app_profile *P, const char *args)
{
    const app_slot_def *cs = NULL;
    for (int i = 0; i < P->nslots; i++) if (P->slot[i].kind == APP_SLOT_CONFIG) { cs = &P->slot[i]; break; }
    if (!cs) return NULL;

    char path[4608];
    if (cs->arg[0] == '/') snprintf(path, sizeof path, "%s", cs->arg);
    else snprintf(path, sizeof path, "%s/%s", P->dir, cs->arg);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "qsim: cannot open config template %s\n", path); return NULL; }
    char *tpl = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) { cap = cap * 2 + 8192; tpl = realloc(tpl, cap); }
        size_t got = fread(tpl + n, 1, 4096, f);
        n += got;
        if (got < 4096) break;
    }
    fclose(f);
    if (!tpl) return NULL;
    tpl[n] = 0;

    /* one substitution pass: $ARGS -> the guest argv after argv[0] */
    size_t alen = strlen(args);
    char *out = malloc(n + 1);
    size_t olen = 0, ocap = n + 1;
    for (size_t i = 0; i < n; ) {
        const char *rep = NULL;
        size_t skip = 0;
        if (!strncmp(tpl + i, "$ARGS", 5)) { rep = args; skip = 5; }
        size_t need = rep ? alen : 1;
        if (olen + need + 1 > ocap) { ocap = (olen + need + 1) * 2; out = realloc(out, ocap); }
        if (rep) { memcpy(out + olen, rep, alen); olen += alen; i += skip; }
        else out[olen++] = tpl[i++];
    }
    out[olen] = 0;
    free(tpl);
    return out;
}

int app_match_result(const app_profile *P, const char *line, uint64_t *nframes)
{
    *nframes = 0;
    if (!P->result_ok) return 0;
    regmatch_t m[2];
    if (regexec(&P->result_rx, line, 2, m, 0)) return 0;
    if (m[1].rm_so >= 0 && isdigit((unsigned char)line[m[1].rm_so]))
        *nframes = strtoull(line + m[1].rm_so, NULL, 10);
    return 1;
}

int app_match_error(const app_profile *P, const char *line)
{
    if (!P->error_ok) return 0;
    return regexec(&P->error_rx, line, 0, NULL, 0) == 0;
}

int app_match_parity(const app_profile *P, int i, const char *line, char *out, size_t outsz)
{
    if (i < 0 || i >= P->nparity || !P->parity[i].compiled) return 0;
    regmatch_t m[2];
    if (regexec(&P->parity[i].rx, line, 2, m, 0)) return 0;
    if (m[1].rm_so < 0) return 0;
    size_t n = (size_t)(m[1].rm_eo - m[1].rm_so);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, line + m[1].rm_so, n);
    out[n] = 0;
    return 1;
}
