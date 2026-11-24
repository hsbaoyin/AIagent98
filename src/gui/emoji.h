/*
 * AIagent98 - emoji pack support.
 *
 * Windows 9x has no color emoji font and CP_ACP cannot represent emoji at
 * all, so a raw emoji character reaching the transcript turned into '?'.
 * The bundled pack gives us two things: BMP artwork for the picker and a
 * stable name for every code point.
 *
 * Text path:
 *   model -> a98_emoji_fold()   -> ":white_heavy_check_mark:" (displayed)
 *   user  -> a98_emoji_expand() -> the real emoji (sent to the API)
 * Folding is idempotent, so history replay and re-render stay stable.
 *
 * The pack is optional at run time: without the BMP folder the shortcodes
 * still work, only the picker loses its pictures.
 */
#ifndef A98_EMOJI_H
#define A98_EMOJI_H

#include "base.h"

/* Longest ":shortcode:" we accept.  The longest name in the current pack is
 * 78 characters, so 72 used to truncate it; leave headroom for packs with
 * longer names.  The bound still keeps a stray colon from scanning the rest
 * of the message.  Shared by expand(), the transcript's picture scanner and
 * the inline-picture renderer. */
#define A98_EMOJI_MAX_TOKEN 96

/* One entry of the bundled pack.  `id` doubles as the asset file name. */
typedef struct {
    unsigned long cp;        /* Unicode code point */
    const char *id;          /* "A98E0042" - also assets/bmp/<size>/<id>.bmp */
    const char *shortcode;   /* ":white_heavy_check_mark:" without colons */
} A98EmojiEntry;

int a98_emoji_count(void);
const A98EmojiEntry *a98_emoji_at(int index);
const A98EmojiEntry *a98_emoji_by_codepoint(unsigned long cp);
const A98EmojiEntry *a98_emoji_by_shortcode(const char *name);

/* Emoji -> ":shortcode:", unknown emoji -> "[U+1F600]".  Returns a
 * malloc'ed UTF-8 string ("" for empty input), or NULL on allocation
 * failure.  Caller frees with a98_free(). */
char *a98_emoji_fold(const char *utf8);

/* ":shortcode:" -> emoji, everything else copied through.  Returns a
 * malloc'ed UTF-8 string, or NULL on allocation failure. */
char *a98_emoji_expand(const char *utf8);

/* Locate the BMP folder.  For each base (<exe_dir>, then the current
 * directory) it tries <base>\emoji\bmp\24, <base>\assets\bmp\24 and
 * <base>\AIagent98-Emoji-Pack-v2.0\assets\bmp\24, then walks up to 5
 * parents.  That is what makes a build\cmake\agent98.exe find the pack at the
 * repository root without copying 1219 BMPs into the build tree.  Returns 1
 * and fills out[]; returns 0 with out[] cleared when nothing exists (the
 * picker then lists names only). */
int a98_emoji_asset_dir(const char *exe_dir, char *out, size_t cap);

/* Same search with both bases supplied, so the host test can prove the
 * "found" and the "missing" path without touching the process CWD. */
int a98_emoji_asset_dir_from(const char *exe_dir, const char *cwd, char *out,
                             size_t cap);

/* <dir>\<id>.bmp for one entry.  Returns 0 on success. */
int a98_emoji_asset_file(const char *dir, const A98EmojiEntry *e, char *out,
                         size_t cap);

#endif /* A98_EMOJI_H */
