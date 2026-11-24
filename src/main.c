/*
 * AIagent98 - minimal CLI for Windows 98 / modern Windows.
 * Pure C, Win32 console.
 */

#include "agent/agent.h"
#include "agent/tools.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/utf8.h"
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static volatile int g_cancel = 0;
static Str g_pending;

static void console_flush_utf8(void)
{
    char *acp;

    if (g_pending.data == NULL || g_pending.len == 0) {
        return;
    }
    acp = a98_utf8_to_acp(g_pending.data);
    if (acp != NULL) {
        fputs(acp, stdout);
        a98_free(acp);
    }
    g_pending.len = 0;
    g_pending.data[0] = '\0';
    fflush(stdout);
}

/* Write a UTF-8 chunk to the ANSI console, holding back an incomplete
 * multi-byte sequence until the next chunk arrives. */
static void console_write_utf8(const char *s)
{
    size_t i;
    size_t good;
    size_t len;
    char *acp;

    if (s == NULL) {
        return;
    }
    if (g_pending.data == NULL) {
        str_init(&g_pending);
    }
    str_append_cstr(&g_pending, s);
    len = g_pending.len;
    i = 0;
    good = 0;
    while (i < len) {
        unsigned char c = (unsigned char)g_pending.data[i];
        int need;
        if (c < 0x80) {
            need = 0;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            i++;
            good = i;
            continue;
        }
        if (i + (size_t)need >= len) {
            break;
        }
        i += (size_t)need + 1;
        good = i;
    }
    if (good > 0) {
        size_t tail = g_pending.len - good;
        char rest[8];

        /* Copy the incomplete multi-byte sequence *before* writing the NUL:
         * a98_utf8_to_acp() stops at that NUL, and the old code then moved the
         * tail from data + good -- one byte past where the NUL had just been
         * written.  The first byte of the split character became 0, so the
         * whole pending tail (and the next chunk) was silently dropped. */
        if (tail >= sizeof(rest)) {
            tail = sizeof(rest) - 1;   /* unreachable: at most 3 bytes */
        }
        if (tail > 0) {
            memcpy(rest, g_pending.data + good, tail);
        }
        g_pending.data[good] = '\0';
        acp = a98_utf8_to_acp(g_pending.data);
        if (acp != NULL) {
            fputs(acp, stdout);
            a98_free(acp);
        }
        fflush(stdout);
        if (tail > 0) {
            memmove(g_pending.data, rest, tail);
            g_pending.len = tail;
            g_pending.data[tail] = '\0';
        } else {
            g_pending.len = 0;
            g_pending.data[0] = '\0';
        }
    }
}

static void console_puts_utf8(const char *s)
{
    console_flush_utf8();
    if (s != NULL) {
        console_write_utf8(s);
    }
    console_flush_utf8();
}

static char *input_to_utf8(const char *s)
{
    size_t len;

    if (s == NULL) {
        return a98_strdup("");
    }
    len = strlen(s);
    if (a98_utf8_valid(s, len)) {
        return a98_strdup(s);
    }
    return a98_acp_to_utf8(s);
}

static BOOL WINAPI console_ctrl(DWORD type)
{
    (void)type;
    g_cancel = 1;
    return TRUE;
}

static void console_line_utf8(const char *prefix, const char *text)
{
    console_flush_utf8();
    fputs(prefix, stdout);
    console_write_utf8(text != NULL ? text : "");
    fputc('\n', stdout);
    fflush(stdout);
}

/* Write UTF-8 text as the local ANSI code page with DOS CRLF line endings, so
 * /export produces the same bytes as write_file() and the GUI exporter. */
static void export_write_utf8(FILE *f, const char *text)
{
    char *acp;
    const char *src;
    const char *p;
    int prev_cr = 0;

    if (f == NULL) {
        return;
    }
    acp = a98_utf8_to_acp(text != NULL ? text : "");
    src = (acp != NULL) ? acp : (text != NULL ? text : "");
    for (p = src; *p != '\0'; p++) {
        if (*p == '\r') {
            fputc('\r', f);
            prev_cr = 1;
        } else if (*p == '\n') {
            if (!prev_cr) {
                fputc('\r', f);
            }
            fputc('\n', f);
            prev_cr = 0;
        } else {
            fputc(*p, f);
            prev_cr = 0;
        }
    }
    a98_free(acp);
}

