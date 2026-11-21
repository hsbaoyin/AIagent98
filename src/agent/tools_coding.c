/*
 * AIagent98 - extra coding tools: build feedback, cross-file replace,
 * symbol outline, restore/snapshot and binary inspection.
 */
#include "tools_coding.h"
#include "agent.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../core/utf8.h"
#include "../../third_party/cjson/cJSON.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static const char *arg_str(cJSON *args, const char *name, const char *def)
{
    const char *v = json_string(args, name);
    return v != NULL ? v : def;
}

/* ---- build: run the shell and parse compiler diagnostics ------------- */

const char schema_build[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Build command. Defaults to build_ow.cmd.\"},"
    "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory relative to the workspace.\"},"
    "\"timeout_sec\":{\"type\":\"integer\",\"description\":\"Default 120.\"}},"
    "\"required\":[]}";

int tool_build_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *cmd = arg_str(args, "command", "build_ow.cmd");
    const char *cwd = arg_str(args, "cwd", NULL);
    int timeout = json_int(args, "timeout_sec", 120);
    cJSON *call;
    char *call_json;
    Str raw;
    const char *p;
    int exit_code = 0;
    int errors = 0, warnings = 0;
    int line_no = 0;

    if (timeout < 1) timeout = 1;
    if (timeout > 300) timeout = 300;
    call = cJSON_CreateObject();
    if (call == NULL) { set_err(err, errcap, "oom"); return -1; }
    cJSON_AddStringToObject(call, "command", cmd);
    if (cwd != NULL && cwd[0] != '\0')
        cJSON_AddStringToObject(call, "cwd", cwd);
    cJSON_AddNumberToObject(call, "timeout_sec", timeout);
    call_json = cJSON_PrintUnformatted(call);
    cJSON_Delete(call);
    if (call_json == NULL) { set_err(err, errcap, "oom"); return -1; }

    str_init(&raw);
    if (tools_execute(a, "run_command", call_json, &raw, err, errcap) != 0) {
        free(call_json);
        str_free(&raw);
        return -1;
    }
    free(call_json);

    p = str_cstr(&raw);
    while (*p != '\0') {
        const char *e = strchr(p, '\n');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        char line[1024];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if (strncmp(line, "exit code:", 10) == 0)
            exit_code = atoi(line + 10);
        else {
            char lower[1024];
            size_t i;
            for (i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)line[i]);
            lower[n] = '\0';
            if (strstr(lower, "error") != NULL || strstr(lower, "fatal") != NULL) {
                errors++;
                if (errors <= 20)
                    str_appendf(out, "E%d: %s\n", errors, line);
            } else if (strstr(lower, "warning") != NULL ||
                       strstr(lower, "warn!") != NULL) {
                warnings++;
            }
        }
        line_no++;
        if (e == NULL) break;
        p = e + 1;
    }
    if (exit_code != 0) errors++;
    if (errors == 0) {
        str_appendf(out, "BUILD OK (%d warnings, %d output lines)\n",
                    warnings, line_no);
    } else {
        str_appendf(out, "BUILD FAILED: %d error(s), %d warning(s)\n",
                    errors, warnings);
    }
    if (raw.len > 0) {
        str_append_cstr(out, "\n--- compiler output ---\n");
        str_append_cstr(out, str_cstr(&raw));
    }
    str_free(&raw);
    return 0;
}
const ToolDef tool_build = {
    "build", "Run the build command and summarise compiler errors/warnings. Needs allow_shell=1.",
    schema_build, 1, tool_build_run
};

/* ---- file walking helpers ------------------------------------------- */

typedef int (*FileFn)(const char *full, const char *rel, void *ctx);

