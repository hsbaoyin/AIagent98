#include "api_openai.h"
#include "tools.h"
#include "tools_skills.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/utf8.h"
#include "../net/http.h"
#include "../net/sse.h"
#include "../net/url.h"
#include "../../third_party/cjson/cJSON.h"

#include <windows.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

void chat_result_init(ChatResult *r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    str_init(&r->content);
}

void chat_result_free(ChatResult *r)
{
    int i;

    if (r == NULL) {
        return;
    }
    for (i = 0; i < r->call_count; i++) {
        a98_free(r->calls[i].id);
        a98_free(r->calls[i].name);
        str_free(&r->calls[i].arguments);
    }
    a98_free(r->calls);
    r->calls = NULL;
    r->call_count = 0;
    r->call_cap = 0;
    str_free(&r->content);
}

static ToolCall *result_call_for_index(ChatResult *r, int index)
{
    int i;

    for (i = 0; i < r->call_count; i++) {
        if (r->calls[i].index == index) {
            return &r->calls[i];
        }
    }
    if (r->call_count == r->call_cap) {
        int cap = r->call_cap ? r->call_cap * 2 : 4;
        ToolCall *calls = (ToolCall *)a98_realloc(r->calls,
                                                  (size_t)cap * sizeof(ToolCall));
        if (calls == NULL) {
            return NULL;
        }
        r->calls = calls;
        r->call_cap = cap;
    }
    memset(&r->calls[r->call_count], 0, sizeof(ToolCall));
    r->calls[r->call_count].index = index;
    str_init(&r->calls[r->call_count].arguments);
    return &r->calls[r->call_count++];
}

/* All three helpers report allocation failures so a truncated tool name/id
 * can never be sent back to the provider (or matched to the wrong call). */
static int append_dup(char **dst, const char *src)
{
    char *joined;

    if (src == NULL) {
        return 0;
    }
    if (*dst == NULL) {
        *dst = a98_strdup(src);
        return *dst != NULL ? 0 : -1;
    }
    joined = a98_strfmt("%s%s", *dst, src);
    if (joined == NULL) {
        return -1;
    }
    a98_free(*dst);
    *dst = joined;
    return 0;
}

/*
 * Streaming providers do not agree on how a tool call's name and id are sent.
 * OpenAI sends each one once, but several OpenAI-compatible servers (proxies,
 * local runtimes) repeat the complete value in every delta.  Blindly appending
 * produced "list_dirlist_dir" -- an unknown tool -- and a mangled
 * tool_call_id, which the provider rejects with HTTP 400 on the next round.
 *
 * A fragment that already begins with what we have is a repeat (keep the
 * longer value); anything else is a real continuation and gets appended.
 */
static int merge_stream_field(char **dst, const char *frag)
{
    size_t have;

    if (frag == NULL || frag[0] == '\0') {
        return 0;
    }
    if (*dst == NULL) {
        *dst = a98_strdup(frag);
        return *dst != NULL ? 0 : -1;
    }
    have = strlen(*dst);
    if (strncmp(*dst, frag, have) == 0) {
        if (frag[have] != '\0') {
            char *full = a98_strdup(frag);

            if (full == NULL) {
                return -1;
            }
            a98_free(*dst);
            *dst = full;
        }
        return 0;
    }
    return append_dup(dst, frag);
}

static int apply_error_object(cJSON *root, char *err, size_t errcap)
{
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const char *msg = NULL;

    if (error == NULL) {
        return 0;
    }
    if (cJSON_IsString(error)) {
        msg = error->valuestring;
    } else if (cJSON_IsObject(error)) {
        msg = json_string(error, "message");
        if (msg == NULL) {
            msg = json_string(error, "type");
        }
    }
    if (msg == NULL) {
        msg = "provider returned an unknown error";
    }
    set_err(err, errcap, "API error: %s", msg);
    return -1;
}

static int apply_tool_calls_array(ChatResult *out, cJSON *calls, int delta_mode,
                                  char *err, size_t errcap)
{
    cJSON *item;
    int i = 0;

    if (calls == NULL || !cJSON_IsArray(calls)) {
        return 0;
    }
    cJSON_ArrayForEach(item, calls) {
        int index = json_int(item, "index", i);
        const char *id = json_string(item, "id");
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(item, "function");
        const char *name = json_string(fn, "name");
        const char *args = json_string(fn, "arguments");
        ToolCall *call;

        if (out->call_count >= 32) {
            set_err(err, errcap, "model returned too many tool calls");
            return -1;
        }
        if (!delta_mode) {
            /* Complete calls: always create a new entry. */
            index = out->call_count;
        }
        call = result_call_for_index(out, index);
        if (call == NULL) {
            set_err(err, errcap, "out of memory");
            return -1;
        }
        if (delta_mode) {
            if (merge_stream_field(&call->id, id) != 0 ||
                merge_stream_field(&call->name, name) != 0) {
                set_err(err, errcap, "out of memory");
                return -1;
            }
        } else {
            if (id != NULL && id[0] != '\0' && call->id == NULL) {
                call->id = a98_strdup(id);
                if (call->id == NULL) {
                    set_err(err, errcap, "out of memory");
                    return -1;
                }
            }
            if (name != NULL && name[0] != '\0' && call->name == NULL) {
                call->name = a98_strdup(name);
                if (call->name == NULL) {
                    set_err(err, errcap, "out of memory");
                    return -1;
                }
            }
        }
        /* A delta that cannot be stored must fail the whole response: a
         * partially accumulated arguments string is still valid JSON and
         * would otherwise send the tool down the wrong path. */
        if (args != NULL && args[0] != '\0' &&
            str_append_cstr(&call->arguments, args) != 0) {
            set_err(err, errcap, "out of memory");
            return -1;
        }
        i++;
    }
    return 0;
}

