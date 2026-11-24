#include "agent.h"
#include "api_openai.h"
#include "tools.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../core/utf8.h"
#include "../../third_party/cjson/cJSON.h"

#define MAX_TOOL_OUTPUT 16384

/* Error sink shared by every entry point in this file.  Callers are allowed
 * to pass err == NULL when they do not want the message; never touch it in
 * that case (same convention as the tools and api_openai.c). */
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

static void emit(Agent *a, AgentEventType type, const char *text)
{
    if (a->on_event != NULL) {
        a->on_event(a->event_ctx, type, text ? text : "");
    }
}

static void emit_delta(Agent *a, const char *text, size_t len)
{
    char *copy;

    if (a->on_event == NULL || len == 0) {
        return;
    }
    copy = a98_strndup(text, len);
    if (copy != NULL) {
        emit(a, AGENT_EV_TEXT, copy);
        a98_free(copy);
    }
}

void agent_stats(const Agent *a, AgentStats *out)
{
    if (a == NULL || out == NULL) {
        return;
    }
    *out = a->stats;
}

static void emit_run_stats(Agent *a, const char *reason)
{
    char buf[256];

    snprintf(buf, sizeof(buf),
             "Run %s: %d round(s), %d tool call(s), %d failed, "
             "%d blocked, %d hint(s)",
             reason ? reason : "finished", a->stats.rounds,
             a->stats.tool_calls, a->stats.failed_calls,
             a->stats.blocked_calls, a->stats.nudge_count);
    emit(a, AGENT_EV_INFO, buf);
}

/* Only command/build results can prove that an acceptance check passed; a
 * read_file result may merely contain the words inside a test source file. */
static int tool_reports_success(const char *name, const char *text)
{
    if (text == NULL) return 0;
    if (strcmp(name, "build") == 0) {
        return strstr(text, "BUILD OK") != NULL;
    }
    if (strcmp(name, "run_command") == 0) {
        if (strstr(text, "exit code: 0") == NULL) return 0;
        return strstr(text, "passed") != NULL ||
               strstr(text, "BUILD OK") != NULL;
    }
    return 0;
}

static int text_cb(void *ctx, const char *text, size_t len)
{
    Agent *a = (Agent *)ctx;
    if (a->cancel != NULL && *a->cancel) {
        return -1;
    }
    emit_delta(a, text, len);
    return 0;
}

void agent_init(Agent *a, Config *cfg)
{
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->cfg = cfg;
    msglist_init(&a->messages);
    if (cfg != NULL) {
        strncpy(a->workspace, cfg->workspace, sizeof(a->workspace) - 1);
        a->workspace[sizeof(a->workspace) - 1] = '\0';
    }
    agent_ensure_dirs(a);
}

void agent_free(Agent *a)
{
    if (a == NULL) {
        return;
    }
    msglist_free(&a->messages);
}

void agent_new_conversation(Agent *a)
{
    if (a != NULL) {
        msglist_clear(&a->messages);
        tools_read_cache_clear();
    }
}

void agent_ensure_dirs(Agent *a)
{
    char sessions[A98_MAX_PATH];

    if (a == NULL) {
        return;
    }
    if (a->workspace[0] != '\0') {
        a98_path_mkdirs(a->workspace);
    }
    if (a98_path_exe_dir(sessions, sizeof(sessions)) == 0) {
        char dir[A98_MAX_PATH];
        a98_path_join(dir, sizeof(dir), sessions, "sessions");
        a98_path_mkdirs(dir);
    }
}

void agent_last_session_path(char *out, size_t cap)
{
    char dir[A98_MAX_PATH];
    char sessions[A98_MAX_PATH];

    if (out == NULL || cap == 0) {
        return;
    }
    if (a98_path_exe_dir(dir, sizeof(dir)) != 0) {
        strcpy(dir, ".");
    }
    a98_path_join(sessions, sizeof(sessions), dir, "sessions");
    a98_path_join(out, cap, sessions, "last_session.json");
}

