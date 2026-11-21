/*
 * AIagent98 - self-drawn transcript view.  See transcript_view.h.
 *
 * Data model: a flat list of physical lines; each line owns a list of runs.
 * A run is either text (ACP bytes + format) or an inline emoji (pack index).
 * Text arrives through EM_REPLACESEL with a format set by EM_SETCHARFORMAT;
 * emoji arrive through A98TV_EMOJI.  Everything the GUI already sends maps
 * onto this model without touching the agent loop.
 *
 * Win9x constraints honoured: no GDI+ / no Unicode-only APIs, handles freed
 * promptly, no more than a few hundred cached bitmaps, no OLE.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transcript_view.h"
#include "emoji.h"
#include "base.h"
#include "str.h"

#ifndef CFM_BACKCOLOR
#define CFM_BACKCOLOR 0x04000000
#endif
#ifndef CFM_COLOR
#define CFM_COLOR 0x40000000
#endif
#ifndef CFM_BOLD
#define CFM_BOLD 0x00000001
#endif
#ifndef CFM_FACE
#define CFM_FACE 0x00200000
#endif
#ifndef CFM_CHARSET
#define CFM_CHARSET 0x08000000
#endif
#ifndef CFE_BOLD
#define CFE_BOLD 0x00000001
#endif

/* Semantic colours - tuned for a light气泡 card on white transcript bg. */
#define TV_YOU_FG      RGB(30, 55, 120)
#define TV_YOU_BG      RGB(232, 240, 254)
#define TV_AI_FG       RGB(32, 32, 32)
#define TV_AI_BG       RGB(248, 248, 250)
#define TV_TOOL_FG     RGB(0, 100, 100)
#define TV_TOOL_BG     RGB(230, 245, 245)
#define TV_RESULT_FG   RGB(20, 110, 40)
#define TV_RESULT_BG   RGB(234, 248, 236)
#define TV_META_FG     RGB(120, 120, 120)
#define TV_ERROR_FG    RGB(190, 30, 30)
#define TV_CODE_BG     RGB(228, 228, 228)
#define TV_INLINE_FG   RGB(150, 0, 150)
#define TV_BUBBLE_EDGE RGB(205, 205, 215)
#define TV_ACCENT_YOU  RGB(60, 100, 190)
#define TV_ACCENT_AI   RGB(120, 160, 120)
#define TV_PAGE_BG     RGB(255, 255, 255)

#define TV_PAD_X       8
#define TV_PAD_Y       5
#define TV_BUBBLE_GAP  10
#define TV_EMOJI_PX    16
#define TV_MARGIN      6
#define TV_MAX_BUBBLE  600
#define TV_CACHE_MAX   64

typedef struct {
    char *text;        /* ACP bytes, NUL-terminated; empty for emoji runs */
    COLORREF fg;
    COLORREF bg;       /* 0 = none */
    int bold;
    int emoji;         /* 0 = text, else 1-based pack index */
} TvRun;

typedef struct {
    TvRun *runs;
    int nruns;
    int cruns;
    int role;
    int folded;        /* tool header: 1 = body hidden */
    int fold_id;       /* groups a tool header with its results */
    int group_first;   /* first physical line of the current bubble group */
    int group_last;    /* last physical line of the group (set on commit) */
    int height;        /* line height in px (max of font and emoji) */
} TvLine;

typedef struct {
    HINSTANCE hinst;
    HWND hwnd;
    HFONT font;
    HFONT bold_font;
    int char_w;        /* average char width for layout estimates */
    int line_h;        /* font line height */

    TvLine *lines;
    int nlines;
    int clines;

    /* current append format (from EM_SETCHARFORMAT) */
    COLORREF cur_fg;
    COLORREF cur_bg;
    int cur_bold;
    char cur_face[LF_FACESIZE];
    BYTE cur_charset;

    /* selection: line index + x offset within line, -1 = none */
    int sel_anchor_line, sel_anchor_x;
    int sel_end_line, sel_end_x;
    int selecting;

    int scroll_y;      /* first visible pixel row */
    int stick_bottom;

    char exe_dir[A98_MAX_PATH];
    char emoji_dir[A98_MAX_PATH];
    int emoji_dir_tried;

    /* emoji bitmap cache: small LRU so 9x GDI handles stay bounded */
    struct { int id; HBITMAP bmp; } cache[TV_CACHE_MAX];
    int cache_used;    /* clock counter */
} TvDoc;

