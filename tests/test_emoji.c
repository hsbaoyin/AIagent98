/*
 * Host-side smoke test for the emoji pack pipeline.
 *
 * Covers what the GUI depends on:
 *   - the code point -> shortcode lookup and the fold/expand round trip that
 *     keeps emoji out of the ANSI edit controls
 *   - asset location: the same search the picker runs, starting from this
 *     executable's directory (build\cmake or build\host), which is how a build
 *     tree finds the tracked artwork in the repository's assets\emoji
 *   - the release layout (<dir>\emoji\bmp\24) via a tiny fixture
 *   - the degradation path when no artwork exists at all
 *   - IDD_EMOJI itself: created with CreateDialogParam, populated, destroyed
 *
 * Build: cmake --build build/cmake --target test_emoji
 * (gui.rc is linked in so the dialog template exists.)
 */

#include "gui/emoji.h"
#include "gui/emoji_ui.h"
#include "core/path.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <richedit.h>

static int failures = 0;

static void expect(const char *what, const char *got, const char *want)
{
    if (got == NULL || strcmp(got, want) != 0) {
        printf("FAIL: %s\n  want: %s\n  got : %s\n",
               what, want, got != NULL ? got : "(null)");
        failures++;
    } else {
        printf("PASS: %s\n", what);
    }
}