int openai_apply_chunk(ChatResult *out, const char *json, size_t len,
                       ApiTextFn on_text, void *ctx, char *err, size_t errcap)
{
    cJSON *root;
    cJSON *choices;
    cJSON *choice;
    cJSON *delta;
    cJSON *content;
    cJSON *calls;
    const char *finish;

    if (out == NULL) {
        return -1;
    }
    root = json_parse(json, len);
    if (root == NULL) {
        set_err(err, errcap, "provider sent invalid JSON in the stream");
        return -1;
    }
    if (apply_error_object(root, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return 0;
    }
    choice = cJSON_GetArrayItem(choices, 0);
    delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    if (delta != NULL) {
        content = cJSON_GetObjectItemCaseSensitive(delta, "content");
        if (cJSON_IsString(content) && content->valuestring != NULL &&
            content->valuestring[0] != '\0') {
            size_t n = strlen(content->valuestring);
            if (str_append(&out->content, content->valuestring, n) != 0) {
                cJSON_Delete(root);
                set_err(err, errcap, "out of memory");
                return -1;
            }
            if (on_text != NULL && on_text(ctx, content->valuestring, n) != 0) {
                cJSON_Delete(root);
                set_err(err, errcap, "response display aborted");
                return -1;
            }
        }
        calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        if (apply_tool_calls_array(out, calls, 1, err, errcap) != 0) {
            cJSON_Delete(root);
            return -1;
        }
    }
    finish = json_string(choice, "finish_reason");
    if (finish != NULL) {
        strncpy(out->finish_reason, finish, sizeof(out->finish_reason) - 1);
        out->finish_reason[sizeof(out->finish_reason) - 1] = '\0';
    }
    cJSON_Delete(root);
    return 0;
}

int openai_apply_full(ChatResult *out, const char *json, size_t len,
                      char *err, size_t errcap)
{
    cJSON *root;
    cJSON *choices;
    cJSON *choice;
    cJSON *message;
    cJSON *content;
    cJSON *calls;
    const char *finish;
    const char *text;

    if (out == NULL) {
        return -1;
    }
    root = json_parse(json, len);
    if (root == NULL) {
        set_err(err, errcap, "provider sent invalid JSON");
        return -1;
    }
    if (apply_error_object(root, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        set_err(err, errcap, "provider response has no choices");
        return -1;
    }
    choice = cJSON_GetArrayItem(choices, 0);
    message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (message == NULL) {
        cJSON_Delete(root);
        set_err(err, errcap, "provider response has no message");
        return -1;
    }
    content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL &&
        str_append_cstr(&out->content, content->valuestring) != 0) {
        cJSON_Delete(root);
        set_err(err, errcap, "out of memory");
        return -1;
    }
    calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (apply_tool_calls_array(out, calls, 0, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    finish = json_string(choice, "finish_reason");
    if (finish != NULL) {
        strncpy(out->finish_reason, finish, sizeof(out->finish_reason) - 1);
        out->finish_reason[sizeof(out->finish_reason) - 1] = '\0';
    }
    text = json_string(root, "model");
    if (text != NULL) {
        log_debug("response model: %s", text);
    }
    cJSON_Delete(root);
    return 0;
}

static cJSON *message_to_json(const Message *m)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *content_item;

    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "role", msg_role_name(m->role));
    if (m->role == MSG_ASSISTANT) {
        if (m->content != NULL && m->content[0] != '\0') {
            cJSON_AddStringToObject(obj, "content", m->content);
        } else {
            cJSON_AddNullToObject(obj, "content");
        }
        if (m->tool_calls_json != NULL && m->tool_calls_json[0] != '\0') {
            cJSON *calls = cJSON_Parse(m->tool_calls_json);
            if (calls != NULL) {
                cJSON_AddItemToObject(obj, "tool_calls", calls);
            }
        }
    } else if (m->role == MSG_TOOL) {
        cJSON_AddStringToObject(obj, "tool_call_id",
                                m->tool_call_id ? m->tool_call_id : "");
        cJSON_AddStringToObject(obj, "content", m->content ? m->content : "");
        if (m->name != NULL && m->name[0] != '\0') {
            cJSON_AddStringToObject(obj, "name", m->name);
        }
    } else {
        content_item = cJSON_CreateString(m->content ? m->content : "");
        if (content_item != NULL) {
            cJSON_AddItemToObject(obj, "content", content_item);
        }
    }
    return obj;
}

static cJSON *build_tools(const Config *cfg)
{
    const ToolDef *defs;
    int count = 0;
    int i;
    cJSON *tools;

    if (tools_all(&count) == NULL || count == 0) {
        return NULL;
    }
    defs = tools_all(&count);
    tools = cJSON_CreateArray();
    if (tools == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        cJSON *tool;
        cJSON *fn;
        cJSON *params;

        if (!tools_profile_allows(cfg ? cfg->tool_profile : NULL,
                                  defs[i].name)) {
            continue;
        }
        tool = cJSON_CreateObject();
        fn = cJSON_CreateObject();
        if (tool == NULL || fn == NULL) {
            cJSON_Delete(tool);
            cJSON_Delete(fn);
            continue;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", defs[i].name);
        cJSON_AddStringToObject(fn, "description", defs[i].description);
        params = cJSON_Parse(defs[i].params_json ? defs[i].params_json : "{}");
        if (params == NULL) {
            params = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(tools, tool);
    }
    return tools;
}

static void append_text_as_utf8(Str *out, const char *text)
{
    char *utf8;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    if (a98_utf8_valid(text, strlen(text))) {
        str_append_cstr(out, text);
        return;
    }
    utf8 = a98_acp_to_utf8(text);
    if (utf8 != NULL) {
        str_append_cstr(out, utf8);
        a98_free(utf8);
    } else {
        str_append_cstr(out, text);
    }
}

static void format_windows_version(char *out, size_t cap)
{
    OSVERSIONINFOA vi;
    const char *name;
    char csd[64];

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA(&vi)) {
        snprintf(out, cap, "Windows 9x");
        return;
    }
    if (vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
        if (vi.dwMajorVersion == 4 && vi.dwMinorVersion == 10) {
            name = vi.dwBuildNumber >= 2222 ? "Windows 98 Second Edition"
                                            : "Windows 98";
        } else if (vi.dwMajorVersion == 4 && vi.dwMinorVersion == 90) {
            name = "Windows Millennium Edition";
        } else if (vi.dwMajorVersion == 4 && vi.dwMinorVersion == 0) {
            name = "Windows 95";
        } else {
            name = "Windows 9x";
        }
    } else if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        name = "Windows NT";
    } else {
        name = "Windows";
    }
    strncpy(csd, vi.szCSDVersion, sizeof(csd) - 1);
    csd[sizeof(csd) - 1] = '\0';
    config_trim(csd);
    snprintf(out, cap, "%s %lu.%lu.%lu%s%s", name,
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
             csd[0] != '\0' ? " " : "", csd);
}

/*
 * Prepend a small runtime preamble to the user's system prompt.  The model
 * needs to know that it is really running on a 9x machine, where the shell
 * is COMMAND.COM rather than cmd.exe or PowerShell.  This keeps it from
 * suggesting commands that cannot work there.
 */
static char *build_system_prompt(const Config *cfg, const char *user_prompt)
{
    Str s;
    char os[160];
    char cwd[A98_MAX_PATH];
    DWORD cwd_len;

    str_init(&s);
    format_windows_version(os, sizeof(os));
    str_append_cstr(&s, "Runtime environment (detected automatically):\n");
    str_appendf(&s, "- OS: %s\n", os);
    str_append_cstr(&s, "- Workspace: ");
    if (cfg != NULL) {
        append_text_as_utf8(&s, cfg->workspace);
    }
    str_append_cstr(&s, " (all relative tool paths resolve here)\n");

    cwd_len = GetCurrentDirectoryA(sizeof(cwd), cwd);
    if (cwd_len > 0 && cwd_len < sizeof(cwd)) {
        str_append_cstr(&s, "- Current directory: ");
        append_text_as_utf8(&s, cwd);
        str_append_cstr(&s, "\n");
    }

    str_append_cstr(&s,
        "- Shell: run_command uses the legacy 16-bit COMMAND.COM, NOT "
        "cmd.exe or PowerShell. Do not use 2>&1, &&, ||, stderr pipes, "
        "curl, or tasklist. Prefer the built-in file tools; use ver, mem, "
        "dir, set, or type for system information.\n"
        "- Batch scripts: Win9x COMMAND.COM only runs .BAT files; a .CMD file "
        "is NOT a batch script and gives 'Bad command or file name'. Always "
        "write scripts as name.BAT and invoke name.BAT (or CALL name.BAT).\n"
        "- COMMAND.COM batch syntax is the old one: no FOR /F, no SET /A, no "
        "IF /I, no %~ modifiers, no delayed expansion (!var!), no PUSHD. "
        "Use plain FOR %%L IN (set) DO, IF EXIST, GOTO :label, CALL :label, "
        "SHIFT, and %1..%9. Test a small BAT first before building on it; if "
        "a script fails, fix the script instead of retrying the same command "
        "with different spellings.\n"
        "- Work efficiently: do not re-read a file with the same path and "
        "arguments if its contents are already in the conversation. Re-read "
        "only after you changed it, or when you need a specific range you "
        "have not seen. When the requested checks (for example, a test "
        "command) have passed, stop calling tools and give the final answer.\n");

    /* Procedural memory: list available skills (progressive disclosure). */
    if (cfg != NULL) {
        char *skills = skills_prompt_block(cfg->workspace);
        if (skills != NULL) {
            if (skills[0] != '\0') str_append_cstr(&s, skills);
            a98_free(skills);
        }
    }

    if (user_prompt != NULL && user_prompt[0] != '\0') {
        str_append_cstr(&s, "\nUser instructions:\n");
        append_text_as_utf8(&s, user_prompt);
    }
    return str_detach(&s);
}

static char *build_request_json(const Config *cfg, const char *system_prompt,
                                const MessageList *messages,
                                char *err, size_t errcap)
{
    cJSON *root;
    cJSON *arr;
    cJSON *tools;
    int i;
    char *text;

    root = cJSON_CreateObject();
    if (root == NULL) {
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "max_tokens", (double)cfg->max_tokens);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(root, "stream", cfg->stream ? 1 : 0);
    if (cfg->stream) {
        cJSON *opts = cJSON_AddObjectToObject(root, "stream_options");
        if (opts != NULL) {
            cJSON_AddBoolToObject(opts, "include_usage", 0);
        }
    }

    arr = cJSON_AddArrayToObject(root, "messages");
    if (arr == NULL) {
        cJSON_Delete(root);
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    {
        char *sys_text = build_system_prompt(cfg, system_prompt);
        cJSON *sys;

        if (sys_text == NULL) {
            cJSON_Delete(root);
            set_err(err, errcap, "out of memory");
            return NULL;
        }
        sys = cJSON_CreateObject();
        if (sys == NULL ||
            cJSON_AddStringToObject(sys, "role", "system") == NULL ||
            cJSON_AddStringToObject(sys, "content", sys_text) == NULL) {
            if (sys != NULL) {
                cJSON_Delete(sys);
            }
            a98_free(sys_text);
            cJSON_Delete(root);
            set_err(err, errcap, "out of memory");
            return NULL;
        }
        cJSON_AddItemToArray(arr, sys);
        a98_free(sys_text);
    }
    for (i = 0; i < messages->count; i++) {
        cJSON *obj = message_to_json(&messages->items[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(arr, obj);
        }
    }

    tools = build_tools(cfg);
    if (tools != NULL) {
        cJSON_AddItemToObject(root, "tools", tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        set_err(err, errcap, "out of memory while encoding the request");
    }
    return text;
}

static int value_has_line_break(const char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        if (*s == '\r' || *s == '\n') {
            return 1;
        }
    }
    return 0;
}

char *api_build_headers(const Config *cfg, int stream)
{
    Str h;
    const char *p;
    const char *k;
    int failed = 0;

    str_init(&h);
    if (cfg->api_key[0] != '\0') {
        /* A pasted key with a stray line break would otherwise split the
         * request or inject a header.  Strip it and say so in the log. */
        if (value_has_line_break(cfg->api_key)) {
            log_warn("api.key contains a line break; it was removed from the "
                     "Authorization header");
        }
        if (str_append_cstr(&h, "Authorization: Bearer ") != 0) failed = 1;
        for (k = cfg->api_key; *k != '\0' && !failed; k++) {
            if (*k != '\r' && *k != '\n') {
                if (str_append_char(&h, *k) != 0) failed = 1;
            }
        }
        if (str_append_cstr(&h, "\r\n") != 0) failed = 1;
    }
    if (str_append_cstr(&h, "Content-Type: application/json\r\n") != 0) failed = 1;
    if (str_appendf(&h, "Accept: %s\r\n",
                    stream ? "text/event-stream" : "application/json") != 0) {
        failed = 1;
    }
    if (cfg->extra_headers[0] != '\0') {
        p = cfg->extra_headers;
        while (*p != '\0' && !failed) {
            const char *start = p;
            size_t len;
            while (*p != '\0' && *p != '|' && *p != '\n') {
                p++;
            }
            len = (size_t)(p - start);
            while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == ' ')) {
                len--;
            }
            while (len > 0 && (*start == ' ' || *start == '\t')) {
                start++;
                len--;
            }
            if (len > 0) {
                size_t i;

                for (i = 0; i < len; i++) {
                    if (start[i] != '\r' && start[i] != '\n') {
                        if (str_append_char(&h, start[i]) != 0) {
                            failed = 1;
                            break;
                        }
                    }
                }
                if (!failed && str_append_cstr(&h, "\r\n") != 0) failed = 1;
            }
            if (*p == '|' || *p == '\n') {
                p++;
            }
        }
    }
    /* A half-built header block would produce a malformed request. */
    if (failed) {
        str_free(&h);
        return NULL;
    }
    return str_detach(&h);
}

static int make_endpoint(const char *base, Url *url, char *err, size_t errcap)
{
    char tmp[900];
    size_t len;
    const char *suffix = "/chat/completions";

    if (base == NULL || base[0] == '\0') {
        set_err(err, errcap, "API base URL is empty");
        return -1;
    }
    if (strlen(base) >= sizeof(tmp)) {
        set_err(err, errcap, "API base URL is too long");
        return -1;
    }
    strcpy(tmp, base);
    len = strlen(tmp);
    {
        /* Strip trailing slashes, but never the "https://" separator itself: a
         * base of "http://a/" used to become "http://a//chat/completions". */
        size_t scheme_len = 0;

        if (a98_strncasecmp(tmp, "https://", 8) == 0) {
            scheme_len = 8;
        } else if (a98_strncasecmp(tmp, "http://", 7) == 0) {
            scheme_len = 7;
        }
        while (len > scheme_len && tmp[len - 1] == '/') {
            tmp[--len] = '\0';
        }
    }
    if (len < strlen(suffix) || a98_strcasecmp(tmp + len - strlen(suffix), suffix) != 0) {
        if (len + strlen(suffix) + 1 > sizeof(tmp)) {
            set_err(err, errcap, "API base URL is too long");
            return -1;
        }
        strcat(tmp, suffix);
    }
    if (url_parse(tmp, url, err, errcap) != 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    NetConn *conn;
    int timeout_sec;
    const HttpResponse *resp;
    int streaming;
    ChatResult *result;
    ApiTextFn on_text;
    void *user;
    SseParser sse;
    Str raw;
    Str error_body;
    int failed;
    char err[320];
} SinkCtx;

static int sse_callback(void *vctx, const char *event, const char *data)
{
    SinkCtx *s = (SinkCtx *)vctx;
    size_t len;

    A98_UNUSED(event);
    if (s->failed) {
        return -1;
    }
    if (data == NULL) {
        return 0;
    }
    len = strlen(data);
    if (strcmp(data, "[DONE]") == 0) {
        s->result->got_done = 1;
        return 0;
    }
    if (openai_apply_chunk(s->result, data, len, s->on_text, s->user,
                           s->err, sizeof(s->err)) != 0) {
        s->failed = 1;
        return -1;
    }
    return 0;
}

static int body_sink(void *vctx, const char *data, size_t len)
{
    SinkCtx *s = (SinkCtx *)vctx;

    if (s->conn != NULL && s->timeout_sec > 0) {
        net_set_deadline(s->conn, (unsigned long)s->timeout_sec);
    }
    if (s->resp != NULL && s->resp->status != 200) {
        if (s->error_body.len < 64 * 1024) {
            return str_append(&s->error_body, data, len);
        }
        return 0;
    }
    if (s->streaming) {
        if (sse_feed(&s->sse, data, len, sse_callback, s) != 0) {
            if (!s->failed) {
                set_err(s->err, sizeof(s->err), "invalid SSE stream");
                s->failed = 1;
            }
            return -1;
        }
        return 0;
    }
    if (len > 4 * 1024 * 1024 || s->raw.len > 4 * 1024 * 1024 - len) {
        set_err(s->err, sizeof(s->err), "response body is too large");
        s->failed = 1;
        return -1;
    }
    return str_append(&s->raw, data, len);
}

static void api_error_from_body(int status, const char *body, size_t len,
                                char *err, size_t errcap)
{
    char detail[256];
    const char *msg = NULL;
    cJSON *root;

    detail[0] = '\0';
    if (len > 0) {
        root = json_parse(body, len);
        if (root != NULL) {
            cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (cJSON_IsObject(e)) {
                msg = json_string(e, "message");
            } else if (cJSON_IsString(e)) {
                msg = e->valuestring;
            }
            if (msg == NULL) {
                msg = json_string(root, "message");
            }
            if (msg != NULL) {
                strncpy(detail, msg, sizeof(detail) - 1);
                detail[sizeof(detail) - 1] = '\0';
            }
            cJSON_Delete(root);
        }
        if (detail[0] == '\0') {
            size_t n = len;
            size_t i;
            if (n >= sizeof(detail)) {
                n = sizeof(detail) - 1;
            }
            for (i = 0; i < n; i++) {
                char c = body[i];
                if (c == '\r' || c == '\n' || c == '\t') {
                    c = ' ';
                }
                detail[i] = c;
            }
            detail[n] = '\0';
            config_trim(detail);
        }
    }
    if (status == 401) {
        set_err(err, errcap, "HTTP 401 Unauthorized - check the API key%s%s",
                detail[0] ? ": " : "", detail);
    } else if (status == 404) {
        set_err(err, errcap, "HTTP 404 Not Found - check the API base URL and model%s%s",
                detail[0] ? ": " : "", detail);
    } else if (status == 429) {
        set_err(err, errcap, "HTTP 429 rate limited%s%s",
                detail[0] ? ": " : "", detail);
    } else {
        set_err(err, errcap, "HTTP %d%s%s", status,
                detail[0] ? ": " : "", detail);
    }
}

/*
 * Transient failures (a dropped dial-up link, a TLS read error, an overloaded
 * provider) are worth retrying; authentication and request errors are not.
 */
#define API_MAX_RETRIES 2

static int retryable_status(int status)
{
    return status == 429 || status == 502 || status == 503 || status == 504;
}

static int retry_delay_ms(int retry_no)
{
    return retry_no <= 1 ? 1000 : 2000;
}

/* Sleep in small slices so Ctrl+C during the backoff still cancels. */
static int retry_sleep(int ms, volatile int *cancel)
{
    int waited = 0;

    while (waited < ms) {
        if (cancel != NULL && *cancel) {
            return -1;
        }
        Sleep(100);
        waited += 100;
    }
    return 0;
}

typedef struct {
    ApiTextFn fn;
    void *user;
    int emitted;
} TextRelay;

static int text_relay(void *ctx, const char *text, size_t len)
{
    TextRelay *relay = (TextRelay *)ctx;

    relay->emitted = 1;
    if (relay->fn != NULL) {
        return relay->fn(relay->user, text, len);
    }
    return 0;
}

/*
 * One HTTP exchange.  Sets *retryable when the failure looks transient and
 * nothing has been shown to the user yet.
 */
static int api_chat_attempt(const Config *cfg, const Url *url,
                            const char *headers, const char *req,
                            ChatResult *out, ApiTextFn on_text, void *ctx,
                            volatile int *cancel, int *retryable,
                            char *err, size_t errcap)
{
    NetConn *conn = NULL;
    HttpResponse resp;
    SinkCtx sink;
    TextRelay relay;
    int rc = -1;
    int stream = cfg->stream;

    *retryable = 0;
    relay.fn = on_text;
    relay.user = ctx;
    relay.emitted = 0;

    conn = net_open(url, cfg->tls_verify, cfg->connect_timeout_sec,
                    cfg->request_timeout_sec, err, errcap);
    if (conn == NULL) {
        *retryable = 1;
        return -1;
    }
    net_set_cancel(conn, cancel);

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    sink.conn = conn;
    sink.timeout_sec = cfg->request_timeout_sec;
    sink.resp = &resp;
    sink.streaming = stream;
    sink.result = out;
    sink.on_text = text_relay;
    sink.user = &relay;
    sse_init(&sink.sse);
    str_init(&sink.raw);
    str_init(&sink.error_body);

    if (http_request(conn, "POST", url, headers, req, strlen(req), &resp,
                     body_sink, &sink, err, errcap) != 0) {
        if (sink.failed) {
            set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
        } else if (err != NULL && errcap > 0 && err[0] == '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        }
        /* No HTTP status means the connection or TLS layer failed.  A
         * half-received body is retryable only while nothing was shown. */
        if (resp.status == 0 || !relay.emitted) {
            *retryable = 1;
        }
        goto done_sink;
    }
    out->http_status = resp.status;
    if (resp.status != 200) {
        api_error_from_body(resp.status, sink.error_body.data,
                            sink.error_body.len, err, errcap);
        if (retryable_status(resp.status) && !relay.emitted) {
            *retryable = 1;
        }
        goto done_sink;
    }
    if (stream) {
        if (sse_flush(&sink.sse, sse_callback, &sink) != 0) {
            if (sink.failed) {
                set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
            }
            goto done_sink;
        }
        if (sink.failed) {
            set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
            goto done_sink;
        }
        if (out->content.len == 0 && out->call_count == 0) {
            set_err(err, errcap, "stream ended without any content or tool calls");
            goto done_sink;
        }
    } else {
        if (sink.raw.len == 0) {
            set_err(err, errcap, "provider returned an empty response");
            goto done_sink;
        }
        if (openai_apply_full(out, sink.raw.data, sink.raw.len, err, errcap) != 0) {
            goto done_sink;
        }
        /* Non-streaming responses are parsed in one go, so deliver the
         * text here just like the SSE path does. */
        if (on_text != NULL && out->content.len > 0) {
            if (text_relay(&relay, out->content.data, out->content.len) != 0) {
                set_err(err, errcap, "response display aborted");
                goto done_sink;
            }
        }
    }
    rc = 0;

done_sink:
    sse_free(&sink.sse);
    str_free(&sink.raw);
    str_free(&sink.error_body);
    net_close(conn);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Plain-text tool call salvage                                        */
/* ------------------------------------------------------------------ */
/*
 * Some OpenAI-compatible runtimes answer with the tool call written into
 * `content` instead of the native `tool_calls` field, and they disagree on
 * the spelling: TOOLS: [{...}], DSML <invoke> tags, {"args":...}, flattened
 * {"name":"read_file","path":"..."}, read_file({...}), and so on.  When a
 * native call is absent we try these forms before treating the reply as a
 * final answer.  Every candidate must name a real tool, so ordinary prose
 * that happens to contain braces cannot turn into a tool call.
 */

static const char *salvage_find_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    if (n == 0) return hay;
    for (; *hay != '\0'; hay++) {
        if (a98_strncasecmp(hay, needle, n) == 0) return hay;
    }
    return NULL;
}

/* Return a heap copy of the first balanced open..close region, honouring JSON
 * string quoting.  The copy includes both delimiters. */
static char *salvage_balanced(const char *start, char open, char close)
{
    int depth = 0;
    int in_str = 0;
    int escaped = 0;
    const char *p;

    for (p = start; *p != '\0'; p++) {
        if (in_str) {
            if (escaped) escaped = 0;
            else if (*p == '\\') escaped = 1;
            else if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (*p == open) depth++;
        else if (*p == close) {
            depth--;
            if (depth == 0) return a98_strndup(start, (size_t)(p - start) + 1);
        }
    }
    return NULL;
}

static int salvage_add_call(ChatResult *out, const char *name,
                            const char *args_text)
{
    ToolCall *call;
    char *normalized;
    cJSON *parsed;
    int index;

    if (out == NULL || name == NULL || tools_find(name) == NULL ||
        out->call_count >= 32) {
        return 0;
    }
    parsed = cJSON_Parse(args_text != NULL && args_text[0] != '\0'
                         ? args_text : "{}");
    if (parsed == NULL || !cJSON_IsObject(parsed)) {
        if (parsed != NULL) cJSON_Delete(parsed);
        parsed = cJSON_CreateObject();
    }
    if (parsed == NULL) return 0;
    normalized = cJSON_PrintUnformatted(parsed);
    cJSON_Delete(parsed);
    if (normalized == NULL) return 0;

    index = out->call_count;
    call = result_call_for_index(out, index);
    if (call == NULL) {
        cJSON_free(normalized);
        return 0;
    }
    call->id = a98_strfmt("call_%d", index);
    call->name = a98_strdup(name);
    str_append_cstr(&call->arguments, normalized);
    cJSON_free(normalized);
    return (call->id != NULL && call->name != NULL) ? 1 : 0;
}

/* Normalise the arguments of one JSON tool object.  Accepts a JSON string,
 * an object under "arguments" or "args", and flattened top-level keys. */
static char *salvage_item_args(cJSON *item)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "arguments");
    cJSON *parsed;

    if (v == NULL) v = cJSON_GetObjectItemCaseSensitive(item, "args");
    if (v != NULL && cJSON_IsString(v) && v->valuestring != NULL) {
        parsed = cJSON_Parse(v->valuestring);
        if (parsed != NULL && cJSON_IsObject(parsed)) {
            char *s = cJSON_PrintUnformatted(parsed);
            cJSON_Delete(parsed);
            return s;
        }
        if (parsed != NULL) cJSON_Delete(parsed);
        return a98_strdup("{}");
    }
    if (v != NULL && cJSON_IsObject(v)) {
        return cJSON_PrintUnformatted(v);
    }
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON *child;

        if (obj == NULL) return NULL;
        child = item->child;
        while (child != NULL) {
            const char *k = child->string;
            if (k != NULL && strcmp(k, "name") != 0 &&
                strcmp(k, "function") != 0 && strcmp(k, "id") != 0 &&
                strcmp(k, "type") != 0 && strcmp(k, "index") != 0) {
                cJSON_AddItemToObject(obj, k, cJSON_Duplicate(child, 1));
            }
            child = child->next;
        }
        {
            char *s = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            return s;
        }
    }
}

static int salvage_from_json(const char *text, ChatResult *out)
{
    const char *p = text;
    int added = 0;

    while (*p != '\0') {
        char open = *p;
        char close;
        char *blob;
        cJSON *root;

        if (open != '[' && open != '{') { p++; continue; }
        close = open == '[' ? ']' : '}';
        blob = salvage_balanced(p, open, close);
        if (blob == NULL) { p++; continue; }
        root = cJSON_Parse(blob);
        cJSON_free(blob);
        if (root != NULL) {
            if (cJSON_IsArray(root)) {
                cJSON *item;
                cJSON_ArrayForEach(item, root) {
                    cJSON *fn = cJSON_GetObjectItemCaseSensitive(item, "function");
                    cJSON *src = fn != NULL ? fn : item;
                    const char *name = json_string(src, "name");
                    char *args = salvage_item_args(src);

                    added += salvage_add_call(out, name, args);
                    if (args != NULL) cJSON_free(args);
                }
            } else if (cJSON_IsObject(root)) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(root, "function");
                cJSON *src = fn != NULL ? fn : root;
                const char *name = json_string(src, "name");
                if (name != NULL) {
                    char *args = salvage_item_args(src);
                    added += salvage_add_call(out, name, args);
                    if (args != NULL) cJSON_free(args);
                }
            }
            cJSON_Delete(root);
        }
        if (added > 0) return added;
        p++;
    }
    return 0;
}

