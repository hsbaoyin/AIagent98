/*
 * Host-side smoke test for the local tools.
 *
 * Build (MinGW):
 *   gcc -std=c99 -O1 -w -Isrc -Isrc/core -Isrc/agent -Isrc/net \
 *       -Ithird_party/cjson -Ithird_party/bearssl/inc \
 *       tests/test_tools.c src/agent/tools.c src/agent/jsonutil.c \
 *       src/core/base.c src/core/config.c src/core/log.c src/core/path.c \
 *       src/core/str.c src/core/utf8.c third_party/cjson/cJSON.c \
 *       -lws2_32 -o build/host/test_tools.exe
 */

#include "agent/agent.h"
#include "agent/tools.h"
#include "core/config.h"
#include "core/path.h"
#include "core/str.h"
#include "core/utf8.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

static void run(Agent *a, const char *name, const char *args)
{
    Str out;
    char err[512];
    int rc;

    str_init(&out);
    err[0] = '\0';
    rc = tools_execute(a, name, args, &out, err, sizeof(err));
    printf("--- %s (rc=%d)\n", name, rc);
    if (err[0] != '\0') {
        printf("error: %s\n", err);
    }
    printf("%s\n", str_cstr(&out));
    if (rc != 0) {
        failures++;
    }
    str_free(&out);
}

/* Run a tool and copy its textual output for assertions. */
static int run_capture(Agent *a, const char *name, const char *args,
                       char *captured, size_t cap)
{
    Str out;
    char err[512];
    int rc;

    str_init(&out);
    err[0] = '\0';
    rc = tools_execute(a, name, args, &out, err, sizeof(err));
    if (captured != NULL && cap > 0) {
        size_t n = out.len < cap - 1 ? out.len : cap - 1;
        memcpy(captured, str_cstr(&out), n);
        captured[n] = '\0';
    }
    if (rc != 0) {
        printf("error: %s\n", err);
    }
    str_free(&out);
    return rc;
}

static int write_raw(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n;

    if (f == NULL) {
        return -1;
    }
    n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static int read_bytes(const char *path, char *buf, size_t cap, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        return -1;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    *out_len = n;
    return 0;
}

/* The sandbox is the README's headline safety feature (allow_outside=0), so
 * it must have assertions, not just a manual review.  Every escape shape the
 * resolver has to reject is tried here; each must fail *and* leave no file
 * behind (the canary path is outside the workspace on purpose). */
static void test_sandbox_escape(Agent *a, const char *tool, const char *path)
{
    char args[1024];
    char captured[512];
    int rc;
    const char *canary = "build/agent98_escape_canary.txt";

    /* Double every backslash so the path survives JSON decoding. */
    char json_path[2 * A98_MAX_PATH];
    size_t j = 0, i = 0;

    for (; path[i] != '\0' && j + 1 < sizeof(json_path); i++) {
        if (path[i] == '\\') {
            json_path[j++] = '\\';
        }
        json_path[j++] = path[i];
    }
    json_path[j] = '\0';
    if (j + 32 >= sizeof(json_path)) {
        printf("SKIP: path too long to build a JSON fixture: %s\n", path);
        return;
    }

    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"x\",\"confirm\":true}",
             json_path);
    rc = run_capture(a, tool, args, captured, sizeof(captured));
    if (rc == 0) {
        printf("FAIL: sandbox let %s through: %s\n", tool, path);
        failures++;
    } else if (a98_path_exists(canary)) {
        printf("FAIL: sandbox escape wrote the canary file: %s\n", path);
        remove(canary);
        failures++;
    } else {
        printf("PASS: sandbox refuses %s (%s)\n", path, tool);
    }
}

