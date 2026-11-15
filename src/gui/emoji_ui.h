/*
 * AIagent98 - emoji picker for the input line.
 *
 * Win9x cannot type an emoji (the edit control is ANSI), so the picker
 * shows the pack's BMP artwork and inserts the matching ":shortcode:".
 * do_send() expands the shortcode back into the real emoji before the
 * request leaves the machine.
 */
#ifndef A98_EMOJI_UI_H
#define A98_EMOJI_UI_H

#include <windows.h>

#include "base.h"

/* Show the modal picker.  On OK, writes ":shortcode:" into out and returns
 * 1; returns 0 when cancelled.  The picker still works without artwork
 * (the list then shows names only). */
int a98_emoji_pick(HINSTANCE hinst, HWND owner, const char *exe_dir,
                   char *out, size_t cap);

/* Create IDD_EMOJI without showing it, report what WM_INITDIALOG produced
 * (icon count, list rows, the asset directory it settled on) and destroy it
 * again.  Used by tests/test_emoji.c so the dialog template, the list view and
 * the artwork loading are covered without a human clicking anything.
 * Returns 1 when the dialog was created, 0 when CreateDialogParam failed
 * (no window station, or a broken template) - check GetLastError then. */
int a98_emoji_dialog_probe(HINSTANCE hinst, const char *exe_dir, int *images,
                           int *items, char *dir, size_t dir_cap);

#endif /* A98_EMOJI_UI_H */