/* DSML: <|DSML|invoke name="read_file"><|DSML|parameter name="path"
 * string="true">file.c</|DSML|parameter></|DSML|invoke> */
static int salvage_from_dsml(const char *text, ChatResult *out)
{
    const char *p = text;
    int added = 0;

    while ((p = salvage_find_ci(p, "invoke")) != NULL) {
        const char *gt = strchr(p, '>');
        char name[128];
        cJSON *args;
        const char *q;

        if (gt == NULL) break;
        name[0] = '\0';
        for (q = p; q < gt; q++) {
            if (a98_strncasecmp(q, "name=", 5) == 0) {
                q += 5;
                if (*q == '"' || *q == '\'') {
                    char quote = *q++;
                    const char *end = strchr(q, quote);
                    if (end != NULL && (size_t)(end - q) < sizeof(name)) {
                        memcpy(name, q, (size_t)(end - q));
                        name[end - q] = '\0';
                    }
                }
                break;
            }
        }
        args = cJSON_CreateObject();
        q = gt + 1;
        if (args != NULL) {
            while (*q != '\0') {
                const char *pa = salvage_find_ci(q, "parameter");
                const char *pgt;
                const char *pend;
                const char *na;

                if (pa == NULL) break;
                pgt = strchr(pa, '>');
                if (pgt == NULL) break;
                pend = salvage_find_ci(pgt + 1, "</");
                if (pend == NULL) break;
                na = NULL;
                for (na = pa; na < pgt; na++) {
                    if (a98_strncasecmp(na, "name=", 5) == 0) {
                        na += 5;
                        if (*na == '"' || *na == '\'') {
                            char quote = *na++;
                            const char *ne = strchr(na, quote);
                            if (ne != NULL && ne < pgt) {
                                char key[128];
                                size_t klen = (size_t)(ne - na);
                                char *value;
                                size_t vlen;

                                if (klen >= sizeof(key)) { na = pgt; break; }
                                memcpy(key, na, klen);
                                key[klen] = '\0';
                                while (pgt + 1 < pend &&
                                       (*(pgt + 1) == ' ' || *(pgt + 1) == '\r' ||
                                        *(pgt + 1) == '\n')) pgt++;
                                vlen = (size_t)(pend - (pgt + 1));
                                value = a98_strndup(pgt + 1, vlen);
                                if (value != NULL) {
                                    config_trim(value);
                                    cJSON_AddStringToObject(args, key, value);
                                    a98_free(value);
                                }
                            }
                        }
                        break;
                    }
                }
                q = pend + 2;
            }
            if (name[0] != '\0') {
                char *argtext = cJSON_PrintUnformatted(args);
                added += salvage_add_call(out, name, argtext);
                if (argtext != NULL) cJSON_free(argtext);
            }
            cJSON_Delete(args);
        }
        p = gt + 1;
    }
    return added;
}

