/*
 * AIagent98 - native Win32 GUI (Windows 95/98/ME compatible).
 *
 * The agent runs on a worker thread.  It reports streamed text and status
 * through PostMessage() to the UI thread; tool confirmations are handed to
 * the UI thread with a modal dialog and an event the worker waits on.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __WATCOMC__
/* The OpenWatcom runtime needs its thread bookkeeping (per-thread data,
 * heap arena, errno) before any C library call is made on a new thread.
 * CreateThread() skips that setup, so use _beginthreadex() instead. */
#include <process.h>
#endif

#include "resource.h"
#include "emoji.h"
#include "emoji_ui.h"
#include "transcript_view.h"

#include "../core/base.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/path.h"
#include "../core/str.h"
#include "../core/utf8.h"
#include "../agent/agent.h"
#include "../agent/tools.h"
#include "../agent/api_openai.h"
#include "../net/http.h"
#include "../net/net.h"
#include "../net/url.h"
#include "../../third_party/cjson/cJSON.h"

#include <richedit.h>
#include <ole2.h>

#ifndef EM_EXLIMITTEXT
#define EM_EXLIMITTEXT (WM_USER + 37)
#endif
#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#endif
/* MinGW-w64 headers only expose the 64-bit-safe alias.  The value is the
 * same -4 that Win9x expects. */
#ifndef GWL_WNDPROC
#define GWL_WNDPROC GWLP_WNDPROC
#endif
#ifndef GB2312_CHARSET
#define GB2312_CHARSET 134
#endif
#ifndef SHIFTJIS_CHARSET
#define SHIFTJIS_CHARSET 128
#endif
#ifndef HANGUL_CHARSET
#define HANGUL_CHARSET 129
#endif

#define A98_WM_EVENT   (WM_APP + 1)
#define A98_WM_DONE    (WM_APP + 2)
#define A98_WM_CONFIRM (WM_APP + 3)

/* RichEdit-only messages used for the auto-scroll pin.  EM_EXLINEFROMCHAR
 * turns RichEdit's character-based EM_GETFIRSTVISIBLELINE into a line
 * number; both are understood by RichEdit 1.0 and 2.0. */
#ifndef EM_EXLINEFROMCHAR
#define EM_EXLINEFROMCHAR (WM_USER + 106)
#endif
#ifndef EM_SETEVENTMASK
#define EM_SETEVENTMASK (WM_USER + 69)
#endif
#ifndef ENM_SCROLL
#define ENM_SCROLL 0x0004
#endif
/* Background colour band for fenced code (RichEdit 2.0+ only). */
#ifndef CFM_BACKCOLOR
#define CFM_BACKCOLOR 0x04000000
#endif

/* DM_SETDEFID: Win9x's dialog manager honours it, MinGW and the Watcom
 * headers both carry it; keep the guard for old headers. */
#ifndef DM_SETDEFID
#define DM_SETDEFID (WM_USER + 1)
#endif

#define A98_CLASS_NAME "AIagent98Window"

#define RICH_CLASS20 "RichEdit20A"
#define RICH_CLASS10 "RichEdit"

#define TRANSCRIPT_LIMIT 0x7FFFFFFEL

/* Timer used for the retro spinner in the status bar. */
#define IDT_SPINNER 1
/* Forced-destroy timer: while closing, WM_CLOSE only arms this timer so a
 * hung worker can still never leave the user with an unclosable window. */
#define IDT_FORCE_DESTROY 2
/* Coalesces streamed deltas: queued text is flushed to the transcript at
 * this interval instead of once per network chunk. */
#define IDT_EVENT_FLUSH 3
#define A98_EVENT_FLUSH_MS 80

/* Once the transcript grows past the soft cap the oldest lines are dropped
 * (the full history lives in the session .json).  On a 9x box with 64 MB of
 * RAM a multi-megabyte RichEdit buffer gets painful fast. */
#define A98_TRANSCRIPT_MAX_LINES 4000

/* How long WM_DESTROY / WinMain wait for the worker before abandoning it. */
#define A98_WORKER_WAIT_MS 3000
/* Grace period between "Stop it and exit?" consent and the forced destroy. */
#define A98_CLOSE_GRACE_MS 10000

#define A98_MAX_SESSIONS 64
#define A98_MAX_HISTORY  64

#define A98_TRANSCRIPT_POINTS 9
#define A98_TWIPS_PER_POINT   20

/* 90s console palette.  Dark, saturated colours read well on the default
 * white RichEdit background and stay legible on Win9x VGA displays. */
#define COL_YOU    RGB(0, 0, 160)
#define COL_ROUND  RGB(100, 100, 100)
#define COL_TOOL   RGB(0, 110, 110)
#define COL_RESULT RGB(0, 120, 0)
#define COL_ERROR  RGB(200, 0, 0)
#define COL_AI     RGB(0, 0, 0)
/* Markdown: fenced code blocks get a grey background band, inline code a
 * muted magenta, bold text keeps black but bold. */
#define COL_CODE_BG RGB(228, 228, 228)
#define COL_CODE_FG RGB(30, 30, 30)
#define COL_INLINE  RGB(150, 0, 150)

typedef struct {
    int type;
    char *text;
} UiEvent;

typedef struct {
    char tool[64];
    char *args;
    HANDLE event;
    int result;
} ConfirmReq;

typedef struct {
    char name[64];              /* display name (ACP, without .json) */
    char path[A98_MAX_PATH];    /* full path (ACP) */
} SessionEntry;

typedef struct {
    HINSTANCE hinst;
    HWND hwnd;
    HWND transcript;
    HWND input;
    HWND send;
    HWND stop;
    HWND newbtn;
    HWND settings;
    HWND workspace_btn;
    HWND emoji_btn;
    HWND session_label;
    HWND session_combo;
    HWND statusbar;
    HFONT font;
    HFONT mono_font;
    HACCEL accel;

    int rich;               /* 0 = EDIT, 1 = RichEdit 1.0, 2 = RichEdit 2.0 */
    HINSTANCE rich_dll;
    int emoji_pics;         /* transcript keeps inline emoji pictures */
    char mono_face[LF_FACESIZE];
    BYTE mono_charset;
    int line_h;             /* transcript line height in pixels */
    int stick_bottom;       /* auto-scroll: 1 while the view is pinned at
                             * the bottom, 0 once the user scrolled up */

    Config cfg;
    char ini_path[A98_MAX_PATH];
    char exe_dir[A98_MAX_PATH];
    char sessions_dir[A98_MAX_PATH];
    char session_path[A98_MAX_PATH]; /* current auto-save target (ACP) */
    SessionEntry sessions[A98_MAX_SESSIONS];
    int session_count;
    Agent agent;

    HANDLE thread;
    DWORD thread_id;
    HANDLE shutdown_event;      /* set on WM_DESTROY so gui_confirm unblocks */
    HANDLE single_mutex;        /* #11 one instance at a time */
    int leaked_worker;          /* worker did not stop: skip all teardown */
    char *pending_input;
    char *last_input;           /* most recent sent message, for Ctrl+R */
    volatile int cancel;
    volatile int running;
    int closing;

    int spin_frame;
    int round_no;
    char status_line[128];      /* UTF-8, without the spinner */

    char *input_history[A98_MAX_HISTORY];
    int input_history_count;
    int input_history_pos;

    /* Streamed deltas are buffered and flushed on IDT_EVENT_FLUSH so a fast
     * model does not flood the 9x message queue with one message per chunk. */
    Str ev_text_buf;

    /* Markdown rendering state.  A trailing line without its newline is kept
     * back so a token split across two deltas is never half-rendered. */
    Str md_pending;
    int md_in_code;
} App;

static App g_app;
static WNDPROC g_input_oldproc;
static App *g_dlg_app;
static const char *g_confirm_tool;
static const char *g_confirm_args;

/* Scratch state for the generic single-line input dialog. */
static char g_inputline_title[96];
static char g_inputline_label[128];
static char g_inputline_result[A98_MAX_PATH];
static int g_inputline_ok;

typedef struct {
    const char *name;
    const char *base;
    const char *model;
} ProviderPreset;

static const ProviderPreset g_presets[] = {
    { "OpenAI",          "https://api.openai.com/v1",       "gpt-4o-mini" },
    { "DeepSeek",        "https://api.deepseek.com/v1",     "deepseek-chat" },
    { "Moonshot (Kimi)", "https://api.moonshot.cn/v1",      "moonshot-v1-8k" },
    { "SiliconFlow",     "https://api.siliconflow.cn/v1",   "Qwen/Qwen2.5-7B-Instruct" },
    { "Ollama (local)",  "http://127.0.0.1:11434/v1",       "llama3.2" },
    { "Custom",          "",                                "" }
};

#define PRESET_COUNT ((int)(sizeof(g_presets) / sizeof(g_presets[0])))

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK confirm_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK tools_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK firstrun_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static INT_PTR CALLBACK inputline_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static int input_line_box(HWND parent, const char *title, const char *label,
                          const char *initial, char *out, size_t cap);
static DWORD WINAPI worker_proc(void *param);
/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int base_matches(const char *a, const char *b)
{
    char ta[512];
    char tb[512];
    size_t la;
    size_t lb;

    copy_str(ta, sizeof(ta), a);
    copy_str(tb, sizeof(tb), b);
    config_trim(ta);
    config_trim(tb);
    la = strlen(ta);
    lb = strlen(tb);
    while (la > 0 && ta[la - 1] == '/') {
        ta[--la] = '\0';
    }
    while (lb > 0 && tb[lb - 1] == '/') {
        tb[--lb] = '\0';
    }
    return a98_strcasecmp(ta, tb) == 0;
}

static int preset_from_base(const char *base)
{
    int i;

    for (i = 0; i + 1 < PRESET_COUNT; i++) {
        if (base_matches(base, g_presets[i].base)) {
            return i;
        }
    }
    return PRESET_COUNT - 1;
}

static void center_window(HWND hwnd)
{
    RECT r;
    RECT ro;
    HWND owner;
    int w;
    int h;

    GetWindowRect(hwnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    owner = GetParent(hwnd);
    if (owner == NULL) {
        owner = GetDesktopWindow();
    }
    GetWindowRect(owner, &ro);
    MoveWindow(hwnd,
               ro.left + ((ro.right - ro.left) - w) / 2,
               ro.top + ((ro.bottom - ro.top) - h) / 2,
               w, h, FALSE);
}

static int msgbox_utf8(HWND owner, const char *utf8, UINT flags)
{
    char *acp = a98_utf8_to_acp(utf8 ? utf8 : "");
    int rc = MessageBoxA(owner, acp != NULL ? acp : "", "AIagent98", flags);

    a98_free(acp);
    return rc;
}

static char *window_text_utf8(HWND h)
{
    int len;
    char *buf;
    char *utf8;

    len = GetWindowTextLengthA(h);
    if (len < 0) {
        len = 0;
    }
    buf = (char *)a98_alloc((size_t)len + 1);
    if (buf == NULL) {
        return NULL;
    }
    GetWindowTextA(h, buf, len + 1);
    if (a98_utf8_valid(buf, strlen(buf))) {
        utf8 = a98_strdup(buf);
    } else {
        utf8 = a98_acp_to_utf8(buf);
    }
    a98_free(buf);
    return utf8;
}

static char *dlg_text_utf8(HWND dlg, int id)
{
    return window_text_utf8(GetDlgItem(dlg, id));
}

static void dlg_set_utf8(HWND dlg, int id, const char *utf8)
{
    char *acp = a98_utf8_to_acp(utf8 ? utf8 : "");

    SetDlgItemTextA(dlg, id, acp != NULL ? acp : "");
    a98_free(acp);
}

static char *absolute_path_utf8(const char *utf8)
{
    char *acp;
    char full[A98_MAX_PATH];
    DWORD n;

    acp = a98_utf8_to_acp(utf8 ? utf8 : "");
    if (acp == NULL) {
        return NULL;
    }
    n = GetFullPathNameA(acp, sizeof(full), full, NULL);
    a98_free(acp);
    if (n == 0 || n >= sizeof(full)) {
        return a98_strdup(utf8 ? utf8 : "");
    }
    return a98_acp_to_utf8(full);
}

/* ------------------------------------------------------------------ */
/* Transcript                                                          */
/* ------------------------------------------------------------------ */

static void transcript_apply_format(App *app, LONG start, LONG end,
                                    COLORREF color, int bold, COLORREF back)
{
    CHARRANGE cr;

    if (app == NULL || app->transcript == NULL || app->rich == 0) {
        return;
    }
    if (end < start) {
        LONG tmp = start;
        start = end;
        end = tmp;
    }
    cr.cpMin = start;
    cr.cpMax = end;
    SendMessageA(app->transcript, EM_EXSETSEL, 0, (LPARAM)&cr);

    if (app->rich == 2) {
        CHARFORMAT2A cf;
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_FACE | CFM_CHARSET;
        if (back != 0) {
            /* RichEdit 2.0 paints a background band; RichEdit 1.0 has no
             * CFM_BACKCOLOR and falls back to the colour-only variant. */
            cf.dwMask |= CFM_BACKCOLOR;
            cf.crBackColor = back;
        }
        cf.dwEffects = bold ? CFE_BOLD : 0;
        cf.crTextColor = color;
        cf.bCharSet = app->mono_charset;
        copy_str(cf.szFaceName, sizeof(cf.szFaceName), app->mono_face);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_SELECTION,
                     (LPARAM)&cf);
    } else {
        CHARFORMAT cf;
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_FACE | CFM_CHARSET;
        cf.dwEffects = bold ? CFE_BOLD : 0;
        if (back != 0 && color == COL_AI) {
            /* Stand-in for the background band on RichEdit 1.0. */
            cf.crTextColor = COL_CODE_FG;
        } else {
            cf.crTextColor = color;
        }
        cf.bCharSet = app->mono_charset;
        copy_str(cf.szFaceName, sizeof(cf.szFaceName), app->mono_face);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_SELECTION,
                     (LPARAM)&cf);
    }
}

/* True while the last visible line is the final line of the buffer, i.e.
 * the view is pinned to the bottom.  Used to stop the auto-scroll from
 * yanking the view back down while the user reads older output. */
static int transcript_at_bottom(App *app)
{
    HWND t;
    RECT rc;
    int total;
    int first;
    int vis;

    if (app == NULL || app->transcript == NULL || app->line_h <= 0) {
        return 1;
    }
    t = app->transcript;
    GetClientRect(t, &rc);
    vis = (rc.bottom - rc.top) / app->line_h;
    total = (int)SendMessageA(t, EM_GETLINECOUNT, 0, 0);
    if (app->rich == 0) {
        /* EDIT reports a line index directly. */
        first = (int)SendMessageA(t, EM_GETFIRSTVISIBLELINE, 0, 0);
    } else {
        /* RichEdit reports the first visible *character*; map it back. */
        LONG fc = (LONG)SendMessageA(t, EM_GETFIRSTVISIBLELINE, 0, 0);
        first = (int)SendMessageA(t, EM_EXLINEFROMCHAR, 0, fc);
        if (first < 0) {
            first = 0;
        }
    }
    return first + vis >= total;
}

