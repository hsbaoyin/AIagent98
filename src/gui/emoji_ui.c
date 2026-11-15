/*
 * AIagent98 - emoji picker (see emoji_ui.h).
 *
 * One icon view of the whole pack, an incremental filter box, and an
 * "Insert" that yields ":shortcode:".  Artwork comes straight from the
 * pack's 24x24 BMPs: a 24-bit DIB with a white background, which is exactly
 * what ImageList_AddMasked() wants for a Win9x-colour mask.
 */
#include "emoji_ui.h"

#include "emoji.h"
#include "resource.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#ifndef LVN_DBLCLK
#define LVN_DBLCLK (LVN_FIRST - 1)
#endif
/* Win95-era headers only carry the 16-bit name DWL_USER. */
#ifndef DWLP_USER
#define DWLP_USER 0x0008
#endif

/* Matches the BMP size the pack ships; 16 would be sharper on VGA but the
 * list is easier to hit with 24. */
#define A98_EMOJI_ICON 24

typedef struct {
    const char *exe_dir;
    char *out;
    size_t cap;
} PickCtx;

static HIMAGELIST g_images;
static HWND g_dlg;
static HWND g_list;
static int g_image_count;
static char g_asset_dir[A98_MAX_PATH];

/* Index i in the image list is index i in the pack table, so a list item
 * only has to remember its table index. */
static void load_images(void)
{
    int i;
    int count = a98_emoji_count();
    HCURSOR old = SetCursor(LoadCursorA(NULL, IDC_WAIT));

    g_images = ImageList_Create(A98_EMOJI_ICON, A98_EMOJI_ICON,
                                ILC_COLOR8 | ILC_MASK, 8, 64);
    if (g_images == NULL) {
        SetCursor(old);
        return;
    }
    for (i = 0; i < count; i++) {
        const A98EmojiEntry *e = a98_emoji_at(i);
        char path[A98_MAX_PATH];
        HBITMAP bmp;

        if (e == NULL) {
            break;
        }
        if (a98_emoji_asset_file(g_asset_dir, e, path, sizeof(path)) != 0) {
            continue;
        }
        bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP,
                                  A98_EMOJI_ICON, A98_EMOJI_ICON,
                                  LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (bmp == NULL) {
            continue;
        }
        if (ImageList_AddMasked(g_images, bmp, RGB(255, 255, 255)) >= 0) {
            g_image_count++;
        }
        DeleteObject(bmp);
    }
    SetCursor(old);
}

static void to_lower(char *s)
{
    int i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            s[i] = (char)(s[i] - 'A' + 'a');
        }
    }
}

static void fill_list(void)
{
    char filter[64];
    int i;
    int n = 0;
    int count;

    if (g_list == NULL || g_dlg == NULL) {
        return;
    }
    filter[0] = '\0';
    GetDlgItemTextA(g_dlg, IDC_EMOJI_FILTER, filter, sizeof(filter));
    to_lower(filter);
    count = a98_emoji_count();

    SendMessageA(g_list, WM_SETREDRAW, (WPARAM)FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (i = 0; i < count; i++) {
        const A98EmojiEntry *e = a98_emoji_at(i);
        LVITEMA it;

        if (e == NULL) {
            continue;
        }
        /* Shortcodes are generated lowercase, so one lowercase copy of the
         * filter is enough for a case-insensitive substring match. */
        if (filter[0] != '\0' && strstr(e->shortcode, filter) == NULL) {
            continue;
        }
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = n;
        it.pszText = (char *)e->shortcode;
        it.lParam = (LPARAM)i;
        if (g_image_count > 0) {
            it.mask |= LVIF_IMAGE;
            it.iImage = i;
        }
        if (ListView_InsertItem(g_list, &it) >= 0) {
            n++;
        }
    }
    SendMessageA(g_list, WM_SETREDRAW, (WPARAM)TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
}

static void insert_selected(PickCtx *ctx)
{
    LVITEMA it;
    int sel;
    const A98EmojiEntry *e;

    /* The probe path creates the dialog with no output buffer; inserting is
     * meaningless there, so bail out instead of writing to NULL. */
    if (ctx == NULL || g_list == NULL || ctx->out == NULL || ctx->cap == 0) {
        return;
    }
    sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0) {
        return;
    }
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(g_list, &it)) {
        return;
    }
    e = a98_emoji_at((int)it.lParam);
    if (e == NULL) {
        return;
    }
    snprintf(ctx->out, ctx->cap, ":%s:", e->shortcode);
    EndDialog(g_dlg, 1);
}

