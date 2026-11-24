/*
 * AIagent98 - emoji picker (see emoji_ui.h).
 *
 * One icon view of the whole pack, an incremental filter box, and an
 * "Insert" that yields ":shortcode:".  Artwork comes straight from the
 * pack's 24x24 BMPs: a 24-bit DIB with a white background, which is exactly
 * what ImageList_AddMasked() wants for a Win9x-colour mask.
 */
#include "emoji_ui.h"

#include "../core/log.h"
#include "emoji.h"
#include "emoji_table.h"
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

/* The dialog keeps PickCtx* in its DWLP_USER slot.  That slot holds a
 * POINTER: the plain SetWindowLongA/GetWindowLongA pair truncates it to
 * 32 bits on x64, and the sign-extended garbage pointer then faults the
 * process the first time insert_selected() runs.  Win9x has no
 * SetWindowLongPtrA export, so 32-bit builds keep the plain call (there a
 * LONG is pointer-sized and correct). */
#ifdef _WIN64
#define A98_DLG_GETPTR(dlg, idx) GetWindowLongPtrA(dlg, idx)
#define A98_DLG_SETPTR(dlg, idx, v) SetWindowLongPtrA(dlg, idx, (LONG_PTR)(v))
#else
#define A98_DLG_GETPTR(dlg, idx) GetWindowLongA(dlg, idx)
#define A98_DLG_SETPTR(dlg, idx, v) SetWindowLongA(dlg, idx, (LONG)(v))
#endif

/* All picker state lives in the dialog's DWLP_USER slot.  The old file-scope
 * statics made two opens leak the previous HIMAGELIST and let one dialog's
 * list view draw with another dialog's images. */
typedef struct {
    const char *exe_dir;
    char *out;
    size_t cap;
    HWND dlg;
    HWND list;
    HIMAGELIST images;
    int image_count;
    char asset_dir[A98_MAX_PATH];
    /* BMPs that fail to load are skipped, so the image list is a COMPACT
     * array: table index i maps to img_of[i], not to i.  -1 = no artwork. */
    int img_of[A98_EMOJI_PACK_COUNT];
} PickCtx;