/* Only follow the tail while the user has not scrolled away from it. */
static void transcript_scroll_after_append(App *app)
{
    if (app == NULL || app->transcript == NULL) {
        return;
    }
    if (app->stick_bottom) {
        SendMessageA(app->transcript, EM_SCROLLCARET, 0, 0);
    }
}

/* Does s[from..len) continue with a ":shortcode:" the pack knows?  The
 * transcript turns those into the pack's artwork, so this scan has to agree
 * with a98_emoji_fold() about what a token looks like. */
static int emoji_token_at(const char *s, size_t len, size_t from, size_t *a,
                          size_t *b, const A98EmojiEntry **out)
{
    size_t i;

    for (i = from; i < len; i++) {
        size_t j;
        size_t n;
        char name[A98_EMOJI_MAX_TOKEN + 1];
        const A98EmojiEntry *e;

        if (s[i] != ':') {
            continue;
        }
        j = i + 1;
        while (j < len && j - i - 1 < A98_EMOJI_MAX_TOKEN) {
            char c = s[j];

            if (c == '_' || c == '-' || c == '+' ||
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) {
                j++;
                continue;
            }
            break;
        }
        n = j - i - 1;
        if (n == 0 || j >= len || s[j] != ':') {
            continue;
        }
        memcpy(name, s + i + 1, n);
        name[n] = '\0';
        e = a98_emoji_by_shortcode(name);
        if (e != NULL) {
            *a = i;
            *b = j + 1;
            *out = e;
            return 1;
        }
    }
    return 0;
}

/* One plain text run, at the end of the transcript, in the given format. */
static void transcript_insert_run(App *app, const char *text, COLORREF color,
                                  int bold)
{
    CHARRANGE cr;
    LONG start;
    LONG end;

    /* Move to the end.  Whether the control collapses the selection or
     * selects everything, cpMax is the end of the text. */
    SendMessageA(app->transcript, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageA(app->transcript, EM_EXGETSEL, 0, (LPARAM)&cr);
    cr.cpMin = cr.cpMax;
    SendMessageA(app->transcript, EM_EXSETSEL, 0, (LPARAM)&cr);
    start = cr.cpMax;

    /* Give the insertion point the CJK-capable face and charset before
     * inserting.  RichEdit 2.0 on Windows 9x otherwise lets the first
     * character of a DBCS run keep the previous run's ANSI charset, so GDI
     * substitutes a font that has no Chinese glyphs and only that first
     * character comes out as a box. */
    transcript_apply_format(app, start, start, color, bold, 0);

    SendMessageA(app->transcript, EM_REPLACESEL, (WPARAM)FALSE,
                 (LPARAM)text);
    SendMessageA(app->transcript, EM_EXGETSEL, 0, (LPARAM)&cr);
    end = cr.cpMax;

    transcript_apply_format(app, start, end, color, bold, 0);

    cr.cpMin = end;
    cr.cpMax = end;
    SendMessageA(app->transcript, EM_EXSETSEL, 0, (LPARAM)&cr);
}

/* Append folded text, drawing every ":shortcode:" the pack knows as an
 * inline GDI bitmap through the self-drawn transcript view.  No RichEdit OLE
 * is involved, so this cannot trigger the Win9x RichEdit insert GPF.
 * With [emoji] inline=0 the shortcode stays as plain text. */
static void transcript_insert_folded(App *app, char *acp, COLORREF color,
                                     int bold)
{
    size_t len = strlen(acp);
    size_t i = 0;

    while (i < len) {
        size_t a = 0;
        size_t b = 0;
        const A98EmojiEntry *e = NULL;
        char keep;

        if (!app->emoji_pics || !emoji_token_at(acp, len, i, &a, &b, &e)) {
            transcript_insert_run(app, acp + i, color, bold);
            return;
        }
        if (a > i) {
            keep = acp[a];
            acp[a] = '\0';
            transcript_insert_run(app, acp + i, color, bold);
            acp[a] = keep;
        }
        /* The control owns the artwork cache and falls back to the shortcode
         * text by itself when the pack is missing, so just hand it the entry. */
        SendMessageA(app->transcript, A98TV_EMOJI, 0, (LPARAM)e);
        i = b;
    }
}

/* Append UTF-8 text, normalising LF to CRLF, and give the inserted range a
 * character format.  On plain EDIT controls the text is appended uncoloured. */
static void transcript_append_colored_bg(App *app, const char *utf8,
                                         COLORREF color, int bold,
                                         COLORREF back)
{
    Str s;
    size_t i;
    char *acp;

    if (app == NULL || app->transcript == NULL || utf8 == NULL) {
        return;
    }
    str_init(&s);
    for (i = 0; utf8[i] != '\0'; i++) {
        if (utf8[i] == '\r') {
            continue;
        }
        if (utf8[i] == '\n') {
            str_append_cstr(&s, "\r\n");
        } else {
            str_append_char(&s, utf8[i]);
        }
    }
    /* Windows 9x has no emoji font and the local code page cannot hold an
     * emoji code point, so fold emoji into ":shortcode:" before the ANSI
     * conversion; otherwise every one of them arrives as '?'. */
    {
        char *folded = a98_emoji_fold(str_cstr(&s));

        acp = a98_utf8_to_acp(folded != NULL ? folded : str_cstr(&s));
        a98_free(folded);
    }
    str_free(&s);
    if (acp == NULL) {
        return;
    }

    if (app->rich == 0) {
        int len = GetWindowTextLengthA(app->transcript);
        SendMessageA(app->transcript, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(app->transcript, EM_REPLACESEL, (WPARAM)FALSE,
                     (LPARAM)acp);
        transcript_scroll_after_append(app);
        a98_free(acp);
        return;
    }

    transcript_insert_folded(app, acp, color, bold);
    transcript_scroll_after_append(app);
    a98_free(acp);
}

static void transcript_append_colored(App *app, const char *utf8,
                                      COLORREF color, int bold)
{
    transcript_append_colored_bg(app, utf8, color, bold, 0);
}

static void transcript_append(App *app, const char *utf8)
{
    transcript_append_colored_bg(app, utf8, COL_AI, 0, 0);
}

static void transcript_line_colored(App *app, const char *prefix,
                                    COLORREF prefix_color, int prefix_bold,
                                    const char *text, COLORREF text_color,
                                    int text_bold)
{
    transcript_append_colored(app, prefix != NULL ? prefix : "",
                              prefix_color, prefix_bold);
    transcript_append_colored(app, text != NULL ? text : "",
                              text_color, text_bold);
    transcript_append_colored(app, "\r\n", text_color, 0);
}

static void transcript_line(App *app, const char *prefix, const char *text)
{
    transcript_line_colored(app, prefix, COL_AI, 0, text, COL_AI, 0);
}

/* ------------------------------------------------------------------ */
/* Lightweight markdown colouring: fenced code blocks get a grey band,
 * inline code magenta, **bold** bold.  Only colours change, never the
 * text itself.  A trailing line without its newline is buffered so a
 * token split across two stream deltas is never half-rendered.       */
/* ------------------------------------------------------------------ */

/* Format of the most recent segment md_emit() painted; the line
 * terminator inherits it so a fenced code band stays unbroken. */
typedef struct {
    COLORREF color;
    int bold;
    COLORREF back;
} MdFmt;

static void md_seg(App *app, const char *utf8, size_t from, size_t to,
                   COLORREF color, int bold, COLORREF back, MdFmt *fmt)
{
    Str s;

    if (to <= from) {
        return;
    }
    str_init(&s);
    str_append(&s, utf8 + from, to - from);
    if (s.len > 0) {
        transcript_append_colored_bg(app, str_cstr(&s), color, bold, back);
        if (fmt != NULL) {
            fmt->color = color;
            fmt->bold = bold;
            fmt->back = back;
        }
    }
    str_free(&s);
}

/* Paint one markdown line.  With newline != 0 a terminator is appended
 * afterwards, formatted like the last segment. */
static void md_emit(App *app, const char *utf8, int newline)
{
    size_t i, seg;
    const char *p;
    MdFmt fmt;

    if (app == NULL || utf8 == NULL) {
        return;
    }
    fmt.color = COL_AI;
    fmt.bold = 0;
    fmt.back = 0;
    p = utf8;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (app->md_in_code) {
        if (strncmp(p, "```", 3) == 0) {
            app->md_in_code = 0;
            transcript_append_colored(app, utf8, COL_ROUND, 0);
            fmt.color = COL_ROUND;
        } else {
            transcript_append_colored_bg(app, utf8, COL_CODE_FG, 0,
                                         COL_CODE_BG);
            fmt.color = COL_CODE_FG;
            fmt.back = COL_CODE_BG;
        }
    } else if (strncmp(p, "```", 3) == 0) {
        app->md_in_code = 1;
        transcript_append_colored(app, utf8, COL_ROUND, 0);
        fmt.color = COL_ROUND;
    } else {
        seg = 0;
        i = 0;
        while (utf8[i] != '\0') {
            if (utf8[i] == '`') {
                size_t j = i + 1;
                while (utf8[j] != '\0' && utf8[j] != '`') {
                    j++;
                }
                if (utf8[j] == '`' && j > i + 1) {
                    md_seg(app, utf8, seg, i, COL_AI, 0, 0, &fmt);
                    md_seg(app, utf8, i + 1, j, COL_INLINE, 0, 0, &fmt);
                    i = j + 1;
                    seg = i;
                    continue;
                }
                i = j;
                continue;
            }
            if (utf8[i] == '*' && utf8[i + 1] == '*') {
                size_t j = i + 2;
                while (utf8[j] != '\0' &&
                       !(utf8[j] == '*' && utf8[j + 1] == '*')) {
                    j++;
                }
                if (utf8[j] == '*' && j > i + 2) {
                    md_seg(app, utf8, seg, i, COL_AI, 0, 0, &fmt);
                    md_seg(app, utf8, i + 2, j, COL_AI, 1, 0, &fmt);
                    i = j + 2;
                    seg = i;
                    continue;
                }
                i += 2;
                continue;
            }
            i++;
        }
        md_seg(app, utf8, seg, strlen(utf8), COL_AI, 0, 0, &fmt);
    }
    if (newline) {
        transcript_append_colored_bg(app, "\r\n", fmt.color, fmt.bold,
                                     fmt.back);
    }
}

static void transcript_md_append(App *app, const char *utf8)
{
    Str all;
    const char *p;
    size_t len, i, start = 0;

    if (app == NULL) {
        return;
    }
    str_init(&all);
    str_append_cstr(&all, str_cstr(&app->md_pending));
    str_clear(&app->md_pending);
    str_append_cstr(&all, utf8 != NULL ? utf8 : "");
    p = str_cstr(&all);
    len = all.len;

    for (i = 0; i < len; i++) {
        if (p[i] != '\n') {
            continue;
        }
        {
            Str line;
            size_t k;

            str_init(&line);
            for (k = start; k < i; k++) {
                if (p[k] != '\r') {
                    str_append_char(&line, p[k]);
                }
            }
            md_emit(app, str_cstr(&line), 1);
            str_free(&line);
        }
        start = i + 1;
    }
    if (start < len) {
        str_append(&app->md_pending, p + start, len - start);
    }
    str_free(&all);
}

/* Show any half-received line (event boundary, end of reply, save,
 * export).  drop=1 throws the buffered fragment away instead (new/load,
 * where history is redrawn). */
static void md_reset(App *app, int flush)
{
    if (app == NULL) {
        return;
    }
    if (flush && app->md_pending.len > 0) {
        md_emit(app, str_cstr(&app->md_pending), 0);
        str_clear(&app->md_pending);
    }
    app->md_in_code = 0;
}

/* Drop the oldest lines once the buffer outgrows the soft cap: on a 9x
 * machine a multi-megabyte RichEdit buffer slows every append down.  The
 * complete history remains available in the session .json file. */
static void transcript_trim(App *app)
{
    int total;

    if (app == NULL || app->transcript == NULL) {
        return;
    }
    total = (int)SendMessageA(app->transcript, EM_GETLINECOUNT, 0, 0);
    if (total <= A98_TRANSCRIPT_MAX_LINES) {
        return;
    }
    SendMessageA(app->transcript, A98TV_TRIM, (WPARAM)A98_TRANSCRIPT_MAX_LINES, 0);
    /* A fenced block may have been cut in half; never leave the renderer
     * stuck in code mode, and drop the half line too. */
    app->md_in_code = 0;
    str_clear(&app->md_pending);
    transcript_line_colored(app, "\r\n[info] ", COL_ROUND, 0,
                            "older lines were trimmed to save memory; the "
                            "full history stays in the session file",
                            COL_ROUND, 0);
}

/* Paint everything buffered so far: the coalesced stream deltas go through
 * the markdown renderer, then the transcript is trimmed.  The timer flush
 * keeps a trailing line without its newline buffered, so a token split
 * across two deltas is never painted as two lines.  finish_line = 1 ends
 * the line: used at event boundaries (round/tool/info), at the end of a
 * reply and before a modal dialog, where the stream state is known. */
static void event_flush_ex(App *app, int finish_line)
{
    int painted = 0;

    if (app == NULL) {
        return;
    }
    if (app->hwnd != NULL) {
        KillTimer(app->hwnd, IDT_EVENT_FLUSH);
    }
    if (app->ev_text_buf.len > 0) {
        transcript_md_append(app, str_cstr(&app->ev_text_buf));
        str_clear(&app->ev_text_buf);
        painted = 1;
    }
    if (finish_line) {
        md_reset(app, 1);
        painted = 1;
    }
    if (painted) {
        transcript_trim(app);
    }
}

static void event_flush(App *app)
{
    event_flush_ex(app, 0);
}

static void transcript_set_default_format(App *app)
{
    if (app == NULL || app->transcript == NULL || app->rich == 0) {
        return;
    }
    if (app->rich == 2) {
        CHARFORMAT2A cf;
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET |
                    CFM_BOLD;
        cf.dwEffects = 0;
        cf.yHeight = A98_TRANSCRIPT_POINTS * A98_TWIPS_PER_POINT;
        cf.crTextColor = COL_AI;
        cf.bCharSet = app->mono_charset;
        copy_str(cf.szFaceName, sizeof(cf.szFaceName), app->mono_face);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_DEFAULT,
                     (LPARAM)&cf);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_ALL,
                     (LPARAM)&cf);
    } else {
        CHARFORMAT cf;
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET |
                    CFM_BOLD;
        cf.dwEffects = 0;
        cf.yHeight = A98_TRANSCRIPT_POINTS * A98_TWIPS_PER_POINT;
        cf.crTextColor = COL_AI;
        cf.bCharSet = app->mono_charset;
        copy_str(cf.szFaceName, sizeof(cf.szFaceName), app->mono_face);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_DEFAULT,
                     (LPARAM)&cf);
        SendMessageA(app->transcript, EM_SETCHARFORMAT, SCF_ALL,
                     (LPARAM)&cf);
    }
}

static void transcript_clear(App *app)
{
    if (app != NULL && app->transcript != NULL) {
        SetWindowTextA(app->transcript, "");
        transcript_set_default_format(app);
        app->md_in_code = 0;
        str_clear(&app->md_pending);
        app->stick_bottom = 1;
    }
}

