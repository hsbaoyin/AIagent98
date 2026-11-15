/*
 * AIagent98 - emoji pack support (see emoji.h for the why).
 *
 * Nothing here touches the Win32 API: the table lookup and the text
 * transforms are portable C so the host smoke test can exercise them.
 */
#include "emoji.h"

#include "path.h"
#include "str.h"
#include "utf8.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "emoji_table.h"

/* Longest ":shortcode:" we accept while expanding; the pack's longest name
 * is well under this, and the bound keeps a stray colon from scanning the
 * rest of the message. */
#define A98_EMOJI_MAX_TOKEN 72

int a98_emoji_count(void)
{
    return A98_EMOJI_PACK_COUNT;
}

const A98EmojiEntry *a98_emoji_at(int index)
{
    if (index < 0 || index >= A98_EMOJI_PACK_COUNT) {
        return NULL;
    }
    return &g_a98_emoji_pack[index];
}

/* The table is generated sorted by code point. */
const A98EmojiEntry *a98_emoji_by_codepoint(unsigned long cp)
{
    int lo = 0;
    int hi = A98_EMOJI_PACK_COUNT - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (g_a98_emoji_pack[mid].cp == cp) {
            return &g_a98_emoji_pack[mid];
        }
        if (g_a98_emoji_pack[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

const A98EmojiEntry *a98_emoji_by_shortcode(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < A98_EMOJI_PACK_COUNT; i++) {
        if (strcmp(g_a98_emoji_pack[i].shortcode, name) == 0) {
            return &g_a98_emoji_pack[i];
        }
    }
    return NULL;
}

/* Code points that are emoji-ish but not in the pack.  They get a readable
 * "[U+1F600]" instead of the '?' the ANSI conversion would produce. */
static int emoji_like(unsigned long cp)
{
    if (cp >= 0x1F000UL && cp <= 0x1FAFFUL) {
        return 1;
    }
    if (cp >= 0x1F1E6UL && cp <= 0x1F1FFUL) { /* regional indicators */
        return 1;
    }
    if (cp >= 0x2600UL && cp <= 0x27BFUL) {
        return 1;
    }
    if (cp >= 0x2B00UL && cp <= 0x2BFFUL) {
        return 1;
    }
    if (cp >= 0xE0020UL && cp <= 0xE007FUL) { /* tag characters */
        return 1;
    }
    if (cp == 0x203CUL || cp == 0x2049UL) {
        return 1;
    }
    return 0;
}

/* Variation selectors, the ZWJ joiner, keycap marks and skin tone modifiers
 * only decorate the character before them; once the base emoji is folded to
 * a shortcode they would be noise. */
static int emoji_decoration(unsigned long cp)
{
    if (cp == 0xFE0EUL || cp == 0xFE0FUL || cp == 0x200DUL || cp == 0x20E3UL) {
        return 1;
    }
    if (cp >= 0x1F3FBUL && cp <= 0x1F3FFUL) {
        return 1;
    }
    return 0;
}

char *a98_emoji_fold(const char *utf8)
{
    Str out;
    size_t i;
    size_t len;

    if (utf8 == NULL) {
        return NULL;
    }
    str_init(&out);
    len = strlen(utf8);
    i = 0;
    while (i < len) {
        unsigned int cp = 0;
        int n = a98_utf8_decode(utf8 + i, len - i, &cp);

        if (n <= 0) {
            /* Not UTF-8: pass the byte through untouched (CP_ACP text from
             * the INI or a session file must survive). */
            str_append_char(&out, utf8[i]);
            i++;
            continue;
        }
        i += (size_t)n;

        if (emoji_decoration(cp)) {
            continue;
        }
        {
            const A98EmojiEntry *e = a98_emoji_by_codepoint(cp);

            if (e != NULL) {
                str_appendf(&out, ":%s:", e->shortcode);
                continue;
            }
        }
        if (emoji_like(cp)) {
            str_appendf(&out, "[U+%04lX]", (unsigned long)cp);
            continue;
        }
        {
            char buf[4];
            int m = a98_utf8_encode(cp, buf);

            if (m > 0) {
                str_append(&out, buf, (size_t)m);
            } else {
                str_append_char(&out, '?');
            }
        }
    }
    return str_detach(&out);
}

/* ------------------------------------------------------------------ */
/* Asset location                                                      */
/* ------------------------------------------------------------------ */

/* How far up from the EXE and the working directory the search may go.  A
 * build tree is build\cmake\agent98.exe, i.e. two levels below the repository
 * root where the pack folder sits; a release install is depth 0. */
#define A98_EMOJI_SEARCH_UP 5

/* Tried in order for each base directory; the repository's own copy wins so a
 * fresh clone is complete without the optional pack beside it. */
static const char *const g_emoji_subdirs[] = {
    "assets\\emoji\\bmp\\24",                /* this repository's copy */
    "emoji\\bmp\\24",                        /* release install */
    "assets\\bmp\\24",                       /* pack unzipped beside the EXE */
    "AIagent98-Emoji-Pack-v2.0\\assets\\bmp\\24", /* optional pack checkout */
    "emoji"
};

static int try_base(const char *base, char *out, size_t cap)
{
    char tmp[A98_MAX_PATH];
    int i;

    if (base == NULL || base[0] == '\0') {
        return 0;
    }
    for (i = 0; i < A98_ARRAY_LEN(g_emoji_subdirs); i++) {
        if (a98_path_join(tmp, sizeof(tmp), base, g_emoji_subdirs[i]) != 0) {
            continue;
        }
        if (a98_path_is_dir(tmp)) {
            strncpy(out, tmp, cap - 1);
            out[cap - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int search_up(const char *start, char *out, size_t cap)
{
    char base[A98_MAX_PATH];
    char parent[A98_MAX_PATH];
    int depth;

    if (start == NULL || start[0] == '\0') {
        return 0;
    }
    strncpy(base, start, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    for (depth = 0; depth <= A98_EMOJI_SEARCH_UP; depth++) {
        if (try_base(base, out, cap)) {
            return 1;
        }
        /* A bare drive ("C:") has nowhere left to go; stopping here also
         * avoids asking for the drive-relative "C:emoji\\bmp\\24". */
        if (strlen(base) <= 2 && base[1] == ':') {
            break;
        }
        a98_path_dirname(base, parent, sizeof(parent));
        if (parent[0] == '\0' || strcmp(parent, ".") == 0 ||
            strcmp(parent, base) == 0) {
            break;
        }
        strcpy(base, parent);
    }
    return 0;
}

int a98_emoji_asset_dir_from(const char *exe_dir, const char *cwd, char *out,
                             size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (search_up(exe_dir, out, cap)) {
        return 1;
    }
    out[0] = '\0';
    if (search_up(cwd, out, cap)) {
        return 1;
    }
    out[0] = '\0';
    return 0;
}

int a98_emoji_asset_dir(const char *exe_dir, char *out, size_t cap)
{
    char cwd[A98_MAX_PATH];

    cwd[0] = '\0';
#ifdef _WIN32
    if (GetCurrentDirectoryA(sizeof(cwd), cwd) == 0) {
        cwd[0] = '\0';
    }
#endif
    return a98_emoji_asset_dir_from(exe_dir, cwd[0] != '\0' ? cwd : NULL,
                                    out, cap);
}

int a98_emoji_asset_file(const char *dir, const A98EmojiEntry *e, char *out,
                         size_t cap)
{
    int n;

    if (dir == NULL || e == NULL || out == NULL || cap == 0) {
        return -1;
    }
    n = snprintf(out, cap, "%s\\%s.bmp", dir, e->id);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int token_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    if (c == '_' || c == '-' || c == '+') {
        return 1;
    }
    return 0;
}

char *a98_emoji_expand(const char *utf8)
{
    Str out;
    size_t i;
    size_t len;

    if (utf8 == NULL) {
        return NULL;
    }
    str_init(&out);
    len = strlen(utf8);
    i = 0;
    while (i < len) {
        if (utf8[i] == ':' && i + 2 < len && token_char(utf8[i + 1])) {
            size_t j = i + 1;

            while (j < len && token_char(utf8[j]) &&
                   j - i - 1 < A98_EMOJI_MAX_TOKEN) {
                j++;
            }
            if (j < len && utf8[j] == ':' && j > i + 1) {
                char name[A98_EMOJI_MAX_TOKEN + 1];
                const A98EmojiEntry *e;
                size_t nl = j - i - 1;

                memcpy(name, utf8 + i + 1, nl);
                name[nl] = '\0';
                e = a98_emoji_by_shortcode(name);
                if (e != NULL) {
                    char buf[4];
                    int m = a98_utf8_encode((unsigned int)e->cp, buf);

                    if (m > 0) {
                        str_append(&out, buf, (size_t)m);
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        str_append_char(&out, utf8[i]);
        i++;
    }
    return str_detach(&out);
}