static char *calls_to_json(const ChatResult *res)
{
    cJSON *arr;
    int i;
    char *text;

    if (res->call_count == 0) {
        return NULL;
    }
    arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < res->call_count; i++) {
        const ToolCall *c = &res->calls[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        char idbuf[64];

        if (obj == NULL || fn == NULL) {
            cJSON_Delete(obj);
            cJSON_Delete(fn);
            continue;
        }
        if (c->id != NULL && c->id[0] != '\0') {
            cJSON_AddStringToObject(obj, "id", c->id);
        } else {
            sprintf(idbuf, "call_%d", i);
            cJSON_AddStringToObject(obj, "id", idbuf);
        }
        cJSON_AddStringToObject(obj, "type", "function");
        cJSON_AddStringToObject(fn, "name", c->name ? c->name : "");
        cJSON_AddStringToObject(fn, "arguments",
                                c->arguments.data ? c->arguments.data : "{}");
        cJSON_AddItemToObject(obj, "function", fn);
        cJSON_AddItemToArray(arr, obj);
    }
    text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return text;
}

static void truncate_output(Str *out)
{
    if (out->len > MAX_TOOL_OUTPUT) {
        out->len = MAX_TOOL_OUTPUT;
        if (out->data != NULL) {
            /* Back up while the first *excluded* byte is a UTF-8 continuation
             * byte.  That leaves out->data[0..out->len) on a character
             * boundary: either the cut fell on ASCII, or the whole trailing
             * multi-byte character is dropped.  Do not look at
             * data[out->len - 1]: a complete multi-byte character also ends
             * with a non-ASCII byte, and trimming it would emit invalid UTF-8.
             */
            while (out->len > 0 &&
                   ((unsigned char)out->data[out->len] & 0xC0) == 0x80) {
                out->len--;
            }
            out->data[out->len] = '\0';
        }
        str_append_cstr(out, "\n[tool output truncated]");
    }
}

static int tool_needs_confirm(const char *name, const char *args_json)
{
    const ToolDef *def = tools_find(name);
    if (def == NULL) return 0;
    if (strcmp(name, "http_request") == 0) {
        /* GET and HEAD are read-only; anything that can send data asks. */
        cJSON *args = args_json != NULL && args_json[0] != '\0'
                      ? cJSON_Parse(args_json) : NULL;
        const char *m = args != NULL ? json_string(args, "method") : NULL;
        int needs = (m != NULL && a98_strcasecmp(m, "GET") != 0 &&
                     a98_strcasecmp(m, "HEAD") != 0);
        if (args != NULL) cJSON_Delete(args);
        return needs;
    }
    if (strcmp(name, "replace_across") == 0) {
        /* Only applying changes asks; a dry run is harmless. */
        cJSON *args = args_json != NULL && args_json[0] != '\0'
                      ? cJSON_Parse(args_json) : NULL;
        int dry = args != NULL ? json_bool(args, "dry_run", 1) : 1;
        if (args != NULL) cJSON_Delete(args);
        return !dry;
    }
    return def->needs_confirm;
}

/*
 * Loop guard: a model that gets stuck tends to repeat the same tool call with
 * the same arguments.  The third consecutive identical call is answered with
 * an error instead of being executed, and a fourth one stops the run.  The
 * signature is "name\x1farguments" so names and arguments cannot collide.
 */
static int guard_matches(const Str *sig, const char *name, const char *args)
{
    size_t nlen = strlen(name);
    size_t alen = strlen(args);

    return sig->len == nlen + 1 + alen &&
           memcmp(sig->data, name, nlen) == 0 &&
           sig->data[nlen] == '\x1f' &&
           memcmp(sig->data + nlen + 1, args, alen) == 0;
}

static void guard_record(Str *sig, const char *name, const char *args)
{
    str_clear(sig);
    str_append_cstr(sig, name);
    str_append_char(sig, '\x1f');
    str_append_cstr(sig, args);
}

/* Rough estimate of the conversation size sent to the API. Not exact token
 * counting, but good enough to decide when to compact on a 9x machine. */
static size_t context_bytes(const MessageList *list)
{
    size_t total = 0;
    int i;
    for (i = 0; i < list->count; i++) {
        const Message *m = &list->items[i];
        if (m->content != NULL) total += strlen(m->content);
        if (m->tool_calls_json != NULL) total += strlen(m->tool_calls_json);
        if (m->name != NULL) total += strlen(m->name);
    }
    return total;
}

/* Deterministic fallback when the summary API call is unavailable.  Keeping
 * the same field layout means the model sees the same shape either way. */