static void render_tool_calls(App *app, const char *json)
{
    cJSON *arr;
    cJSON *item;

    if (json == NULL || json[0] == '\0') {
        return;
    }
    arr = cJSON_Parse(json);
    if (arr == NULL) {
        return;
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return;
    }
    cJSON_ArrayForEach(item, arr) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(item, "function");
        const char *name = NULL;
        const char *args = NULL;
        Str s;

        if (cJSON_IsObject(fn)) {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(fn, "name");
            cJSON *a = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
            if (cJSON_IsString(n) && n->valuestring != NULL) {
                name = n->valuestring;
            }
            if (cJSON_IsString(a) && a->valuestring != NULL) {
                args = a->valuestring;
            }
        }
        str_init(&s);
        str_appendf(&s, "%s(%s)", name != NULL ? name : "tool",
                    args != NULL ? args : "{}");
        transcript_line_colored(app, "[tool] ", COL_TOOL, 0,
                                str_cstr(&s), COL_TOOL, 0);
        str_free(&s);
    }
    cJSON_Delete(arr);
}

static void render_history(App *app)
{
    int i;
    int round = 0;

    if (app == NULL) {
        return;
    }
    transcript_clear(app);
    for (i = 0; i < app->agent.messages.count; i++) {
        Message *m = &app->agent.messages.items[i];
        if (m->role == MSG_USER) {
            transcript_line_colored(app, "You: ", COL_YOU, 1,
                                    m->content != NULL ? m->content : "",
                                    COL_AI, 0);
        } else if (m->role == MSG_ASSISTANT) {
            char roundbuf[64];

            round++;
            sprintf(roundbuf, "History (round %d)", round);
            transcript_line_colored(app, "\r\n[round] ", COL_ROUND, 0,
                                    roundbuf, COL_ROUND, 0);
            if (m->content != NULL && m->content[0] != '\0') {
                transcript_line_colored(app, "Assistant: ", COL_AI, 1,
                                        m->content, COL_AI, 0);
            }
            render_tool_calls(app, m->tool_calls_json);
        } else if (m->role == MSG_TOOL) {
            transcript_line_colored(app, "[result] ", COL_RESULT, 0,
                                    m->content != NULL ? m->content : "",
                                    COL_RESULT, 0);
        }
        transcript_append(app, "\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* Status bar and control state                                        */
/* ------------------------------------------------------------------ */

/* Most GUI strings are UTF-8, but a workspace path read straight from the
 * INI is raw ACP.  Accept either representation and convert as needed. */
static char *path_to_acp(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    if (a98_utf8_valid(s, strlen(s))) {
        return a98_utf8_to_acp(s);
    }
    return a98_strdup(s);
}

static char *path_to_utf8(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    if (a98_utf8_valid(s, strlen(s))) {
        return a98_strdup(s);
    }
    return a98_acp_to_utf8(s);
}

static void status_update(App *app)
{
    char buf[512];
    char tail[64];
    char *acp;
    static const char spin[] = "|/-\\";
    char c;

    if (app == NULL || app->statusbar == NULL) {
        return;
    }
    if (app->running) {
        c = spin[app->spin_frame & 3];
        snprintf(buf, sizeof(buf), "[ %c ] %s", c,
                 app->status_line[0] != '\0' ? app->status_line : "Running");
    } else {
        snprintf(buf, sizeof(buf), "[ - ] %s",
                 app->status_line[0] != '\0' ? app->status_line : "Ready");
    }
    acp = a98_utf8_to_acp(buf);
    SendMessageA(app->statusbar, SB_SETTEXT, 0, (LPARAM)(acp != NULL ? acp : ""));
    a98_free(acp);

    /* Conversation scale: the current round and how much text the view
     * holds.  A rough cue for "this session is getting long" long before
     * the context window fills up. */
    if (app->round_no > 0) {
        int kb = GetWindowTextLengthA(app->transcript);
        snprintf(tail, sizeof(tail), "Round %d  -  %d.%d k chars",
                 app->round_no, kb / 1024, (kb % 1024) * 10 / 1024);
    } else {
        copy_str(tail, sizeof(tail), "Round 0");
    }
    SendMessageA(app->statusbar, SB_SETTEXT, 1, (LPARAM)tail);

    acp = path_to_acp(app->cfg.workspace);
    snprintf(buf, sizeof(buf), "Workspace: %s", acp != NULL ? acp : "");
    SendMessageA(app->statusbar, SB_SETTEXT, 2, (LPARAM)buf);
    a98_free(acp);
}

static void status_set(App *app, const char *utf8)
{
    if (app == NULL) {
        return;
    }
    copy_str(app->status_line, sizeof(app->status_line),
             utf8 != NULL ? utf8 : "");
    status_update(app);
}

static void update_status(App *app)
{
    char title[256];
    char *m;

    if (app == NULL) {
        return;
    }
    if (!app->running && app->status_line[0] == '\0') {
        copy_str(app->status_line, sizeof(app->status_line), "Ready");
    }
    status_update(app);

    m = a98_utf8_to_acp(app->cfg.model);
    snprintf(title, sizeof(title), "AIagent98 - %s", m != NULL ? m : "");
    a98_free(m);
    SetWindowTextA(app->hwnd, title);
}

static void set_running(App *app, int running)
{
    if (app == NULL) {
        return;
    }
    app->running = running;
    EnableWindow(app->send, !running);
    EnableWindow(app->input, !running);
    EnableWindow(app->stop, running);
    EnableWindow(app->newbtn, !running);
    EnableWindow(app->settings, !running);
    EnableWindow(app->session_combo, !running);
    if (!running) {
        copy_str(app->status_line, sizeof(app->status_line), "Ready");
        status_update(app);
        SetFocus(app->input);
    }
}

/* ------------------------------------------------------------------ */
/* DOSKEY-style prompt history                                         */
/* ------------------------------------------------------------------ */

static void input_set_text(App *app, const char *utf8)
{
    char *acp;
    int len;

    if (app == NULL || app->input == NULL) {
        return;
    }
    acp = a98_utf8_to_acp(utf8 != NULL ? utf8 : "");
    SetWindowTextA(app->input, acp != NULL ? acp : "");
    a98_free(acp);
    len = GetWindowTextLengthA(app->input);
    SendMessageA(app->input, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

static void input_history_add(App *app, const char *utf8)
{
    int i;

    if (app == NULL || utf8 == NULL || utf8[0] == '\0') {
        return;
    }
    if (app->input_history_count > 0 &&
        strcmp(app->input_history[app->input_history_count - 1], utf8) == 0) {
        app->input_history_pos = app->input_history_count;
        return;
    }
    if (app->input_history_count >= A98_MAX_HISTORY) {
        a98_free(app->input_history[0]);
        for (i = 1; i < A98_MAX_HISTORY; i++) {
            app->input_history[i - 1] = app->input_history[i];
        }
        app->input_history_count--;
    }
    app->input_history[app->input_history_count] = a98_strdup(utf8);
    if (app->input_history[app->input_history_count] != NULL) {
        app->input_history_count++;
    }
    app->input_history_pos = app->input_history_count;
}

static void input_history_nav(App *app, int direction)
{
    if (app == NULL || app->input_history_count == 0) {
        return;
    }
    if (direction < 0) {
        if (app->input_history_pos > 0) {
            app->input_history_pos--;
        }
        input_set_text(app, app->input_history[app->input_history_pos]);
    } else {
        if (app->input_history_pos < app->input_history_count - 1) {
            app->input_history_pos++;
            input_set_text(app, app->input_history[app->input_history_pos]);
        } else {
            app->input_history_pos = app->input_history_count;
            input_set_text(app, "");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Session history                                                     */
/* ------------------------------------------------------------------ */

static void session_scan(App *app)
{
    char pattern[A98_MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i;
    int j;
    SessionEntry tmp;

    if (app == NULL) {
        return;
    }
    app->session_count = 0;
    a98_path_mkdirs(app->sessions_dir);
    if (a98_path_join(pattern, sizeof(pattern), app->sessions_dir,
                      "*.json") != 0) {
        return;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        size_t len;

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (app->session_count >= A98_MAX_SESSIONS) {
            break;
        }
        copy_str(app->sessions[app->session_count].name,
                 sizeof(app->sessions[app->session_count].name),
                 fd.cFileName);
        len = strlen(app->sessions[app->session_count].name);
        if (len > 5 &&
            a98_strcasecmp(app->sessions[app->session_count].name + len - 5,
                           ".json") == 0) {
            app->sessions[app->session_count].name[len - 5] = '\0';
        }
        if (a98_path_join(app->sessions[app->session_count].path,
                          sizeof(app->sessions[app->session_count].path),
                          app->sessions_dir, fd.cFileName) != 0) {
            /* A failed join would put an empty path into the list; selecting
             * that entry would be a no-op or worse.  Skip the file. */
            continue;
        }
        app->session_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Newest first: the filename starts with YYYYMMDD_HHMM. */
    for (i = 1; i < app->session_count; i++) {
        tmp = app->sessions[i];
        j = i - 1;
        while (j >= 0 && strcmp(app->sessions[j].name, tmp.name) < 0) {
            app->sessions[j + 1] = app->sessions[j];
            j--;
        }
        app->sessions[j + 1] = tmp;
    }
}

static void session_combo_fill(App *app, const char *select_path)
{
    int i;
    int sel = -1;

    if (app == NULL || app->session_combo == NULL) {
        return;
    }
    SendMessageA(app->session_combo, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < app->session_count; i++) {
        SendMessageA(app->session_combo, CB_ADDSTRING, 0,
                     (LPARAM)app->sessions[i].name);
        if (select_path != NULL &&
            strcmp(app->sessions[i].path, select_path) == 0) {
            sel = i;
        }
    }
    SendMessageA(app->session_combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void sanitize_session_summary(const char *acp, char *out, size_t cap)
{
    size_t o = 0;
    size_t i = 0;
    CPINFO cpi;
    int dbcs;

    if (out == NULL || cap == 0) {
        return;
    }
    /* Only pair bytes on DBCS code pages (GBK, Shift-JIS, ...).  On a
     * single-byte code page 0xE9 is a letter, not a lead byte. */
    dbcs = (GetCPInfo(GetACP(), &cpi) != 0 && cpi.MaxCharSize > 1);
    while (acp != NULL && acp[i] != '\0' && o + 1 < cap && o < 32) {
        unsigned char c = (unsigned char)acp[i];

        if (c < 0x20) {
            i++;
            continue;
        }
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (o > 0 && out[o - 1] != '_') {
                out[o++] = '_';
            }
            i++;
            continue;
        }
        if (c >= 0x81 && dbcs) {
            /* Keep DBCS pairs (GBK/Shift-JIS) intact. */
            if (acp[i + 1] == '\0' || o + 2 >= cap) {
                break;
            }
            out[o++] = acp[i++];
            out[o++] = acp[i++];
        } else {
            out[o++] = acp[i++];
        }
    }
    while (o > 0 && (out[o - 1] == '_' || out[o - 1] == ' ')) {
        o--;
    }
    out[o] = '\0';

    /* Avoid DOS reserved device names on Windows 9x */
    if (a98_strcasecmp(out, "CON") == 0 || a98_strcasecmp(out, "PRN") == 0 ||
        a98_strcasecmp(out, "AUX") == 0 || a98_strcasecmp(out, "NUL") == 0 ||
        a98_strcasecmp(out, "COM1") == 0 || a98_strcasecmp(out, "COM2") == 0 ||
        a98_strcasecmp(out, "COM3") == 0 || a98_strcasecmp(out, "COM4") == 0 ||
        a98_strcasecmp(out, "LPT1") == 0 || a98_strcasecmp(out, "LPT2") == 0 ||
        a98_strcasecmp(out, "LPT3") == 0) {
        copy_str(out, cap, "_session");
        return;
    }

    if (o == 0) {
        copy_str(out, cap, "session");
    }
}

static int build_session_path(App *app)
{
    const char *first = NULL;
    char *acp = NULL;
    char summary[64];
    char stamp[32];
    char name[128];
    char candidate[A98_MAX_PATH];
    SYSTEMTIME st;
    int i;

    if (app == NULL) {
        return -1;
    }
    for (i = 0; i < app->agent.messages.count; i++) {
        if (app->agent.messages.items[i].role == MSG_USER) {
            first = app->agent.messages.items[i].content;
            break;
        }
    }
    /* GetLocalTime() instead of localtime(): the latter returns a pointer to a
     * shared static buffer that the worker thread's logger also uses. */
    GetLocalTime(&st);
    sprintf(stamp, "%04d%02d%02d_%02d%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    acp = a98_utf8_to_acp(first != NULL ? first : "");
    sanitize_session_summary(acp, summary, sizeof(summary));
    a98_free(acp);

    snprintf(name, sizeof(name), "%s_%s.json", stamp, summary);
    a98_path_join(candidate, sizeof(candidate), app->sessions_dir, name);
    if (a98_path_exists(candidate) &&
        strcmp(candidate, app->session_path) != 0) {
        for (i = 2; i < 100; i++) {
            snprintf(name, sizeof(name), "%s_%s_%d.json", stamp, summary, i);
            a98_path_join(candidate, sizeof(candidate), app->sessions_dir,
                          name);
            if (!a98_path_exists(candidate)) {
                break;
            }
        }
        if (a98_path_exists(candidate)) {
            /* Every numbered name is taken.  Returning the last candidate
             * would silently overwrite an older session file; refuse
             * instead so auto-save just skips this round. */
            log_warn("auto-save: no free session name for %s", stamp);
            return -1;
        }
    }
    copy_str(app->session_path, sizeof(app->session_path), candidate);
    return 0;
}

static void auto_save_session(App *app)
{
    char err[512];

    if (app == NULL || app->agent.messages.count == 0) {
        return;
    }
    if (app->session_path[0] == '\0') {
        if (build_session_path(app) != 0) {
            return;
        }
    }
    err[0] = '\0';
    if (agent_save_session(&app->agent, app->session_path, err,
                           sizeof(err)) != 0) {
        log_warn("GUI auto-save failed: %s", err);
        return;
    }
    session_scan(app);
    session_combo_fill(app, app->session_path);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static void gui_on_event(void *ctx, AgentEventType type, const char *text)
{
    App *app = (App *)ctx;
    UiEvent *ev;

    if (app == NULL || app->hwnd == NULL) {
        return;
    }
    ev = (UiEvent *)a98_alloc(sizeof(*ev));
    if (ev == NULL) {
        return;
    }
    ev->type = (int)type;
    ev->text = a98_strdup(text != NULL ? text : "");
    if (ev->text == NULL) {
        a98_free(ev);
        return;
    }
    if (!PostMessageA(app->hwnd, A98_WM_EVENT, 0, (LPARAM)ev)) {
        a98_free(ev->text);
        a98_free(ev);
    }
}

static int gui_confirm(void *ctx, const char *tool, const char *args_json)
{
    App *app = (App *)ctx;
    ConfirmReq req;

    if (app == NULL) {
        return 0;
    }
    memset(&req, 0, sizeof(req));
    copy_str(req.tool, sizeof(req.tool), tool != NULL ? tool : "");
    req.args = a98_strdup(args_json != NULL ? args_json : "{}");
    if (req.args == NULL) {
        return 0;
    }
    req.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (req.event == NULL) {
        a98_free(req.args);
        return 0;
    }
    if (!PostMessageA(app->hwnd, A98_WM_CONFIRM, 0, (LPARAM)&req)) {
        CloseHandle(req.event);
        a98_free(req.args);
        return 0;
    }
    /* The UI thread keeps using `req` (a stack frame of this thread) and
     * req.args until it signals this event, so the wait must never be
     * abandoned while the window is alive.  But the wait may not be infinite:
     * only the UI thread can signal the event, and it stops pumping messages
     * during WM_DESTROY -- a plain INFINITE wait there hung the worker forever
     * and turned the 3 s teardown timeout into a deadlock.  The shutdown event
     * is set first in WM_DESTROY, so a confirm that will never be dispatched
     * (window gone) wakes up here and is denied. */
    {
        HANDLE waits[2];
        DWORD w;

        waits[0] = req.event;
        waits[1] = app->shutdown_event;
        for (;;) {
            w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                break;              /* answered by the UI thread */
            }
            if (w == WAIT_OBJECT_0 + 1) {
                req.result = 0;     /* teardown: deny and move on */
                break;
            }
            /* WAIT_FAILED (event handle lost) or an unexpected code: deny
             * rather than hang.  The UI thread can no longer be inside the
             * dialog here -- it shares this thread's view of app->cancel. */
            req.result = 0;
            break;
        }
    }
    CloseHandle(req.event);
    a98_free(req.args);
    return req.result;
}

static DWORD WINAPI worker_proc(void *param)
{
    App *app = (App *)param;
    char err[512];
    int rc;
    char *msg;

    err[0] = '\0';
    rc = agent_run(&app->agent, app->pending_input, err, sizeof(err));
    a98_free(app->pending_input);
    app->pending_input = NULL;

    msg = a98_strdup(err);
    if (!PostMessageA(app->hwnd, A98_WM_DONE, (WPARAM)(rc == 0),
                      (LPARAM)msg)) {
        a98_free(msg);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

/* Shared tail of do_send / do_resend: display the message, remember it for
 * Ctrl+R, then start the worker thread.  `text` is owned by the caller. */
static void start_request(App *app, const char *text)
{
    DWORD tid;

    transcript_append(app, "\r\n");
    transcript_line_colored(app, "You: ", COL_YOU, 1, text, COL_AI, 0);
    transcript_append(app, "\r\n");

    a98_free(app->last_input);
    app->last_input = a98_strdup(text);

    app->pending_input = a98_strdup(text);
    if (app->pending_input == NULL) {
        msgbox_utf8(app->hwnd, "Out of memory.", MB_OK | MB_ICONERROR);
        return;
    }
    app->cancel = 0;
    app->closing = 0;
    app->round_no = 0;
    app->spin_frame = 0;
    set_running(app, 1);
    status_set(app, "Connecting...");

#ifdef __WATCOMC__
    {
        unsigned crt_tid = 0;
        unsigned long h = _beginthreadex(NULL, 0,
                                         (unsigned (__stdcall *)(void *))
                                         worker_proc,
                                         app, 0, &crt_tid);
        app->thread = (HANDLE)h;
        tid = (DWORD)crt_tid;
    }
#else
    app->thread = CreateThread(NULL, 0, worker_proc, app, 0, &tid);
#endif
    if (app->thread == NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot start the worker thread (error %lu).",
                 (unsigned long)GetLastError());
        a98_free(app->pending_input);
        app->pending_input = NULL;
        set_running(app, 0);
        update_status(app);
        msgbox_utf8(app->hwnd, buf, MB_OK | MB_ICONERROR);
        return;
    }
    app->thread_id = tid;
}

static void do_send(App *app)
{
    char *text;
    char *trimmed;

    if (app == NULL || app->running) {
        return;
    }
    text = window_text_utf8(app->input);
    if (text == NULL) {
        return;
    }
    trimmed = text;
    while (*trimmed == ' ' || *trimmed == '\t' ||
           *trimmed == '\r' || *trimmed == '\n') {
        trimmed++;
    }
    if (trimmed != text) {
        memmove(text, trimmed, strlen(trimmed) + 1);
    }
    {
        size_t n = strlen(text);
        while (n > 0) {
            char c = text[n - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                text[--n] = '\0';
            } else {
                break;
            }
        }
    }
    if (text[0] == '\0') {
        a98_free(text);
        return;
    }
    if (app->cfg.api_key[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No API key is configured.\n\n"
                    "Open Tools > Settings and enter one.",
                    MB_OK | MB_ICONWARNING);
        a98_free(text);
        return;
    }

    {
        /* ":shortcode:" from the picker (or typed by hand) goes to the model
         * as the real emoji; the transcript folds it back for display, and
         * the prompt history keeps the readable form. */
        char *display = a98_emoji_fold(text);
        char *expanded = a98_emoji_expand(text);

        if (expanded != NULL) {
            a98_free(text);
            text = expanded;
        }
        input_history_add(app, display != NULL ? display : text);
        a98_free(display);
    }
    SetWindowTextA(app->input, "");
    app->input_history_pos = app->input_history_count;
    start_request(app, text);
    a98_free(text);
}

/* Tools > Insert emoji (Ctrl+J) and the Emoji button: open the picker and
 * drop its ":shortcode:" at the caret. */
static void do_emoji_pick(App *app)
{
    char token[160];

    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!a98_emoji_pick(app->hinst, app->hwnd, app->exe_dir, token,
                        sizeof(token))) {
        return;
    }
    /* EM_REPLACESEL inserts at the caret and replaces a selection, which is
     * exactly how typing an emoji should behave.  The token is plain ASCII,
     * so no code page conversion is needed. */
    SendMessageA(app->input, EM_REPLACESEL, (WPARAM)TRUE, (LPARAM)token);
    SetFocus(app->input);
}

/* Ctrl+R / File > Resend last message. */
static void do_resend(App *app)
{
    if (app == NULL || app->running) {
        return;
    }
    if (app->last_input == NULL || app->last_input[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "Nothing to resend yet: send a message first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (app->cfg.api_key[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No API key is configured.\n\n"
                    "Open Tools > Settings and enter one.",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    start_request(app, app->last_input);
}

static void do_stop(App *app)
{
    if (app == NULL || !app->running) {
        return;
    }
    app->cancel = 1;
    status_set(app, "Cancelling...");
}

static void do_new(App *app)
{
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    agent_new_conversation(&app->agent);
    app->session_path[0] = '\0';
    app->round_no = 0;
    app->spin_frame = 0;
    transcript_clear(app);
    session_combo_fill(app, NULL);
    status_set(app, "Ready");
    update_status(app);
}

static int choose_session_file(App *app, int save, char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    char file[A98_MAX_PATH];

    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    file[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter =
        "AIagent98 sessions (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = save ? "Save session" : "Load session";
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) {
        copy_str(out, cap, file);
        return 1;
    }
    return 0;
}

static void do_save(App *app)
{
    char path[A98_MAX_PATH];
    char err[512];

    md_reset(app, 1);
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!choose_session_file(app, 1, path, sizeof(path))) {
        return;
    }
    err[0] = '\0';
    if (agent_save_session(&app->agent, path, err, sizeof(err)) == 0) {
        char *u = a98_acp_to_utf8(path);
        copy_str(app->session_path, sizeof(app->session_path), path);
        session_scan(app);
        session_combo_fill(app, app->session_path);
        transcript_line_colored(app, "\r\n[info] Session saved: ",
                                COL_ROUND, 0, u != NULL ? u : "",
                                COL_ROUND, 0);
        a98_free(u);
        status_set(app, "Session saved");
    } else {
        char *u = a98_acp_to_utf8(err[0] ? err : "unknown error");
        transcript_line_colored(app, "\r\n[error] Save failed: ",
                                COL_ERROR, 1, u != NULL ? u : "",
                                COL_ERROR, 0);
        a98_free(u);
    }
}

static void do_load(App *app)
{
    char path[A98_MAX_PATH];
    char err[512];

    md_reset(app, 1);
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!choose_session_file(app, 0, path, sizeof(path))) {
        return;
    }
    err[0] = '\0';
    if (agent_load_session(&app->agent, path, err, sizeof(err)) == 0) {
        char *u = a98_acp_to_utf8(path);
        copy_str(app->session_path, sizeof(app->session_path), path);
        render_history(app);
        session_scan(app);
        session_combo_fill(app, app->session_path);
        transcript_line_colored(app, "\r\n[info] Session loaded: ",
                                COL_ROUND, 0, u != NULL ? u : "",
                                COL_ROUND, 0);
        a98_free(u);
        status_set(app, "Session loaded");
    } else {
        char *u = a98_acp_to_utf8(err[0] ? err : "unknown error");
        transcript_line_colored(app, "\r\n[error] Load failed: ",
                                COL_ERROR, 1, u != NULL ? u : "",
                                COL_ERROR, 0);
        a98_free(u);
    }
}

static void do_session_select(App *app, int idx)
{
    char err[512];
    char *u;

    md_reset(app, 1);
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        session_combo_fill(app, app->session_path);
        return;
    }
    if (idx < 0 || idx >= app->session_count) {
        return;
    }
    if (strcmp(app->sessions[idx].path, app->session_path) == 0) {
        return;
    }
    err[0] = '\0';
    if (agent_load_session(&app->agent, app->sessions[idx].path, err,
                           sizeof(err)) == 0) {
        u = a98_acp_to_utf8(app->sessions[idx].name);
        copy_str(app->session_path, sizeof(app->session_path),
                 app->sessions[idx].path);
        render_history(app);
        transcript_line_colored(app, "\r\n[info] Loaded session: ",
                                COL_ROUND, 0, u != NULL ? u : "",
                                COL_ROUND, 0);
        a98_free(u);
        status_set(app, "Session loaded");
    } else {
        u = a98_acp_to_utf8(err[0] ? err : "unknown error");
        transcript_line_colored(app, "\r\n[error] Load failed: ",
                                COL_ERROR, 1, u != NULL ? u : "",
                                COL_ERROR, 0);
        a98_free(u);
        session_combo_fill(app, app->session_path);
    }
}

/* ------------------------------------------------------------------ */
/* Session maintenance: rename / delete the current .json              */
/* ------------------------------------------------------------------ */

static int name_has_bad_chars(const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        switch (s[i]) {
        case '\\': case '/': case ':': case '*': case '?': case '"':
        case '<': case '>': case '|':
            return 1;
        }
    }
    return 0;
}

static void do_session_rename(App *app)
{
    char name[80];
    char newname[80];
    char newpath[A98_MAX_PATH];
    const char *fn;
    DWORD attrs;

    md_reset(app, 1);
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (app->session_path[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No session file to rename yet.\n\n"
                    "Send a message first (it auto-saves), or use "
                    "File > Save session.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    /* Suggest the current file name, e.g. 20261115_1510_summary.json. */
    fn = strrchr(app->session_path, '\\');
    fn = (fn != NULL) ? fn + 1 : app->session_path;
    copy_str(name, sizeof(name), fn);
    if (!input_line_box(app->hwnd, "Rename session",
                        "New file name:", name,
                        newname, sizeof(newname))) {
        return;
    }
    {
        size_t n = strlen(newname);
        while (n > 0 && (newname[n - 1] == ' ' || newname[n - 1] == '\t')) {
            newname[--n] = '\0';
        }
    }
    if (newname[0] == '\0') {
        return;
    }
    if (name_has_bad_chars(newname)) {
        msgbox_utf8(app->hwnd,
                    "The name contains characters that are not allowed "
                    "in file names (\\ / : * ? \" < > |).",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (strlen(newname) < 5 ||
        a98_strcasecmp(newname + strlen(newname) - 5, ".json") != 0) {
        size_t n = strlen(newname);
        if (n + 5 >= sizeof(newname)) {
            msgbox_utf8(app->hwnd, "The name is too long.",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        strcat(newname, ".json");
    }
    a98_path_join(newpath, sizeof(newpath), app->sessions_dir, newname);
    attrs = GetFileAttributesA(newpath);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        msgbox_utf8(app->hwnd, "A session with that name already exists.",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (!MoveFileA(app->session_path, newpath)) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Cannot rename the file (error %lu).",
                 (unsigned long)GetLastError());
        msgbox_utf8(app->hwnd, msg, MB_OK | MB_ICONERROR);
        return;
    }
    copy_str(app->session_path, sizeof(app->session_path), newpath);
    session_scan(app);
    session_combo_fill(app, app->session_path);
    status_set(app, "Session renamed");
}

static void do_session_delete(App *app)
{
    char msg[600];
    char *u;

    md_reset(app, 1);
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (app->session_path[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No session file to delete yet.\n\n"
                    "The file only appears once a message has been sent "
                    "(auto-save) or after File > Save session.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    u = a98_acp_to_utf8(app->session_path);
    snprintf(msg, sizeof(msg), "Delete this session file?\n\n%s",
             u != NULL ? u : app->session_path);
    a98_free(u);
    if (msgbox_utf8(app->hwnd, msg,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    DeleteFileA(app->session_path);
    /* The in-memory conversation stays; only the file is gone. */
    app->session_path[0] = '\0';
    session_scan(app);
    session_combo_fill(app, NULL);
    status_set(app, "Session deleted");
}

static void do_export(App *app)
{
    OPENFILENAMEA ofn;
    char file[A98_MAX_PATH];
    char *buf;
    int len;
    int cap;
    FILE *f;
    Str s;
    size_t i;

    md_reset(app, 1);
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    file[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter =
        "Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = "Export transcript";
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&ofn)) {
        return;
    }

    len = GetWindowTextLengthA(app->transcript);
    if (len < 0) {
        len = 0;
    }
    /* RichEdit stores paragraph breaks as CR but WM_GETTEXT expands them to
     * CRLF, so the returned text can be up to twice as long as the length
     * reported by WM_GETTEXTLENGTH. */
    if ((size_t)len > 0x3FFFFFFFU) {
        cap = 0x7FFFFFFE;
    } else {
        cap = len * 2 + 1;
    }
    buf = (char *)a98_alloc((size_t)cap + 1);
    if (buf == NULL) {
        msgbox_utf8(app->hwnd, "Out of memory.", MB_OK | MB_ICONERROR);
        return;
    }
    GetWindowTextA(app->transcript, buf, cap);

    /* The transcript control already holds CP_ACP text.  Normalise the
     * paragraph separators to CRLF for Win9x Notepad. */
    str_init(&s);
    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\r') {
            if (buf[i + 1] == '\n') {
                i++;
            }
            str_append_cstr(&s, "\r\n");
        } else if (buf[i] == '\n') {
            str_append_cstr(&s, "\r\n");
        } else {
            str_append_char(&s, buf[i]);
        }
    }
    a98_free(buf);

    f = fopen(file, "wb");
    if (f == NULL) {
        char *u = a98_acp_to_utf8(file);
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot write:\n%s",
                 u != NULL ? u : file);
        a98_free(u);
        str_free(&s);
        msgbox_utf8(app->hwnd, msg, MB_OK | MB_ICONERROR);
        return;
    }
    if (s.len > 0) {
        fwrite(str_cstr(&s), 1, s.len, f);
    }
    fclose(f);
    str_free(&s);

    {
        char *u = a98_acp_to_utf8(file);
        transcript_line_colored(app, "\r\n[info] Transcript exported: ",
                                COL_ROUND, 0, u != NULL ? u : "",
                                COL_ROUND, 0);
        a98_free(u);
    }
    status_set(app, "Transcript exported");
}

static void do_open_workspace(App *app)
{
    char *acp;
    HINSTANCE r;

    if (app == NULL) {
        return;
    }
    acp = path_to_acp(app->cfg.workspace);
    if (acp == NULL) {
        return;
    }
    a98_path_mkdirs(acp);
    r = ShellExecuteA(app->hwnd, "open", acp, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        char msg[512];
        char *u = path_to_utf8(app->cfg.workspace);
        snprintf(msg, sizeof(msg), "Cannot open the workspace:\n%s",
                 u != NULL ? u : "");
        msgbox_utf8(app->hwnd, msg, MB_OK | MB_ICONERROR);
        a98_free(u);
    }
    a98_free(acp);
}

static void settings_test_connection(HWND dlg, App *app)
{
    Config tmpcfg;
    char *base;
    char *key;
    char *extra;
    char *headers;
    char urlbuf[900];
    char err[512];
    char result[320];
    Url url;
    HttpResponse resp;
    NetConn *conn;
    size_t len;
    int verify;
    int rc;

    (void)app;
    base = dlg_text_utf8(dlg, IDC_API_BASE);
    key = dlg_text_utf8(dlg, IDC_API_KEY);
    extra = dlg_text_utf8(dlg, IDC_EXTRA_HEADERS);
    if (base == NULL || key == NULL || extra == NULL) {
        a98_free(base);
        a98_free(key);
        a98_free(extra);
        msgbox_utf8(dlg, "Out of memory.", MB_OK | MB_ICONERROR);
        return;
    }
    config_trim(base);
    config_trim(key);
    config_trim(extra);
    if (base[0] == '\0') {
        dlg_set_utf8(dlg, IDC_TEST_RESULT,
                     "Enter an API base URL first.");
        goto done;
    }

    len = strlen(base);
    if (len >= sizeof(urlbuf)) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "API base URL is too long.");
        goto done;
    }
    memcpy(urlbuf, base, len + 1);
    {
        static const char cc[] = "/chat/completions";
        size_t cl = sizeof(cc) - 1;
        if (len >= cl && a98_strcasecmp(urlbuf + len - cl, cc) == 0) {
            len -= cl;
            urlbuf[len] = '\0';
        }
    }
    while (len > 0 && urlbuf[len - 1] == '/') {
        urlbuf[--len] = '\0';
    }
    if (len + 8 > sizeof(urlbuf)) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "API base URL is too long.");
        goto done;
    }
    strcat(urlbuf, "/models");

    err[0] = '\0';
    if (url_parse(urlbuf, &url, err, sizeof(err)) != 0) {
        snprintf(result, sizeof(result), "Invalid URL: %s", err);
        dlg_set_utf8(dlg, IDC_TEST_RESULT, result);
        goto done;
    }

    memset(&tmpcfg, 0, sizeof(tmpcfg));
    copy_str(tmpcfg.api_key, sizeof(tmpcfg.api_key), key);
    copy_str(tmpcfg.extra_headers, sizeof(tmpcfg.extra_headers), extra);
    headers = api_build_headers(&tmpcfg, 0);
    if (headers == NULL) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "Out of memory.");
        goto done;
    }

    verify = IsDlgButtonChecked(dlg, IDC_TLS_VERIFY) == BST_CHECKED;
    dlg_set_utf8(dlg, IDC_TEST_RESULT, "Testing connection...");
    UpdateWindow(dlg);
    SetCursor(LoadCursorA(NULL, IDC_WAIT));

    /* This runs on the UI thread, so keep the worst case short. */
    conn = net_open(&url, verify, 8, 12, err, sizeof(err));
    if (conn == NULL) {
        snprintf(result, sizeof(result), "Failed: %s",
                 err[0] != '\0' ? err : "cannot connect");
    } else {
        memset(&resp, 0, sizeof(resp));
        rc = http_request(conn, "GET", &url, headers, NULL, 0, &resp,
                          NULL, NULL, err, sizeof(err));
        net_close(conn);
        if (rc != 0) {
            snprintf(result, sizeof(result), "Failed: %s",
                     err[0] != '\0' ? err : "request failed");
        } else if (resp.status == 200) {
            strcpy(result, "Connected successfully (HTTP 200).");
        } else if (resp.status == 401 || resp.status == 403) {
            snprintf(result, sizeof(result),
                     "Authentication failed (HTTP %d). Check the API key.",
                     resp.status);
        } else if (resp.status == 404) {
            strcpy(result,
                   "Endpoint not found (HTTP 404). Check the API base URL.");
        } else if (resp.status == 429) {
            strcpy(result, "Reachable, but rate limited (HTTP 429).");
        } else if (resp.status >= 500) {
            snprintf(result, sizeof(result),
                     "Server error (HTTP %d). The endpoint is reachable.",
                     resp.status);
        } else {
            snprintf(result, sizeof(result), "HTTP %d %s", resp.status,
                     resp.reason);
        }
    }
    a98_free(headers);
    SetCursor(LoadCursorA(NULL, IDC_ARROW));
    dlg_set_utf8(dlg, IDC_TEST_RESULT, result);

done:
    a98_free(base);
    a98_free(key);
    a98_free(extra);
}

static void do_settings(App *app)
{
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_dlg_app = app;
    DialogBoxParamA(app->hinst, MAKEINTRESOURCEA(IDD_SETTINGS),
                    app->hwnd, settings_proc, 0);
    g_dlg_app = NULL;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
/* ------------------------------------------------------------------ */

/* Show only the controls belonging to the selected settings tab. */
static void settings_show_tab(HWND dlg, int sel)
{
    static const int page[3][24] = {
        { IDC_L_PROVIDER, IDC_PROVIDER, IDC_L_PRESET, IDC_L_API_BASE,
          IDC_API_BASE, IDC_L_API_KEY, IDC_API_KEY, IDC_L_MODEL, IDC_MODEL,
          IDC_L_PROMPT, IDC_SYSTEM_PROMPT, IDC_L_TEMP, IDC_TEMPERATURE,
          IDC_L_MAXTOK, IDC_MAX_TOKENS, IDC_L_MAXSTEP, IDC_MAX_STEPS,
          IDC_L_HEADERS, IDC_EXTRA_HEADERS, IDC_STREAM, IDC_TEST,
          IDC_TEST_RESULT, 0 },
        { IDC_L_WORKSPACE, IDC_WORKSPACE, IDC_ALLOW_SHELL, IDC_ALLOW_OUTSIDE,
          IDC_TLS_VERIFY, IDC_EMOJI_INLINE, IDC_L_CONNTO, IDC_CONNECT_TIMEOUT,
          IDC_L_REQTO, IDC_REQUEST_TIMEOUT, IDC_L_LOG, IDC_LOG_LEVEL,
          IDC_FULL_ACCESS, IDC_AUTO_CONFIRM, IDC_L_WARN1, IDC_L_WARN2, 0 },
        { IDC_NET_ALLOW, IDC_L_HOSTS, IDC_NET_ALLOW_HOSTS, IDC_L_MAXBYTES,
          IDC_NET_MAX_BYTES, IDC_L_SEARCHURL, IDC_NET_SEARCH_URL, IDC_L_TOKEN,
          IDC_NET_GITHUB_TOKEN, IDC_L_NET1, IDC_L_NET2, IDC_L_NET3, 0 }
    };
    int t, i;
    if (sel < 0) sel = 0;
    if (sel > 2) sel = 2;
    for (t = 0; t < 3; t++) {
        for (i = 0; page[t][i] != 0; i++) {
            HWND c = GetDlgItem(dlg, page[t][i]);
            if (c != NULL) ShowWindow(c, t == sel ? SW_SHOW : SW_HIDE);
        }
    }
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    App *app = g_dlg_app;

    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int i;
        int idx;
        char tmp[64];
        HWND tab;

        if (app == NULL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        tab = GetDlgItem(dlg, IDC_SETTINGS_TAB);
        if (tab != NULL) {
            TC_ITEM ti;
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = "API & Model";
            TabCtrl_InsertItem(tab, 0, &ti);
            ti.pszText = "Agent";
            TabCtrl_InsertItem(tab, 1, &ti);
            ti.pszText = "Network";
            TabCtrl_InsertItem(tab, 2, &ti);
            TabCtrl_SetCurSel(tab, 0);
        }
        settings_show_tab(dlg, 0);
        for (i = 0; i < PRESET_COUNT; i++) {
            SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_ADDSTRING, 0,
                                (LPARAM)g_presets[i].name);
        }
        idx = preset_from_base(app->cfg.api_base);
        SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_SETCURSEL, (WPARAM)idx, 0);
        dlg_set_utf8(dlg, IDC_API_BASE, app->cfg.api_base);
        dlg_set_utf8(dlg, IDC_API_KEY, app->cfg.api_key);
        dlg_set_utf8(dlg, IDC_MODEL, app->cfg.model);
        dlg_set_utf8(dlg, IDC_SYSTEM_PROMPT, app->cfg.system_prompt);
        dlg_set_utf8(dlg, IDC_EXTRA_HEADERS, app->cfg.extra_headers);
        sprintf(tmp, "%.2f", app->cfg.temperature);
        SetDlgItemTextA(dlg, IDC_TEMPERATURE, tmp);
        /* cfg.workspace may be raw ACP when it came from the INI; show it in
         * UTF-8 like every other dialog string. */
        {
            char *u = path_to_utf8(app->cfg.workspace);
            dlg_set_utf8(dlg, IDC_WORKSPACE, u != NULL ? u : "");
            a98_free(u);
        }
        SetDlgItemInt(dlg, IDC_MAX_STEPS, (UINT)app->cfg.max_steps, FALSE);
        SetDlgItemInt(dlg, IDC_MAX_TOKENS, (UINT)app->cfg.max_tokens, FALSE);
        CheckDlgButton(dlg, IDC_TLS_VERIFY,
                       app->cfg.tls_verify ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ALLOW_SHELL,
                       app->cfg.allow_shell ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_FULL_ACCESS,
                       app->cfg.full_access ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_AUTO_CONFIRM,
                       app->cfg.auto_confirm ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ALLOW_OUTSIDE,
                       app->cfg.allow_outside ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_EMOJI_INLINE,
                       app->cfg.emoji_inline ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NET_ALLOW,
                       app->cfg.net_allow ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_STREAM,
                       app->cfg.stream ? BST_CHECKED : BST_UNCHECKED);
        dlg_set_utf8(dlg, IDC_NET_ALLOW_HOSTS, app->cfg.net_allow_hosts);
        dlg_set_utf8(dlg, IDC_NET_SEARCH_URL, app->cfg.net_search_url);
        dlg_set_utf8(dlg, IDC_NET_GITHUB_TOKEN, app->cfg.net_github_token);
        sprintf(tmp, "%d", app->cfg.connect_timeout_sec);
        SetDlgItemTextA(dlg, IDC_CONNECT_TIMEOUT, tmp);
        sprintf(tmp, "%d", app->cfg.request_timeout_sec);
        SetDlgItemTextA(dlg, IDC_REQUEST_TIMEOUT, tmp);
        sprintf(tmp, "%d", app->cfg.log_level);
        SetDlgItemTextA(dlg, IDC_LOG_LEVEL, tmp);
        sprintf(tmp, "%d", app->cfg.net_max_bytes);
        SetDlgItemTextA(dlg, IDC_NET_MAX_BYTES, tmp);
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "");
        center_window(dlg);
        return TRUE;
    }

    case WM_NOTIFY:
        if (lp != 0 && ((NMHDR *)lp)->idFrom == IDC_SETTINGS_TAB &&
            ((NMHDR *)lp)->code == TCN_SELCHANGE) {
            HWND tab = GetDlgItem(dlg, IDC_SETTINGS_TAB);
            settings_show_tab(dlg, tab != NULL ? TabCtrl_GetCurSel(tab) : 0);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PROVIDER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendDlgItemMessageA(dlg, IDC_PROVIDER,
                                                   CB_GETCURSEL, 0, 0);
                if (idx >= 0 && idx < PRESET_COUNT - 1) {
                    dlg_set_utf8(dlg, IDC_API_BASE, g_presets[idx].base);
                    dlg_set_utf8(dlg, IDC_MODEL, g_presets[idx].model);
                    dlg_set_utf8(dlg, IDC_TEST_RESULT, "");
                }
            }
            return TRUE;
        case IDC_TEST:
            settings_test_connection(dlg, app);
            return TRUE;
        case IDC_FULL_ACCESS:
            if (HIWORD(wp) == BN_CLICKED &&
                IsDlgButtonChecked(dlg, IDC_FULL_ACCESS) == BST_CHECKED) {
                msgbox_utf8(dlg,
                    "DANGER: full system access removes the workspace sandbox\n"
                    "and the shell command ban list, and turns on auto-confirm.\n\n"
                    "The model will be able to read/write ANY file and run ANY\n"
                    "COMMAND.COM command without asking, including destructive\n"
                    "ones. Only enable this on a disposable VM.",
                    MB_OK | MB_ICONWARNING);
            }
            return TRUE;
        case IDC_AUTO_CONFIRM:
            if (HIWORD(wp) == BN_CLICKED &&
                IsDlgButtonChecked(dlg, IDC_AUTO_CONFIRM) == BST_CHECKED) {
                msgbox_utf8(dlg,
                    "Auto-confirm runs every tool call without showing the\n"
                    "confirmation dialog, including writes and shell commands.\n"
                    "Only enable this when you fully trust the model.",
                    MB_OK | MB_ICONWARNING);
            }
            return TRUE;
        case IDOK: {
            char *base;
            char *key;
            char *model;
            char *workspace;
            char *prompt;
            char *headers;
            char *temp;
            char *abs_ws;
            BOOL ok;
            double t;

            base = dlg_text_utf8(dlg, IDC_API_BASE);
            key = dlg_text_utf8(dlg, IDC_API_KEY);
            model = dlg_text_utf8(dlg, IDC_MODEL);
            workspace = dlg_text_utf8(dlg, IDC_WORKSPACE);
            prompt = dlg_text_utf8(dlg, IDC_SYSTEM_PROMPT);
            headers = dlg_text_utf8(dlg, IDC_EXTRA_HEADERS);
            temp = dlg_text_utf8(dlg, IDC_TEMPERATURE);
            if (base == NULL || key == NULL || model == NULL ||
                workspace == NULL || prompt == NULL || headers == NULL ||
                temp == NULL) {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "Out of memory.", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            config_trim(base);
            config_trim(key);
            config_trim(model);
            config_trim(prompt);
            config_trim(headers);
            config_trim(temp);
            if (base[0] == '\0') {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "The API base URL cannot be empty.",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (workspace[0] == '\0') {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "The workspace path cannot be empty.",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            abs_ws = absolute_path_utf8(workspace);
            if (abs_ws != NULL) {
                a98_free(workspace);
                workspace = abs_ws;
            }

            copy_str(app->cfg.api_base, sizeof(app->cfg.api_base), base);
            copy_str(app->cfg.api_key, sizeof(app->cfg.api_key), key);
            copy_str(app->cfg.model, sizeof(app->cfg.model), model);
            copy_str(app->cfg.system_prompt, sizeof(app->cfg.system_prompt),
                     prompt);
            copy_str(app->cfg.extra_headers, sizeof(app->cfg.extra_headers),
                     headers);
            copy_str(app->cfg.workspace, sizeof(app->cfg.workspace),
                     workspace);
            /* cfg.workspace stays UTF-8 (INI + dialog), but everything the
             * ANSI file APIs touch must be ACP: CreateDirectoryA,
             * GetFullPathNameA, the sandbox prefix checks and the run_command
             * workdir all consume agent.workspace verbatim.  A UTF-8 string
             * typed on a CJK code page used to create a mojibake directory
             * and poisoned the sandbox root comparison with it. */
            {
                char *ws_acp = path_to_acp(workspace);
                if (ws_acp != NULL) {
                    copy_str(app->agent.workspace,
                             sizeof(app->agent.workspace), ws_acp);
                    a98_path_mkdirs(ws_acp);
                    a98_free(ws_acp);
                }
            }
            t = temp[0] != '\0' ? atof(temp) : app->cfg.temperature;
            if (t < 0.0) {
                t = 0.0;
            }
            if (t > 2.0) {
                t = 2.0;
            }
            app->cfg.temperature = t;
            /* 0 = unlimited, otherwise 1..200. */
            app->cfg.max_steps =
                (int)GetDlgItemInt(dlg, IDC_MAX_STEPS, &ok, FALSE);
            if (!ok) {
                app->cfg.max_steps = 32;
            } else if (app->cfg.max_steps > 200) {
                app->cfg.max_steps = 200;
            }
            app->cfg.max_tokens =
                (int)GetDlgItemInt(dlg, IDC_MAX_TOKENS, &ok, FALSE);
            if (!ok || app->cfg.max_tokens < 1) {
                app->cfg.max_tokens = 1;
            }
            if (app->cfg.max_tokens > 32768) {
                app->cfg.max_tokens = 32768;
            }
            app->cfg.tls_verify =
                IsDlgButtonChecked(dlg, IDC_TLS_VERIFY) == BST_CHECKED;
            app->cfg.allow_shell =
                IsDlgButtonChecked(dlg, IDC_ALLOW_SHELL) == BST_CHECKED;
            app->cfg.full_access =
                IsDlgButtonChecked(dlg, IDC_FULL_ACCESS) == BST_CHECKED;
            app->cfg.auto_confirm =
                IsDlgButtonChecked(dlg, IDC_AUTO_CONFIRM) == BST_CHECKED;
            if (app->cfg.full_access) {
                app->cfg.auto_confirm = 1;
            }
            app->cfg.stream =
                IsDlgButtonChecked(dlg, IDC_STREAM) == BST_CHECKED;
            app->cfg.allow_outside =
                IsDlgButtonChecked(dlg, IDC_ALLOW_OUTSIDE) == BST_CHECKED;
            app->cfg.emoji_inline =
                IsDlgButtonChecked(dlg, IDC_EMOJI_INLINE) == BST_CHECKED;
            app->cfg.net_allow =
                IsDlgButtonChecked(dlg, IDC_NET_ALLOW) == BST_CHECKED;

            /* Numeric fields with validation and safe fallbacks. */
            {
                int v = (int)GetDlgItemInt(dlg, IDC_CONNECT_TIMEOUT, &ok, FALSE);
                app->cfg.connect_timeout_sec =
                    (ok && v >= 1 && v <= 600) ? v : 30;
                v = (int)GetDlgItemInt(dlg, IDC_REQUEST_TIMEOUT, &ok, FALSE);
                app->cfg.request_timeout_sec =
                    (ok && v >= 1 && v <= 3600) ? v : 180;
                v = (int)GetDlgItemInt(dlg, IDC_LOG_LEVEL, &ok, FALSE);
                app->cfg.log_level = (ok && v >= 0 && v <= 4) ? v : 2;
                v = (int)GetDlgItemInt(dlg, IDC_NET_MAX_BYTES, &ok, FALSE);
                app->cfg.net_max_bytes =
                    (ok && v >= 1024 && v <= 8 * 1024 * 1024) ? v : 262144;
            }
            /* Network strings need the same UTF-8 -> ACP handling as the
             * other text fields; dlg_text_utf8 already does it. */
            {
                char *h = dlg_text_utf8(dlg, IDC_NET_ALLOW_HOSTS);
                char *u = dlg_text_utf8(dlg, IDC_NET_SEARCH_URL);
                char *g = dlg_text_utf8(dlg, IDC_NET_GITHUB_TOKEN);
                if (h != NULL) {
                    copy_str(app->cfg.net_allow_hosts,
                             sizeof(app->cfg.net_allow_hosts), h);
                    a98_free(h);
                }
                if (u != NULL) {
                    copy_str(app->cfg.net_search_url,
                             sizeof(app->cfg.net_search_url), u);
                    a98_free(u);
                }
                if (g != NULL) {
                    copy_str(app->cfg.net_github_token,
                             sizeof(app->cfg.net_github_token), g);
                    a98_free(g);
                }
            }

            if (config_save(&app->cfg, app->ini_path) != 0) {
                msgbox_utf8(dlg, "Settings could not be saved to the INI file.",
                            MB_OK | MB_ICONWARNING);
            }
            update_status(app);

            a98_free(base);
            a98_free(key);
            a98_free(model);
            a98_free(workspace);
            a98_free(prompt);
            a98_free(headers);
            a98_free(temp);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK confirm_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        {
            char summary[1200];
            int danger = 0;
            tools_format_confirm(g_confirm_tool, g_confirm_args, summary,
                                 sizeof(summary), &danger);
            dlg_set_utf8(dlg, IDC_CONFIRM_TOOL,
                         danger >= 2 ? "HIGH RISK tool call" :
                         (danger == 1 ? "Write operation" : "Tool call"));
            dlg_set_utf8(dlg, IDC_CONFIRM_ARGS, summary);
            center_window(dlg);
            if (danger >= 2) {
                /* Destructive tools default to Deny: Cancel becomes the
                 * default button.  BM_SETSTYLE alone is unreliable on Win9x --
                 * the dialog manager keeps its own default id, so also tell it
                 * explicitly with DM_SETDEFID or Enter could still trigger
                 * "Allow".  Return TRUE so the dialog manager keeps this
                 * focus instead of resetting it to the first control. */
                SendMessageA(GetDlgItem(dlg, IDCANCEL), BM_SETSTYLE,
                             (WPARAM)BS_DEFPUSHBUTTON, TRUE);
                SendMessageA(GetDlgItem(dlg, IDOK), BM_SETSTYLE,
                             (WPARAM)BS_PUSHBUTTON, TRUE);
                SendMessageA(dlg, DM_SETDEFID, IDCANCEL, 0);
                SetFocus(GetDlgItem(dlg, IDCANCEL));
            } else {
                SetFocus(GetDlgItem(dlg, IDOK));
            }
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* Show all tools in a scrollable list with a detail pane. The previous
 * MessageBox truncated at 40 entries and had no scrollbar. */
static INT_PTR CALLBACK tools_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int count = 0, i;
        const ToolDef *all = tools_all(&count);
        HWND list = GetDlgItem(dlg, IDC_TOOLS_LIST);
        for (i = 0; i < count; i++) {
            char label[160];
            snprintf(label, sizeof(label), "%s%s", all[i].name,
                     all[i].needs_confirm ? "  [confirm]" : "");
            SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)label);
        }
        if (count > 0) {
            SendMessageA(list, LB_SETCURSEL, 0, 0);
            /* fill detail for the first entry */
            {
                Str s;
                char *acp;
                str_init(&s);
                str_appendf(&s, "%s\r\n\r\n%s\r\n\r\n%s",
                            all[0].name,
                            all[0].description ? all[0].description : "",
                            all[0].params_json ? all[0].params_json : "");
                acp = a98_utf8_to_acp(str_cstr(&s));
                SetDlgItemTextA(dlg, IDC_TOOLS_DETAIL,
                                acp ? acp : str_cstr(&s));
                a98_free(acp);
                str_free(&s);
            }
        }
        center_window(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_TOOLS_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            HWND list = GetDlgItem(dlg, IDC_TOOLS_LIST);
            int idx = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
            int count = 0;
            const ToolDef *all = tools_all(&count);
            if (idx >= 0 && idx < count) {
                Str s;
                char *acp;
                str_init(&s);
                str_appendf(&s, "Name: %s\r\nConfirm: %s\r\n\r\n%s\r\n\r\nParameters:\r\n%s",
                            all[idx].name,
                            all[idx].needs_confirm ? "yes" : "no",
                            all[idx].description ? all[idx].description : "",
                            all[idx].params_json ? all[idx].params_json : "");
                acp = a98_utf8_to_acp(str_cstr(&s));
                SetDlgItemTextA(dlg, IDC_TOOLS_DETAIL,
                                acp ? acp : str_cstr(&s));
                a98_free(acp);
                str_free(&s);
            }
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        center_window(dlg);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* First-run wizard: shown when no API key is configured yet.  "Later"
 * keeps the previous behaviour (an empty config, prompted at send).   */
/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK firstrun_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    App *app = g_dlg_app;

    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int i;

        if (app == NULL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        for (i = 0; i < PRESET_COUNT; i++) {
            SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_ADDSTRING, 0,
                                (LPARAM)g_presets[i].name);
        }
        SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_SETCURSEL,
                            (WPARAM)preset_from_base(app->cfg.api_base), 0);
        dlg_set_utf8(dlg, IDC_API_BASE, app->cfg.api_base);
        dlg_set_utf8(dlg, IDC_MODEL, app->cfg.model);
        CheckDlgButton(dlg, IDC_STREAM,
                       app->cfg.stream ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ALLOW_SHELL, BST_UNCHECKED);
        center_window(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PROVIDER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendDlgItemMessageA(dlg, IDC_PROVIDER,
                                                   CB_GETCURSEL, 0, 0);
                if (idx >= 0 && idx < PRESET_COUNT - 1) {
                    dlg_set_utf8(dlg, IDC_API_BASE, g_presets[idx].base);
                    dlg_set_utf8(dlg, IDC_MODEL, g_presets[idx].model);
                }
            }
            return TRUE;
        case IDOK: {
            char *base = dlg_text_utf8(dlg, IDC_API_BASE);
            char *key = dlg_text_utf8(dlg, IDC_API_KEY);
            char *model = dlg_text_utf8(dlg, IDC_MODEL);

            if (base == NULL || key == NULL || model == NULL) {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                msgbox_utf8(dlg, "Out of memory.", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            config_trim(base);
            config_trim(key);
            config_trim(model);
            if (base[0] == '\0' || model[0] == '\0') {
                msgbox_utf8(dlg,
                            "Both the API base and a model are required.",
                            MB_OK | MB_ICONWARNING);
                a98_free(base);
                a98_free(key);
                a98_free(model);
                return TRUE;
            }
            copy_str(app->cfg.api_base, sizeof(app->cfg.api_base), base);
            copy_str(app->cfg.api_key, sizeof(app->cfg.api_key), key);
            copy_str(app->cfg.model, sizeof(app->cfg.model), model);
            app->cfg.stream =
                IsDlgButtonChecked(dlg, IDC_STREAM) == BST_CHECKED;
            app->cfg.allow_shell =
                IsDlgButtonChecked(dlg, IDC_ALLOW_SHELL) == BST_CHECKED;
            if (config_save(&app->cfg, app->ini_path) != 0) {
                msgbox_utf8(dlg,
                            "Settings could not be saved to the INI file.",
                            MB_OK | MB_ICONWARNING);
            }
            update_status(app);
            a98_free(base);
            a98_free(key);
            a98_free(model);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* Generic single-line prompt (used by session rename).  The caller seeds
 * g_inputline_result with the initial text and reads back the answer. */
static INT_PTR CALLBACK inputline_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowTextA(dlg, g_inputline_title);
        SetDlgItemTextA(dlg, IDC_INPUTLINE_LABEL, g_inputline_label);
        SetDlgItemTextA(dlg, IDC_INPUTLINE_EDIT, g_inputline_result);
        SendDlgItemMessageA(dlg, IDC_INPUTLINE_EDIT, EM_SETSEL,
                            0, (LPARAM)-1);
        center_window(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            char *t = dlg_text_utf8(dlg, IDC_INPUTLINE_EDIT);
            char *acp = (t != NULL) ? a98_utf8_to_acp(t) : NULL;

            a98_free(t);
            if (acp != NULL) {
                copy_str(g_inputline_result, sizeof(g_inputline_result), acp);
                g_inputline_ok = 1;
            }
            a98_free(acp);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            g_inputline_ok = 0;
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        g_inputline_ok = 0;
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

static int input_line_box(HWND parent, const char *title, const char *label,
                          const char *initial, char *out, size_t cap)
{
    copy_str(g_inputline_title, sizeof(g_inputline_title), title);
    copy_str(g_inputline_label, sizeof(g_inputline_label), label);
    copy_str(g_inputline_result, sizeof(g_inputline_result), initial);
    g_inputline_ok = 0;
    DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_INPUTLINE), parent,
                    inputline_proc, 0);
    if (!g_inputline_ok) {
        return 0;
    }
    copy_str(out, cap, g_inputline_result);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Input subclass: Enter sends, Ctrl+Enter inserts a newline           */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Alt+Up / Alt+Down cycle the prompt history, DOSKEY style.  Windows
     * delivers these as WM_SYSKEYDOWN because Alt is held down. */
    if (wp == VK_UP || wp == VK_DOWN) {
        if (msg == WM_SYSKEYDOWN ||
            (msg == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000) != 0)) {
            input_history_nav(&g_app, wp == VK_UP ? -1 : 1);
            return 0;
        }
    }
    if (msg == WM_CHAR) {
        g_app.input_history_pos = g_app.input_history_count;
        if (wp == '\r') {
            if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
                PostMessageA(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(IDC_SEND, BN_CLICKED),
                             (LPARAM)GetDlgItem(GetParent(hwnd), IDC_SEND));
                return 0;
            }
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            return 0;
        }
    }
    return CallWindowProcA(g_input_oldproc, hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Window creation and layout                                          */
/* ------------------------------------------------------------------ */

static void layout_status_parts(App *app)
{
    int edges[3];
    RECT rc;

    if (app == NULL || app->statusbar == NULL) {
        return;
    }
    GetClientRect(app->statusbar, &rc);
    /* part 0: status (flex), part 1: round/chars (fixed), part 2: workspace */
    edges[0] = rc.right - 390;
    if (edges[0] < 160) {
        edges[0] = 160;
    }
    edges[1] = edges[0] + 150;
    edges[2] = -1;
    SendMessageA(app->statusbar, SB_SETPARTS, 3, (LPARAM)edges);
}

static void layout(App *app)
{
    RECT rc;
    int w;
    int h;
    int y;
    int iy;
    int ty;
    const int margin = 6;
    const int btn_h = 24;
    const int input_h = 84;
    const int status_h = 22;
    const int top_h = 24;

    if (app == NULL || app->hwnd == NULL) {
        return;
    }
    GetClientRect(app->hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    /* Native Win9x status bar sits flush against the bottom edge. */
    MoveWindow(app->statusbar, 0, h - status_h, w, status_h, TRUE);
    layout_status_parts(app);

    y = h - status_h - 4 - btn_h;
    MoveWindow(app->send, margin, y, 64, btn_h, TRUE);
    MoveWindow(app->stop, margin + 68, y, 64, btn_h, TRUE);
    MoveWindow(app->newbtn, margin + 136, y, 64, btn_h, TRUE);
    MoveWindow(app->settings, margin + 204, y, 72, btn_h, TRUE);
    MoveWindow(app->workspace_btn, margin + 280, y, 84, btn_h, TRUE);
    MoveWindow(app->emoji_btn, margin + 368, y, 60, btn_h, TRUE);

    iy = y - 4 - input_h;
    if (iy < margin + top_h + 4 + 40) {
        iy = margin + top_h + 4 + 40;
    }
    MoveWindow(app->input, margin, iy, w - 2 * margin, input_h, TRUE);

    ty = margin;
    MoveWindow(app->session_label, margin, ty + 4, 50, 16, TRUE);
    /* The 120 px height is the drop-down extent, not the closed height.
     * Win9x needs it in the window rect or the list is only one row tall. */
    MoveWindow(app->session_combo, margin + 52, ty,
               w - 2 * margin - 52, 120, TRUE);

    MoveWindow(app->transcript, margin, ty + top_h + 4,
               w - 2 * margin, iy - 4 - (ty + top_h + 4), TRUE);
}

/* ------------------------------------------------------------------ */
/* Window placement persistence ([window] section of the INI)          */
/* ------------------------------------------------------------------ */

static void save_window_placement(App *app)
{
    WINDOWPLACEMENT wp;
    char num[32];

    if (app == NULL || app->hwnd == NULL || app->ini_path[0] == '\0') {
        return;
    }
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(app->hwnd, &wp)) {
        return;
    }
    WritePrivateProfileStringA("window", "saved", "1", app->ini_path);
    sprintf(num, "%ld", (long)wp.rcNormalPosition.left);
    WritePrivateProfileStringA("window", "left", num, app->ini_path);
    sprintf(num, "%ld", (long)wp.rcNormalPosition.top);
    WritePrivateProfileStringA("window", "top", num, app->ini_path);
    sprintf(num, "%ld",
            (long)(wp.rcNormalPosition.right - wp.rcNormalPosition.left));
    WritePrivateProfileStringA("window", "width", num, app->ini_path);
    sprintf(num, "%ld",
            (long)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top));
    WritePrivateProfileStringA("window", "height", num, app->ini_path);
    WritePrivateProfileStringA("window", "maximized",
                               IsZoomed(app->hwnd) ? "1" : "0",
                               app->ini_path);
}

/* Restore x/y/w/h/maximized from the INI.  Missing or implausible values
 * fall back to the previous defaults; the rect is kept at least partly on
 * the (primary) screen so a moved monitor cannot strand the window. */
static void load_window_placement(App *app, int *x, int *y, int *w, int *h,
                                  int *maximized)
{
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);

    *x = CW_USEDEFAULT;
    *y = CW_USEDEFAULT;
    *w = 800;
    *h = 620;
    *maximized = 0;
    if (app->ini_path[0] == '\0' ||
        GetPrivateProfileIntA("window", "saved", 0, app->ini_path) == 0) {
        return;
    }
    {
        int l = (int)GetPrivateProfileIntA("window", "left", 0, app->ini_path);
        int t = (int)GetPrivateProfileIntA("window", "top", 0, app->ini_path);
        int rw = (int)GetPrivateProfileIntA("window", "width", 800,
                                            app->ini_path);
        int rh = (int)GetPrivateProfileIntA("window", "height", 620,
                                            app->ini_path);

        if (rw < 520) {
            rw = 520;               /* matches WM_GETMINMAXINFO */
        }
        if (rh < 380) {
            rh = 380;
        }
        if (rw > cx) {
            rw = cx;
        }
        if (rh > cy) {
            rh = cy;
        }
        if (l < -rw + 80) {
            l = -rw + 80;
        }
        if (l > cx - 80) {
            l = cx - 80;
        }
        if (t < 0) {
            t = 0;
        }
        if (t > cy - 60) {
            t = cy - 60;
        }
        *x = l;
        *y = t;
        *w = rw;
        *h = rh;
        *maximized =
            GetPrivateProfileIntA("window", "maximized", 0, app->ini_path) != 0;
    }
}

static int load_rich_edit(App *app)
{
    app->rich_dll = LoadLibraryA("riched20.dll");
    if (app->rich_dll != NULL) {
        app->rich = 2;
        return 2;
    }
    app->rich_dll = LoadLibraryA("riched32.dll");
    if (app->rich_dll != NULL) {
        app->rich = 1;
        return 1;
    }
    app->rich = 0;
    return 0;
}

static void create_controls(App *app, HWND hwnd)
{
    DWORD input_style;

    /* The transcript is now a self-drawn control.  Emoji are GDI bitmaps,
     * not RichEdit OLE objects, which fault on real Windows 9x.  Keep
     * app->rich == 2 so the existing EM_* formatting path still runs. */
    app->transcript = a98tv_create(app->hinst, hwnd, IDC_TRANSCRIPT,
                                   app->mono_font, 0, 0, 100, 100);
    if (app->transcript != NULL) {
        SendMessageA(app->transcript, A98TV_SETDIR, 0, (LPARAM)app->exe_dir);
        app->rich = 2;
    }

    input_style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                  ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    app->input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 input_style, 0, 0, 100, 100, hwnd,
                                 (HMENU)IDC_INPUT, app->hinst, NULL);

    app->send = CreateWindowExA(0, "BUTTON", "Send",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                BS_PUSHBUTTON, 0, 0, 64, 24, hwnd,
                                (HMENU)IDC_SEND, app->hinst, NULL);
    app->stop = CreateWindowExA(0, "BUTTON", "Stop",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                BS_PUSHBUTTON, 0, 0, 64, 24, hwnd,
                                (HMENU)IDC_STOP, app->hinst, NULL);
    app->newbtn = CreateWindowExA(0, "BUTTON", "New",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_PUSHBUTTON, 0, 0, 64, 24, hwnd,
                                  (HMENU)IDC_NEW, app->hinst, NULL);
    app->settings = CreateWindowExA(0, "BUTTON", "Settings",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON, 0, 0, 72, 24, hwnd,
                                    (HMENU)IDC_SETTINGS, app->hinst, NULL);
    app->workspace_btn = CreateWindowExA(0, "BUTTON", "Workspace",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         BS_PUSHBUTTON, 0, 0, 84, 24, hwnd,
                                         (HMENU)IDC_OPEN_WORKSPACE, app->hinst,
                                         NULL);
    app->emoji_btn = CreateWindowExA(0, "BUTTON", "Emoji",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     BS_PUSHBUTTON, 0, 0, 60, 24, hwnd,
                                     (HMENU)IDC_EMOJI, app->hinst, NULL);

    app->session_label = CreateWindowExA(0, "STATIC", "History:",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT |
                                         SS_NOPREFIX, 0, 0, 50, 16, hwnd,
                                         (HMENU)IDC_SESSION_LABEL,
                                         app->hinst, NULL);
    app->session_combo = CreateWindowExA(0, "COMBOBOX", "",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         WS_VSCROLL | CBS_DROPDOWNLIST,
                                         0, 0, 100, 120, hwnd,
                                         (HMENU)IDC_SESSION, app->hinst, NULL);

    app->statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "",
                                     WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                     0, 0, 100, 22, hwnd,
                                     (HMENU)IDC_STATUS, app->hinst, NULL);

    if (app->font != NULL) {
        SendMessageA(app->input, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->send, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->stop, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->newbtn, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->settings, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->workspace_btn, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->emoji_btn, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->session_label, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->session_combo, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->statusbar, WM_SETFONT, (WPARAM)app->font, TRUE);
    }
    if (app->mono_font != NULL) {
        SendMessageA(app->transcript, WM_SETFONT, (WPARAM)app->mono_font,
                     TRUE);
    }

    if (app->transcript != NULL) {
        SendMessageA(app->transcript, EM_EXLIMITTEXT, 0, (LPARAM)TRANSCRIPT_LIMIT);
        SendMessageA(app->transcript, EM_SETBKGNDCOLOR, 0,
                     (LPARAM)GetSysColor(COLOR_WINDOW));
        /* Inline emoji are GDI bitmaps drawn by the self-drawn view; no
         * RichEdit OLE is involved, so this is safe on real Win9x. */
        app->emoji_pics = app->cfg.emoji_inline ? 1 : 0;
        log_info("emoji inline pictures %s (self-drawn)",
                 app->emoji_pics ? "enabled" : "off ([emoji] inline=0)");
    }
    transcript_set_default_format(app);
    app->stick_bottom = 1;

    /* Measure the transcript line height once; mono font, so it never
     * changes and "am I at the bottom?" needs no recalculation. */
    if (app->transcript != NULL) {
        HDC dc = GetDC(app->transcript);
        HFONT old = (HFONT)SelectObject(dc, app->mono_font);
        TEXTMETRICA tm;

        if (GetTextMetricsA(dc, &tm)) {
            app->line_h = tm.tmHeight;
        }
        SelectObject(dc, old);
        ReleaseDC(app->transcript, dc);
    }

    /* The window-long slot carries a function POINTER: on x64 the plain
     * SetWindowLongA truncates it, so use the Ptr call there.  Win9x has no
     * SetWindowLongPtrA export and a 32-bit LONG is pointer-sized, so 32-bit
     * builds keep the original call. */
#ifdef _WIN64
    g_input_oldproc = (WNDPROC)SetWindowLongPtrA(app->input, GWLP_WNDPROC,
                                                 (LONG_PTR)input_proc);
#else
    g_input_oldproc = (WNDPROC)SetWindowLongA(app->input, GWL_WNDPROC,
                                              (LONG)input_proc);
#endif
}

/* ------------------------------------------------------------------ */
/* Main window procedure                                               */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App *app = &g_app;

    switch (msg) {
    case WM_CREATE:
        app = (App *)((CREATESTRUCT *)lp)->lpCreateParams;
        app->hwnd = hwnd;
        create_controls(app, hwnd);
        layout(app);
        session_scan(app);
        session_combo_fill(app, NULL);
        copy_str(app->status_line, sizeof(app->status_line), "Ready");
        set_running(app, 0);
        update_status(app);
        SetTimer(hwnd, IDT_SPINNER, 150, NULL);
        SetFocus(app->input);
        return 0;

    case WM_SIZE:
        layout(app);
        return 0;

    case WM_TIMER:
        if (wp == IDT_SPINNER && app->running) {
            app->spin_frame = (app->spin_frame + 1) & 3;
            status_update(app);
        }
        if (wp == IDT_EVENT_FLUSH) {
            /* Coalesced streamed deltas: flush whatever accumulated since
             * the last tick. */
            event_flush(app);
        }
        if (wp == IDT_FORCE_DESTROY) {
            /* The user consented to stop-and-exit, but the worker is still
             * running (long request with no data, or a confirm dialog the
             * worker already abandoned).  Without this timer the only
             * DestroyWindow path was A98_WM_DONE, leaving a "Cancelling..."
             * window that only Task Manager could kill -- and killing the
             * process skipped the whole close orchestration. */
            KillTimer(hwnd, IDT_FORCE_DESTROY);
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 520;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 380;
        return 0;

    case WM_SETFOCUS:
        if (!app->running) {
            SetFocus(app->input);
        }
        return 0;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            do_stop(app);
            return 0;
        }
        break;
    case A98TV_VIEWPORT_CHANGED:
        /* The self-drawn view scrolled: re-pin only if we're at the bottom. */
        app->stick_bottom = transcript_at_bottom(app);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_VSCROLL && LOWORD(wp) == IDC_TRANSCRIPT) {
            /* The user scrolled (or we scrolled back to the tail): re-pin
             * only if the view really is at the bottom again. */
            app->stick_bottom = transcript_at_bottom(app);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDC_SEND:
            do_send(app);
            return 0;
        case IDC_STOP:
            do_stop(app);
            return 0;
        case IDC_NEW:
        case IDM_NEW:
            do_new(app);
            return 0;
        case IDM_RESEND:
            do_resend(app);
            return 0;
        case IDM_SESSION_RENAME:
            do_session_rename(app);
            return 0;
        case IDM_SESSION_DELETE:
            do_session_delete(app);
            return 0;
        case IDM_OPENLOG:
            {
                char log_path[A98_MAX_PATH];
                HINSTANCE r;

                a98_path_join(log_path, sizeof(log_path), app->exe_dir,
                              "agent98.log");
                r = ShellExecuteA(hwnd, "open", log_path, NULL, NULL,
                                  SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32) {
                    msgbox_utf8(app->hwnd,
                                "No log file to open yet (agent98.log next "
                                "to the executable).",
                                MB_OK | MB_ICONINFORMATION);
                }
            }
            return 0;
        case IDC_SETTINGS:
        case IDM_SETTINGS:
            do_settings(app);
            return 0;
        case IDC_EMOJI:
        case IDM_EMOJI:
            do_emoji_pick(app);
            return 0;
        case IDC_OPEN_WORKSPACE:
        case IDM_WORKSPACE:
            do_open_workspace(app);
            return 0;
        case IDM_SAVE:
            do_save(app);
            return 0;
        case IDM_LOAD:
            do_load(app);
            return 0;
        case IDM_EXPORT:
            do_export(app);
            return 0;
        case IDC_SESSION:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendMessageA(app->session_combo, CB_GETCURSEL,
                                            0, 0);
                do_session_select(app, idx);
            }
            return 0;

        case IDM_TOOLS_LIST:
            DialogBoxParamA(app->hinst, MAKEINTRESOURCEA(IDD_TOOLS),
                            hwnd, tools_proc, 0);
            return 0;
        case IDM_ABOUT:
            DialogBoxParamA(app->hinst, MAKEINTRESOURCEA(IDD_ABOUT),
                            hwnd, about_proc, 0);
            return 0;
        case IDM_EXIT:
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;

    case A98_WM_EVENT: {
        UiEvent *ev = (UiEvent *)lp;
        if (ev != NULL) {
            switch (ev->type) {
            case AGENT_EV_TEXT:
                if (app->round_no > 0) {
                    snprintf(app->status_line, sizeof(app->status_line),
                             "Running (Round %d, replying)", app->round_no);
                    status_update(app);
                }
                /* Buffer the delta; IDT_EVENT_FLUSH paints it in one go so
                 * a fast stream does not flood the message queue. */
                str_append_cstr(&app->ev_text_buf, ev->text);
                SetTimer(hwnd, IDT_EVENT_FLUSH, A98_EVENT_FLUSH_MS, NULL);
                break;
            case AGENT_EV_ROUND:
                event_flush_ex(app, 1); /* end the text line, keep [round] */
                app->round_no++;
                snprintf(app->status_line, sizeof(app->status_line),
                         "Running (Round %d)", app->round_no);
                status_update(app);
                transcript_line_colored(app, "\r\n[round] ", COL_ROUND, 0,
                                        ev->text, COL_ROUND, 0);
                break;
            case AGENT_EV_TOOL_START:
                event_flush_ex(app, 1);
                snprintf(app->status_line, sizeof(app->status_line),
                         "Running (Round %d, tool)", app->round_no);
                status_update(app);
                transcript_line_colored(app, "\r\n[tool] ", COL_TOOL, 0,
                                        ev->text, COL_TOOL, 0);
                break;
            case AGENT_EV_TOOL_RESULT:
                event_flush_ex(app, 1);
                snprintf(app->status_line, sizeof(app->status_line),
                         "Running (Round %d)", app->round_no);
                status_update(app);
                transcript_line_colored(app, "[result] ", COL_RESULT, 0,
                                        ev->text, COL_RESULT, 0);
                break;
            case AGENT_EV_INFO:
                event_flush_ex(app, 1);
                transcript_line_colored(app, "[info] ", COL_ROUND, 0,
                                        ev->text, COL_ROUND, 0);
                break;
            default:
                event_flush_ex(app, 1);
                transcript_line(app, "", ev->text);
                break;
            }
            a98_free(ev->text);
            a98_free(ev);
        }
        return 0;
    }

    case A98_WM_DONE: {
        char *err = (char *)lp;
        int ok = (int)wp;

        /* The reply is complete: paint any buffered trailing fragment. */
        event_flush_ex(app, 1);
        if (app->thread != NULL) {
            CloseHandle(app->thread);
            app->thread = NULL;
        }
        if (!ok) {
            if (err != NULL && err[0] != '\0') {
                transcript_line_colored(app, "\r\n[error] ", COL_ERROR, 1,
                                        err, COL_ERROR, 0);
            } else {
                transcript_line_colored(app, "\r\n[error] ", COL_ERROR, 1,
                                        "request failed", COL_ERROR, 0);
            }
        } else {
            transcript_append(app, "\r\n");
        }
        a98_free(err);
        set_running(app, 0);
        if (!app->closing) {
            auto_save_session(app);
        }
        update_status(app);
        if (app->closing) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case A98_WM_CONFIRM: {
        ConfirmReq *req = (ConfirmReq *)lp;
        if (req != NULL) {
            event_flush_ex(app, 1);
            if (app->cancel) {
                /* Stop was pressed before the dialog appeared; deny without
                 * bothering the user. */
                req->result = 0;
            } else {
                g_confirm_tool = req->tool;
                g_confirm_args = req->args;
                /* DialogBoxParamA returns -1 when it cannot create the dialog
                 * (resource/loading failure); anything but an explicit
                 * IDOK-style 1 must count as "denied", never "allowed". */
                req->result = (DialogBoxParamA(app->hinst,
                                               MAKEINTRESOURCEA(IDD_CONFIRM),
                                               hwnd, confirm_proc, 0) == 1);
                g_confirm_tool = NULL;
                g_confirm_args = NULL;
            }
            SetEvent(req->event);
        }
        return 0;
    }

    case WM_CLOSE:
        if (app->running) {
            if (MessageBoxA(hwnd,
                            "A request is still running.\n\n"
                            "Stop it and exit?",
                            "AIagent98",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 0;
            }
            app->closing = 1;
            app->cancel = 1;
            status_set(app, "Cancelling...");
            SetTimer(hwnd, IDT_FORCE_DESTROY, A98_CLOSE_GRACE_MS, NULL);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
        /* System shutdown / logoff.  No interactive prompt here: during
         * logoff a MessageBox can hang the whole shutdown.  Just cancel the
         * worker and let the close path run. */
        app->closing = 1;
        app->cancel = 1;
        return TRUE;

    case WM_ENDSESSION:
        if (wp != 0) {
            /* The session is really going away; without an explicit destroy
             * the default handling would bypass our close orchestration
             * entirely and kill the process with the worker mid-flight. */
            if (app->running) {
                app->closing = 1;
                app->cancel = 1;
            }
            DestroyWindow(hwnd);
        } else {
            /* Shutdown was vetoed elsewhere; back to normal operation. */
            app->closing = 0;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_SPINNER);
        KillTimer(hwnd, IDT_FORCE_DESTROY);
        KillTimer(hwnd, IDT_EVENT_FLUSH);
        app->cancel = 1;
        save_window_placement(app);
        str_free(&app->ev_text_buf);
        str_free(&app->md_pending);
        /* Wake a worker blocked in gui_confirm *before* waiting: its event
         * can only be signalled from the UI thread, which is busy right now.
         * The worker sees the shutdown event and denies the pending tool. */
        if (app->shutdown_event != NULL) {
            SetEvent(app->shutdown_event);
        }
        if (app->thread != NULL) {
            /* Close orchestration: give the worker a bounded grace period.
             * If it does not stop (stuck in an un-cancellable socket call),
             * do NOT free agent/net/log state under its feet -- agent_free
             * would release the message list it is traversing, net_cleanup
             * would WSACleanup under an active recv, log_shutdown would
             * close the FILE* and critical section it writes to.  The
             * worker is deliberately left running and the process exit
             * reclaims everything. */
            if (WaitForSingleObject(app->thread, A98_WORKER_WAIT_MS) ==
                WAIT_OBJECT_0) {
                CloseHandle(app->thread);
            } else {
                app->leaked_worker = 1;
                log_warn("worker still running at destroy; abandoning it "
                         "without freeing shared state");
            }
            app->thread = NULL;
        }
        /* Posted payloads for messages that will never be dispatched (the
         * window is going away) are leaked unless drained here. */
        {
            MSG drain;
            while (PeekMessageA(&drain, hwnd, A98_WM_EVENT, A98_WM_CONFIRM,
                                PM_REMOVE)) {
                if (drain.message == A98_WM_EVENT) {
                    UiEvent *ev = (UiEvent *)drain.lParam;
                    if (ev != NULL) {
                        a98_free(ev->text);
                        a98_free(ev);
                    }
                } else if (drain.message == A98_WM_DONE) {
                    a98_free((char *)drain.lParam);
                }
                /* A98_WM_CONFIRM: req lives on the worker's stack; the
                 * worker cleans it up itself once the shutdown event wakes
                 * it.  Never touch it from here. */
            }
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static HFONT make_ui_font(void)
{
    NONCLIENTMETRICSA ncm;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return CreateFontIndirectA(&ncm.lfMessageFont);
    }
    if (sizeof(ncm) > sizeof(int)) {
        ncm.cbSize = sizeof(ncm) - sizeof(int);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0)) {
            return CreateFontIndirectA(&ncm.lfMessageFont);
        }
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* Classic fixed-pitch console font.  Fixedsys is the canonical Win9x
 * terminal face; on CJK systems NSimSun keeps Chinese text aligned. */
static HFONT make_mono_font(App *app)
{
    LOGFONTA lf;
    HDC hdc;
    int height;
    UINT acp;
    const char *face;
    BYTE charset;

    memset(&lf, 0, sizeof(lf));
    hdc = GetDC(NULL);
    height = -MulDiv(A98_TRANSCRIPT_POINTS,
                     GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);

    lf.lfHeight = height;
    lf.lfWeight = FW_NORMAL;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;

    acp = GetACP();
    if (acp == 936) {
        face = "NSimSun";
        charset = GB2312_CHARSET;
    } else if (acp == 932) {
        face = "MS Gothic";
        charset = SHIFTJIS_CHARSET;
    } else if (acp == 949) {
        face = "GulimChe";
        charset = HANGUL_CHARSET;
    } else {
        face = "Fixedsys";
        charset = ANSI_CHARSET;
    }
    /* Pin the charset instead of using DEFAULT_CHARSET.  A run whose charset
     * does not match the face makes GDI pick a substitute font, which on a
     * DBCS system is a Latin face with no CJK glyphs. */
    lf.lfCharSet = charset;
    copy_str(lf.lfFaceName, sizeof(lf.lfFaceName), face);
    copy_str(app->mono_face, sizeof(app->mono_face), face);
    app->mono_charset = charset;
    return CreateFontIndirectA(&lf);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    App *app = &g_app;
    WNDCLASSEXA wc;
    MSG msg;
    char log_path[A98_MAX_PATH];
    int rich;
    int i;
    int win_x, win_y, win_w, win_h, win_max;

    (void)prev;
    (void)cmdline;

    memset(app, 0, sizeof(*app));
    app->hinst = hinst;
    /* RichEdit stores an inline emoji picture as an OLE object, so the UI
     * thread has to have OLE up before the transcript exists. */
    OleInitialize(NULL);

    /* One instance at a time: two processes would race the INI writes and
     * the session auto-save.  A second launch just raises the first one. */
    app->single_mutex = CreateMutexA(NULL, FALSE, "AIagent98SingleInstance");
    if (app->single_mutex == NULL ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prevwnd = FindWindowA(A98_CLASS_NAME, NULL);

        if (prevwnd != NULL) {
            ShowWindow(prevwnd, IsIconic(prevwnd) ? SW_RESTORE
                                                  : SW_SHOWNORMAL);
            SetForegroundWindow(prevwnd);
        } else {
            MessageBoxA(NULL, "AIagent98 is already running.", "AIagent98",
                        MB_OK | MB_ICONINFORMATION);
        }
        if (app->single_mutex != NULL) {
            CloseHandle(app->single_mutex);
            app->single_mutex = NULL;
        }
        return 0;
    }

    if (a98_path_exe_dir(app->exe_dir, sizeof(app->exe_dir)) != 0) {
        strcpy(app->exe_dir, ".");
    }
    a98_path_join(app->sessions_dir, sizeof(app->sessions_dir),
                  app->exe_dir, "sessions");
    a98_path_mkdirs(app->sessions_dir);

    /* Open the log before touching the configuration.  If the process dies
     * during startup, this file is the only clue we get on Windows 9x. */
    a98_path_join(log_path, sizeof(log_path), app->exe_dir, "agent98.log");
    log_init(log_path, LOG_LEVEL_DEBUG);
    log_info("AIagent98 GUI starting (version %s)", A98_VERSION);
    log_info("Executable directory: %s", app->exe_dir);

    config_default_path(app->ini_path, sizeof(app->ini_path));
    config_defaults(&app->cfg, app->exe_dir);
    if (config_ensure(&app->cfg, app->ini_path) != 0) {
        log_warn("Cannot load or create %s", app->ini_path);
    }
    log_set_level(app->cfg.log_level);
    log_info("Configuration loaded from %s", app->ini_path);

    if (net_init() != 0) {
        msgbox_utf8(NULL, "Failed to initialise Winsock.",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Set from WM_DESTROY; lets gui_confirm unblock when the UI thread stops
     * pumping messages.  Created before the window so a confirm request can
     * never race it. */
    app->shutdown_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (app->shutdown_event == NULL) {
        msgbox_utf8(NULL, "Out of resources while starting up.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }

    /* The status bar is one of the original Win95 common controls. */
    InitCommonControls();

    agent_init(&app->agent, &app->cfg);
    agent_new_conversation(&app->agent);
    agent_ensure_dirs(&app->agent);
    app->agent.on_event = gui_on_event;
    app->agent.event_ctx = app;
    app->agent.on_confirm = gui_confirm;
    app->agent.confirm_ctx = app;
    app->agent.cancel = &app->cancel;

    rich = load_rich_edit(app);
    log_info("RichEdit mode: %d", rich);
    app->font = make_ui_font();
    app->mono_font = make_mono_font(app);
    app->accel = LoadAcceleratorsA(hinst, MAKEINTRESOURCEA(IDR_ACCEL));
    load_window_placement(app, &win_x, &win_y, &win_w, &win_h, &win_max);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(IDI_APP));
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    }
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = A98_CLASS_NAME;

    if (!RegisterClassExA(&wc)) {
        msgbox_utf8(NULL, "Cannot register the main window class.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }
    if (!a98tv_register(hinst)) {
        msgbox_utf8(NULL, "Cannot register the transcript view class.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }

    app->hwnd = CreateWindowExA(0, A98_CLASS_NAME, "AIagent98",
                                WS_OVERLAPPEDWINDOW,
                                win_x, win_y, win_w, win_h,
                                NULL,
                                LoadMenuA(hinst, MAKEINTRESOURCEA(IDR_MAINMENU)),
                                hinst, app);
    if (app->hwnd == NULL) {
        msgbox_utf8(NULL, "Cannot create the main window.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }

    ShowWindow(app->hwnd, win_max ? SW_MAXIMIZE
                                  : (show == 0 ? SW_SHOWNORMAL : show));
    UpdateWindow(app->hwnd);

    if (app->cfg.api_key[0] == '\0') {
        /* First launch without configuration: walk through the minimum
         * (base, key, model) instead of a plain message box. */
        g_dlg_app = app;
        DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_FIRSTRUN), app->hwnd,
                        firstrun_proc, 0);
        g_dlg_app = NULL;
        update_status(app);
    }

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (app->accel != NULL &&
            TranslateAcceleratorA(app->hwnd, app->accel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    app->cancel = 1;
    if (app->shutdown_event != NULL) {
        SetEvent(app->shutdown_event);
    }
    if (app->thread != NULL) {
        /* WM_DESTROY already waited once; this is the no-window fallback. */
        if (WaitForSingleObject(app->thread, A98_WORKER_WAIT_MS) ==
            WAIT_OBJECT_0) {
            CloseHandle(app->thread);
        } else {
            app->leaked_worker = 1;
            log_warn("worker still running at exit; abandoning it without "
                     "freeing shared state");
        }
        app->thread = NULL;
    }
    if (!app->leaked_worker) {
        agent_free(&app->agent);
        net_cleanup();
        log_info("AIagent98 GUI stopped");
        log_shutdown();
    } else {
        /* The worker is still alive and may be touching the agent state,
         * sockets and log file: do not free/close any of it.  Deliberate
         * leak; the process exit reclaims everything. */
        log_info("AIagent98 GUI stopped with the worker abandoned");
    }
    if (app->shutdown_event != NULL) {
        CloseHandle(app->shutdown_event);
        app->shutdown_event = NULL;
    }
    if (app->font != NULL) {
        DeleteObject(app->font);
    }
    if (app->mono_font != NULL) {
        DeleteObject(app->mono_font);
    }
    for (i = 0; i < app->input_history_count; i++) {
        a98_free(app->input_history[i]);
        app->input_history[i] = NULL;
    }
    a98_free(app->last_input);
    app->last_input = NULL;
    if (app->rich_dll != NULL) {
        FreeLibrary(app->rich_dll);
    }
    OleUninitialize();
    if (app->single_mutex != NULL) {
        /* Releases the named mutex so a new instance can start right away. */
        CloseHandle(app->single_mutex);
        app->single_mutex = NULL;
    }
    return (int)msg.wParam;
}