static void on_event(void *ctx, AgentEventType type, const char *text)
{
    Agent *a = (Agent *)ctx;

    switch (type) {
    case AGENT_EV_TEXT:
        console_write_utf8(text);
        break;
    case AGENT_EV_ROUND: {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s (tools=%d, failed=%d)",
                 text ? text : "", a != NULL ? a->stats.tool_calls : 0,
                 a != NULL ? a->stats.failed_calls : 0);
        console_line_utf8("\n[round ", buf);
        break;
    }
    case AGENT_EV_TOOL_START:
        console_line_utf8("\n[tool] ", text);
        break;
    case AGENT_EV_TOOL_RESULT:
        console_line_utf8("[result] ", text);
        break;
    case AGENT_EV_INFO:
        console_line_utf8("[info] ", text);
        break;
    default:
        break;
    }
}

static int g_auto_yes = 0;

/* Read one answer line into `out`, trimming leading/trailing whitespace.
 * Returns 0 on EOF/error.  If the user typed more than the buffer holds, the
 * rest of that line is drained here: leaving it in stdin would make the next
 * read at the main prompt look like a command the user never intended. */
static int read_answer(char *out, size_t cap)
{
    char line[256];
    size_t n;
    char *s;

    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (fgets(line, (int)sizeof(line), stdin) == NULL) {
        return 0;
    }
    n = strlen(line);
    if (n > 0 && line[n - 1] != '\n') {
        int ch;
        /* No newline in the buffer: discard the overflow up to EOL/EOF. */
        while ((ch = fgetc(stdin)) != '\n' && ch != EOF) {
            /* discard */
        }
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    s = line;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return 1;
}

static int on_confirm(void *ctx, const char *tool, const char *args_json)
{
    char answer[64];
    char summary[1024];
    int danger = 0;
    (void)ctx;
    if (g_auto_yes) {
        return 1;
    }
    tools_format_confirm(tool, args_json, summary, sizeof(summary), &danger);
    console_flush_utf8();
    fputs("\n========== CONFIRM ==========\n", stdout);
    if (danger >= 2) {
        fputs("*** HIGH RISK ***\n", stdout);
    } else if (danger == 1) {
        fputs("*** WRITE OPERATION ***\n", stdout);
    }
    console_write_utf8(summary);
    fputs("\n=============================\n", stdout);
    fputs(danger >= 2 ? "Allow? type YES to confirm: " : "Allow? [y/N]: ", stdout);
    fflush(stdout);
    if (!read_answer(answer, sizeof(answer))) {
        return 0;
    }
    if (danger >= 2) {
        /* Exact, case-sensitive match: YES/yes/Yes only.  "YESplease" and
         * other prefixes are no longer accepted for high-risk actions. */
        return (strcmp(answer, "YES") == 0 || strcmp(answer, "yes") == 0 ||
                strcmp(answer, "Yes") == 0);
    }
    return (strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 ||
            strcmp(answer, "yes") == 0 || strcmp(answer, "Yes") == 0 ||
            strcmp(answer, "YES") == 0);
}

static void mask_key(const char *key, char *out, size_t cap)
{
    size_t n;
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (key == NULL || key[0] == '\0') {
        strncpy(out, "(empty)", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    n = strlen(key);
    /* Never print leading characters: for OpenAI-style keys they reveal the
     * "sk-" prefix and the key format.  For long keys only the length and the
     * last four characters are shown, so the user can still tell keys apart. */
    if (n > 8) {
        snprintf(out, cap, "****(%u chars, tail=%.4s)", (unsigned)n, key + n - 4);
    } else {
        snprintf(out, cap, "****(%u chars)", (unsigned)n);
    }
}

/* Case-insensitive check for query/fragment parameter names that carry
 * credentials. */
static int url_secret_param(const char *name, size_t len)
{
    static const char *const names[] = {
        "key", "api_key", "apikey", "api-key", "token", "access_token",
        "refresh_token", "auth", "authorization", "secret", "password",
        "pass", "pwd", "code", "sig", "signature"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *nm = names[i];
        size_t k;
        if (strlen(nm) != len) {
            continue;
        }
        for (k = 0; k < len; k++) {
            char a = name[k];
            char b = nm[k];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (k == len) {
            return 1;
        }
    }
    return 0;
}

/* Copy `url` into `out` with credentials removed before display:
 *   - userinfo ("https://user:pass@host/...") is dropped;
 *   - values of key/token/access_token/... query and fragment parameters
 *     become "***".
 * The copy is truncated safely to `cap`. */
static void redact_url(const char *url, char *out, size_t cap)
{
    const char *p;
    const char *userinfo = NULL;
    size_t o = 0;

    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (url == NULL || url[0] == '\0') {
        return;
    }

    /* Find userinfo, if present, in the authority section.  The scheme itself
     * is kept; only "user:pass@" is dropped below. */
    p = strstr(url, "://");
    if (p != NULL) {
        const char *r = p + 3;
        while (*r != '\0' && *r != '/' && *r != '?' && *r != '#' && *r != '@') {
            r++;
        }
        if (*r == '@') {
            userinfo = p + 3;
        }
    }

    /* Copy through the path up to the first query/fragment marker. */
    p = url;
    while (*p != '\0' && *p != '?' && *p != '#' && o + 1 < cap) {
        if (p == userinfo) {
            while (*p != '\0' && *p != '@') {
                p++;
            }
            if (*p == '@') {
                p++;
            }
            userinfo = NULL;
            continue;
        }
        out[o++] = *p++;
    }

    /* '?' (query) and '#' (fragment) use the same name=value&... syntax. */
    while ((*p == '?' || *p == '#') && o + 1 < cap) {
        out[o++] = *p++;
        while (*p != '\0' && *p != '?' && *p != '#' && o + 1 < cap) {
            const char *param = p;
            const char *eq = NULL;
            size_t i;
            size_t name_len;

            while (*p != '\0' && *p != '&' && *p != ';' &&
                   *p != '?' && *p != '#') {
                if (*p == '=' && eq == NULL) {
                    eq = p;
                }
                p++;
            }
            name_len = (eq != NULL) ? (size_t)(eq - param) : (size_t)(p - param);
            if (eq != NULL && url_secret_param(param, name_len)) {
                /* Keep "name=" but replace the value with a fixed mask. */
                for (i = 0; i <= name_len && o + 1 < cap; i++) {
                    out[o++] = param[i];
                }
                {
                    const char *mask = "***";
                    size_t m;
                    for (m = 0; mask[m] != '\0' && o + 1 < cap; m++) {
                        out[o++] = mask[m];
                    }
                }
            } else {
                size_t plen = (size_t)(p - param);
                for (i = 0; i < plen && o + 1 < cap; i++) {
                    out[o++] = param[i];
                }
            }
            if (*p == '&' || *p == ';') {
                if (o + 1 < cap) {
                    out[o++] = *p;
                }
                p++;
            }
        }
    }
    out[o] = '\0';
}

static void print_usage_argv(void)
{
    printf("Usage: agent98_cli [options]\n");
    printf("  --ini PATH         Config INI path\n");
    printf("  --workspace PATH   Override workspace\n");
    printf("  --message TEXT     Send one message and exit\n");
    printf("  --yes, -y          Auto-allow tool confirms (DANGEROUS)\n");
    printf("  --no-shell         Disable shell commands\n");
    printf("  -h, --help         Show this help\n");
}


static void ensure_workspace(const char *path)
{
    if (path == NULL || path[0] == '\0') return;
    CreateDirectoryA(path, NULL);
}

/* Split an interactive command into its token (ending at the first whitespace
 * or at the NUL) and the argument that follows it.  Returns the token length. */
static size_t command_token(const char *cmd, const char **arg_out)
{
    size_t n = 0;

    while (cmd[n] != '\0' && cmd[n] != ' ' && cmd[n] != '\t') {
        n++;
    }
    if (arg_out != NULL) {
        const char *a = cmd + n;
        while (*a == ' ' || *a == '\t') {
            a++;
        }
        *arg_out = a;
    }
    return n;
}

/* Exact comparison of the command token against a literal command name. */
static int token_equals(const char *cmd, size_t toklen, const char *name)
{
    size_t n = strlen(name);
    return toklen == n && strncmp(cmd, name, n) == 0;
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  /help              Show this help\n");
    printf("  /new               Start a new conversation\n");
    printf("  /stop              Show how to cancel (use Ctrl+C for a running request)\n");
    printf("  /save [path]       Save session\n");
    printf("  /load [path]       Load session\n");
    printf("  /continue          Load the last session (/load with no path)\n");
    printf("  /stats             Show rounds/tool calls from the last run\n");
    printf("  /export [path]     Export transcript as text\n");
    printf("  /config            Show config (API key masked, URL credentials redacted)\n");
    printf("  /tools             List available tools\n");
    printf("  /workspace [path]  Show or set workspace for this process\n");
    printf("  /model [name]      Show or set model for this process\n");
    printf("  /quit              Exit\n");
    printf("Note: /save, /load and /export accept a path typed by you; they are NOT\n");
    printf("      restricted to the agent workspace sandbox (the model cannot call them).\n");
}

int main(int argc, char **argv)
{
    char exe_dir[A98_MAX_PATH];
    char ini_path[A98_MAX_PATH];
    char log_path[A98_MAX_PATH];
    char sess_path[A98_MAX_PATH];
    char input[4096];
    char err[512];
    char key_masked[128];
    char api_base_redacted[640];
    Config cfg;
    Agent agent;
    int have_key;
    int i;
    const char *opt_ini = NULL;
    const char *opt_workspace = NULL;
    const char *opt_message = NULL;
    int opt_no_shell = 0;
    int oneshot = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage_argv();
            return 0;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            g_auto_yes = 1;
        } else if (strcmp(argv[i], "--no-shell") == 0) {
            opt_no_shell = 1;
        } else if (strcmp(argv[i], "--ini") == 0 && i + 1 < argc) {
            opt_ini = argv[++i];
        } else if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            opt_workspace = argv[++i];
        } else if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) {
            opt_message = argv[++i];
            oneshot = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage_argv();
            return 1;
        }
    }

    SetConsoleCtrlHandler(console_ctrl, TRUE);

    if (a98_path_exe_dir(exe_dir, sizeof(exe_dir)) != 0) {
        strcpy(exe_dir, ".");
    }

    a98_path_join(log_path, sizeof(log_path), exe_dir, "agent98.log");
    /* Start at INFO so config_load() warnings are captured; the configured
     * level is applied once the INI has been read. */
    log_init(log_path, LOG_LEVEL_INFO);

    if (opt_ini != NULL) {
        strncpy(ini_path, opt_ini, sizeof(ini_path) - 1);
        ini_path[sizeof(ini_path) - 1] = '\0';
    } else {
        a98_path_join(ini_path, sizeof(ini_path), exe_dir, "AGENT98.INI");
    }

    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg, ini_path);
    if (cfg.log_level < LOG_LEVEL_ERROR) {
        cfg.log_level = LOG_LEVEL_ERROR;
    } else if (cfg.log_level > LOG_LEVEL_TRACE) {
        cfg.log_level = LOG_LEVEL_TRACE;
    }
    log_set_level(cfg.log_level);
    if (!a98_path_exists(ini_path)) {
        config_save(&cfg, ini_path);
    }

    if (opt_workspace != NULL && opt_workspace[0] != '\0') {
        strncpy(cfg.workspace, opt_workspace, sizeof(cfg.workspace) - 1);
        cfg.workspace[sizeof(cfg.workspace) - 1] = '\0';
    }
    if (opt_no_shell) {
        cfg.allow_shell = 0;
    }

    log_info("Workspace: %s", cfg.workspace);
    ensure_workspace(cfg.workspace);

    if (net_init() != 0) {
        fprintf(stderr, "Failed to initialize network (Winsock).\n");
        log_shutdown();
        return 1;
    }

    agent_init(&agent, &cfg);
    agent.on_event = on_event;
    agent.event_ctx = &agent;
    agent.on_confirm = on_confirm;
    agent.confirm_ctx = NULL;
    agent.cancel = &g_cancel;

    have_key = cfg.api_key[0] != '\0';
    printf("AIagent98 CLI\n");
    printf("Config: %s\n", ini_path);
    printf("Log: %s\n", log_path);
    printf("Workspace: %s\n", cfg.workspace);
    printf("Model: %s\n", cfg.model);
    redact_url(cfg.api_base, api_base_redacted, sizeof(api_base_redacted));
    printf("API base: %s\n", api_base_redacted);
    if (g_auto_yes) {
        printf("WARNING: --yes is set; tool confirms are auto-allowed.\n");
    }
    if (!have_key) {
        printf("\nWARNING: api.key is empty in %s\n", ini_path);
        printf("Edit [api] key=... then restart.\n");
    }
    if (!oneshot) {
        printf("\nType /help for commands. Ctrl+C cancels current request.\n\n");
    }

    if (oneshot) {
        char *cmd;
        int status = 1;

        if (!have_key) {
            fprintf(stderr, "api.key is not set.\n");
            goto oneshot_cleanup;
        }
        g_cancel = 0;
        cmd = input_to_utf8(opt_message);
        if (cmd == NULL) {
            cmd = a98_strdup(opt_message);
        }
        err[0] = '\0';
        if (agent_run(&agent, cmd, err, sizeof(err)) != 0) {
            if (err[0]) console_line_utf8("[error] ", err);
            else printf("\n[error] agent_run failed\n");
            a98_free(cmd);
            goto oneshot_cleanup;
        }
        console_flush_utf8();
        printf("\n");
        a98_free(cmd);
        status = 0;

oneshot_cleanup:
        /* All oneshot exits must release the console's pending UTF-8 buffer,
         * the agent, Winsock and the log file just like the interactive path. */
        str_free(&g_pending);
        agent_free(&agent);
        net_cleanup();
        log_shutdown();
        return status;
    }

    for (;;) {
        char *line;
        char *cmd;

        g_cancel = 0;
        printf("> ");
        fflush(stdout);
        line = fgets(input, (int)sizeof(input), stdin);
        if (line == NULL) {
            break;
        }
        {
            size_t n = strlen(input);
            while (n > 0 && (input[n - 1] == '\n' || input[n - 1] == '\r')) {
                input[--n] = '\0';
            }
        }
        if (input[0] == '\0') {
            continue;
        }

        cmd = input_to_utf8(input);
        if (cmd == NULL) {
            continue;
        }

        if (cmd[0] == '/') {
            const char *arg;
            size_t toklen = command_token(cmd, &arg);

            if (arg[0] == '\0' && token_equals(cmd, toklen, "/help")) {
                print_help();
            } else if (arg[0] == '\0' &&
                       (token_equals(cmd, toklen, "/quit") ||
                        token_equals(cmd, toklen, "/exit"))) {
                a98_free(cmd);
                break;
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/new")) {
                agent_new_conversation(&agent);
                printf("[info] New conversation started.\n");
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/stop")) {
                /* agent_run() runs synchronously on this thread, so stdin at
                 * this prompt cannot be used to interrupt a request already in
                 * flight; only the console Ctrl+C handler sets g_cancel.  Say
                 * so instead of pretending /stop cancels anything. */
                printf("[info] /stop cannot interrupt a running request; press Ctrl+C instead.\n");
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/tools")) {
                int count = 0, ti;
                const ToolDef *all = tools_all(&count);
                printf("Available tools (%d):\n", count);
                for (ti = 0; ti < count; ti++) {
                    printf("  %-14s %s%s\n", all[ti].name,
                           all[ti].description ? all[ti].description : "",
                           all[ti].needs_confirm ? " [confirm]" : "");
                }
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/config")) {
                mask_key(cfg.api_key, key_masked, sizeof(key_masked));
                redact_url(cfg.api_base, api_base_redacted, sizeof(api_base_redacted));
                printf("Config path: %s\n", ini_path);
                printf("api.base=%s\napi.model=%s\napi.key=%s\nworkspace=%s\n"
                       "tls.verify=%d\nallow_shell=%d\n",
                       api_base_redacted, cfg.model, key_masked,
                       cfg.workspace, cfg.tls_verify, cfg.allow_shell);
            } else if (token_equals(cmd, toklen, "/workspace")) {
                if (arg[0] == '\0') {
                    printf("workspace=%s\n", cfg.workspace);
                } else {
                    strncpy(cfg.workspace, arg, sizeof(cfg.workspace) - 1);
                    cfg.workspace[sizeof(cfg.workspace) - 1] = '\0';
                    ensure_workspace(cfg.workspace);
                    strncpy(agent.workspace, cfg.workspace, sizeof(agent.workspace) - 1);
                    agent.workspace[sizeof(agent.workspace) - 1] = '\0';
                    printf("[info] workspace set to %s\n", cfg.workspace);
                }
            } else if (token_equals(cmd, toklen, "/model")) {
                if (arg[0] == '\0') {
                    printf("model=%s\n", cfg.model);
                } else {
                    strncpy(cfg.model, arg, sizeof(cfg.model) - 1);
                    cfg.model[sizeof(cfg.model) - 1] = '\0';
                    printf("[info] model set to %s\n", cfg.model);
                }
            } else if (token_equals(cmd, toklen, "/save")) {
                const char *sp;
                if (arg[0] == '\0') {
                    agent_last_session_path(sess_path, sizeof(sess_path));
                    sp = sess_path;
                } else {
                    sp = arg;
                }
                err[0] = '\0';
                if (agent_save_session(&agent, sp, err, sizeof(err)) == 0) {
                    printf("[info] Session saved: %s\n", sp);
                } else {
                    printf("[error] Save failed: %s\n", err[0] ? err : "unknown");
                }
            } else if (token_equals(cmd, toklen, "/load")) {
                const char *sp;
                if (arg[0] == '\0') {
                    agent_last_session_path(sess_path, sizeof(sess_path));
                    sp = sess_path;
                } else {
                    sp = arg;
                }
                err[0] = '\0';
                if (agent_load_session(&agent, sp, err, sizeof(err)) == 0) {
                    printf("[info] Session loaded: %s\n", sp);
                } else {
                    printf("[error] Load failed: %s\n", err[0] ? err : "unknown");
                }
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/continue")) {
                agent_last_session_path(sess_path, sizeof(sess_path));
                err[0] = '\0';
                if (agent_load_session(&agent, sess_path, err, sizeof(err)) == 0) {
                    printf("[info] Continued from %s\n", sess_path);
                } else {
                    printf("[error] Continue failed: %s\n",
                           err[0] ? err : "unknown");
                }
            } else if (arg[0] == '\0' && token_equals(cmd, toklen, "/stats")) {
                AgentStats st;
                agent_stats(&agent, &st);
                printf("Last run: %d round(s), %d tool call(s), %d failed, "
                       "%d blocked, %d hint(s)\n",
                       st.rounds, st.tool_calls, st.failed_calls,
                       st.blocked_calls, st.nudge_count);
            } else if (token_equals(cmd, toklen, "/export")) {
                FILE *ef;
                int mi;
                /* The path is typed by the user at the prompt, not supplied by
                 * the model, so it is deliberately NOT confined to the
                 * agent workspace sandbox. */
                if (arg[0] == '\0') {
                    printf("usage: /export path.txt\n");
                } else {
                    ef = fopen(arg, "wb");
                    if (ef == NULL) {
                        printf("[error] cannot write %s\n", arg);
                    } else {
                        for (mi = 0; mi < agent.messages.count; mi++) {
                            Message *msg = &agent.messages.items[mi];
                            fprintf(ef, "### %s\r\n", msg_role_name(msg->role));
                            if (msg->content != NULL) {
                                export_write_utf8(ef, msg->content);
                            }
                            fputs("\r\n", ef);
                        }
                        fclose(ef);
                        printf("[info] Exported to %s\n", arg);
                    }
                }
            } else {
                printf("Unknown command. Type /help.\n");
            }
            a98_free(cmd);
            continue;
        }

        err[0] = '\0';
        if (cfg.api_key[0] == '\0') {
            printf("[error] api.key is not set. Edit %s and restart.\n", ini_path);
            a98_free(cmd);
            continue;
        }

        printf("\n");
        fflush(stdout);
        if (agent_run(&agent, cmd, err, sizeof(err)) != 0) {
            if (err[0] != '\0') {
                console_line_utf8("[error] ", err);
            } else {
                printf("\n[error] agent_run failed\n");
            }
        }
        console_flush_utf8();
        printf("\n\n");
        a98_free(cmd);
    }

    console_flush_utf8();
    str_free(&g_pending);
    agent_free(&agent);
    net_cleanup();
    log_shutdown();
    return 0;
}