static char *local_structured_summary(const MessageList *list, int to_remove)
{
    Str s;
    int i;
    int have_task = 0;
    int have_tool = 0;
    const char *last_command = "none";
    const char *last_result = "none";

    str_init(&s);
    str_append_cstr(&s, "Task: ");
    for (i = 0; i < to_remove; i++) {
        const Message *m = &list->items[i];
        if (m->role == MSG_USER && m->content != NULL && m->content[0] != '\0') {
            str_append_cstr(&s, m->content);
            have_task = 1;
            break;
        }
    }
    if (!have_task) str_append_cstr(&s, "(see earlier messages)");
    str_append_cstr(&s, "\nFiles read: (paths are in the earlier tool calls)\n");
    str_append_cstr(&s, "Files changed: ");
    for (i = 0; i < to_remove; i++) {
        const Message *m = &list->items[i];
        if (m->role == MSG_ASSISTANT && m->tool_calls_json != NULL) {
            if (strstr(m->tool_calls_json, "write_file") != NULL ||
                strstr(m->tool_calls_json, "edit") != NULL ||
                strstr(m->tool_calls_json, "apply_patch") != NULL) {
                str_append_cstr(&s, "yes (see earlier tool calls)\n");
                have_tool = 1;
                break;
            }
        }
    }
    if (!have_tool) str_append_cstr(&s, "none\n");
    for (i = 0; i < to_remove; i++) {
        const Message *m = &list->items[i];
        if (m->role != MSG_TOOL || m->name == NULL) continue;
        if (strcmp(m->name, "run_command") == 0 ||
            strcmp(m->name, "build") == 0) {
            last_command = m->name;
            last_result = m->content != NULL ? m->content : "";
        }
    }
    str_appendf(&s, "Last test command: %s\nLast test result: %.300s\n",
                last_command, last_result);
    str_append_cstr(&s,
        "Outstanding work: continue from the kept messages; do not assume "
        "tests passed\nDo not re-read: files already read above unless changed\n");
    return str_detach(&s);
}

/* Compact old messages into one summary via an extra API call. Returns 0 if
 * nothing happened or compaction succeeded, -1 only on a hard error that
 * should stop the run. Never aborts the run on a failed summary. */
static int maybe_compact(Agent *a, char *err, size_t errcap)
{
    Config *cfg = a->cfg;
    size_t bytes;
    int keep;
    MessageList prompt;
    ChatResult res;
    Str summary;
    char sum_err[512];
    int rc = 0;

    if (cfg->compact_threshold <= 0) return 0;
    bytes = context_bytes(&a->messages);
    if (bytes < (size_t)cfg->compact_threshold) return 0;

    keep = cfg->compact_keep;
    if (keep < 2) keep = 2;
    if (keep > 40) keep = 40;
    if (a->messages.count <= keep + 2) return 0;   /* nothing worth compacting */

    emit(a, AGENT_EV_INFO, "Compacting conversation (context limit)...");
    log_info("compacting conversation: %lu bytes, %d messages, keep %d",
             (unsigned long)bytes, a->messages.count, keep);

    /* Build a small prompt from the messages being removed. */
    msglist_init(&prompt);
    {
        int to_remove = a->messages.count - keep;
        int i;
        static const char *sum_sys =
            "You summarize agent conversations. Output ONLY these fields, "
            "one per line, in this exact order:\n"
            "Task: <what the user asked for>\n"
            "Files read: <paths, comma separated>\n"
            "Files changed: <paths and what was done>\n"
            "Last test command: <command or none>\n"
            "Last test result: <pass/fail with key output>\n"
            "Outstanding work: <what remains, or none>\n"
            "Do not re-read: <paths already read and unchanged>\n"
            "Be faithful and brief; never invent a result.";
        msglist_add(&prompt, MSG_USER, "Summarize the following conversation:");
        for (i = 0; i < to_remove; i++) {
            const Message *m = &a->messages.items[i];
            if (m->content != NULL && m->content[0] != '\0') {
                switch (m->role) {
                case MSG_USER:      msglist_add(&prompt, MSG_USER, m->content); break;
                case MSG_ASSISTANT: msglist_add(&prompt, MSG_ASSISTANT, m->content); break;
                case MSG_TOOL: {
                    char buf[2048];
                    snprintf(buf, sizeof(buf), "[tool %s result] %.1500s",
                             m->name ? m->name : "", m->content);
                    msglist_add(&prompt, MSG_USER, buf);
                    break;
                }
                default: break;
                }
            }
        }
        chat_result_init(&res);
        sum_err[0] = '\0';
        str_init(&summary);
        if (api_chat(cfg, sum_sys, &prompt, &res, NULL, NULL, a->cancel,
                     sum_err, sizeof(sum_err)) == 0 &&
            res.content.len > 0) {
            if (str_append(&summary, str_cstr(&res.content),
                           res.content.len) != 0) {
                log_warn("compaction failed: out of memory building summary");
            } else if (msglist_compact(&a->messages, str_cstr(&summary),
                                       keep) == 0) {
                char info[128];
                snprintf(info, sizeof(info),
                         "Compacted conversation: %d messages kept + 1 summary",
                         keep);
                emit(a, AGENT_EV_INFO, info);
                log_info("compaction done: %s", info);
            } else {
                log_warn("compaction failed to rebuild message list");
            }
        } else {
            char *fallback = local_structured_summary(&a->messages, to_remove);

            log_warn("compaction summary request failed (%s); using a local "
                     "structured summary", sum_err);
            if (fallback != NULL &&
                msglist_compact(&a->messages, fallback, keep) == 0) {
                char info[128];
                snprintf(info, sizeof(info),
                         "Compacted conversation locally: %d messages kept "
                         "+ 1 summary", keep);
                emit(a, AGENT_EV_INFO, info);
            } else {
                log_warn("local compaction could not rebuild the message list");
            }
            a98_free(fallback);
        }
        str_free(&summary);
        chat_result_free(&res);
    }
    msglist_free(&prompt);
    (void)err; (void)errcap;
    return rc;
}