static void ok(int cond, const char *what)
{
    if (cond) {
        printf("PASS: %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* Directory the running test lives in; the GUI derives its own the same way
 * (a98_path_exe_dir in main_gui.c). */
static void exe_here(char *out, size_t cap)
{
    char path[A98_MAX_PATH];

    path[0] = '\0';
    GetModuleFileNameA(NULL, path, sizeof(path));
    a98_path_dirname(path, out, cap);
}

/* LoadImage at the size the picker asks for; proves the file is really usable
 * and not just present. */
static int loads_as_24x24(const char *path)
{
    HBITMAP bmp;
    BITMAP bm;
    int good = 0;

    bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 24, 24,
                              LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (bmp == NULL) {
        printf("  (LoadImage failed for %s, error %lu)\n", path,
               (unsigned long)GetLastError());
        return 0;
    }
    if (GetObject(bmp, sizeof(bm), &bm) && bm.bmWidth == 24 &&
        bm.bmHeight == 24 && bm.bmBitsPixel == 24) {
        good = 1;
    } else {
        printf("  (%s is %dx%d/%d bpp)\n", path, (int)bm.bmWidth,
               (int)bm.bmHeight, (int)bm.bmBitsPixel);
    }
    DeleteObject(bmp);
    return good;
}

static void test_text_pipeline(void)
{
    const A98EmojiEntry *e;

    if (a98_emoji_count() != 1219) {
        printf("FAIL: pack has %d entries, expected 1219\n",
               a98_emoji_count());
        failures++;
    } else {
        printf("PASS: pack has 1219 entries\n");
    }

    e = a98_emoji_by_codepoint(0x1F600UL);
    expect("U+1F600 shortcode", e != NULL ? e->shortcode : NULL,
           "grinning_face");
    expect("U+1F600 asset id", e != NULL ? e->id : NULL, "A98E0672");

    e = a98_emoji_by_shortcode("white_heavy_check_mark");
    if (e == NULL || e->cp != 0x2705UL) {
        printf("FAIL: :white_heavy_check_mark: did not resolve to U+2705\n");
        failures++;
    } else {
        printf("PASS: :white_heavy_check_mark: resolves to U+2705\n");
    }
    ok(a98_emoji_by_codepoint(0x1FAF9UL) == NULL &&
           a98_emoji_by_shortcode("no_such_emoji") == NULL,
       "unknown code point and shortcode stay unknown");

    /* U+2705 WHITE HEAVY CHECK MARK, the classic "\xE2\x9C\x85". */
    {
        char *folded = a98_emoji_fold("ok \xE2\x9C\x85");

        expect("fold check mark", folded, "ok :white_heavy_check_mark:");
        a98_free(folded);
    }
    /* An emoji followed by VS16: the selector must not leak into the text. */
    {
        char *folded = a98_emoji_fold("a\xF0\x9F\x98\x80\xEF\xB8\x8F!");

        expect("fold drops VS16", folded, "a:grinning_face:!");
        a98_free(folded);
    }
    /* An emoji the pack does not carry still must not become '?'. */
    {
        char *folded = a98_emoji_fold("\xF0\x9F\xAB\xB9");

        expect("fold unknown emoji", folded, "[U+1FAF9]");
        a98_free(folded);
    }
    /* Plain text, CJK and colons are untouched; invalid UTF-8 passes through
     * byte for byte so ACP text from the INI survives. */
    {
        char *folded = a98_emoji_fold("12:30 \xE4\xB8\xAD\xE6\x96\x87");

        expect("fold leaves text alone", folded,
               "12:30 \xE4\xB8\xAD\xE6\x96\x87");
        a98_free(folded);
    }
    {
        char *folded = a98_emoji_fold("\x81");

        expect("fold keeps invalid bytes", folded, "\x81");
        a98_free(folded);
    }
    {
        char *expanded = a98_emoji_expand(":grinning_face: hi");

        expect("expand to the real emoji", expanded, "\xF0\x9F\x98\x80 hi");
        a98_free(expanded);
    }
    {
        char *expanded = a98_emoji_expand("12:30 and :not_an_emoji:");

        expect("expand leaves other colons alone", expanded,
               "12:30 and :not_an_emoji:");
        a98_free(expanded);
    }
    /* The pair must round-trip: what the user picks is what the model gets,
     * and what comes back is what the transcript shows. */
    {
        char *expanded = a98_emoji_expand(":thumbs_up_sign:");
        char *folded = expanded != NULL ? a98_emoji_fold(expanded) : NULL;

        expect("round trip", folded, ":thumbs_up_sign:");
        a98_free(folded);
        a98_free(expanded);
    }
}

/* One BMP per table entry is what makes the artwork complete; counting the
 * folder proves a fresh clone ships the whole pack subset and not a sample. */
static int count_bmps(const char *dir)
{
    char pattern[A98_MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    if (a98_path_join(pattern, sizeof(pattern), dir, "*.bmp") != 0) {
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* The asset search as the running GUI performs it, from this build directory
 * with the real working directory. */
static void test_asset_location(char *dir_out, size_t cap)
{
    char exe_dir[A98_MAX_PATH];
    char dir[A98_MAX_PATH];
    char path[A98_MAX_PATH];
    const A98EmojiEntry *e;

    exe_here(exe_dir, sizeof(exe_dir));
    printf("  exe dir: %s\n", exe_dir);

    dir[0] = '\0';
    ok(a98_emoji_asset_dir(exe_dir, dir, sizeof(dir)) == 1,
       "asset directory found from the build tree");
    ok(a98_path_is_dir(dir), "the reported asset directory exists");
    printf("  assets : %s\n", dir);
    /* Guard against a false positive: it has to be the tracked 24 px folder,
     * not just any directory that happens to hold bitmaps. */
    ok(strstr(dir, "assets\\emoji\\bmp\\24") != NULL,
       "asset directory is the repository's assets\\emoji\\bmp\\24");
    {
        int n = count_bmps(dir);

        printf("  files  : %d BMPs, table has %d entries\n", n,
               a98_emoji_count());
        ok(n > 0 && n == a98_emoji_count(),
           "the asset directory holds exactly one BMP per table entry");
    }

    e = a98_emoji_by_codepoint(0x1F600UL);
    ok(e != NULL &&
           a98_emoji_asset_file(dir, e, path, sizeof(path)) == 0 &&
           a98_path_exists(path),
       "the resolved directory holds the BMP for U+1F600");
    if (a98_path_exists(path)) {
        printf("  bitmap : %s\n", path);
        ok(loads_as_24x24(path), "that BMP loads as 24x24/24bpp");
    } else {
        failures++;
    }

    /* The EXE walk must not depend on where the process was started from. */
    {
        char other[A98_MAX_PATH];

        other[0] = '\0';
        ok(a98_emoji_asset_dir_from(exe_dir, "C:\\a98_no_such_cwd", other,
                                    sizeof(other)) == 1,
           "location works without help from the CWD");
        expect("same directory either way", other, dir);
    }

    if (dir_out != NULL && cap > 0) {
        strncpy(dir_out, dir, cap - 1);
        dir_out[cap - 1] = '\0';
    }
}

/* A release install keeps the artwork next to the EXE as emoji\bmp\24.  Build
 * that layout under build/ from one real pack file and let the search find it
 * with no working-directory help. */
static void test_release_layout(const char *pack_dir)
{
    char src[A98_MAX_PATH];
    char dst[A98_MAX_PATH];
    char dir[A98_MAX_PATH];
    char path[A98_MAX_PATH];
    const char *root = "build\\emoji_asset_probe";
    const A98EmojiEntry *e = a98_emoji_by_shortcode("grinning_face");

    if (pack_dir == NULL || e == NULL) {
        printf("SKIP: release layout fixture (no pack)\n");
        return;
    }
    if (a98_emoji_asset_file(pack_dir, e, src, sizeof(src)) != 0 ||
        !a98_path_exists(src)) {
        printf("SKIP: release layout fixture (%s missing)\n", src);
        return;
    }
    a98_path_join(dst, sizeof(dst), root, "emoji\\bmp\\24");
    if (a98_path_mkdirs(dst) != 0) {
        printf("SKIP: cannot create %s\n", dst);
        return;
    }
    a98_path_join(path, sizeof(path), dst, "A98E0672.bmp");
    if (!CopyFileA(src, path, FALSE) && !a98_path_exists(path)) {
        printf("SKIP: cannot copy the fixture BMP (%lu)\n",
               (unsigned long)GetLastError());
        return;
    }

    dir[0] = '\0';
    ok(a98_emoji_asset_dir_from(root, "C:\\a98_no_such_cwd", dir,
                                sizeof(dir)) == 1,
       "release layout emoji\\bmp\\24 next to the EXE is found");
    printf("  release: %s\n", dir);
    ok(strstr(dir, "emoji_asset_probe") != NULL,
       "the release layout wins over walking further up");
    ok(loads_as_24x24(path), "the fixture BMP loads as 24x24/24bpp");
}

/* No artwork anywhere: the search reports nothing, the picker still lists the
 * shortcodes, and nothing crashes. */
static void test_missing_assets(void)
{
    char dir[A98_MAX_PATH];
    char path[A98_MAX_PATH];
    const A98EmojiEntry *e = a98_emoji_by_codepoint(0x1F600UL);

    dir[0] = 'x';
    ok(a98_emoji_asset_dir_from("C:\\a98_no_such_dir\\build",
                                "C:\\a98_no_such_dir", dir, sizeof(dir)) == 0,
       "missing artwork reports no directory");
    ok(dir[0] == '\0', "the failed search clears the output buffer");
    ok(a98_emoji_asset_file(NULL, e, path, sizeof(path)) == -1,
       "asset_file rejects a NULL directory");
    ok(a98_emoji_asset_file("C:\\x", NULL, path, sizeof(path)) == -1,
       "asset_file rejects a NULL entry");
}

/* Prove IDD_EMOJI builds and fills without showing a window. */
static void test_dialog_probe(const char *exe_dir, const char *asset_dir)
{
    int images = -1;
    int items = -1;
    char dir[A98_MAX_PATH];
    char exe[A98_MAX_PATH];
    HINSTANCE inst = GetModuleHandleA(NULL);

    InitCommonControls();
    dir[0] = '\0';
    if (a98_emoji_dialog_probe(inst, exe_dir, &images, &items, dir,
                               sizeof(dir)) != 1) {
        printf("SKIP: IDD_EMOJI could not be created (CreateDialogParam "
               "failed, error %lu) - no window station in this session?\n",
               (unsigned long)GetLastError());
        return;
    }
    printf("  dialog : %d icons, %d rows, dir=%s\n", images, items, dir);
    ok(images > 0, "IDD_EMOJI loaded the pack artwork into its image list");
    ok(items == a98_emoji_count(),
       "IDD_EMOJI listed every shortcode in the list view");
    if (asset_dir != NULL && asset_dir[0] != '\0') {
        expect("dialog and picker agree on the asset directory", dir,
               asset_dir);
    }

    /* And the degradation path: a template that cannot find artwork still
     * builds and still lists names. */
    exe[0] = '\0';
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    dir[0] = '\0';
    images = -1;
    items = -1;
    if (a98_emoji_dialog_probe(inst, "C:\\a98_no_such_dir", &images, &items,
                               dir, sizeof(dir)) == 1) {
        /* The CWD may still legitimately provide the pack, so only assert the
         * dialog survived and produced rows. */
        ok(items == a98_emoji_count(),
           "IDD_EMOJI still lists names when the EXE path has no artwork");
    } else {
        printf("SKIP: degradation dialog probe (%lu)\n",
               (unsigned long)GetLastError());
    }
}

/* The transcript shows emoji as the pack's artwork.  Two things have to hold:
 * the RTF we build is the bare-metafile form RichEdit accepts (a prefixed
 * Aldus metafile is silently dropped), and the control keeps the \pict group
 * once it has our OLE callback - without one RichEdit throws the picture away
 * without an error, which is exactly the bug this covers. */
typedef struct {
    char *p;
    size_t cap;
    size_t pos;
} TestOut;

static DWORD CALLBACK test_out_cb(DWORD_PTR cookie, LPBYTE buf, LONG cb,
                                  LONG *pcb)
{
    TestOut *s = (TestOut *)cookie;
    size_t room = s->cap - s->pos;
    size_t n = (cb > 0 && (size_t)cb < room) ? (size_t)cb : room;

    memcpy(s->p + s->pos, buf, n);
    s->pos += n;
    *pcb = (LONG)n;
    return n == (size_t)cb ? 0 : 1;
}

static void test_transcript_pictures(const char *exe_dir)
{
    static char out[8192];
    const A98EmojiEntry *e = a98_emoji_by_codepoint(0x1F600UL);
    const char *token = ":grinning_face:";
    char *rtf;
    HWND wnd;
    EDITSTREAM es;
    TestOut so;

    InitCommonControls();
    if (e == NULL) {
        printf("SKIP: transcript picture test (no U+1F600 in the pack)\n");
        return;
    }

    rtf = a98_emoji_picture_rtf(exe_dir, e);
    ok(rtf != NULL, "a picture RTF was built for U+1F600");
    if (rtf == NULL) {
        return;
    }
    printf("  rtf    : %d chars\n", (int)strlen(rtf));
    ok(strncmp(rtf, "{\\rtf1", 6) == 0, "the picture is a complete RTF body");
    ok(strstr(rtf, "\\wmetafile8") != NULL,
       "the picture is a \\wmetafile8 (what WordPad also writes)");
    /* \picw/\pich are 0.01mm (HIMETRIC): 16px @96dpi is ~423, not 16. */
    ok(strstr(rtf, "\\picw423\\pich423") != NULL,
       "the picture declares its native size in HIMETRIC units");
    ok(strstr(rtf, "\\picwgoal240\\pichgoal240") != NULL,
       "the picture is laid out at the 16 px inline size");
    ok(strstr(rtf, "d7cdc69a") == NULL,
       "the metafile carries no Aldus prefix RichEdit would reject");
    a98_free(rtf);

    if (LoadLibraryA("riched20.dll") == NULL) {
        printf("SKIP: no RichEdit 2.0 here, the transcript falls back to "
               "text\n");
        return;
    }
    OleInitialize(NULL);
    wnd = CreateWindowExA(0, "RichEdit20A", "",
                          WS_POPUP | ES_MULTILINE | ES_READONLY,
                          0, 0, 400, 200, NULL, NULL,
                          GetModuleHandleA(NULL), NULL);
    if (wnd == NULL) {
        printf("SKIP: RichEdit20A could not be created (error %lu)\n",
               (unsigned long)GetLastError());
        return;
    }

    ok(a98_emoji_ole_attach(wnd) == 1,
       "the transcript accepts the OLE storage callback");

    SetWindowTextA(wnd, "hi :grinning_face: yo");
    {
        CHARRANGE cr;

        cr.cpMin = 3;
        cr.cpMax = 3 + (LONG)strlen(token);
        SendMessageA(wnd, EM_EXSETSEL, 0, (LPARAM)&cr);
    }
    ok(a98_emoji_insert_picture(wnd, exe_dir, e) == 1,
       "the picture streams into the selected shortcode");

    memset(&so, 0, sizeof(so));
    so.p = out;
    so.cap = sizeof(out) - 1;
    memset(&es, 0, sizeof(es));
    es.dwCookie = (DWORD_PTR)&so;
    es.pfnCallback = test_out_cb;
    SendMessageA(wnd, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    out[so.pos] = '\0';
    ok(strstr(out, "\\pict") != NULL,
       "RichEdit kept the picture instead of dropping it");
    ok(strstr(out, token) == NULL,
       "the shortcode was replaced, not duplicated");
    ok(strstr(out, "hi ") != NULL && strstr(out, " yo") != NULL,
       "the text around the picture survived");

    DestroyWindow(wnd);
    OleUninitialize();
}

int main(void)
{
    char exe_dir[A98_MAX_PATH];
    char pack_dir[A98_MAX_PATH];

    printf("AIagent98 emoji pack test\n\n");
    test_text_pipeline();
    printf("\n-- asset location\n");
    test_asset_location(pack_dir, sizeof(pack_dir));
    printf("\n-- release layout\n");
    test_release_layout(pack_dir);
    printf("\n-- missing artwork\n");
    test_missing_assets();
    printf("\n-- dialog\n");
    exe_here(exe_dir, sizeof(exe_dir));
    test_dialog_probe(exe_dir, pack_dir);
    printf("\n-- transcript pictures\n");
    test_transcript_pictures(exe_dir);

    if (failures != 0) {
        printf("\n%d emoji test(s) failed\n", failures);
        return 1;
    }
    printf("\nall emoji tests passed\n");
    return 0;
}
