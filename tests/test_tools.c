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
#include "agent/api_openai.h"
#include "agent/tools.h"
#include "core/config.h"
#include "core/path.h"
#include "core/str.h"
#include "core/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
/* winsock2.h must precede windows.h on MinGW, otherwise winsock.h conflicts. */
#include <winsock2.h>
#include <windows.h>
#include <process.h>

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
    /* fread() cannot tell "file filled the buffer" from "file was longer":
     * a truncated read would make the CRLF/byte assertions compare a prefix. */
    return n >= cap ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Negative security tests.  smoke_tools.py only proves that a string exists
 * in tools.c; these exercise the real code paths.
 * ------------------------------------------------------------------------- */

/* Run a tool and capture both its output and its error text so we can assert
 * on the exact refusal reason, not just on a non-zero return code. */
static int run_capture_err(Agent *a, const char *name, const char *args,
                           char *out_buf, size_t out_cap,
                           char *err_buf, size_t err_cap)
{
    Str out;
    int rc;

    str_init(&out);
    err_buf[0] = '\0';
    rc = tools_execute(a, name, args, &out, err_buf, err_cap);
    if (out_buf != NULL && out_cap > 0) {
        size_t n = out.len < out_cap - 1 ? out.len : out_cap - 1;
        memcpy(out_buf, str_cstr(&out), n);
        out_buf[n] = '\0';
    }
    str_free(&out);
    return rc;
}

/* allow_shell=1, full_access=0: the ban list must reject the classic Win9x
 * disk-wipe shapes, including the ".com"/".exe" and inline "/" evasions. */
static void test_dangerous_commands(Agent *a)
{
    static const char *const banned[] = {
        "format.com a:", "deltree.exe /y", "deltree/y", NULL
    };
    int saved_shell = a->cfg->allow_shell;
    int saved_full = a->cfg->full_access;
    int i;

    a->cfg->allow_shell = 1;
    a->cfg->full_access = 0;
    for (i = 0; banned[i] != NULL; i++) {
        char args[256];
        char err[512];
        char out[256];
        int rc;

        /* Keep the timeout short: if the ban ever regresses, a real format /
         * deltree must not be left running while the test reports the bug. */
        snprintf(args, sizeof(args),
                 "{\"command\":\"%s\",\"timeout_sec\":1}", banned[i]);
        rc = run_capture_err(a, "run_command", args, out, sizeof(out),
                             err, sizeof(err));
        if (rc == 0 || strstr(err, "blocked dangerous command") == NULL) {
            printf("FAIL: dangerous command was not rejected: %s (rc=%d, "
                   "err=%s)\n", banned[i], rc, err);
            failures++;
        } else {
            printf("PASS: dangerous command rejected: %s (%s)\n",
                   banned[i], err);
        }
    }
    a->cfg->allow_shell = saved_shell;
    a->cfg->full_access = saved_full;
}

static void test_session_info_full_access(Agent *a)
{
    char captured[1024];
    char err[512];
    int saved_full = a->cfg->full_access;
    int rc;

    a->cfg->full_access = 1;
    rc = run_capture_err(a, "session_info", "{}", captured, sizeof(captured),
                         err, sizeof(err));
    a->cfg->full_access = saved_full;
    if (rc != 0 || strstr(captured, "sandbox: DISABLED") == NULL) {
        printf("FAIL: full_access session_info did not report DISABLED "
               "(rc=%d, out=%s, err=%s)\n", rc, captured, err);
        failures++;
    } else {
        printf("PASS: full_access session_info reports sandbox DISABLED\n");
    }
}

/* Minimal in-process OpenAI mock.  The confirm callback is only consulted by
 * agent_run(), so testing it requires a real request/response pair; binding
 * an ephemeral loopback port keeps this self-contained (no node, no fixed
 * port, no server process). */
typedef struct {
    SOCKET listen_sock;
    unsigned short port;
    const char **responses;
    int count;
} MockApi;