int agent_run(Agent *a, const char *user_text, char *err, size_t errcap)
{
    int step;
    int unlimited;
    Str guard_sig;
    Str round_guard_sig;
    int guard_count = 0;
    int round_guard_count = 0;
    int saw_write = 0;

    if (a == NULL || a->cfg == NULL || user_text == NULL) {
        return -1;
    }
    /* All error reporting goes through set_err(), which handles a NULL err
     * itself, so no local scratch buffer is needed here. */
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (a->workspace[0] == '\0') {
        strncpy(a->workspace, a->cfg->workspace, sizeof(a->workspace) - 1);
        a->workspace[sizeof(a->workspace) - 1] = '\0';
    }
    agent_ensure_dirs(a);
    memset(&a->stats, 0, sizeof(a->stats));
    str_init(&guard_sig);
    str_init(&round_guard_sig);
    if (msglist_add(&a->messages, MSG_USER, user_text) == NULL) {
        str_free(&guard_sig);
        str_free(&round_guard_sig);
        set_err(err, errcap, "out of memory");
        return -1;
    }
    /* max_steps == 0 means "no limit": stop only when the model finishes or
     * the user cancels. */
    unlimited = a->cfg->max_steps <= 0;

    for (step = 0; ; step++) {
        ChatResult res;
        char *calls_json = NULL;
        char roundbuf[64];
        int rc;
        int guard_tripped = 0;
        int round_block = 0;

        if (!unlimited && step >= a->cfg->max_steps) {
            emit_run_stats(a, "stopped at max_steps");
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            set_err(err, errcap,
                    "stopped after %d tool rounds (max_steps); send another "
                    "message to continue, or set max_steps=0 for unlimited",
                    a->cfg->max_steps);
            return -1;
        }
        if (a->cancel != NULL && *a->cancel) {
            emit_run_stats(a, "cancelled");
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            set_err(err, errcap, "cancelled by user");
            return -1;
        }
        a->stats.rounds++;
        /* Compact before sending if the conversation has grown too large. */
        maybe_compact(a, err, errcap);
        if (unlimited) {
            sprintf(roundbuf, "Thinking (round %d)...", step + 1);
        } else {
            sprintf(roundbuf, "Thinking (round %d/%d)...", step + 1,
                    a->cfg->max_steps);
        }
        emit(a, AGENT_EV_ROUND, roundbuf);
        chat_result_init(&res);

        rc = api_chat(a->cfg, a->cfg->system_prompt, &a->messages, &res,
                      text_cb, a, a->cancel, err, errcap);
        if (rc != 0) {
            chat_result_free(&res);
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            if (err != NULL && errcap > 0 && err[0] == '\0') {
                set_err(err, errcap, "API request failed");
            }
            return -1;
        }

        calls_json = calls_to_json(&res);
        if (calls_json != NULL) {
            {
                char _rb[2048];
                size_t _n = calls_json ? strlen(calls_json) : 0;
                if (_n >= sizeof(_rb)) _n = sizeof(_rb) - 1;
                if (calls_json) memcpy(_rb, calls_json, _n);
                _rb[_n] = 0;
                /* crude redact */
                {
                    char *q = _rb;
                    while ((q = strstr(q, "api_key")) != NULL) {
                        q += 7;
                        while (*q && *q != ',' && *q != '}') { if (*q!='"'&&*q!='\\'&&*q!=':'&&*q!=' ') *q='*'; q++; }
                    }
                }
                log_debug("tool calls: %s", _rb);
            }
        }
        if (msglist_add_assistant(&a->messages,
                                  res.content.data ? res.content.data : "",
                                  calls_json) == NULL) {
            cJSON_free(calls_json);
            chat_result_free(&res);
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            set_err(err, errcap, "out of memory");
            return -1;
        }
        cJSON_free(calls_json);

        if (res.call_count == 0) {
            if (res.content.len == 0) {
                chat_result_free(&res);
                str_free(&guard_sig);
                emit_run_stats(a, "failed (empty response)");
                str_free(&round_guard_sig);
                set_err(err, errcap, "model returned an empty response");
                return -1;
            }
            emit_run_stats(a, "completed");
            chat_result_free(&res);
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            return 0;
        }

        /* Round-level guard: a model that alternates between two tool calls
         * (A, B, A, B, ...) never repeats the same call twice in a row, so the
         * per-call guard below cannot see it.  Compare the whole set of calls
         * issued in this round with the previous round's set instead.
         * round_block stays 0 unless the round repeats; it never touches the
         * per-call counter, so the documented 3-call / 4-call thresholds keep
         * working independently. */
        {
            Str this_round;
            int r_idx;

            str_init(&this_round);
            for (r_idx = 0; r_idx < res.call_count; r_idx++) {
                const char *cn = res.calls[r_idx].name ? res.calls[r_idx].name : "";
                const char *ca = res.calls[r_idx].arguments.data
                                     ? res.calls[r_idx].arguments.data : "";
                str_append_cstr(&this_round, cn);
                str_append_char(&this_round, '\x1f');
                str_append_cstr(&this_round, ca);
                str_append_char(&this_round, '\x1e');
            }
            if (round_guard_sig.len > 0 &&
                this_round.len == round_guard_sig.len &&
                memcmp(this_round.data, round_guard_sig.data,
                       this_round.len) == 0) {
                round_guard_count++;
            } else {
                str_clear(&round_guard_sig);
                str_append(&round_guard_sig, this_round.data, this_round.len);
                round_guard_count = 1;
            }
            if (round_guard_count >= 3) {
                round_block = round_guard_count;
                if (round_guard_count >= 4) {
                    guard_tripped = 1;
                }
            }
            str_free(&this_round);
        }

        {
            int i;
            for (i = 0; i < res.call_count; i++) {
                ToolCall *c = &res.calls[i];
                const char *name = c->name ? c->name : "";
                const char *args = c->arguments.data ? c->arguments.data : "{}";
                Str out;
                char tool_err[512];
                char *summary;
                int allowed = 1;
                int blocked = 0;
                int block_repeat = 0;
                int executed = 0;

                a->stats.tool_calls++;
                if (a->cancel != NULL && *a->cancel) {
                    chat_result_free(&res);
                    str_free(&guard_sig);
                    str_free(&round_guard_sig);
                    set_err(err, errcap, "cancelled by user");
                    return -1;
                }
                summary = a98_strfmt("%s(%s)", name, args);
                emit(a, AGENT_EV_TOOL_START, summary ? summary : name);
                a98_free(summary);

                if (guard_tripped) {
                    /* The run is already stopping; still answer this call so
                     * the history stays valid for the next user turn. */
                    blocked = 1;
                    block_repeat = round_guard_count > guard_count
                                       ? round_guard_count : guard_count;
                } else if (round_block) {
                    blocked = 1;
                    block_repeat = round_block;
                } else if (guard_matches(&guard_sig, name, args)) {
                    guard_count++;
                    block_repeat = guard_count;
                    if (guard_count >= 3) {
                        blocked = 1;
                        if (guard_count >= 4) {
                            guard_tripped = 1;
                        }
                    }
                } else {
                    guard_record(&guard_sig, name, args);
                    guard_count = 1;
                }

                str_init(&out);
                tool_err[0] = '\0';
                if (blocked) {
                    if (guard_tripped) {
                        str_appendf(&out,
                                    "Tool call blocked by loop guard: %s was "
                                    "called with identical arguments %d times "
                                    "in a row. The run is being stopped; do not "
                                    "repeat this call.", name, block_repeat);
                    } else {
                        str_appendf(&out,
                                    "Tool call blocked by loop guard: %s was "
                                    "called with identical arguments %d times "
                                    "in a row. Do not repeat it; use different "
                                    "arguments or a different tool, or finish "
                                    "and answer the user.", name, block_repeat);
                    }
                    a->stats.blocked_calls++;
                    a->stats.failed_calls++;
                    log_warn("loop guard: blocked repeated tool call %s (x%d)",
                             name, block_repeat);
                } else {
                    /* auto_confirm (implied by full_access) runs every tool
                     * without showing the dialog. Otherwise ask, and fail
                     * closed when no UI callback is wired up. */
                    if (a->cfg != NULL && a->cfg->auto_confirm) {
                        allowed = 1;
                        log_info("auto-confirm: allowing %s", name);
                    } else if (tool_needs_confirm(name, args)) {
                        if (a->on_confirm != NULL) {
                            allowed = a->on_confirm(a->confirm_ctx, name,
                                                    args);
                        } else {
                            allowed = 0;
                            log_warn("confirm callback missing; denied %s",
                                     name);
                        }
                    }
                    if (!allowed) {
                        a->stats.failed_calls++;
                        str_append_cstr(&out, "Tool execution denied by the user.");
                    } else if (tools_execute(a, name, args, &out, tool_err,
                                             sizeof(tool_err)) != 0) {
                        a->stats.failed_calls++;
                        str_appendf(&out, "Tool error: %s", tool_err);
                    } else {
                        executed = 1;
                        if (tools_tool_is_writer(name)) saw_write = 1;
                    }
                }
                if (executed && saw_write && a->cfg != NULL &&
                    a->cfg->nudge_on_success &&
                    tool_reports_success(name, str_cstr(&out))) {
                    str_append_cstr(&out,
                        "\n[system hint] Acceptance checks passed after the "
                        "last file change. Stop calling tools and give the "
                        "final answer now.");
                    a->stats.nudge_count++;
                    emit(a, AGENT_EV_INFO,
                         "Acceptance checks passed; hint sent to the model.");
                }
                truncate_output(&out);
                /* Tool text can carry local-code-page paths (list_dir echoes
                 * the resolved ACP path and the FindFirstFileA names), and the
                 * API rejects the whole request body when it is not valid
                 * UTF-8: "invalid unicode code point".  Repair it once, before
                 * it reaches the transcript or the message list. */
                {
                    char *fixed = a98_utf8_sanitize(str_cstr(&out));

                    if (fixed != NULL) {
                        str_clear(&out);
                        str_append_cstr(&out, fixed);
                        a98_free(fixed);
                    }
                }
                emit(a, AGENT_EV_TOOL_RESULT, str_cstr(&out));
                if (msglist_add_tool(&a->messages,
                                     c->id ? c->id : "",
                                     name, str_cstr(&out)) == NULL) {
                    str_free(&out);
                    chat_result_free(&res);
                    str_free(&guard_sig);
                    str_free(&round_guard_sig);
                    set_err(err, errcap, "out of memory");
                    return -1;
                }
                str_free(&out);
            }
        }
        chat_result_free(&res);

        if (guard_tripped) {
            emit_run_stats(a, "stopped by loop guard");
            str_free(&guard_sig);
            str_free(&round_guard_sig);
            set_err(err, errcap,
                    "loop guard stopped the run: the model kept repeating "
                     "the same tool call");
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

static char *read_file_all(const char *path, size_t max_len, size_t *out_len)
{
    FILE *f;
    long size;
    char *buf;
    size_t n;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if ((size_t)size > max_len) {
        size = (long)max_len;
    }
    rewind(f);
    buf = (char *)a98_alloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

int agent_save_session(Agent *a, const char *path, char *err, size_t errcap)
{
    cJSON *root;
    cJSON *arr;
    char *text;
    char tmp[A98_MAX_PATH + 8];
    FILE *f;
    int i;
    int ok = 1;

    if (a == NULL || path == NULL) {
        return -1;
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        set_err(err, errcap, "out of memory");
        return -1;
    }
    cJSON_AddNumberToObject(root, "version", 1);
    {
        char timebuf[64];
        sprintf(timebuf, "%ld", (long)time(NULL));
        cJSON_AddStringToObject(root, "saved_at", timebuf);
    }
    arr = cJSON_AddArrayToObject(root, "messages");
    for (i = 0; i < a->messages.count; i++) {
        Message *m = &a->messages.items[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON *tc;
        if (obj == NULL) {
            continue;
        }
        cJSON_AddStringToObject(obj, "role", msg_role_name(m->role));
        if (m->content != NULL) {
            cJSON_AddStringToObject(obj, "content", m->content);
        }
        if (m->tool_calls_json != NULL) {
            tc = cJSON_Parse(m->tool_calls_json);
            if (tc != NULL) {
                cJSON_AddItemToObject(obj, "tool_calls", tc);
            }
        }
        if (m->tool_call_id != NULL) {
            cJSON_AddStringToObject(obj, "tool_call_id", m->tool_call_id);
        }
        if (m->name != NULL) {
            cJSON_AddStringToObject(obj, "name", m->name);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        set_err(err, errcap, "could not encode the session");
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        cJSON_free(text);
        set_err(err, errcap, "cannot write %s", tmp);
        return -1;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        ok = 0;
    }
    fclose(f);
    cJSON_free(text);
    if (!ok) {
        remove(tmp);
        set_err(err, errcap, "failed to write the session file");
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        set_err(err, errcap, "cannot replace %s", path);
        return -1;
    }
    log_info("session saved: %s (%d messages)", path, a->messages.count);
    return 0;
}

int agent_load_session(Agent *a, const char *path, char *err, size_t errcap)
{
    size_t len = 0;
    char *text;
    cJSON *root;
    cJSON *arr;
    cJSON *item;

    if (a == NULL || path == NULL) {
        return -1;
    }
    text = read_file_all(path, 8 * 1024 * 1024, &len);
    if (text == NULL) {
        set_err(err, errcap, "cannot read %s", path);
        return -1;
    }
    root = json_parse(text, len);
    a98_free(text);
    if (root == NULL) {
        set_err(err, errcap, "session file is not valid JSON");
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        set_err(err, errcap, "session file has no messages array");
        return -1;
    }
    msglist_clear(&a->messages);
    cJSON_ArrayForEach(item, arr) {
        const char *role = json_string(item, "role");
        const char *content = json_string(item, "content");
        /* A session saved before the tool output was repaired can still hold
         * raw CP_ACP bytes; replaying it would fail the same way. */
        char *content_fixed = a98_utf8_sanitize(content);

        if (content_fixed != NULL) {
            content = content_fixed;
        }
        const char *tc_id = json_string(item, "tool_call_id");
        const char *name = json_string(item, "name");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(item, "tool_calls");
        char *tc_text = NULL;

        if (tc != NULL) {
            tc_text = cJSON_PrintUnformatted(tc);
        }
        if (role == NULL) {
            a98_free(content_fixed);
            a98_free(tc_text);
            continue;
        }
        if (strcmp(role, "user") == 0) {
            msglist_add(&a->messages, MSG_USER, content ? content : "");
        } else if (strcmp(role, "assistant") == 0) {
            msglist_add_assistant(&a->messages, content ? content : "", tc_text);
        } else if (strcmp(role, "tool") == 0) {
            msglist_add_tool(&a->messages, tc_id ? tc_id : "",
                             name ? name : "", content ? content : "");
        } else if (strcmp(role, "system") == 0) {
            /* The system prompt lives in the config, not in history. */
        }
        a98_free(content_fixed);
        a98_free(tc_text);
    }
    cJSON_Delete(root);
    tools_read_cache_clear();
    log_info("session loaded: %s (%d messages)", path, a->messages.count);
    return 0;
}