static int g_registered = 0;

static void tv_recalc(TvDoc *d);
static void tv_scroll_to_bottom(TvDoc *d);

/* ---- bitmap cache --------------------------------------------------- */

static void cache_clear(TvDoc *d)
{
    int i;
    for (i = 0; i < TV_CACHE_MAX; i++) {
        if (d->cache[i].bmp != NULL) {
            DeleteObject(d->cache[i].bmp);
            d->cache[i].bmp = NULL;
            d->cache[i].id = 0;
        }
    }
}

/* Per-slot age for the little LRU; a higher number means more recently used. */
static int g_cache_age[TV_CACHE_MAX];

static HBITMAP cache_get(TvDoc *d, int id)
{
    int i, oldest, oldest_age;
    char path[A98_MAX_PATH];
    const A98EmojiEntry *e;
    HBITMAP bmp;

    e = a98_emoji_at(id - 1);
    if (e == NULL) return NULL;
    for (i = 0; i < TV_CACHE_MAX; i++) {
        if (d->cache[i].id == id && d->cache[i].bmp != NULL) {
            g_cache_age[i] = ++d->cache_used;
            return d->cache[i].bmp;
        }
    }
    if (!d->emoji_dir_tried) {
        d->emoji_dir_tried = 1;
        if (!a98_emoji_asset_dir(d->exe_dir, d->emoji_dir, sizeof(d->emoji_dir)))
            d->emoji_dir[0] = '\0';
    }
    if (d->emoji_dir[0] == '\0') return NULL;
    if (a98_emoji_asset_file(d->emoji_dir, e, path, sizeof(path)) != 0)
        return NULL;
    bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, TV_EMOJI_PX,
                              TV_EMOJI_PX, LR_LOADFROMFILE);
    if (bmp == NULL) return NULL;
    oldest = 0;
    oldest_age = g_cache_age[0];
    for (i = 1; i < TV_CACHE_MAX; i++) {
        if (g_cache_age[i] < oldest_age) { oldest_age = g_cache_age[i]; oldest = i; }
    }
    if (d->cache[oldest].bmp != NULL) DeleteObject(d->cache[oldest].bmp);
    d->cache[oldest].id = id;
    d->cache[oldest].bmp = bmp;
    g_cache_age[oldest] = ++d->cache_used;
    return bmp;
}

/* ---- document helpers ---------------------------------------------- */

static TvLine *new_line(TvDoc *d)
{
    TvLine *ln;
    if (d->nlines >= d->clines) {
        int nc = d->clines ? d->clines * 2 : 64;
        TvLine *p = (TvLine *)realloc(d->lines, (size_t)nc * sizeof(TvLine));
        if (p == NULL) return NULL;
        d->lines = p;
        d->clines = nc;
    }
    ln = &d->lines[d->nlines++];
    memset(ln, 0, sizeof(*ln));
    ln->height = d->line_h;
    ln->group_first = d->nlines - 1;
    ln->group_last = d->nlines - 1;
    return ln;
}