static INT_PTR CALLBACK emoji_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PickCtx *ctx = (PickCtx *)GetWindowLongA(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        ctx = (PickCtx *)lp;
        SetWindowLongA(dlg, DWLP_USER, (LONG)ctx);
        g_dlg = dlg;
        g_image_count = 0;
        g_asset_dir[0] = '\0';
        g_list = GetDlgItem(dlg, IDC_EMOJI_LIST);
        if (a98_emoji_asset_dir(ctx->exe_dir, g_asset_dir,
                                sizeof(g_asset_dir))) {
            load_images();
        }
        if (g_images != NULL && g_image_count > 0) {
            ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
        }
        fill_list();
        {
            char hint[160];

            if (g_image_count > 0) {
                snprintf(hint, sizeof(hint), "%d pictures from %s",
                         g_image_count, g_asset_dir);
            } else {
                snprintf(hint, sizeof(hint),
                         "No pictures found - unzip the pack's "
                         "assets\\bmp\\24 as emoji\\bmp\\24 next to the EXE. "
                         "Names work without it.");
            }
            SetDlgItemTextA(dlg, IDC_EMOJI_HINT, hint);
        }
        SetFocus(g_list);
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            insert_selected(ctx);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        case IDC_EMOJI_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                fill_list();
            }
            return TRUE;
        default:
            break;
        }
        return TRUE;

    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == LVN_DBLCLK) {
            insert_selected(ctx);
            return TRUE;
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

int a98_emoji_pick(HINSTANCE hinst, HWND owner, const char *exe_dir,
                   char *out, size_t cap)
{
    PickCtx ctx;
    INT_PTR rc;

    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    ctx.exe_dir = exe_dir;
    ctx.out = out;
    ctx.cap = cap;

    rc = DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_EMOJI), owner,
                         emoji_proc, (LPARAM)&ctx);

    if (g_images != NULL) {
        ImageList_Destroy(g_images);
        g_images = NULL;
    }
    g_dlg = NULL;
    g_list = NULL;
    g_image_count = 0;
    return (rc == 1 && out[0] != '\0') ? 1 : 0;
}

int a98_emoji_dialog_probe(HINSTANCE hinst, const char *exe_dir, int *images,
                           int *items, char *dir, size_t dir_cap)
{
    PickCtx ctx;
    HWND dlg;

    if (images != NULL) {
        *images = 0;
    }
    if (items != NULL) {
        *items = 0;
    }
    if (dir != NULL && dir_cap > 0) {
        dir[0] = '\0';
    }
    ctx.exe_dir = exe_dir;
    ctx.out = NULL;
    ctx.cap = 0;

    /* CreateDialogParamA sends WM_INITDIALOG synchronously, so by the time it
     * returns the list view is already populated. */
    dlg = CreateDialogParamA(hinst, MAKEINTRESOURCEA(IDD_EMOJI), NULL,
                             emoji_proc, (LPARAM)&ctx);
    if (dlg == NULL) {
        return 0;
    }
    if (images != NULL) {
        *images = g_image_count;
    }
    if (items != NULL && g_list != NULL) {
        *items = (int)SendMessageA(g_list, LVM_GETITEMCOUNT, 0, 0);
    }
    if (dir != NULL && dir_cap > 0) {
        strncpy(dir, g_asset_dir, dir_cap - 1);
        dir[dir_cap - 1] = '\0';
    }

    DestroyWindow(dlg);
    /* The list view carries LVS_SHAREIMAGELISTS, so the image list is ours to
     * free - and only after the control that draws it is gone. */
    if (g_images != NULL) {
        ImageList_Destroy(g_images);
        g_images = NULL;
    }
    g_dlg = NULL;
    g_list = NULL;
    g_image_count = 0;
    return 1;
}