static unsigned __stdcall mock_api_thread(void *p)
{
    MockApi *m = (MockApi *)p;
    int i;

    /* Serve up to 8 requests.  The reply is chosen from the request body, not
     * from a request counter: api_chat may retry a failed attempt, and a
     * counter-based mock would then hand out the wrong reply. */
    for (i = 0; i < 8; i++) {
        fd_set fds;
        struct timeval tv;
        SOCKET c;
        char request[16384];
        size_t rlen = 0;
        size_t header_end = 0;
        size_t body_need = 0;
        char packet[2048];
        int packet_len;
        const char *reply;

        FD_ZERO(&fds);
        FD_SET(m->listen_sock, &fds);
        tv.tv_sec = 20;
        tv.tv_usec = 0;
        if (select(0, &fds, NULL, NULL, &tv) <= 0) {
            printf("  mock: timed out waiting for request %d\n", i);
            break;
        }
        c = accept(m->listen_sock, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        while (rlen < sizeof(request) - 1) {
            int n = recv(c, request + rlen, (int)(sizeof(request) - 1 - rlen), 0);
            if (n <= 0) break;
            rlen += (size_t)n;
            request[rlen] = '\0';
            if (header_end == 0) {
                char *sep = strstr(request, "\r\n\r\n");
                char *cl;

                if (sep == NULL) continue;
                header_end = (size_t)(sep + 4 - request);
                cl = strstr(request, "Content-Length:");
                if (cl != NULL) body_need = (size_t)strtoul(cl + 15, NULL, 10);
            }
            if (rlen >= header_end + body_need) break;
        }
        /* A tool result in the request means this is round 2 -> final answer.
         * Otherwise it is the first round -> ask for the write_file call. */
        reply = (strstr(request, "\"role\":\"tool\"") != NULL ||
                 strstr(request, "\"role\": \"tool\"") != NULL)
                ? m->responses[1] : m->responses[0];
        /* One write, then an explicit graceful shutdown: two writes followed
         * by closesocket() raced with the client's read deadline on Windows. */
        packet_len = _snprintf(packet, sizeof(packet),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
                    (unsigned)strlen(reply), reply);
        if (packet_len > 0 && packet_len < (int)sizeof(packet)) {
            send(c, packet, packet_len, 0);
        }
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    return 0;
}

static int mock_api_start(MockApi *m, const char **responses, int count)
{
    WSADATA wsa;
    SOCKADDR_IN addr;
    int len;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    m->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m->listen_sock == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  /* let the OS pick a free port */
    if (bind(m->listen_sock, (SOCKADDR *)&addr, sizeof(addr)) != 0 ||
        listen(m->listen_sock, 2) != 0) {
        closesocket(m->listen_sock);
        WSACleanup();
        return -1;
    }
    len = sizeof(addr);
    if (getsockname(m->listen_sock, (SOCKADDR *)&addr, &len) != 0) {
        closesocket(m->listen_sock);
        WSACleanup();
        return -1;
    }
    m->port = (unsigned short)ntohs(addr.sin_port);
    m->responses = responses;
    m->count = count;
    return 0;
}

static void mock_api_finish(MockApi *m, uintptr_t thread)
{
    closesocket(m->listen_sock);
    if (thread != 0) {
        WaitForSingleObject((HANDLE)thread, INFINITE);
        CloseHandle((HANDLE)thread);
    }
    WSACleanup();
}

/* Build a non-streaming Chat Completions reply with cJSON so the nested
 * tool-call arguments cannot be broken by escaping mistakes in the test. */
static char *mock_json(const char *content, int with_tool_call)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *msg = cJSON_CreateObject();
    char *text;

    cJSON_AddItemToObject(root, "choices", choices);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(choice, "message", msg);
    if (content != NULL) cJSON_AddStringToObject(msg, "content", content);
    else cJSON_AddNullToObject(msg, "content");
    if (with_tool_call) {
        cJSON *calls = cJSON_CreateArray();
        cJSON *call = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();

        cJSON_AddItemToObject(msg, "tool_calls", calls);
        cJSON_AddItemToArray(calls, call);
        cJSON_AddStringToObject(call, "id", "call_confirm_probe");
        cJSON_AddStringToObject(call, "type", "function");
        cJSON_AddItemToObject(call, "function", fn);
        cJSON_AddStringToObject(fn, "name", "write_file");
        cJSON_AddStringToObject(fn, "arguments",
            "{\"path\":\"build/host/confirm_probe.txt\","
            "\"content\":\"x\"}");
        cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
    } else {
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static int confirm_deny(void *ctx, const char *tool, const char *args_json)
{
    int *calls = (int *)ctx;
    (void)tool;
    (void)args_json;
    (*calls)++;
    return 0;  /* the user pressed Deny */
}

static int agent_messages_contain(const Agent *a, const char *needle)
{
    int i;
    for (i = 0; i < a->messages.count; i++) {
        const char *c = a->messages.items[i].content;
        if (c != NULL && strstr(c, needle) != NULL) return 1;
    }
    return 0;
}

typedef struct {
    int rc;
    int callback_calls;
    int saw_denied;
    int file_written;
    char err[512];
} ConfirmResult;

static void run_confirm_case(int auto_confirm, ConfirmResult *r)
{
    static const char *probe_path = "build/host/confirm_probe.txt";
    char *owned[2];
    const char *responses[2];
    MockApi mock;
    uintptr_t th;
    Config cfg2;
    Agent a2;
    char base[128];
    int deny_calls = 0;

    memset(r, 0, sizeof(*r));
    owned[0] = mock_json(NULL, 1);  /* round 1: ask for a write_file */
    owned[1] = mock_json("done", 0);/* round 2: final answer */
    if (owned[0] == NULL || owned[1] == NULL) {
        snprintf(r->err, sizeof(r->err), "cannot build mock responses");
        r->rc = -1;
        cJSON_free(owned[0]);
        cJSON_free(owned[1]);
        return;
    }
    responses[0] = owned[0];
    responses[1] = owned[1];
    if (mock_api_start(&mock, responses, 2) != 0) {
        snprintf(r->err, sizeof(r->err), "cannot bind the in-process mock API");
        r->rc = -1;
        cJSON_free(owned[0]);
        cJSON_free(owned[1]);
        return;
    }

    memset(&cfg2, 0, sizeof(cfg2));
    config_defaults(&cfg2, NULL);
    strcpy(cfg2.workspace, ".");
    cfg2.stream = 0;           /* non-streaming JSON reply */
    cfg2.max_steps = 3;
    cfg2.connect_timeout_sec = 15;
    cfg2.request_timeout_sec = 15;
    cfg2.auto_confirm = auto_confirm;
    snprintf(base, sizeof(base), "http://127.0.0.1:%u", mock.port);
    strcpy(cfg2.api_base, base);

    memset(&a2, 0, sizeof(a2));
    a2.cfg = &cfg2;
    strcpy(a2.workspace, ".");
    msglist_init(&a2.messages);
    a2.on_confirm = confirm_deny;
    a2.confirm_ctx = &deny_calls;

    remove(probe_path);
    th = _beginthreadex(NULL, 0, mock_api_thread, &mock, 0, NULL);
    if (th == 0) {
        snprintf(r->err, sizeof(r->err), "cannot start the mock API thread");
        r->rc = -1;
    } else {
        r->rc = agent_run(&a2, "confirm probe", r->err, sizeof(r->err));
        mock_api_finish(&mock, th);
        r->callback_calls = deny_calls;
        r->saw_denied = agent_messages_contain(&a2, "denied by the user");
        r->file_written = a98_path_exists(probe_path);
        if (r->file_written) remove(probe_path);
    }
    msglist_free(&a2.messages);
    cJSON_free(owned[0]);
    cJSON_free(owned[1]);
}

static void test_confirm_gate(void)
{
    ConfirmResult denied, automatic;

    run_confirm_case(0, &denied);
    if (denied.rc != 0) {
        printf("FAIL: confirm-deny run failed: %s\n", denied.err);
        failures++;
    }
    if (denied.callback_calls != 1) {
        printf("FAIL: confirm callback called %d times, expected 1\n",
               denied.callback_calls);
        failures++;
    } else if (!denied.saw_denied) {
        printf("FAIL: denied tool call did not reach the transcript\n");
        failures++;
    } else if (denied.file_written) {
        printf("FAIL: denied write_file still created the file\n");
        failures++;
    } else {
        printf("PASS: confirm callback returning false denied the write\n");
    }

    /* auto_confirm=1 is a deliberate behaviour change: the callback must not
     * even be asked and the same write_file now really executes. */
    run_confirm_case(1, &automatic);
    if (automatic.rc != 0) {
        printf("FAIL: auto_confirm run failed: %s\n", automatic.err);
        failures++;
    }
    if (automatic.callback_calls != 0) {
        printf("FAIL: auto_confirm still asked the callback (%d calls)\n",
               automatic.callback_calls);
        failures++;
    } else if (automatic.saw_denied) {
        printf("FAIL: auto_confirm run reported a denial\n");
        failures++;
    } else if (!automatic.file_written) {
        printf("FAIL: auto_confirm did not execute the write\n");
        failures++;
    } else {
        printf("PASS: auto_confirm bypasses the callback and runs the tool\n");
    }
}

/* The sandbox is the README's headline safety feature (allow_outside=0), so
 * it must have assertions, not just a manual review.  Every escape shape the
 * resolver has to reject is tried here; each must fail *and* leave no file
 * behind.  The canary is derived per case from the resolver's own calculation
 * (workspace + user path -> GetFullPathNameA), because a fixed canary inside
 * the workspace could never trigger the assertion. */
static void test_sandbox_escape(Agent *a, const char *tool, const char *path)
{
    char args[1024];
    char captured[512];
    char candidate[A98_MAX_PATH * 2];
    char canary[A98_MAX_PATH * 2];
    DWORD n;
    int rc;
    int is_write;
    int canary_preexisting;

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

    /* Reproduce a98_path_resolve_in_root's calculation for THIS case: absolute
     * paths are used as-is, relative ones are joined to the workspace first.
     * The result is the exact absolute file a buggy resolver would write. */
    if (a98_path_is_absolute(path)) {
        if (strlen(path) >= sizeof(candidate)) {
            printf("SKIP: escape path too long: %s\n", path);
            return;
        }
        strcpy(candidate, path);
    } else if (a98_path_join(candidate, sizeof(candidate), a->workspace,
                             path) != 0) {
        printf("SKIP: cannot join escape path: %s\n", path);
        return;
    }
    n = GetFullPathNameA(candidate, sizeof(canary), canary, NULL);
    if (n == 0 || n >= sizeof(canary)) {
        printf("SKIP: cannot resolve escape target: %s\n", path);
        return;
    }

    /* Only a write can create the canary.  Never delete a file that existed
     * before the test (a read probe may legitimately name a real file). */
    is_write = (strcmp(tool, "write_file") == 0 ||
                strcmp(tool, "append_file") == 0 ||
                strcmp(tool, "edit") == 0 ||
                strcmp(tool, "apply_patch") == 0);
    canary_preexisting = a98_path_exists(canary);

    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"x\",\"confirm\":true}",
             json_path);
    rc = run_capture(a, tool, args, captured, sizeof(captured));
    if (rc == 0) {
        printf("FAIL: sandbox let %s through: %s\n", tool, path);
        failures++;
    }
    if (is_write && !canary_preexisting && a98_path_exists(canary)) {
        printf("FAIL: sandbox escape wrote the canary file: %s\n", canary);
        remove(canary);
        failures++;
    } else {
        printf("PASS: sandbox refuses %s (%s) [canary %s]\n",
               path, tool, canary);
    }
}

int main(void)
{
    Config cfg;
    Agent a;

    memset(&cfg, 0, sizeof(cfg));
    config_defaults(&cfg, NULL);
    if (cfg.max_steps != 0) {
        printf("FAIL: default max_steps is %d, expected 0 (unlimited)\n",
               cfg.max_steps);
        failures++;
    } else {
        printf("PASS: default max_steps is unlimited\n");
    }
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

        /* Unique canary name per case, so a stale file from another case (or
         * an earlier run) cannot mask the assertion. */
        test_sandbox_escape(&a, "write_file",
                            "../../agent98_escape_canary_1.txt");
        test_sandbox_escape(&a, "write_file",
                            "..\\..\\agent98_escape_canary_2.txt");
        test_sandbox_escape(&a, "read_file",
                            "..\\..\\CMakeLists.txt");
        test_sandbox_escape(&a, "read_file",
                            "C:\\Windows\\a98_escape_canary_4.ini");
        test_sandbox_escape(&a, "write_file",
                            "\\\\.\\pipe\\agent98_escape_5");

        /* Absolute workspace path with .. traversal ("C:/ws/../.." shape). */
        if (GetFullPathNameA(".", A98_MAX_PATH, abs_ws, NULL) > 0) {
            snprintf(abs_escape, sizeof(abs_escape),
                     "%s\\..\\..\\agent98_escape_canary_6.txt", abs_ws);
            test_sandbox_escape(&a, "write_file", abs_escape);
            /* 8.3 short names of the workspace itself must not slip past
             * the prefix comparison either; short names only exist for
             * long components, so ask for the full absolute path. */
            if (GetShortPathNameA(abs_ws, short_ws, sizeof(short_ws)) > 0 &&
                strcmp(short_ws, abs_ws) != 0) {
                snprintf(abs_escape, sizeof(abs_escape),
                         "%s\\..\\..\\agent98_escape_canary_7.txt", short_ws);
                test_sandbox_escape(&a, "write_file", abs_escape);
            } else {
                printf("SKIP: 8.3 short-name escape (no short names here)\n");
            }
        }
    }

    printf("\n-- negative security tests\n");
    test_dangerous_commands(&a);
    test_session_info_full_access(&a);
    test_confirm_gate();

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

        /* The other direction: the model sends UTF-8 while the directory was
         * created in CP_ACP.  resolve_path has to bridge the two, otherwise
         * any Chinese path argument misses. */
        rc = run_capture(&a, "list_dir",
                         "{\"path\":\"build/host/\xe6\x96\xb0\xe5\xbb\xba\"}",
                         captured, sizeof(captured));
        if (rc != 0) {
            printf("FAIL: a UTF-8 path argument did not resolve\n");
            failures++;
        } else {
            printf("PASS: a UTF-8 path argument resolves the CP_ACP directory\n");
        }

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

    /* Round-efficiency features: config defaults, profiles, text-call salvage,
     * unchanged-read cache and the one-shot toolchain probe. */
    {
        Config eff_cfg;
        Agent eff;
        ChatResult res;
        char c1[1024];
        char c2[1024];
        int rc;

        config_defaults(&eff_cfg, NULL);
        if (!eff_cfg.tool_call_fallback || !eff_cfg.read_cache ||
            !eff_cfg.nudge_on_success ||
            strcmp(eff_cfg.tool_profile, "coding") != 0) {
            printf("FAIL: efficiency defaults are not enabled\n");
            failures++;
        } else {
            printf("PASS: efficiency defaults are enabled\n");
        }
        {
            Config rt_cfg;
            Config rt_loaded;
            const char *rt_ini = "build/host/efficiency_test.ini";

            config_defaults(&rt_cfg, "build/host");
            rt_cfg.tool_call_fallback = 0;
            rt_cfg.read_cache = 0;
            rt_cfg.nudge_on_success = 0;
            strcpy(rt_cfg.tool_profile, "docs");
            config_defaults(&rt_loaded, "build/host");
            if (config_save(&rt_cfg, rt_ini) != 0 ||
                config_load(&rt_loaded, rt_ini) != 0 ||
                rt_loaded.tool_call_fallback != 0 ||
                rt_loaded.read_cache != 0 ||
                rt_loaded.nudge_on_success != 0 ||
                strcmp(rt_loaded.tool_profile, "docs") != 0) {
                printf("FAIL: efficiency settings did not survive INI round trip\n");
                failures++;
            } else {
                printf("PASS: efficiency settings survive an INI round trip\n");
            }
        }
        if (tools_profile_allows("coding", "http_request") ||
            !tools_profile_allows("coding", "read_file") ||
            tools_profile_allows("docs", "write_file") ||
            !tools_profile_allows("full", "write_file")) {
            printf("FAIL: tool profile filtering is wrong\n");
            failures++;
        } else {
            printf("PASS: tool profile filtering works\n");
        }

        chat_result_init(&res);
        str_append_cstr(&res.content,
            "TOOLS:\n[{\"name\":\"read_file\","
            "\"arguments\":\"{\\\"path\\\":\\\"x.c\\\"}\"}]");
        if (api_salvage_tool_calls(&res) != 1 || res.call_count != 1 ||
            res.calls[0].name == NULL ||
            strcmp(res.calls[0].name, "read_file") != 0 ||
            strstr(res.calls[0].arguments.data, "x.c") == NULL) {
            printf("FAIL: string-form tool arguments were not salvaged\n");
            failures++;
        } else {
            printf("PASS: string-form tool arguments were salvaged\n");
        }
        chat_result_free(&res);

        chat_result_init(&res);
        str_append_cstr(&res.content,
            "{\"name\":\"write_file\",\"path\":\"a.c\",\"content\":\"x\"}");
        if (api_salvage_tool_calls(&res) != 1 || res.call_count != 1 ||
            res.calls[0].name == NULL ||
            strcmp(res.calls[0].name, "write_file") != 0 ||
            strstr(res.calls[0].arguments.data, "a.c") == NULL) {
            printf("FAIL: flattened tool arguments were not salvaged\n");
            failures++;
        } else {
            printf("PASS: flattened tool arguments were salvaged\n");
        }
        chat_result_free(&res);

        chat_result_init(&res);
        str_append_cstr(&res.content, "I am done; no tools needed.");
        if (api_salvage_tool_calls(&res) != 0 || res.call_count != 0) {
            printf("FAIL: ordinary prose was mistaken for a tool call\n");
            failures++;
        } else {
            printf("PASS: ordinary prose is not a tool call\n");
        }
        chat_result_free(&res);

        chat_result_init(&res);
        str_append_cstr(&res.content,
            "<|DSML|invoke name=\"read_file\">"
            "<|DSML|parameter name=\"path\" string=\"true\">dsml.c"
            "</|DSML|parameter></|DSML|invoke>");
        if (api_salvage_tool_calls(&res) != 1 || res.call_count != 1 ||
            res.calls[0].name == NULL ||
            strcmp(res.calls[0].name, "read_file") != 0 ||
            strstr(res.calls[0].arguments.data, "dsml.c") == NULL) {
            printf("FAIL: DSML tool call was not salvaged\n");
            failures++;
        } else {
            printf("PASS: DSML tool call was salvaged\n");
        }
        chat_result_free(&res);

        strcpy(eff_cfg.workspace, "build/host");
        agent_init(&eff, &eff_cfg);
        tools_read_cache_clear();
        run_capture(&eff, "write_file",
                    "{\"path\":\"cache_test.txt\",\"content\":\"one\\n\"}",
                    c1, sizeof(c1));
        rc = run_capture(&eff, "read_file",
                         "{\"path\":\"cache_test.txt\"}", c1, sizeof(c1));
        rc |= run_capture(&eff, "read_file",
                          "{\"path\":\"cache_test.txt\"}", c2, sizeof(c2));
        if (rc != 0 || strstr(c1, "one") == NULL ||
            strstr(c2, "unchanged since") == NULL) {
            printf("FAIL: unchanged re-read was not short-circuited\n");
            failures++;
        } else {
            printf("PASS: unchanged re-read is short-circuited\n");
        }
        run_capture(&eff, "write_file",
                    "{\"path\":\"cache_test.txt\",\"content\":\"two\\n\"}",
                    c1, sizeof(c1));
        rc = run_capture(&eff, "read_file",
                         "{\"path\":\"cache_test.txt\"}", c2, sizeof(c2));
        if (rc != 0 || strstr(c2, "two") == NULL ||
            strstr(c2, "unchanged since") != NULL) {
            printf("FAIL: write did not invalidate the read cache\n");
            failures++;
        } else {
            printf("PASS: write invalidates the read cache\n");
        }
        agent_free(&eff);

        rc = run_capture(&a, "toolchain_probe", "{}", c1, sizeof(c1));
        if (rc != 0 || strstr(c1, "Toolchain probe:") == NULL ||
            strstr(c1, "Recommended command:") == NULL) {
            printf("FAIL: toolchain_probe did not report a command\n");
            failures++;
        } else {
            printf("PASS: toolchain_probe reports a build command\n");
        }
    }

    if (failures != 0) {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nall tool tests passed\n");
    return 0;
}