int main(void)
{
    Config cfg;
    Agent a;

    memset(&cfg, 0, sizeof(cfg));
    config_defaults(&cfg, NULL);
    strcpy(cfg.workspace, ".");
    cfg.allow_shell = 1;

    memset(&a, 0, sizeof(a));
    a.cfg = &cfg;
    strcpy(a.workspace, ".");

    run(&a, "write_file",
        "{\"path\":\"build/host/tool_test.txt\",\"content\":\"hello\\nworld\\n\"}");
    run(&a, "read_file", "{\"path\":\"build/host/tool_test.txt\"}");
    run(&a, "list_dir", "{\"path\":\"build/host\"}");
    run(&a, "search_files",
        "{\"path\":\"build/host\",\"pattern\":\"world\"}");
    /* COMMAND.COM on Windows 9x understands neither "&" nor "1>&2"; the
     * test only checks that a command runs and its output is captured. */
    run(&a, "run_command", "{\"command\":\"echo stdout-line\"}");

    /* Text files must come out with DOS line endings, even when the model
     * sends bare LF; existing CRLF pairs must not be doubled. */
    {
        static const char expect_text[] = "@echo off\r\necho hi\r\necho mixed\r\n";
        static const char expect_bin[] = "a\nb";
        char buf[128];
        size_t n = 0;

        run(&a, "write_file",
            "{\"path\":\"build/host/tool_crlf.bat\",\"content\":"
            "\"@echo off\\necho hi\\r\\necho mixed\\n\"}");
        if (read_bytes("build/host/tool_crlf.bat", buf, sizeof(buf), &n) != 0 ||
            n != strlen(expect_text) || memcmp(buf, expect_text, n) != 0) {
            printf("FAIL: text file did not use CRLF line endings\n");
            failures++;
        } else {
            printf("PASS: text file uses CRLF line endings\n");
        }

        run(&a, "write_file",
            "{\"path\":\"build/host/tool_bin.bin\",\"content\":\"a\\nb\"}");
        if (read_bytes("build/host/tool_bin.bin", buf, sizeof(buf), &n) != 0 ||
            n != strlen(expect_bin) || memcmp(buf, expect_bin, n) != 0) {
            printf("FAIL: binary file was rewritten\n");
            failures++;
        } else {
            printf("PASS: binary file keeps its bytes\n");
        }
    }

    /* Chinese text must be written in the local code page with CRLF and be
     * converted back to UTF-8 when read, so Notepad/COMMAND.COM and the model
     * both see readable text. */
    {
        /* "中文测试" in UTF-8. */
        static const char utf8_cn[] =
            "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95";
        /* The same text in GBK (CP936) with DOS line endings. */
        static const char gbk_cn[] =
            "\xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4"
            "\r\n\xB5\xDA\xB6\xFE\xD0\xD0\r\n";
        char buf[512];
        char captured[1024];
        size_t n = 0;
        size_t i;
        int lone_lf = 0;
        int rc;

        run(&a, "write_file",
            "{\"path\":\"build/host/tool_cn.txt\",\"content\":"
            "\"\\u4e2d\\u6587\\u6d4b\\u8bd5\\n\\u7b2c\\u4e8c\\u884c\\n\"}");
        if (read_bytes("build/host/tool_cn.txt", buf, sizeof(buf), &n) != 0) {
            printf("FAIL: cannot read back tool_cn.txt\n");
            failures++;
        } else {
            for (i = 0; i < n; i++) {
                if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
                    lone_lf = 1;
                }
            }
            if (lone_lf) {
                printf("FAIL: Chinese text has bare LF line endings\n");
                failures++;
            } else {
                printf("PASS: Chinese text uses CRLF line endings\n");
            }
            if (GetACP() == 936) {
                if (n != sizeof(gbk_cn) - 1 ||
                    memcmp(buf, gbk_cn, sizeof(gbk_cn) - 1) != 0) {
                    printf("FAIL: Chinese text was not written as GBK\n");
                    failures++;
                } else {
                    printf("PASS: Chinese text written as GBK\n");
                }
            } else {
                printf("SKIP: GBK byte check (ACP=%u)\n", (unsigned)GetACP());
            }
        }

        rc = run_capture(&a, "read_file",
                         "{\"path\":\"build/host/tool_cn.txt\"}",
                         captured, sizeof(captured));
        if (rc != 0 || strstr(captured, utf8_cn) == NULL) {
            printf("FAIL: read_file did not return UTF-8 Chinese text\n");
            failures++;
        } else {
            printf("PASS: read_file restores UTF-8 Chinese text\n");
        }

        /* A GBK file created by a Win9x application must be converted. */
        if (write_raw("build/host/tool_gbk.txt", gbk_cn,
                      sizeof(gbk_cn) - 1) != 0) {
            printf("FAIL: cannot create the GBK fixture\n");
            failures++;
        } else {
            rc = run_capture(&a, "read_file",
                             "{\"path\":\"build/host/tool_gbk.txt\"}",
                             captured, sizeof(captured));
            if (rc != 0 || strstr(captured, utf8_cn) == NULL) {
                printf("FAIL: GBK file was not converted to UTF-8\n");
                failures++;
            } else {
                printf("PASS: GBK file is converted to UTF-8\n");
            }
        }

        /* A UTF-8 BOM must be dropped, not passed to the model. */
        if (write_raw("build/host/tool_bom.txt",
                      "\xEF\xBB\xBF" "\xE4\xB8\xAD\xE6\x96\x87", 9) != 0) {
            printf("FAIL: cannot create the BOM fixture\n");
            failures++;
        } else {
            rc = run_capture(&a, "read_file",
                             "{\"path\":\"build/host/tool_bom.txt\"}",
                             captured, sizeof(captured));
            if (rc != 0 ||
                strncmp(captured, "\xE4\xB8\xAD\xE6\x96\x87", 6) != 0) {
                printf("FAIL: UTF-8 BOM was not stripped\n");
                failures++;
            } else {
                printf("PASS: UTF-8 BOM is stripped\n");
            }
        }
    }

    /* rmdir_force must refuse the workspace root even when the INI path is
     * relative and carries a trailing separator. */
    {
        char saved_ws[sizeof(a.workspace)];
        int rc;
        FILE *f;

        strcpy(saved_ws, a.workspace);
        strcpy(cfg.workspace, "build\\host\\wsroot\\");
        strcpy(a.workspace, "build\\host\\wsroot\\");
        a98_path_mkdirs("build\\host\\wsroot\\sub");
        f = fopen("build\\host\\wsroot\\keep.txt", "wb");
        if (f != NULL) {
            fputs("keep", f);
            fclose(f);
        }

        rc = run_capture(&a, "rmdir_force",
                         "{\"path\":\".\",\"confirm\":true}", NULL, 0);
        if (rc == 0 || !a98_path_is_dir("build\\host\\wsroot")) {
            printf("FAIL: rmdir_force deleted the workspace root\n");
            failures++;
        } else {
            printf("PASS: rmdir_force refuses the workspace root\n");
        }

        rc = run_capture(&a, "rmdir_force",
                         "{\"path\":\"sub\",\"confirm\":true}", NULL, 0);
        if (rc != 0 || a98_path_is_dir("build\\host\\wsroot\\sub")) {
            printf("FAIL: rmdir_force did not remove a subdirectory\n");
            failures++;
        } else {
            printf("PASS: rmdir_force removes a subdirectory\n");
        }

        strcpy(a.workspace, saved_ws);
        strcpy(cfg.workspace, saved_ws);
    }

    /* The sandbox is the README's headline safety feature, so it gets real
     * assertions: every escape shape the resolver must reject, including the
     * ones that only differ in separators, absolute forms, device paths and
     * 8.3 short names. */
    {
        char abs_ws[A98_MAX_PATH];
        char short_ws[A98_MAX_PATH];
        char abs_escape[A98_MAX_PATH + 64];

        test_sandbox_escape(&a, "write_file",
                            "../../agent98_escape_canary.txt");
        test_sandbox_escape(&a, "write_file",
                            "..\\..\\agent98_escape_canary.txt");
        test_sandbox_escape(&a, "read_file",
                            "..\\..\\CMakeLists.txt");
        test_sandbox_escape(&a, "read_file",
                            "C:\\Windows\\system.ini");
        test_sandbox_escape(&a, "write_file",
                            "\\\\.\\pipe\\agent98_escape");

        /* Absolute workspace path with .. traversal ("C:/ws/../.." shape). */
        if (GetFullPathNameA(".", A98_MAX_PATH, abs_ws, NULL) > 0) {
            snprintf(abs_escape, sizeof(abs_escape),
                     "%s\\..\\..\\agent98_escape_canary.txt", abs_ws);
            test_sandbox_escape(&a, "write_file", abs_escape);
            /* 8.3 short names of the workspace itself must not slip past
             * the prefix comparison either; short names only exist for
             * long components, so ask for the full absolute path. */
            if (GetShortPathNameA(abs_ws, short_ws, sizeof(short_ws)) > 0 &&
                strcmp(short_ws, abs_ws) != 0) {
                snprintf(abs_escape, sizeof(abs_escape),
                         "%s\\..\\..\\agent98_escape_canary.txt", short_ws);
                test_sandbox_escape(&a, "write_file", abs_escape);
            } else {
                printf("SKIP: 8.3 short-name escape (no short names here)\n");
            }
        }
    }

    /* Everything the model receives has to be valid UTF-8: the API rejects
     * the whole request body otherwise.  Tools hand back ACP paths (a CP936
     * workspace name, FindFirstFileA results), so list_dir used to put GBK
     * bytes into the body - "invalid unicode code point" - and the transcript
     * showed "????????". */
    {
        static const char gbk_ws[] = "build\\host\\\xd0\xc2\xbd\xa8";
        static const char utf8_ws[] = "\xe6\x96\xb0\xe5\xbb\xba";
        char saved_ws[sizeof(a.workspace)];
        char captured[2048];
        char *fixed;
        int rc;

        strcpy(saved_ws, a.workspace);
        a98_path_mkdirs(gbk_ws);
        strcpy(a.workspace, gbk_ws);
        rc = run_capture(&a, "list_dir", "{\"path\":\".\"}", captured,
                         sizeof(captured));
        strcpy(a.workspace, saved_ws);

        if (rc != 0) {
            printf("FAIL: list_dir on a CP_ACP workspace name failed\n");
            failures++;
        }
        /* What the agent does before the message leaves the process. */
        fixed = a98_utf8_sanitize(captured);
        {
            const char *text = fixed != NULL ? fixed : captured;

            if (!a98_utf8_valid(text, strlen(text))) {
                printf("FAIL: repaired tool output is still not UTF-8\n");
                failures++;
            } else if (strstr(text, utf8_ws) == NULL) {
                printf("FAIL: the workspace name did not survive the repair\n");
                failures++;
            } else {
                printf("PASS: ACP tool output is repaired to UTF-8\n");
            }
        }
        a98_free(fixed);

        /* Already valid text must come back untouched (NULL = nothing to do),
         * so UTF-8 content next to ACP bytes is never double-converted. */
        if (a98_utf8_sanitize("plain ascii \xe4\xb8\xad") != NULL) {
            printf("FAIL: sanitize rewrote already valid UTF-8\n");
            failures++;
        } else {
            printf("PASS: sanitize leaves valid UTF-8 alone\n");
        }
        {
            /* One GBK character (D0 C2 = U+65B0) next to a real UTF-8 emoji:
             * the ACP pair must not be mistaken for UTF-8 U+00BD. */
            char *mixed = a98_utf8_sanitize("\xd0\xc2 ok \xf0\x9f\x98\x80");

            if (mixed == NULL || !a98_utf8_valid(mixed, strlen(mixed)) ||
                strstr(mixed, "\xe6\x96\xb0") == NULL ||
                strstr(mixed, "\xf0\x9f\x98\x80") == NULL) {
                printf("FAIL: mixed ACP/UTF-8 text was not repaired\n");
                failures++;
            } else {
                printf("PASS: mixed ACP and UTF-8 text is repaired\n");
            }
            a98_free(mixed);
        }
    }

    if (failures != 0) {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nall tool tests passed\n");
    return 0;
}
