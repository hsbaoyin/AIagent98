/* AIagent98 - pluggable skills. See skills.h. */
#include "skills.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

void skills_init(SkillList *list)
{
    if (list != NULL) memset(list, 0, sizeof(*list));
}

static void skill_free_one(Skill *s)
{
    if (s == NULL) return;
    a98_free(s->description);
    a98_free(s->body);
    s->description = NULL;
    s->body = NULL;
}

void skills_free(SkillList *list)
{
    int i;
    if (list == NULL) return;
    for (i = 0; i < list->count; i++) skill_free_one(&list->items[i]);
    a98_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

int skills_valid_name(const char *name)
{
    size_t i, n;
    if (name == NULL || name[0] == '\0') return 0;
    n = strlen(name);
    if (n > 48) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int skills_dir(const char *workspace, char *out, size_t cap)
{
    if (workspace == NULL || workspace[0] == '\0' || out == NULL || cap == 0)
        return -1;
    if (snprintf(out, cap, "%s\\skills", workspace) >= (int)cap) return -1;
    return 0;
}

/* Read a whole file as bytes. Returns malloc'ed NUL-terminated buffer. */
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    size_t n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size < 0 || size > 2 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char *)a98_alloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    if (len != NULL) *len = n;
    return buf;
}

/* Parse a SKILL.md buffer: frontmatter between --- fences, rest is body.
 * Fills name/description (UTF-8) and body (UTF-8). Returns 0 on success. */
static int parse_skill(const char *text, const char *path,
                       Skill *out)
{
    const char *body = text;
    const char *p;
    char name[64] = "", desc[512] = "";

    /* strip UTF-8 BOM */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text += 3;
    }

    if (strncmp(text, "---", 3) == 0) {
        p = text + 3;
        /* skip to end of the first line (the opening ---) */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        {
            const char *end = strstr(p, "\n---");
            if (end != NULL) {
                char fm[1024];
                size_t flen = (size_t)(end - p);
                const char *line;
                if (flen >= sizeof(fm)) flen = sizeof(fm) - 1;
                memcpy(fm, p, flen);
                fm[flen] = '\0';
                line = fm;
                while (*line != '\0') {
                    const char *e = strchr(line, '\n');
                    size_t llen = e != NULL ? (size_t)(e - line) : strlen(line);
                    char kv[1024];
                    size_t k;
                    if (llen >= sizeof(kv)) llen = sizeof(kv) - 1;
                    memcpy(kv, line, llen);
                    kv[llen] = '\0';
                    /* key: value */
                    {
                        char *colon = strchr(kv, ':');
                        if (colon != NULL) {
                            const char *val = colon + 1;
                            while (*val == ' ' || *val == '\t') val++;
                            /* strip a leading quote (we always write one) */
                            if (*val == '"') val++;
                            /* trim trailing spaces/quotes */
                            size_t vlen = strlen(val);
                            while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == ' ' ||
                                   val[vlen-1] == '\t' || val[vlen-1] == '"')) vlen--;
                            if (strncmp(kv, "name:", 5) == 0 && vlen < sizeof(name)) {
                                memcpy(name, val, vlen); name[vlen] = '\0';
                            } else if (strncmp(kv, "description:", 12) == 0 && vlen < sizeof(desc)) {
                                memcpy(desc, val, vlen); desc[vlen] = '\0';
                            }
                        }
                    }
                    if (e == NULL) break;
                    line = e + 1;
                }
                body = end + 4;  /* after "\n---" */
                while (*body == '\r' || *body == '\n') body++;
            }
        }
    }

    /* Fallback name from file name if frontmatter had none. */
    if (name[0] == '\0') {
        const char *slash = strrchr(path, '\\');
        if (slash == NULL) slash = strrchr(path, '/');
        {
            const char *base = slash != NULL ? slash + 1 : path;
            size_t k = 0;
            while (*base != '\0' && *base != '.' && k < sizeof(name) - 1) {
                name[k++] = *base++;
            }
            name[k] = '\0';
        }
    }
    if (!skills_valid_name(name)) return -1;

    memset(out, 0, sizeof(*out));
    strcpy(out->name, name);
    out->description = a98_strdup(desc);
    out->body = a98_strdup(body);
    strcpy(out->path, path);
    if (out->description == NULL || out->body == NULL) {
        skill_free_one(out);
        return -1;
    }
    return 0;
}