/* read_file({"path":"x"}) and read_file: {"path":"x"} */
static int salvage_from_named(const char *text, ChatResult *out)
{
    const ToolDef *defs;
    int count = 0;
    int i;
    int added = 0;

    defs = tools_all(&count);
    for (i = 0; i < count; i++) {
        const char *p = text;

        while ((p = salvage_find_ci(p, defs[i].name)) != NULL) {
            const char *q = p + strlen(defs[i].name);

            while (*q == ' ' || *q == '\t') q++;
            if (*q == ':' || *q == '(') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                if (*q == '{') {
                    char *blob = salvage_balanced(q, '{', '}');
                    if (blob != NULL) {
                        added += salvage_add_call(out, defs[i].name, blob);
                        cJSON_free(blob);
                    }
                }
            }
            p++;
        }
    }
    return added;
}

int api_salvage_tool_calls(ChatResult *out)
{
    const char *text;
    int added = 0;

    if (out == NULL || out->call_count != 0 || out->content.len == 0) {
        return 0;
    }
    text = str_cstr(&out->content);
    added = salvage_from_dsml(text, out);
    if (added == 0) added = salvage_from_json(text, out);
    if (added == 0) added = salvage_from_named(text, out);
    return added;
}

int api_chat(const Config *cfg, const char *system_prompt,
             const MessageList *messages, ChatResult *out,
             ApiTextFn on_text, void *ctx, volatile int *cancel,
             char *err, size_t errcap)
{
    Url url;
    char *req = NULL;
    char *extra = NULL;
    int rc = -1;
    int attempt;

    if (cfg == NULL || messages == NULL || out == NULL) {
        return -1;
    }
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (make_endpoint(cfg->api_base, &url, err, errcap) != 0) {
        return -1;
    }
    if (cfg->api_key[0] == '\0') {
        log_warn("API key is empty; only local/unauthorised endpoints will work");
    }
    req = build_request_json(cfg, system_prompt, messages, err, errcap);
    if (req == NULL) {
        return -1;
    }
    extra = api_build_headers(cfg, cfg->stream);
    if (extra == NULL) {
        set_err(err, errcap, "out of memory");
        goto done;
    }

    for (attempt = 0; attempt <= API_MAX_RETRIES; attempt++) {
        int retryable = 0;

        if (cancel != NULL && *cancel) {
            set_err(err, errcap, "cancelled by user");
            goto done;
        }
        if (attempt > 0) {
            /* Discard anything from the failed attempt before retrying. */
            chat_result_free(out);
            chat_result_init(out);
        }
        if (err != NULL && errcap > 0) {
            err[0] = '\0';
        }
        if (api_chat_attempt(cfg, &url, extra, req, out, on_text, ctx,
                             cancel, &retryable, err, errcap) == 0) {
            rc = 0;
            goto done;
        }
        if (!retryable || attempt == API_MAX_RETRIES) {
            if (retryable) {
                log_warn("giving up after %d attempts: %s", attempt + 1,
                         (err != NULL && err[0] != '\0') ? err : "network error");
            }
            goto done;
        }
        log_warn("request failed (%s); retrying in %d s (attempt %d of %d)",
                 (err != NULL && err[0] != '\0') ? err : "network error",
                 retry_delay_ms(attempt + 1) / 1000,
                 attempt + 2, API_MAX_RETRIES + 1);
        if (retry_sleep(retry_delay_ms(attempt + 1), cancel) != 0) {
            set_err(err, errcap, "cancelled by user");
            goto done;
        }
    }

done:
    if (rc == 0 && cfg != NULL && cfg->tool_call_fallback &&
        api_salvage_tool_calls(out) > 0) {
        log_warn("salvaged tool call(s) from a plain-text model reply; "
                 "set tool_call_fallback=0 to enforce native tool_calls");
        /* The raw text was just a differently encoded tool call.  Do not let
         * the agent treat it as a final answer as well. */
        str_clear(&out->content);
    }
    a98_free(extra);
    if (req != NULL) {
        cJSON_free(req);
    }
    return rc;
}
