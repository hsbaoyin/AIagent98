/*
 * AIagent98 - self-drawn transcript view (Windows 95/98/ME, OpenWatcom C).
 *
 * Replaces the RichEdit transcript with an owner-drawn control so inline
 * emoji are plain GDI BitBlts instead of RichEdit OLE objects (which GPF on
 * real Win9x).  The control emulates the handful of EM_* messages the GUI
 * already sends, so main_gui.c's write path, markdown colouring, session
 * replay and export need no changes.
 *
 * Visual style: bubble cards (You right, Assistant left), coloured role
 * labels, a left accent bar, fenced-code bands, and collapsible tool calls.
 */
#ifndef A98_TRANSCRIPT_VIEW_H
#define A98_TRANSCRIPT_VIEW_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A98TV_CLASSNAME "A98TranscriptView"

/* Private messages (WM_APP + n). */
#define A98TV_EMOJI   (WM_APP + 10) /* lParam = const A98EmojiEntry* insert */
#define A98TV_TRIM    (WM_APP + 11) /* wParam = max physical lines          */
#define A98TV_SETDIR  (WM_APP + 12) /* lParam = const char* exe dir (ACP)  */
#define A98TV_VIEWPORT_CHANGED (WM_APP + 13) /* posted to parent on scroll    */

/* Register the window class.  Idempotent.  hinst must stay valid. */
int a98tv_register(HINSTANCE hinst);

/* Create the control as a child of parent.  font may be NULL (uses
 * DEFAULT_GUI_FONT).  Returns NULL on failure. */
HWND a98tv_create(HINSTANCE hinst, HWND parent, int ctrl_id, HFONT font,
                  int x, int y, int w, int h);

/* Line kinds, decided from the first run of a fresh logical line. */
enum {
    A98TV_ROLE_AI = 0,     /* Assistant / plain text */
    A98TV_ROLE_YOU,        /* "You: " */
    A98TV_ROLE_TOOL,       /* "[tool] ..." - collapsible header */
    A98TV_ROLE_RESULT,     /* "[result] ..." - folded body of a tool */
    A98TV_ROLE_META,       /* "[round]", "[info]" */
    A98TV_ROLE_ERROR       /* "[error]" */
};

#ifdef __cplusplus
}
#endif

#endif /* A98_TRANSCRIPT_VIEW_H */