static void walk_files(const char *dir, const char *prefix, FileFn fn, void *ctx,
                       int depth)
{
    char pattern[A98_MAX_PATH + 8];
    char full[A98_MAX_PATH];
    char rel[A98_MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int skip_snapshots;

    if (depth > 16) return;
    skip_snapshots = (prefix[0] == '\0' &&
                      a98_strcasecmp(strrchr(dir, '\\') != NULL ?
                                     strrchr(dir, '\\') + 1 : dir,
                                     ".snapshots") == 0);
    if (skip_snapshots) return;

    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern))
        return;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName) >=
            (int)sizeof(full)) continue;
        if (prefix[0] != '\0') {
            if (snprintf(rel, sizeof(rel), "%s\\%s", prefix, fd.cFileName) >=
                (int)sizeof(rel)) continue;
        } else {
            snprintf(rel, sizeof(rel), "%s", fd.cFileName);
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_files(full, rel, fn, ctx, depth + 1);
        } else {
            fn(full, rel, ctx);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int glob_match(const char *pat, const char *name)
{
    const char *p = pat, *n = name, *star = NULL, *star_n = NULL;
    while (*n) {
        if (*p == '*') { star = p++; star_n = n; }
        else if (*p == '?' || (unsigned char)tolower((unsigned char)*p) ==
                 (unsigned char)tolower((unsigned char)*n)) { p++; n++; }
        else if (star) { p = star + 1; n = ++star_n; }
        else return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* ---- replace_across -------------------------------------------------- */

const char schema_replace_across[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"Literal text to find.\"},"
    "\"replacement\":{\"type\":\"string\",\"description\":\"Replacement text.\"},"
    "\"include\":{\"type\":\"string\",\"description\":\"Optional basename glob, e.g. *.c.\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search (default .).\"},"
    "\"dry_run\":{\"type\":\"boolean\",\"description\":\"Default true: list hits without changing files.\"},"
    "\"max_files\":{\"type\":\"integer\",\"description\":\"Default 100.\"}},"
    "\"required\":[\"pattern\",\"replacement\"]}";

typedef struct {
    const char *pattern;
    const char *replacement;
    const char *include;
    int dry_run;
    int max_files;
    int files_hit;
    int replacements;
    Str *out;
    char *err;
    size_t errcap;
} ReplaceCtx;

static char *file_to_utf8(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    char *raw;
    size_t n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    rewind(f);
    if (*size < 0 || *size > 4 * 1024 * 1024) { fclose(f); return NULL; }
    raw = (char *)a98_alloc((size_t)*size + 1);
    if (raw == NULL) { fclose(f); return NULL; }
    n = fread(raw, 1, (size_t)*size, f);
    fclose(f);
    raw[n] = '\0';
    {
        size_t i;
        for (i = 0; i < n; i++) if (raw[i] == '\0') { a98_free(raw); return NULL; }
    }
    if (a98_utf8_valid(raw, n)) return raw;
    {
        char *conv = a98_acp_to_utf8(raw);
        a98_free(raw);
        return conv;
    }
}

static int write_utf8_text(const char *path, const char *text)
{
    char *local = a98_utf8_to_acp(text);
    const char *src = local != NULL ? local : text;
    FILE *f = fopen(path, "wb");
    const char *p;
    if (f == NULL) { a98_free(local); return -1; }
    for (p = src; *p != '\0'; p++) {
        if (*p == '\n' && (p == src || p[-1] != '\r')) fputc('\r', f);
        fputc(*p, f);
    }
    fclose(f);
    a98_free(local);
    return 0;
}

static int replace_cb(const char *full, const char *rel, void *ctx)
{
    ReplaceCtx *rc = (ReplaceCtx *)ctx;
    char base[A98_MAX_PATH];
    char *name;
    char *text;
    long size;
    int count = 0;
    char *p;

    if (rc->files_hit >= rc->max_files) return 0;
    if (rc->include != NULL && rc->include[0] != '\0') {
        strcpy(base, strrchr(rel, '\\') != NULL ? strrchr(rel, '\\') + 1 : rel);
        if (!glob_match(rc->include, base)) return 0;
    }
    text = file_to_utf8(full, &size);
    if (text == NULL) return 0;

    p = text;
    while ((p = strstr(p, rc->pattern)) != NULL) {
        count++;
        p += strlen(rc->pattern);
    }
    if (count > 0) {
        char *result;
        size_t plen = strlen(rc->pattern);
        size_t rlen = strlen(rc->replacement);
        size_t need = strlen(text) + (rlen > plen ? (rlen - plen) * (size_t)count : 0) + 1;
        size_t o = 0;
        const char *src = text;
        result = (char *)a98_alloc(need);
        if (result != NULL) {
            while ((p = strstr(src, rc->pattern)) != NULL) {
                memcpy(result + o, src, (size_t)(p - src));
                o += (size_t)(p - src);
                memcpy(result + o, rc->replacement, rlen);
                o += rlen;
                src = p + plen;
            }
            strcpy(result + o, src);
            if (!rc->dry_run) {
                if (write_utf8_text(full, result) == 0) {
                    rc->files_hit++;
                    rc->replacements += count;
                    str_appendf(rc->out, "%s: %d replacement(s)\n", rel, count);
                } else {
                    set_err(rc->err, rc->errcap, "cannot write: %s", rel);
                }
            } else {
                rc->files_hit++;
                rc->replacements += count;
                str_appendf(rc->out, "%s: %d match(es) [dry run]\n", rel, count);
            }
            a98_free(result);
        }
    }
    (void)name;
    a98_free(text);
    return 0;
}

int tool_replace_across_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char dir[A98_MAX_PATH];
    const char *pattern = arg_str(args, "pattern", NULL);
    const char *repl = arg_str(args, "replacement", NULL);
    const char *include = arg_str(args, "include", NULL);
    int dry_run = json_bool(args, "dry_run", 1);
    int max_files = json_int(args, "max_files", 100);
    ReplaceCtx rc;

    if (pattern == NULL || pattern[0] == '\0' || repl == NULL) {
        set_err(err, errcap, "pattern and replacement are required");
        return -1;
    }
    if (max_files < 1) max_files = 1;
    if (max_files > 500) max_files = 500;
    if (tools_resolve_path(a, arg_str(args, "path", "."), dir, sizeof(dir),
                           err, errcap) != 0) return -1;
    if (!a98_path_is_dir(dir)) { set_err(err, errcap, "not a directory"); return -1; }

    rc.pattern = pattern; rc.replacement = repl; rc.include = include;
    rc.dry_run = dry_run; rc.max_files = max_files;
    rc.files_hit = 0; rc.replacements = 0;
    rc.out = out; rc.err = err; rc.errcap = errcap;
    walk_files(dir, "", replace_cb, &rc, 0);
    str_appendf(out, "%s: %d file(s), %d replacement(s)\n",
                dry_run ? "Would change" : "Changed",
                rc.files_hit, rc.replacements);
    if (err[0] != '\0') return -1;
    return 0;
}
const ToolDef tool_replace_across = {
    "replace_across",
    "Search-and-replace across many files (dry-run by default). Requires approval when applying.",
    schema_replace_across, 0, tool_replace_across_run
};

/* ---- outline --------------------------------------------------------- */

const char schema_outline[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"max_items\":{\"type\":\"integer\"}},"
    "\"required\":[\"path\"]}";

int tool_outline_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    int max_items = json_int(args, "max_items", 150);
    char *text;
    long size;
    char *p;
    int line_no = 0, count = 0;
    static const char *const kw[] = {
        "if", "for", "while", "switch", "return", "typedef", "struct",
        "enum", "union", "sizeof", "case", "do", "else", NULL
    };

    if (tools_resolve_path(a, arg_str(args, "path", NULL), path, sizeof(path),
                           err, errcap) != 0) return -1;
    if (max_items < 1) max_items = 1;
    if (max_items > 500) max_items = 500;
    text = file_to_utf8(path, &size);
    if (text == NULL) { set_err(err, errcap, "cannot read text file: %s", path); return -1; }

    p = text;
    while (*p != '\0' && count < max_items) {
        const char *e = strchr(p, '\n');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        char line[2048];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        char *t;
        char *open;
        memcpy(line, p, n);
        line[n] = '\0';
        line_no++;
        t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, "#define", 7) == 0 && t[7] == ' ') {
            char *name = t + 8;
            while (*name == ' ' || *name == '\t') name++;
            {
                char nm[256];
                size_t k = 0;
                while (name[k] && (isalnum((unsigned char)name[k]) || name[k] == '_') &&
                       k < sizeof(nm) - 1) { nm[k] = name[k]; k++; }
                nm[k] = '\0';
                str_appendf(out, "%s:%d: #define %s\n", path, line_no, nm);
            }
            count++;
        } else if (*t != '#' && *t != '*' && *t != '/' && *t != '\0') {
            /* Definitions have a return type before the name and do not end
             * in ';' (a call). This keeps va_start(), printf() etc out. */
            char *last = line + n - 1;
            while (last > line && (*last == ' ' || *last == '\t' || *last == '\r'))
                last--;
            open = strchr(t, '(');
            if (open != NULL && strchr(open, ')') != NULL && *last != ';') {
                char nm[256];
                size_t k = 0;
                char *end = open;
                int ok = 1;
                int i;
                while (end > t && (end[-1] == ' ' || end[-1] == '\t')) end--;
                while (k < sizeof(nm) - 1 && end > t &&
                       (isalnum((unsigned char)end[-1]) || end[-1] == '_')) {
                    nm[k] = end[-1]; k++; end--;
                }
                nm[k] = '\0';
                /* reverse */
                for (i = 0; i < k / 2; i++) { char c = nm[i]; nm[i] = nm[k-1-i]; nm[k-1-i] = c; }
                if (k == 0 || !isalpha((unsigned char)nm[0]) || end <= t) ok = 0;
                for (i = 0; kw[i] != NULL; i++)
                    if (strcmp(nm, kw[i]) == 0) { ok = 0; break; }
                if (ok) {
                    str_appendf(out, "%s:%d: %s()\n", path, line_no, nm);
                    count++;
                }
            }
        }
        if (e == NULL) break;
        p = e + 1;
    }
    a98_free(text);
    if (count == 0) str_append_cstr(out, "(no symbols found)\n");
    return 0;
}
const ToolDef tool_outline = {
    "outline", "List function and #define definitions in a source file.",
    schema_outline, 0, tool_outline_run
};

/* ---- restore --------------------------------------------------------- */

const char schema_restore[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}";

int tool_restore_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    char bak[A98_MAX_PATH + 8];
    FILE *src, *dst;
    char buf[8192];
    size_t n;

    if (tools_resolve_path(a, arg_str(args, "path", NULL), path, sizeof(path),
                           err, errcap) != 0) return -1;
    if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak)) {
        set_err(err, errcap, "path too long");
        return -1;
    }
    src = fopen(bak, "rb");
    if (src == NULL) { set_err(err, errcap, "no backup file: %s", bak); return -1; }
    dst = fopen(path, "wb");
    if (dst == NULL) { fclose(src); set_err(err, errcap, "cannot write: %s", path); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
    str_appendf(out, "restored %s from %s\n", path, bak);
    return 0;
}
const ToolDef tool_restore = {
    "restore", "Restore a file from its .bak backup. Requires approval.",
    schema_restore, 1, tool_restore_run
};

