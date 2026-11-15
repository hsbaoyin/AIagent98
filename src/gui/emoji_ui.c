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
#include <ole2.h>
#include <richedit.h>
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

/* ------------------------------------------------------------------ */
/* Inline pictures in the transcript                                   */
/* ------------------------------------------------------------------ */
/*
 * RichEdit builds an object out of an RTF \pict group and asks the host where
 * to keep it through IRichEditOleCallback::GetNewStorage.  With no callback
 * it parses the group and throws it away without an error - which is why the
 * transcript used to be text only.  An in-memory docfile per picture is all
 * it needs.
 *
 * The picture itself is a bare Windows metafile: a METAHEADER followed by
 * one SetDIBitsToDevice record, no Aldus "placeable" prefix (WordPad writes
 * the bare form too; RichEdit rejects a prefixed one).  It is recorded at
 * A98_EMOJI_INLINE_PX rather than the pack's 24, so the icon fits the 9pt
 * transcript line instead of doubling its height.
 */

#define A98_EMOJI_INLINE_PX    16
#define A98_EMOJI_TWIPS_PER_PX 15

#if defined(_WIN64)
typedef DWORD_PTR A98Cookie;
#else
typedef DWORD A98Cookie;
#endif

/* The control reads the RTF through this; one picture is a few kilobytes. */
typedef struct {
    const char *p;
    size_t len;
    size_t pos;
} PicStream;

static DWORD CALLBACK pic_stream_cb(A98Cookie cookie, LPBYTE buf, LONG cb,
                                    LONG *pcb)
{
    PicStream *s = (PicStream *)cookie;
    size_t left = s->len - s->pos;
    size_t n = left;

    if (cb > 0 && (size_t)cb < left) {
        n = (size_t)cb;
    }
    memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    *pcb = (LONG)n;
    return 0;
}

/* ---- IRichEditOleCallback ---------------------------------------- */
/* Only GetNewStorage matters here; the rest is the minimum a C vtable has
 * to carry so RichEdit can call through it. */

typedef struct PicOle PicOle;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PicOle *, const IID *const,
                                                void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(PicOle *);
    ULONG (STDMETHODCALLTYPE *Release)(PicOle *);
    HRESULT (STDMETHODCALLTYPE *GetNewStorage)(PicOle *, IStorage **);
    HRESULT (STDMETHODCALLTYPE *GetInPlaceContext)(PicOle *, void **, void **,
                                                   void **);
    HRESULT (STDMETHODCALLTYPE *ShowContainerUI)(PicOle *, BOOL);
    HRESULT (STDMETHODCALLTYPE *QueryAcceptData)(PicOle *, IDataObject *,
                                                 CLIPFORMAT *, DWORD, BOOL,
                                                 void *);
    HRESULT (STDMETHODCALLTYPE *DeleteObject)(PicOle *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *GetClipboardData)(PicOle *, void **,
                                                  CLIPFORMAT, void **);
    HRESULT (STDMETHODCALLTYPE *GetDragDropEffect)(PicOle *, BOOL, DWORD,
                                                   DWORD *);
    HRESULT (STDMETHODCALLTYPE *GetContextMenu)(PicOle *, WORD, IUnknown *,
                                                void **);
} PicOleVtbl;

struct PicOle {
    const PicOleVtbl *vtbl;
    long refs;
};