static void load_images(PickCtx *ctx)
{
    int i;
    int count = a98_emoji_count();
    HCURSOR old = SetCursor(LoadCursorA(NULL, IDC_WAIT));

    /* Re-entrant init: never leak a previous image list. */
    if (ctx->images != NULL) {
        ImageList_Destroy(ctx->images);
        ctx->images = NULL;
    }
    ctx->image_count = 0;
    for (i = 0; i < count; i++) ctx->img_of[i] = -1;

    ctx->images = ImageList_Create(A98_EMOJI_ICON, A98_EMOJI_ICON,
                                   ILC_COLOR8 | ILC_MASK, 8, 64);
    if (ctx->images == NULL) {
        SetCursor(old);
        return;
    }
    for (i = 0; i < count; i++) {
        const A98EmojiEntry *e = a98_emoji_at(i);
        char path[A98_MAX_PATH];
        HBITMAP bmp;
        int k;

        if (e == NULL) {
            break;
        }
        if (a98_emoji_asset_file(ctx->asset_dir, e, path, sizeof(path)) != 0) {
            continue;
        }
        bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP,
                                  A98_EMOJI_ICON, A98_EMOJI_ICON,
                                  LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (bmp == NULL) {
            continue;
        }
        k = ImageList_AddMasked(ctx->images, bmp, RGB(255, 255, 255));
        if (k >= 0) {
            ctx->img_of[i] = k;
            ctx->image_count++;
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

static void fill_list(PickCtx *ctx)
{
    char filter[64];
    int i;
    int n = 0;
    int count;

    if (ctx == NULL || ctx->list == NULL || ctx->dlg == NULL) {
        return;
    }
    filter[0] = '\0';
    GetDlgItemTextA(ctx->dlg, IDC_EMOJI_FILTER, filter, sizeof(filter));
    to_lower(filter);
    count = a98_emoji_count();

    SendMessageA(ctx->list, WM_SETREDRAW, (WPARAM)FALSE, 0);
    ListView_DeleteAllItems(ctx->list);
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
        /* `i` is the pack-table index, not the image-list index: skipped
         * BMPs make the image list a compact array. */
        if (ctx->img_of[i] >= 0) {
            it.mask |= LVIF_IMAGE;
            it.iImage = ctx->img_of[i];
        }
        if (ListView_InsertItem(ctx->list, &it) >= 0) {
            n++;
        }
    }
    SendMessageA(ctx->list, WM_SETREDRAW, (WPARAM)TRUE, 0);
    InvalidateRect(ctx->list, NULL, TRUE);
}

static void insert_selected(PickCtx *ctx)
{
    LVITEMA it;
    int sel;
    const A98EmojiEntry *e;

    /* The probe path creates the dialog with no output buffer; inserting is
     * meaningless there, so bail out instead of writing to NULL. */
    if (ctx == NULL || ctx->list == NULL || ctx->out == NULL || ctx->cap == 0) {
        return;
    }
    sel = ListView_GetNextItem(ctx->list, -1, LVNI_SELECTED);
    if (sel < 0) {
        return;
    }
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(ctx->list, &it)) {
        return;
    }
    e = a98_emoji_at((int)it.lParam);
    if (e == NULL) {
        return;
    }
    snprintf(ctx->out, ctx->cap, ":%s:", e->shortcode);
    EndDialog(ctx->dlg, 1);
}

static INT_PTR CALLBACK emoji_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PickCtx *ctx = (PickCtx *)A98_DLG_GETPTR(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        ctx = (PickCtx *)lp;
        A98_DLG_SETPTR(dlg, DWLP_USER, ctx);
        {
            int i;
            for (i = 0; i < A98_EMOJI_PACK_COUNT; i++) ctx->img_of[i] = -1;
        }
        ctx->dlg = dlg;
        ctx->image_count = 0;
        ctx->asset_dir[0] = '\0';
        ctx->list = GetDlgItem(dlg, IDC_EMOJI_LIST);
        if (a98_emoji_asset_dir(ctx->exe_dir, ctx->asset_dir,
                                sizeof(ctx->asset_dir))) {
            load_images(ctx);
        }
        if (ctx->images != NULL && ctx->image_count > 0) {
            ListView_SetImageList(ctx->list, ctx->images, LVSIL_NORMAL);
        }
        fill_list(ctx);
        {
            char hint[160];

            if (ctx->image_count > 0) {
                snprintf(hint, sizeof(hint), "%d pictures from %s",
                         ctx->image_count, ctx->asset_dir);
            } else {
                snprintf(hint, sizeof(hint),
                         "No pictures found - unzip the pack's "
                         "assets\\bmp\\24 as emoji\\bmp\\24 next to the EXE. "
                         "Names work without it.");
            }
            SetDlgItemTextA(dlg, IDC_EMOJI_HINT, hint);
        }
        SetFocus(ctx->list);
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
                fill_list(ctx);
            }
            return TRUE;
        default:
            break;
        }
        return TRUE;

    case WM_NOTIFY:
        /* Other controls can send WM_NOTIFY to the dialog; only the pack
         * list's double-click means "insert". */
        if (lp != 0 && ((NMHDR *)lp)->idFrom == IDC_EMOJI_LIST &&
            ((NMHDR *)lp)->code == LVN_DBLCLK) {
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
    memset(&ctx, 0, sizeof(ctx));
    ctx.exe_dir = exe_dir;
    ctx.out = out;
    ctx.cap = cap;

    rc = DialogBoxParamA(hinst, MAKEINTRESOURCEA(IDD_EMOJI), owner,
                         emoji_proc, (LPARAM)&ctx);

    /* The list view carries LVS_SHAREIMAGELISTS, so the image list is ours to
     * free - and only after the dialog/control that draws it is gone. */
    if (ctx.images != NULL) {
        ImageList_Destroy(ctx.images);
        ctx.images = NULL;
    }
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
    memset(&ctx, 0, sizeof(ctx));
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
        *images = ctx.image_count;
    }
    if (items != NULL && ctx.list != NULL) {
        *items = (int)SendMessageA(ctx.list, LVM_GETITEMCOUNT, 0, 0);
    }
    if (dir != NULL && dir_cap > 0) {
        strncpy(dir, ctx.asset_dir, dir_cap - 1);
        dir[dir_cap - 1] = '\0';
    }

    DestroyWindow(dlg);
    /* The list view carries LVS_SHAREIMAGELISTS, so the image list is ours to
     * free - and only after the control that draws it is gone. */
    if (ctx.images != NULL) {
        ImageList_Destroy(ctx.images);
        ctx.images = NULL;
    }
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
    /* IID_IRichEditOleCallback = {00020D03-0000-0000-C000-000000000046}.
     * Declared locally so very old Win9x headers need not export the symbol. */
    static const IID iid_iricholecb = {
        0x00020D03UL, 0x0000, 0x0000,
        { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
    };

    if (out == NULL) return E_POINTER;
    *out = NULL;
    if (iid == NULL) return E_NOINTERFACE;
    if (memcmp(iid, &IID_IUnknown, sizeof(IID)) == 0 ||
        memcmp(iid, &iid_iricholecb, sizeof(IID)) == 0) {
        *out = me;
        me->refs++;
        return S_OK;
    }
    return E_NOINTERFACE;
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
    ILockBytes *lock = NULL;
    HRESULT hr;

    (void)me;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    /* A docfile layered on an HGLOBAL-backed ILockBytes is the in-memory
     * storage that works on Windows 9x as well as NT; StgCreateDocfile(NULL,
     * ...) is not reliable there. */
    hr = CreateILockBytesOnHGlobal(NULL, TRUE, &lock);
    if (FAILED(hr) || lock == NULL) {
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    hr = StgCreateDocfileOnILockBytes(lock,
                                      STGM_CREATE | STGM_READWRITE |
                                          STGM_SHARE_EXCLUSIVE | STGM_DIRECT,
                                      0, out);
    lock->lpVtbl->Release(lock);
    return hr;
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

/* Hex form of a one-record metafile that draws the BMP at the inline size.
 *
 * SetDIBitsToDevice() into a metafile DC records META_SETDIBTODEV but drops
 * the DIB bits on NT, so playback is blank.  A DIB-section source DC blitted
 * into the metafile records META_DIBBITBLT with the pixel bits embedded,
 * which is the form Riched20 actually paints.  Map the 24px art down to the
 * inline size through a 1:1 window/viewport extent so the display size is
 * right.  The RTF header's \picw/\pich describe the metafile's size in
 * 0.01mm/HIMETRIC units; \picwgoal/\pichgoal (twips) set the display size. */
static char *pic_metafile_hex(const char *path)
{
    static const char digits[] = "0123456789abcdef";
    HBITMAP bmp;
    DIBSECTION ds;
    HDC mf;
    HDC src;
    HBITMAP old;
    HMETAFILE hmf;
    HGLOBAL h;
    UINT size;
    const unsigned char *bits;
    char *hex;
    size_t i;
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
    hgt = (int)ds.dsBmih.biHeight;
    if (hgt < 0) {
        hgt = -hgt;
    }
    if (ds.dsBmih.biWidth <= 0 || hgt <= 0) {
        DeleteObject(bmp);
        return NULL;
    }

    src = CreateCompatibleDC(NULL);
    if (src == NULL) {
        DeleteObject(bmp);
        return NULL;
    }
    old = (HBITMAP)SelectObject(src, bmp);

    mf = CreateMetaFileA(NULL);
    if (mf != NULL) {
        SetMapMode(mf, MM_ANISOTROPIC);
        SetWindowExtEx(mf, A98_EMOJI_INLINE_PX, A98_EMOJI_INLINE_PX, NULL);
        SetViewportExtEx(mf, A98_EMOJI_INLINE_PX, A98_EMOJI_INLINE_PX, NULL);
        BitBlt(mf, 0, 0, (int)ds.dsBmih.biWidth, hgt, src, 0, 0, SRCCOPY);
        hmf = CloseMetaFile(mf);
    } else {
        hmf = NULL;
    }
    if (old != NULL) {
        SelectObject(src, old);
    }
    DeleteDC(src);
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
    if (bits == NULL) {
        DeleteMetaFile(hmf);
        GlobalFree(h);
        return NULL;
    }
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
    /* \picw/\pich are in hundredths of a millimetre (0.01mm / HIMETRIC):
     * 16px at 96dpi is ~423 units.  The old value of 16 described a
     * 0.16mm picture, so RichEdit reserved \picwgoal space but scaled the
     * metafile down to a sub-pixel smudge.  \picwgoal/\pichgoal are twips
     * and set the actual displayed size. */
    {
        int hm = A98_EMOJI_INLINE_PX * 2540 / 96;
        int goal = A98_EMOJI_INLINE_PX * A98_EMOJI_TWIPS_PER_PX;
        snprintf(head, sizeof(head),
                 "{\\rtf1\\ansi\\deff0{\\pict\\wmetafile8"
                 "\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d ",
                 hm, hm, goal, goal);
    }
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
    /* Two log lines around the one call that can take the control down on a
     * 9x box: if the log stops between them, RichEdit/OLE is the fault. */
    log_info("emoji picture %s: streaming %lu chars of RTF", e->id,
             (unsigned long)strlen(rtf));
    SendMessageA(edit, EM_STREAMIN, SF_RTF | SFF_SELECTION, (LPARAM)&es);
    log_info("emoji picture %s: streamin done, dwError %lu", e->id,
             (unsigned long)es.dwError);
    a98_free(rtf);
    return es.dwError == 0 ? 1 : 0;
}