int skills_scan(const char *workspace, SkillList *list)
{
    char dir[A98_MAX_PATH];
    char pattern[A98_MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int kept = 0;

    skills_free(list);
    if (skills_dir(workspace, dir, sizeof(dir)) != 0) return 0;
    if (snprintf(pattern, sizeof(pattern), "%s\\*.md", dir) >= (int)sizeof(pattern))
        return 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* no skills dir is fine */

    do {
        char path[A98_MAX_PATH];
        char *text;
        Skill s, *p;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName) >= (int)sizeof(path))
            continue;
        text = read_all(path, NULL);
        if (text == NULL) continue;
        if (parse_skill(text, path, &s) == 0) {
            if (list->count == list->cap) {
                int nc = list->cap ? list->cap * 2 : 8;
                Skill *np = (Skill *)a98_realloc(list->items, (size_t)nc * sizeof(Skill));
                if (np == NULL) { skill_free_one(&s); a98_free(text); break; }
                list->items = np;
                list->cap = nc;
            }
            p = &list->items[list->count++];
            *p = s;
            kept++;
        }
        a98_free(text);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return kept;
}

const Skill *skills_find(const SkillList *list, const char *name)
{
    int i;
    if (list == NULL || name == NULL) return NULL;
    for (i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, name) == 0) return &list->items[i];
    }
    return NULL;
}

int skills_save(const char *workspace, const char *name,
                const char *description, const char *body,
                char *err, size_t errcap)
{
    char dir[A98_MAX_PATH], path[A98_MAX_PATH];
    char *local;
    FILE *f;
    const char *src = body != NULL ? body : "";
    size_t i;

    if (!skills_valid_name(name)) {
        set_err(err, errcap, "invalid skill name (use letters, digits, - and _)");
        return -1;
    }
    if (skills_dir(workspace, dir, sizeof(dir)) != 0) {
        set_err(err, errcap, "workspace path is too long");
        return -1;
    }
    CreateDirectoryA(dir, NULL);
    if (snprintf(path, sizeof(path), "%s\\%s.md", dir, name) >= (int)sizeof(path)) {
        set_err(err, errcap, "skill path is too long");
        return -1;
    }
    f = fopen(path, "wb");
    if (f == NULL) { set_err(err, errcap, "cannot write skill file: %s", path); return -1; }

    /* Write frontmatter. Content from the model is UTF-8; write bytes as-is
     * so CJK descriptions stay intact. */
    fprintf(f, "---\r\nname: %s\r\ndescription: ", name);
    local = description != NULL ? (char *)description : (char *)"";
    /* strip CR/LF from description so it stays one line */
    fputc('"', f);
    for (i = 0; local[i] != '\0'; i++) {
        if (local[i] == '\r' || local[i] == '\n') continue;
        fputc(local[i], f);
    }
    fprintf(f, "\"\r\n---\r\n");
    for (i = 0; src[i] != '\0'; i++) fputc(src[i], f);
    fclose(f);
    return 0;
}

int skills_delete(const char *workspace, const char *name,
                  char *err, size_t errcap)
{
    char dir[A98_MAX_PATH], path[A98_MAX_PATH];
    if (!skills_valid_name(name)) {
        set_err(err, errcap, "invalid skill name");
        return -1;
    }
    if (skills_dir(workspace, dir, sizeof(dir)) != 0 ||
        snprintf(path, sizeof(path), "%s\\%s.md", dir, name) >= (int)sizeof(path)) {
        set_err(err, errcap, "skill path is too long");
        return -1;
    }
    if (!DeleteFileA(path)) {
        set_err(err, errcap, "cannot delete skill file: %s", path);
        return -1;
    }
    return 0;
}