/* ---- snapshot -------------------------------------------------------- */

const char schema_snapshot[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Optional label appended to the timestamp.\"}},"
    "\"required\":[]}";

typedef struct {
    const char *from;
    const char *to;
    int copied;
    char *err;
    size_t errcap;
} SnapCtx;

static void copy_one_file(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    FILE *g;
    char buf[8192];
    size_t n;
    if (f == NULL) return;
    g = fopen(dst, "wb");
    if (g == NULL) { fclose(f); return; }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, g);
    fclose(f);
    fclose(g);
}

static int snap_cb(const char *full, const char *rel, void *ctx)
{
    SnapCtx *sc = (SnapCtx *)ctx;
    char dst[A98_MAX_PATH];
    char *slash;

    if (snprintf(dst, sizeof(dst), "%s\\%s", sc->to, rel) >= (int)sizeof(dst))
        return 0;
    /* Create parent directories. */
    slash = dst;
    while ((slash = strchr(slash, '\\')) != NULL) {
        char save = *slash;
        *slash = '\0';
        if (!a98_path_is_dir(dst)) CreateDirectoryA(dst, NULL);
        *slash = save;
        slash++;
    }
    copy_one_file(full, dst);
    sc->copied++;
    return 0;
}

int tool_snapshot_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *name = arg_str(args, "name", "");
    char dest[A98_MAX_PATH];
    char label[64];
    time_t now;
    struct tm *lt;
    SnapCtx sc;

    if (a == NULL || a->cfg == NULL || a->workspace[0] == '\0') {
        set_err(err, errcap, "workspace is not set");
        return -1;
    }
    now = time(NULL);
    lt = localtime(&now);
    if (lt == NULL) { set_err(err, errcap, "cannot get time"); return -1; }
    strncpy(label, name, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    {
        size_t i;
        for (i = 0; label[i] != '\0'; i++)
            if (!isalnum((unsigned char)label[i]) && label[i] != '-' && label[i] != '_')
                label[i] = '_';
    }
    if (snprintf(dest, sizeof(dest), "%s\\.snapshots\\%04d%02d%02d_%02d%02d%02d%s%s",
                 a->workspace, lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                 lt->tm_hour, lt->tm_min, lt->tm_sec,
                 label[0] ? "_" : "", label) >= (int)sizeof(dest)) {
        set_err(err, errcap, "snapshot path is too long");
        return -1;
    }
    if (!a98_path_is_dir(dest)) {
        if (!CreateDirectoryA(dest, NULL)) {
            /* parent .snapshots may not exist yet */
            char parent[A98_MAX_PATH];
            char *slash = strrchr(dest, '\\');
            if (slash != NULL) {
                size_t plen = (size_t)(slash - dest);
                memcpy(parent, dest, plen);
                parent[plen] = '\0';
                CreateDirectoryA(parent, NULL);
            }
            CreateDirectoryA(dest, NULL);
        }
    }
    sc.from = a->workspace; sc.to = dest; sc.copied = 0;
    sc.err = err; sc.errcap = errcap;
    walk_files(a->workspace, "", snap_cb, &sc, 0);
    str_appendf(out, "snapshot saved: %s (%d files)\n", dest, sc.copied);
    return 0;
}
const ToolDef tool_snapshot = {
    "snapshot", "Copy the whole workspace to a timestamped folder for rollback.",
    schema_snapshot, 0, tool_snapshot_run
};