static TvRun *new_run(TvLine *ln)
{
    TvRun *r;
    if (ln->nruns >= ln->cruns) {
        int nc = ln->cruns ? ln->cruns * 2 : 4;
        TvRun *p = (TvRun *)realloc(ln->runs, (size_t)nc * sizeof(TvRun));
        if (p == NULL) return NULL;
        ln->runs = p;
        ln->cruns = nc;
    }
    r = &ln->runs[ln->nruns++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void doc_clear(TvDoc *d)
{
    int i, j;
    for (i = 0; i < d->nlines; i++) {
        for (j = 0; j < d->lines[i].nruns; j++) {
            free(d->lines[i].runs[j].text);
        }
        free(d->lines[i].runs);
    }
    free(d->lines);
    d->lines = NULL;
    d->nlines = 0;
    d->clines = 0;
    d->scroll_y = 0;
    d->sel_anchor_line = d->sel_end_line = -1;
}

static void doc_init_fmt(TvDoc *d)
{
    d->cur_fg = TV_AI_FG;
    d->cur_bg = 0;
    d->cur_bold = 0;
    d->cur_face[0] = '\0';
    d->cur_charset = 0;
}

/* Classify a fresh logical line from its first run's text. */
static int classify_line(TvLine *ln)
{
    TvRun *r;
    if (ln->nruns == 0 || ln->runs[0].text == NULL) return A98TV_ROLE_AI;
    r = &ln->runs[0];
    if (strncmp(r->text, "You:", 4) == 0) return A98TV_ROLE_YOU;
    if (strncmp(r->text, "[tool]", 6) == 0) return A98TV_ROLE_TOOL;
    if (strncmp(r->text, "[result]", 8) == 0) return A98TV_ROLE_RESULT;
    if (strncmp(r->text, "[error]", 7) == 0) return A98TV_ROLE_ERROR;
    if (strncmp(r->text, "[round]", 7) == 0 || strncmp(r->text, "[info]", 6) == 0)
        return A98TV_ROLE_META;
    if (strncmp(r->text, "Assistant:", 10) == 0) return A98TV_ROLE_AI;
    return A98TV_ROLE_AI;
}

/* Append text (ACP) to the last physical line, splitting on CRLF and
 * wrapping to TV_MAX_BUBBLE px.  A brand-new logical line is classified. */
static void tv_append_text(TvDoc *d, const char *text)
{
    const char *p = text;
    TvLine *ln;
    TvRun *r;
    Str buf;
    int fresh = 0;

    if (d->nlines == 0) {
        ln = new_line(d);
        if (ln == NULL) return;
        fresh = 1;
    }
    ln = &d->lines[d->nlines - 1];

    str_init(&buf);
    while (*p != '\0') {
        if (*p == '\r' || *p == '\n') {
            /* commit the current line */
            if (buf.len > 0) {
                r = new_run(ln);
                if (r != NULL) {
                    r->text = a98_strdup(str_cstr(&buf));
                    r->fg = d->cur_fg; r->bg = d->cur_bg; r->bold = d->cur_bold;
                }
                str_clear(&buf);
            }
            if (fresh) { ln->role = classify_line(ln); fresh = 0; }
            ln->group_last = d->nlines - 1;
            ln = new_line(d);
            if (ln == NULL) break;
            fresh = 1;
            /* skip CRLF pairs */
            if (*p == '\r' && *(p + 1) == '\n') p += 2;
            else p += 1;
            continue;
        }
        str_append_char(&buf, *p);
        p++;
    }
    if (buf.len > 0) {
        r = new_run(ln);
        if (r != NULL) {
            r->text = a98_strdup(str_cstr(&buf));
            r->fg = d->cur_fg; r->bg = d->cur_bg; r->bold = d->cur_bold;
        }
    }
    if (fresh && ln->nruns > 0) { ln->role = classify_line(ln); }
    ln->group_last = d->nlines - 1;
    str_free(&buf);
}

/* Append an inline emoji run. */
static void tv_append_emoji(TvDoc *d, const A98EmojiEntry *e)
{
    TvLine *ln;
    TvRun *r;
    char tok[64];

    if (e == NULL) return;
    if (d->nlines == 0) {
        ln = new_line(d);
        if (ln == NULL) return;
    } else {
        ln = &d->lines[d->nlines - 1];
    }
    if (ln->nruns == 0) ln->role = classify_line(ln);
    r = new_run(ln);
    if (r == NULL) return;
    snprintf(tok, sizeof(tok), ":%s:", e->shortcode);
    r->text = a98_strdup(tok);
    r->emoji = (int)(e - a98_emoji_at(0)) + 1;
    r->fg = d->cur_fg;
    if (ln->height < TV_EMOJI_PX + 4) ln->height = TV_EMOJI_PX + 4;
    ln->group_last = d->nlines - 1;
}

/* Rebuild plain ACP text (for WM_GETTEXT / export). */
static size_t tv_plain_text(TvDoc *d, char *out, size_t cap)
{
    size_t n = 0;
    int i, j;
    for (i = 0; i < d->nlines; i++) {
        for (j = 0; j < d->lines[i].nruns; j++) {
            TvRun *r = &d->lines[i].runs[j];
            size_t k;
            if (r->text == NULL) continue;
            for (k = 0; r->text[k] != '\0'; k++) {
                if (n + 1 < cap) out[n++] = r->text[k];
            }
        }
        if (n + 2 < cap) { out[n++] = '\r'; out[n++] = '\n'; }
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* total pixel height of all (visible) lines */
static int tv_content_height(TvDoc *d)
{
    int i, h = 0;
    for (i = 0; i < d->nlines; i++) {
        TvLine *ln = &d->lines[i];
        if (ln->role == A98TV_ROLE_TOOL && ln->folded) {
            h += ln->height;
            continue;
        }
        if (ln->role == A98TV_ROLE_RESULT) {
            /* find the last preceding tool header; if folded, skip */
            int k;
            for (k = i - 1; k >= 0; k--) {
                if (d->lines[k].role == A98TV_ROLE_TOOL) {
                    if (d->lines[k].folded) { h += 0; }
                    break;
                }
                if (d->lines[k].role != A98TV_ROLE_RESULT) break;
            }
        }
        h += ln->height;
    }
    return h;
}

static int tv_line_y(TvDoc *d, int idx)
{
    int i, y = 0;
    for (i = 0; i < idx && i < d->nlines; i++) {
        TvLine *ln = &d->lines[i];
        if (ln->role == A98TV_ROLE_TOOL && ln->folded) { y += ln->height; continue; }
        y += ln->height;
    }
    return y;
}

static void tv_recalc(TvDoc *d)
{
    RECT rc;
    int content, view;

    GetClientRect(d->hwnd, &rc);
    content = tv_content_height(d);
    view = rc.bottom - rc.top;
    if (d->stick_bottom) {
        d->scroll_y = content > view ? content - view : 0;
    } else if (d->scroll_y > content - view) {
        d->scroll_y = content - view > 0 ? content - view : 0;
    }
    if (d->scroll_y < 0) d->scroll_y = 0;
    if (d->hwnd != NULL) InvalidateRect(d->hwnd, NULL, FALSE);
}

static void tv_scroll_to_bottom(TvDoc *d)
{
    d->stick_bottom = 1;
    tv_recalc(d);
}

#ifndef GWLP_USERDATA
#define GWLP_USERDATA (-21)
#endif
/* Win9x has no SetWindowLongPtrA; LONG is pointer-sized there.  MinGW/x64
 * does, and truncating a 64-bit pointer into cbWndExtra would crash. */
#ifdef _WIN64
#define TV_SET_PTR(h, v)  SetWindowLongPtrA((h), GWLP_USERDATA, (LONG_PTR)(v))
#define TV_GET_PTR(h)     ((void *)GetWindowLongPtrA((h), GWLP_USERDATA))
#else
#define TV_SET_PTR(h, v)  SetWindowLongA((h), GWLP_USERDATA, (LONG)(v))
#define TV_GET_PTR(h)     ((void *)GetWindowLongA((h), GWLP_USERDATA))
#endif

static TvDoc *tv_from_hwnd(HWND hwnd)
{
    return (TvDoc *)TV_GET_PTR(hwnd);
}

/* ---- painting ------------------------------------------------------- */

static COLORREF role_fg(int role) {
    switch (role) {
    case A98TV_ROLE_YOU: return TV_YOU_FG;
    case A98TV_ROLE_TOOL: return TV_TOOL_FG;
    case A98TV_ROLE_RESULT: return TV_RESULT_FG;
    case A98TV_ROLE_META: return TV_META_FG;
    case A98TV_ROLE_ERROR: return TV_ERROR_FG;
    default: return TV_AI_FG;
    }
}
static COLORREF role_bg(int role) {
    switch (role) {
    case A98TV_ROLE_YOU: return TV_YOU_BG;
    case A98TV_ROLE_TOOL: return TV_TOOL_BG;
    case A98TV_ROLE_RESULT: return TV_RESULT_BG;
    default: return TV_AI_BG;
    }
}
static COLORREF role_accent(int role) {
    return role == A98TV_ROLE_YOU ? TV_ACCENT_YOU : TV_ACCENT_AI;
}

static int run_pixel_width(TvDoc *d, TvRun *r, HDC dc, HFONT font)
{
    HFONT old;
    SIZE sz;
    if (r->emoji) return TV_EMOJI_PX + 2;
    if (r->text == NULL || r->text[0] == '\0') return 0;
    old = (HFONT)SelectObject(dc, font);
    GetTextExtentPoint32A(dc, r->text, (int)strlen(r->text), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static void tv_paint(TvDoc *d)
{
    HDC dc, mem;
    RECT rc, rcClient;
    HBITMAP mb, oldb;
    HFONT oldf;
    int i, j, y, x;
    int view_w, view_h;



    dc = GetDC(d->hwnd);
    if (dc == NULL) return;
    GetClientRect(d->hwnd, &rcClient);
    view_w = rcClient.right - rcClient.left;
    view_h = rcClient.bottom - rcClient.top;

    if (view_w <= 0 || view_h <= 0) { ReleaseDC(d->hwnd, dc); return; }

    mem = CreateCompatibleDC(dc);
    mb = CreateCompatibleBitmap(dc, view_w, view_h);
    oldb = (HBITMAP)SelectObject(mem, mb);

    SetBkMode(mem, TRANSPARENT);

    /* page background: native window colour, like the old EDIT/RichEdit */
    {
        HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        FillRect(mem, &rcClient, br);
        DeleteObject(br);
    }

    oldf = (HFONT)SelectObject(mem, d->font);

    y = -d->scroll_y;
    for (i = 0; i < d->nlines; i++) {
        TvLine *ln = &d->lines[i];
        int hidden = 0;
        if (ln->role == A98TV_ROLE_TOOL && ln->folded) hidden = 0;
        if (ln->role == A98TV_ROLE_RESULT) {
            int k;
            for (k = i - 1; k >= 0; k--) {
                if (d->lines[k].role == A98TV_ROLE_TOOL) {
                    if (d->lines[k].folded) hidden = 1;
                    break;
                }
            }
        }
        if (y + ln->height > 0 && y < view_h && !hidden) {
            /* Plain-text transcript: no bubbles, no borders, no accent bar.
             * Every line starts at the left margin and keeps its semantic
             * colour.  Inline emoji are still drawn as GDI bitmaps. */
            RECT lrc;
            lrc.left = 0;
            lrc.top = y;
            lrc.right = view_w;
            lrc.bottom = y + ln->height;

            /* selection highlight (full-width, plain background) */
            if (d->sel_anchor_line >= 0 && d->sel_end_line >= 0) {
                int lo = d->sel_anchor_line < d->sel_end_line ? d->sel_anchor_line : d->sel_end_line;
                int hi = d->sel_anchor_line < d->sel_end_line ? d->sel_end_line : d->sel_anchor_line;
                if (i >= lo && i <= hi) {
                    HBRUSH sb = CreateSolidBrush(RGB(180, 210, 255));
                    FillRect(mem, &lrc, sb);
                    DeleteObject(sb);
                }
            }

            x = TV_MARGIN;
            for (j = 0; j < ln->nruns; j++) {
                TvRun *r = &ln->runs[j];
                int w = run_pixel_width(d, r, mem, r->bold ? d->bold_font : d->font);
                if (r->emoji) {
                    HBITMAP bmp = cache_get(d, r->emoji);
                    if (bmp != NULL) {
                        HDC src = CreateCompatibleDC(mem);
                        HBITMAP ob = (HBITMAP)SelectObject(src, bmp);
                        BitBlt(mem, x, y + (ln->height - TV_EMOJI_PX) / 2,
                               TV_EMOJI_PX, TV_EMOJI_PX, src, 0, 0, SRCCOPY);
                        SelectObject(src, ob);
                        DeleteDC(src);
                    } else if (r->text != NULL) {
                        SetTextColor(mem, role_fg(ln->role));
                        TextOutA(mem, x, y + 2, r->text, (int)strlen(r->text));
                    }
                } else if (r->text != NULL && r->text[0] != '\0') {
                    COLORREF fg = role_fg(ln->role);
                    if (r->fg != TV_AI_FG) fg = r->fg;
                    SetTextColor(mem, fg);
                    {
                        HFONT usef = r->bold ? d->bold_font : d->font;
                        HFONT of2 = (HFONT)SelectObject(mem, usef);
                        TextOutA(mem, x, y + 2, r->text, (int)strlen(r->text));
                        SelectObject(mem, of2);
                    }
                }
                x += w;
            }

            /* fold affordance for tool calls (plain leading marker) */
            if (ln->role == A98TV_ROLE_TOOL) {
                char mark[4];
                strcpy(mark, ln->folded ? "+" : "-");
                SetTextColor(mem, TV_TOOL_FG);
                TextOutA(mem, TV_MARGIN - 6, y + 2, mark, 1);
            }
        }

        y += ln->height;
    }


    BitBlt(dc, 0, 0, view_w, view_h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldf);
    SelectObject(mem, oldb);
    DeleteObject(mb);
    DeleteDC(mem);
    ReleaseDC(d->hwnd, dc);

}

/* ---- window procedure ---------------------------------------------- */

static LRESULT CALLBACK tv_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TvDoc *d = tv_from_hwnd(hwnd);

    switch (msg) {
    case WM_CREATE:
        /* Store the doc here so WS_VISIBLE children that paint during
         * CreateWindowExA already have a valid pointer. */
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lp;
            if (cs != NULL && cs->lpCreateParams != NULL) {
                TV_SET_PTR(hwnd, cs->lpCreateParams);
            }
        }
        return 0;

    case WM_SETTEXT:
        if (d != NULL) {
            doc_clear(d);
            doc_init_fmt(d);
            d->stick_bottom = 1;
            tv_recalc(d);
        }
        return 0;

    case WM_GETTEXTLENGTH:
        if (d == NULL) return 0;
        {
            char *buf = (char *)malloc((size_t)d->nlines * 256 + 1024);
            size_t n;
            if (buf == NULL) return 0;
            n = tv_plain_text(d, buf, (size_t)d->nlines * 256 + 1024);
            free(buf);
            return (LRESULT)n;
        }

    case WM_GETTEXT:
        if (d == NULL) return 0;
        tv_plain_text(d, (char *)lp, (size_t)wp);
        return lstrlenA((char *)lp);

    case WM_SETFONT:
        if (d != NULL && lp != 0) {
            HDC dc;
            TEXTMETRICA tm;
            LOGFONTA lf;
            d->font = (HFONT)lp;
            memset(&lf, 0, sizeof(lf));
            strcpy(lf.lfFaceName, "MS Sans Serif");
            lf.lfHeight = -11;
            lf.lfWeight = FW_BOLD;
            d->bold_font = CreateFontIndirectA(&lf);
            dc = GetDC(hwnd);
            if (dc != NULL) {
                HFONT of = (HFONT)SelectObject(dc, d->font);
                if (GetTextMetricsA(dc, &tm)) {
                    d->line_h = tm.tmHeight + 2;
                    d->char_w = tm.tmAveCharWidth;
                }
                SelectObject(dc, of);
                ReleaseDC(hwnd, dc);
            }
            tv_recalc(d);
        }
        return 0;

    case WM_SIZE:
        if (d != NULL) tv_recalc(d);
        return 0;

    case EM_SETBKGNDCOLOR:
    case EM_SETLIMITTEXT:
    case EM_EXLIMITTEXT:
    case EM_SETEVENTMASK:
        return 0;

    case EM_SETCHARFORMAT:
        if (d != NULL && lp != 0) {
            CHARFORMAT2A *cf2 = (CHARFORMAT2A *)lp;
            if (cf2->dwMask & CFM_COLOR) d->cur_fg = cf2->crTextColor;
            if (cf2->dwMask & CFM_BOLD) d->cur_bold = (cf2->dwEffects & CFE_BOLD) ? 1 : 0;
            if ((cf2->dwMask & CFM_BACKCOLOR) && cf2->crBackColor != 0)
                d->cur_bg = cf2->crBackColor;
            else
                d->cur_bg = 0;
            if (cf2->dwMask & CFM_FACE) {
                strncpy(d->cur_face, cf2->szFaceName, LF_FACESIZE - 1);
                d->cur_face[LF_FACESIZE - 1] = '\0';
            }
            if (cf2->dwMask & CFM_CHARSET) d->cur_charset = cf2->bCharSet;
        }
        return 0;

    case EM_SETSEL:
    case EM_EXSETSEL:
        /* selection handled by mouse; ignore programmatic caret moves */
        return 0;

    case EM_EXGETSEL:
        if (lp != 0) { ((CHARRANGE *)lp)->cpMin = 0; ((CHARRANGE *)lp)->cpMax = 0; }
        return 0;

    case EM_REPLACESEL:
        if (d != NULL && lp != 0) {
            tv_append_text(d, (const char *)lp);
            tv_recalc(d);
        }
        return 0;

    case EM_GETLINECOUNT:
        return d != NULL ? d->nlines : 0;

    case EM_GETFIRSTVISIBLELINE:
        if (d == NULL) return 0;
        return (d->line_h > 0) ? (d->scroll_y / d->line_h) : 0;

    case EM_EXLINEFROMCHAR:
        if (lp != 0) *(long *)lp = 0;
        return (d != NULL && d->line_h > 0) ? (d->scroll_y / d->line_h) : 0;

    case EM_LINEINDEX:
        return 0;

    case EM_SCROLLCARET:
        if (d != NULL) tv_scroll_to_bottom(d);
        return 0;

    case A98TV_EMOJI:
        if (d != NULL && lp != 0) {
            tv_append_emoji(d, (const A98EmojiEntry *)lp);
            tv_recalc(d);
        }
        return 0;

    case A98TV_TRIM:
        if (d != NULL) {
            int maxl = (int)wp;
            if (maxl > 0 && d->nlines > maxl) {
                int cut = maxl / 2;
                int i, j;
                for (i = 0; i < cut; i++) {
                    for (j = 0; j < d->lines[i].nruns; j++) free(d->lines[i].runs[j].text);
                    free(d->lines[i].runs);
                }
                memmove(d->lines, d->lines + cut,
                        (size_t)(d->nlines - cut) * sizeof(TvLine));
                d->nlines -= cut;
                d->stick_bottom = 1;
                tv_recalc(d);
            }
        }
        return 0;

    case A98TV_SETDIR:
        if (d != NULL && lp != 0) {
            strncpy(d->exe_dir, (const char *)lp, A98_MAX_PATH - 1);
            d->exe_dir[A98_MAX_PATH - 1] = '\0';
            d->emoji_dir_tried = 0;
            cache_clear(d);
        }
        return 0;

    case WM_VSCROLL:
        if (d != NULL) {
            RECT rc;
            int content, view, delta = 0;
            GetClientRect(hwnd, &rc);
            content = tv_content_height(d);
            view = rc.bottom - rc.top;
            switch (LOWORD(wp)) {
            case SB_LINEUP: delta = -d->line_h; break;
            case SB_LINEDOWN: delta = d->line_h; break;
            case SB_PAGEUP: delta = -view; break;
            case SB_PAGEDOWN: delta = view; break;
            case SB_TOP: delta = -d->scroll_y; break;
            case SB_BOTTOM: delta = content - d->scroll_y; break;
            default: break;
            }
            d->scroll_y += delta;
            if (d->scroll_y < 0) d->scroll_y = 0;
            if (d->scroll_y > content - view) d->scroll_y = content - view > 0 ? content - view : 0;
            d->stick_bottom = (d->scroll_y >= content - view - 2);
            InvalidateRect(hwnd, NULL, FALSE);
            PostMessage(GetParent(hwnd), A98TV_VIEWPORT_CHANGED, 0, 0);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (d != NULL) {
            int delta = (short)HIWORD(wp);
            d->scroll_y -= (delta / 120) * (d->line_h * 3);
            if (d->scroll_y < 0) d->scroll_y = 0;
            {
                RECT rc; int content, view;
                GetClientRect(hwnd, &rc);
                content = tv_content_height(d); view = rc.bottom - rc.top;
                if (d->scroll_y > content - view) d->scroll_y = content - view > 0 ? content - view : 0;
                d->stick_bottom = (d->scroll_y >= content - view - 2);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            PostMessage(GetParent(hwnd), A98TV_VIEWPORT_CHANGED, 0, 0);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (d != NULL) {
            int x = LOWORD(lp), y = HIWORD(lp);
            SetFocus(hwnd);
            d->selecting = 1;
            d->sel_anchor_line = d->sel_end_line = (y + d->scroll_y) / d->line_h;
            d->sel_anchor_x = d->sel_end_x = x;
            /* fold toggle: click the leading +/- marker of a tool header */
            {
                int idx = (y + d->scroll_y) / d->line_h;
                if (idx >= 0 && idx < d->nlines && d->lines[idx].role == A98TV_ROLE_TOOL
                    && x <= TV_MARGIN + 2) {
                    d->lines[idx].folded = !d->lines[idx].folded;
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (d != NULL && d->selecting) {
            int x = LOWORD(lp), y = HIWORD(lp);
            d->sel_end_line = (y + d->scroll_y) / d->line_h;
            d->sel_end_x = x;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (d != NULL) d->selecting = 0;
        return 0;

    case WM_COPY:
    case WM_KEYDOWN:
        if (d != NULL) {
            if (msg == WM_COPY || (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))) {
                char *buf;
                size_t n, cap;
                cap = (size_t)d->nlines * 256 + 1024;
                buf = (char *)malloc(cap);
                if (buf != NULL) {
                    n = tv_plain_text(d, buf, cap);
                    if (n > 0 && OpenClipboard(hwnd)) {
                        HGLOBAL h;
                        char *p;
                        EmptyClipboard();
                        h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
                        p = (char *)GlobalLock(h);
                        memcpy(p, buf, n); p[n] = '\0';
                        GlobalUnlock(h);
                        SetClipboardData(CF_TEXT, h);
                        CloseClipboard();
                    }
                    free(buf);
                }
                return 0;
            }
            if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
                RECT rc; int content, view, delta = 0;
                GetClientRect(hwnd, &rc);
                content = tv_content_height(d); view = rc.bottom - rc.top;
                if (wp == VK_UP) delta = -d->line_h;
                else if (wp == VK_DOWN) delta = d->line_h;
                else if (wp == VK_PRIOR) delta = -view;
                else delta = view;
                d->scroll_y += delta;
                if (d->scroll_y < 0) d->scroll_y = 0;
                if (d->scroll_y > content - view) d->scroll_y = content - view > 0 ? content - view : 0;
                d->stick_bottom = (d->scroll_y >= content - view - 2);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (d != NULL) tv_paint(d);
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_DESTROY:
        if (d != NULL) {
            doc_clear(d);
            cache_clear(d);
            if (d->bold_font != NULL) DeleteObject(d->bold_font);
        }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

int a98tv_register(HINSTANCE hinst)
{
    WNDCLASSA wc;
    if (g_registered) return 1;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = tv_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = A98TV_CLASSNAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.cbWndExtra = sizeof(LONG);
    if (RegisterClassA(&wc) == 0) return 0;
    g_registered = 1;
    return 1;
}

HWND a98tv_create(HINSTANCE hinst, HWND parent, int ctrl_id, HFONT font,
                  int x, int y, int w, int h)
{
    TvDoc *d;
    HWND hwnd;

    if (!a98tv_register(hinst)) return NULL;
    d = (TvDoc *)calloc(1, sizeof(TvDoc));
    if (d == NULL) return NULL;
    d->hinst = hinst;
    d->cur_fg = TV_AI_FG;
    d->cur_bg = 0;
    d->cur_bold = 0;
    d->sel_anchor_line = d->sel_end_line = -1;
    d->stick_bottom = 1;

    hwnd = CreateWindowExA(WS_EX_CLIENTEDGE, A98TV_CLASSNAME, "",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                           x, y, w, h, parent, (HMENU)ctrl_id, hinst, d);
    if (hwnd == NULL) { free(d); return NULL; }
    d->hwnd = hwnd;
    if (font != NULL) {
        SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    } else {
        SendMessageA(hwnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }
    return hwnd;
}