static HRESULT STDMETHODCALLTYPE pic_qi(PicOle *me, const IID *const iid,
                                        void **out)
{
    (void)iid;
    *out = me;
    me->refs++;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE pic_addref(PicOle *me)
{
    return (ULONG)++me->refs;
}

static ULONG STDMETHODCALLTYPE pic_release(PicOle *me)
{
    return (ULONG)--me->refs;
}

static HRESULT STDMETHODCALLTYPE pic_new_storage(PicOle *me, IStorage **out)
{
    (void)me;
    /* NULL name: a docfile that lives in memory, which is exactly what an
     * inline icon needs. */
    return StgCreateDocfile(NULL,
                            STGM_READWRITE | STGM_SHARE_EXCLUSIVE |
                                STGM_CREATE | STGM_DIRECT,
                            0, out);
}

static HRESULT STDMETHODCALLTYPE pic_inplace(PicOle *me, void **a, void **b,
                                             void **c)
{
    (void)me;
    (void)a;
    (void)b;
    (void)c;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE pic_show(PicOle *me, BOOL show)
{
    (void)me;
    (void)show;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pic_accept(PicOle *me, IDataObject *data,
                                            CLIPFORMAT *cf, DWORD effect,
                                            BOOL accept, void *src)
{
    (void)me;
    (void)data;
    (void)cf;
    (void)effect;
    (void)accept;
    (void)src;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pic_delete(PicOle *me, IUnknown *obj)
{
    (void)me;
    (void)obj;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE pic_clipboard(PicOle *me, void **data,
                                               CLIPFORMAT cf, void **res)
{
    (void)me;
    (void)data;
    (void)cf;
    (void)res;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE pic_dnd(PicOle *me, BOOL from, DWORD keys,
                                         DWORD *effect)
{
    (void)me;
    (void)from;
    (void)keys;
    *effect = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pic_menu(PicOle *me, WORD sel,
                                          IUnknown *obj, void **hmenu)
{
    (void)me;
    (void)sel;
    (void)obj;
    (void)hmenu;
    return E_NOTIMPL;
}

static const PicOleVtbl g_pic_vtbl = {
    pic_qi,     pic_addref,  pic_release, pic_new_storage, pic_inplace,
    pic_show,   pic_accept,  pic_delete,  pic_clipboard,   pic_dnd,
    pic_menu,
};

/* One shared instance; the control only borrows it. */
static PicOle g_pic_ole = { &g_pic_vtbl, 1 };

int a98_emoji_ole_attach(HWND edit)
{
    if (edit == NULL) {
        return 0;
    }
    return SendMessageA(edit, EM_SETOLECALLBACK, 0,
                        (LPARAM)&g_pic_ole) ? 1 : 0;
}

/* ---- picture build ----------------------------------------------- */

/* Resolved once per EXE directory: the transcript asks for one picture per
 * emoji, and the pack search walks five directories per base. */
static char g_pic_dir[A98_MAX_PATH];
static char g_pic_base[A98_MAX_PATH];
static int g_pic_state; /* 0 = unknown, 1 = found, -1 = missing */

static const char *pic_dir(const char *exe_dir)
{
    const char *base = exe_dir != NULL ? exe_dir : "";

    if (g_pic_state != 0 && strcmp(base, g_pic_base) == 0) {
        return g_pic_state > 0 ? g_pic_dir : NULL;
    }
    strncpy(g_pic_base, base, sizeof(g_pic_base) - 1);
    g_pic_base[sizeof(g_pic_base) - 1] = '\0';
    g_pic_dir[0] = '\0';
    g_pic_state = a98_emoji_asset_dir(exe_dir, g_pic_dir,
                                      sizeof(g_pic_dir))
                      ? 1
                      : -1;
    return g_pic_state > 0 ? g_pic_dir : NULL;
}

/* Hex form of a one-record metafile that draws the BMP at the inline size. */
static char *pic_metafile_hex(const char *path)
{
    static const char digits[] = "0123456789abcdef";
    HBITMAP bmp;
    DIBSECTION ds;
    BITMAPINFO bmi;
    HDC mf;
    HMETAFILE hmf;
    HGLOBAL h;
    UINT size;
    const unsigned char *bits;
    char *hex;
    size_t i;
    int w;
    int hgt;

    bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0,
                              LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (bmp == NULL) {
        return NULL;
    }
    memset(&ds, 0, sizeof(ds));
    if (!GetObjectA(bmp, sizeof(ds), &ds) || ds.dsBm.bmBits == NULL) {
        DeleteObject(bmp);
        return NULL;
    }
    w = (int)ds.dsBmih.biWidth;
    hgt = (int)ds.dsBmih.biHeight;
    if (hgt < 0) {
        hgt = -hgt;
    }
    if (w <= 0 || hgt <= 0) {
        DeleteObject(bmp);
        return NULL;
    }
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader = ds.dsBmih;
    bmi.bmiHeader.biHeight = hgt;

    mf = CreateMetaFileA(NULL);
    if (mf == NULL) {
        DeleteObject(bmp);
        return NULL;
    }
    SetDIBitsToDevice(mf, 0, 0, A98_EMOJI_INLINE_PX, A98_EMOJI_INLINE_PX, 0,
                      0, 0, hgt, ds.dsBm.bmBits, &bmi, DIB_RGB_COLORS);
    hmf = CloseMetaFile(mf);
    DeleteObject(bmp);
    if (hmf == NULL) {
        return NULL;
    }

    size = GetMetaFileBitsEx(hmf, 0, NULL);
    if (size == 0) {
        DeleteMetaFile(hmf);
        return NULL;
    }
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (h == NULL) {
        DeleteMetaFile(hmf);
        return NULL;
    }
    bits = (const unsigned char *)GlobalLock(h);
    GetMetaFileBitsEx(hmf, size, (LPVOID)bits);
    DeleteMetaFile(hmf);

    hex = (char *)a98_alloc((size_t)size * 2 + 1);
    if (hex == NULL) {
        GlobalUnlock(h);
        GlobalFree(h);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        hex[i * 2] = digits[bits[i] >> 4];
        hex[i * 2 + 1] = digits[bits[i] & 15];
    }
    hex[(size_t)size * 2] = '\0';
    GlobalUnlock(h);
    GlobalFree(h);
    return hex;
}

char *a98_emoji_picture_rtf(const char *exe_dir, const A98EmojiEntry *e)
{
    const char *dir;
    char path[A98_MAX_PATH];
    char head[160];
    char *hex;
    char *out;
    size_t cap;

    dir = pic_dir(exe_dir);
    if (dir == NULL || e == NULL) {
        return NULL;
    }
    if (a98_emoji_asset_file(dir, e, path, sizeof(path)) != 0) {
        return NULL;
    }
    hex = pic_metafile_hex(path);
    if (hex == NULL) {
        return NULL;
    }
    snprintf(head, sizeof(head),
             "{\\rtf1\\ansi\\deff0{\\pict\\wmetafile8"
             "\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d ",
             A98_EMOJI_INLINE_PX, A98_EMOJI_INLINE_PX,
             A98_EMOJI_INLINE_PX * A98_EMOJI_TWIPS_PER_PX,
             A98_EMOJI_INLINE_PX * A98_EMOJI_TWIPS_PER_PX);
    cap = strlen(head) + strlen(hex) + 3;
    out = (char *)a98_alloc(cap);
    if (out != NULL) {
        snprintf(out, cap, "%s%s}}", head, hex);
    }
    a98_free(hex);
    return out;
}

int a98_emoji_insert_picture(HWND edit, const char *exe_dir,
                             const A98EmojiEntry *e)
{
    char *rtf;
    EDITSTREAM es;
    PicStream in;

    if (edit == NULL) {
        return 0;
    }
    rtf = a98_emoji_picture_rtf(exe_dir, e);
    if (rtf == NULL) {
        return 0;
    }
    memset(&in, 0, sizeof(in));
    in.p = rtf;
    in.len = strlen(rtf);
    memset(&es, 0, sizeof(es));
    es.dwCookie = (A98Cookie)(size_t)&in;
    es.pfnCallback = (EDITSTREAMCALLBACK)pic_stream_cb;
    SendMessageA(edit, EM_STREAMIN, SF_RTF | SFF_SELECTION, (LPARAM)&es);
    a98_free(rtf);
    return es.dwError == 0 ? 1 : 0;
}