/* ---- hexdump --------------------------------------------------------- */

const char schema_hexdump[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"Byte offset (default 0).\"},"
    "\"length\":{\"type\":\"integer\",\"description\":\"Bytes to show (default 256, max 4096).\"}},"
    "\"required\":[\"path\"]}";

int tool_hexdump_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    long offset = (long)json_int(args, "offset", 0);
    int length = json_int(args, "length", 256);
    unsigned char buf[4096];
    FILE *f;
    size_t n;
    size_t i;

    if (tools_resolve_path(a, arg_str(args, "path", NULL), path, sizeof(path),
                           err, errcap) != 0) return -1;
    if (offset < 0) offset = 0;
    if (length < 1) length = 1;
    if (length > 4096) length = 4096;
    f = fopen(path, "rb");
    if (f == NULL) { set_err(err, errcap, "cannot open: %s", path); return -1; }
    if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); set_err(err, errcap, "seek failed"); return -1; }
    n = fread(buf, 1, (size_t)length, f);
    fclose(f);
    if (n == 0) { str_append_cstr(out, "(empty range)\n"); return 0; }
    for (i = 0; i < n; i += 16) {
        size_t j;
        str_appendf(out, "%08lx  ", (unsigned long)(offset + (long)i));
        for (j = 0; j < 16; j++) {
            if (i + j < n) str_appendf(out, "%02x ", buf[i + j]);
            else str_append_cstr(out, "   ");
            if (j == 7) str_append_char(out, ' ');
        }
        str_append_char(out, ' ');
        for (j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = buf[i + j];
            str_append_char(out, (c >= 32 && c < 127) ? (char)c : '.');
        }
        str_append_char(out, '\n');
    }
    str_appendf(out, "(%lu bytes shown)\n", (unsigned long)n);
    return 0;
}
const ToolDef tool_hexdump = {
    "hexdump", "Show a binary file as offset/hex/ASCII dump.",
    schema_hexdump, 0, tool_hexdump_run
};





