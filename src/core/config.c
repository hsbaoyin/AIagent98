#include "config.h"
#include "log.h"

#include <windows.h>
#include <errno.h>
#include <limits.h>

#define DEFAULT_SYSTEM_PROMPT \
    "You are AIagent98, a helpful AI assistant running on Windows 98. " \
    "You can call tools to inspect files, edit files, search text and run " \
    "commands in the user's workspace. Paths are relative to the workspace. " \
    "Use tools when they are needed, keep answers concise, and never claim a " \
    "tool result you did not receive. Destructive tools require user approval."

void config_trim(char *s)
{
    char *start;
    size_t len;

    if (s == NULL) {
        return;
    }
    start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
}

void config_defaults(Config *cfg, const char *exe_dir)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->api_base, "https://api.openai.com/v1", sizeof(cfg->api_base) - 1);
    strncpy(cfg->model, "gpt-4o-mini", sizeof(cfg->model) - 1);
    strncpy(cfg->system_prompt, DEFAULT_SYSTEM_PROMPT, sizeof(cfg->system_prompt) - 1);
    if (exe_dir != NULL && exe_dir[0] != '\0') {
        a98_path_join(cfg->workspace, sizeof(cfg->workspace), exe_dir, "workspace");
    } else {
        strncpy(cfg->workspace, "workspace", sizeof(cfg->workspace) - 1);
    }
    cfg->max_tokens = 2048;
    cfg->temperature = 0.7;
    cfg->stream = 1;
    cfg->tls_verify = 1;
    cfg->allow_outside = 0;
    /* Shell execution stays off until the user turns it on in Settings or the
     * INI.  dist/SAMPLE.INI and the README have always documented 0. */
    cfg->allow_shell = 0;
    cfg->full_access = 0;
    cfg->auto_confirm = 0;
    cfg->emoji_inline = 0;
    cfg->net_allow = 0;
    cfg->net_allow_hosts[0] = '\0';
    cfg->net_max_bytes = 262144;
    strcpy(cfg->net_search_url,
           "https://html.duckduckgo.com/html/?q=%s");
    cfg->net_github_token[0] = '\0';
    /* Unlimited by default: a finite cap makes agent_run() abort a turn
     * mid-task, which looks like the conversation has stopped. The loop
     * guard (repeated calls / repeated rounds) is what actually prevents
     * runaway API usage. Users who still want a hard cap can set 1..200. */
    cfg->max_steps = 0;
    cfg->tool_call_fallback = 1;
    cfg->read_cache = 1;
    cfg->nudge_on_success = 1;
    strcpy(cfg->tool_profile, "coding");
    /* 24 KB ~ roughly 6-8k tokens; safe on 9x and before typical 8k/16k
     * context limits. 0 disables. Keep the last 6 messages (~3 rounds). */
    cfg->compact_threshold = 24576;
    cfg->compact_keep = 6;
    cfg->connect_timeout_sec = 30;
    cfg->request_timeout_sec = 180;
    cfg->log_level = 2;
}

static void read_str(const char *ini, const char *section, const char *key,
                     const char *def, char *out, size_t cap)
{
    /* The staging buffer must be able to hold the biggest destination we use,
     * which is system_prompt (4096).  Sizing it from A98_MAX_PATH truncated
     * long prompts silently -- and config_save() then wrote the truncated
     * value back to the INI. */
    char tmp[4096];
    DWORD n;
    DWORD size;

    if (out == NULL || cap == 0) {
        return;
    }
    size = (DWORD)(cap < sizeof(tmp) ? cap : sizeof(tmp));
    n = GetPrivateProfileStringA(section, key, "", tmp, size, ini);
    if (n > 0) {
        config_trim(tmp);
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = '\0';
        if (n >= size - 1) {
            log_warn("configuration [%s] %s is longer than %lu bytes; "
                     "it was truncated", section, key,
                     (unsigned long)size - 1);
        }
    } else if (def != NULL && def != out) {
        strncpy(out, def, cap - 1);
        out[cap - 1] = '\0';
        config_trim(out);
    }
}

static int read_int(const char *ini, const char *section, const char *key, int def)
{
    /* GetPrivateProfileIntA returns 0 for every unparsable value, which would
     * turn a typo like max_steps=abc into "unlimited".  Read the string and
     * only accept something strtol can fully consume. */
    char tmp[64];
    char *end;
    long v;
    DWORD n;
    int bad;

    n = GetPrivateProfileStringA(section, key, "", tmp, sizeof(tmp), ini);
    if (n == 0) {
        return def;
    }
    config_trim(tmp);
    if (tmp[0] == '\0') {
        return def;
    }
    errno = 0;
    v = strtol(tmp, &end, 10);
    bad = (end == tmp || *end != '\0' || errno == ERANGE);
#if LONG_MAX > INT_MAX || LONG_MIN < INT_MIN
    /* Only needed where long is wider than int (on Win32 they are both 32
     * bits).  Guarding avoids "comparison always false" warnings there. */
    if (v < (long)INT_MIN || v > (long)INT_MAX) {
        bad = 1;
    }
#endif
    if (bad) {
        log_warn("configuration [%s] %s = \"%s\" is out of range or not a "
                 "number; using the default %d", section, key, tmp, def);
        return def;
    }
    return (int)v;
}

/* Portable finite test: x-x is NaN exactly for +inf, -inf and NaN, so it is
 * false only for those values.  Avoids isfinite()/_finite() portability
 * problems with the old CRT used on Windows 9x. */
static int a98_isfinite(double x)
{
    double d = x - x;
    return d == d;
}

/* atof() happily accepts "nan" and "inf"; those values passed the <0 / >2
 * checks and were then written back as "nan" by sprintf("%.2f").  Consume the
 * whole string and reject every non-finite value. */
static double read_double(const char *ini, const char *section, const char *key,
                          double def)
{
    char tmp[64];
    char *end;
    double v;
    DWORD n;

    n = GetPrivateProfileStringA(section, key, "", tmp, sizeof(tmp), ini);
    if (n == 0) {
        return def;
    }
    config_trim(tmp);
    if (tmp[0] == '\0') {
        return def;
    }
    errno = 0;
    v = strtod(tmp, &end);
    if (end == tmp || *end != '\0' || errno == ERANGE || !a98_isfinite(v)) {
        log_warn("configuration [%s] %s = \"%s\" is not a finite number; "
                 "using the default %.2f", section, key, tmp, def);
        return def;
    }
    return v;
}

static void config_exe_dir(char *out, size_t cap)
{
    if (a98_path_exe_dir(out, cap) != 0) {
        strncpy(out, ".", cap - 1);
        out[cap - 1] = '\0';
    }
}

void config_default_path(char *out, size_t cap)
{
    char dir[A98_MAX_PATH];
    if (out == NULL || cap == 0) {
        return;
    }
    config_exe_dir(dir, sizeof(dir));
    a98_path_join(out, cap, dir, "agent98.ini");
}

int config_load(Config *cfg, const char *ini_path)
{
    char def_workspace[A98_MAX_PATH];
    char exe_dir[A98_MAX_PATH];

    if (cfg == NULL || ini_path == NULL) {
        return -1;
    }
    config_exe_dir(exe_dir, sizeof(exe_dir));
    config_defaults(cfg, exe_dir);

    read_str(ini_path, "api", "base", "https://api.openai.com/v1",
             cfg->api_base, sizeof(cfg->api_base));
    read_str(ini_path, "api", "key", "", cfg->api_key, sizeof(cfg->api_key));
    read_str(ini_path, "api", "model", "gpt-4o-mini",
             cfg->model, sizeof(cfg->model));
    read_str(ini_path, "api", "system_prompt", DEFAULT_SYSTEM_PROMPT,
             cfg->system_prompt, sizeof(cfg->system_prompt));
    read_str(ini_path, "api", "extra_headers", "", cfg->extra_headers,
             sizeof(cfg->extra_headers));
    cfg->max_tokens = read_int(ini_path, "api", "max_tokens", cfg->max_tokens);
    cfg->temperature = read_double(ini_path, "api", "temperature",
                                   cfg->temperature);
    cfg->stream = read_int(ini_path, "api", "stream", cfg->stream);

    read_str(ini_path, "agent", "workspace", cfg->workspace,
             cfg->workspace, sizeof(cfg->workspace));
    cfg->allow_outside = read_int(ini_path, "agent", "allow_outside", cfg->allow_outside);
    cfg->allow_shell = read_int(ini_path, "agent", "allow_shell", cfg->allow_shell);
    cfg->full_access = read_int(ini_path, "agent", "full_access", cfg->full_access);
    cfg->auto_confirm = read_int(ini_path, "agent", "auto_confirm", cfg->auto_confirm);
    /* full_access is the "I know what I'm doing" mode: it implies
     * auto_confirm so the model runs without being prompted for every call. */
    if (cfg->full_access) {
        cfg->auto_confirm = 1;
    }
    cfg->emoji_inline = read_int(ini_path, "emoji", "inline", cfg->emoji_inline);
    cfg->net_allow = read_int(ini_path, "net", "allow", cfg->net_allow);
    read_str(ini_path, "net", "allow_hosts", cfg->net_allow_hosts,
             cfg->net_allow_hosts, sizeof(cfg->net_allow_hosts));
    cfg->net_max_bytes = read_int(ini_path, "net", "max_bytes", cfg->net_max_bytes);
    read_str(ini_path, "net", "search_url", cfg->net_search_url,
             cfg->net_search_url, sizeof(cfg->net_search_url));
    read_str(ini_path, "net", "github_token", cfg->net_github_token,
             cfg->net_github_token, sizeof(cfg->net_github_token));
    if (cfg->net_max_bytes < 1024) {
        cfg->net_max_bytes = 1024;
    }
    if (cfg->net_max_bytes > 8 * 1024 * 1024) {
        cfg->net_max_bytes = 8 * 1024 * 1024;
    }
    cfg->max_steps = read_int(ini_path, "agent", "max_steps", cfg->max_steps);
    cfg->tool_call_fallback =
        read_int(ini_path, "agent", "tool_call_fallback", cfg->tool_call_fallback);
    cfg->read_cache =
        read_int(ini_path, "agent", "read_cache", cfg->read_cache);
    cfg->nudge_on_success =
        read_int(ini_path, "agent", "nudge_on_success", cfg->nudge_on_success);
    read_str(ini_path, "agent", "tool_profile", cfg->tool_profile,
             cfg->tool_profile, sizeof(cfg->tool_profile));
    config_trim(cfg->tool_profile);
    /* Canonicalise so the rest of the code can use exact strcmp. */
    if (a98_strcasecmp(cfg->tool_profile, "full") == 0) {
        strcpy(cfg->tool_profile, "full");
    } else if (a98_strcasecmp(cfg->tool_profile, "network") == 0) {
        strcpy(cfg->tool_profile, "network");
    } else if (a98_strcasecmp(cfg->tool_profile, "docs") == 0) {
        strcpy(cfg->tool_profile, "docs");
    } else {
        if (cfg->tool_profile[0] != '\0') {
            log_warn("configuration [agent] tool_profile = \"%s\" is unknown; "
                     "using coding", cfg->tool_profile);
        }
        strcpy(cfg->tool_profile, "coding");
    }
    cfg->compact_threshold =
        read_int(ini_path, "agent", "compact_threshold", cfg->compact_threshold);
    cfg->compact_keep =
        read_int(ini_path, "agent", "compact_keep", cfg->compact_keep);
    if (cfg->compact_threshold < 0) cfg->compact_threshold = 0;
    if (cfg->compact_keep < 2) cfg->compact_keep = 2;
    if (cfg->compact_keep > 40) cfg->compact_keep = 40;
    cfg->connect_timeout_sec = read_int(ini_path, "agent", "connect_timeout",
                                        cfg->connect_timeout_sec);
    cfg->request_timeout_sec = read_int(ini_path, "agent", "request_timeout",
                                        cfg->request_timeout_sec);

    cfg->tls_verify =
        read_int(ini_path, "tls", "verify", cfg->tls_verify) != 0;

    cfg->log_level = read_int(ini_path, "log", "level", cfg->log_level);
    if (cfg->log_level < LOG_LEVEL_ERROR) {
        cfg->log_level = LOG_LEVEL_ERROR;
    }
    if (cfg->log_level > LOG_LEVEL_TRACE) {
        cfg->log_level = LOG_LEVEL_TRACE;
    }

    if (cfg->workspace[0] == '\0') {
        config_exe_dir(def_workspace, sizeof(def_workspace));
        a98_path_join(cfg->workspace, sizeof(cfg->workspace),
                      def_workspace, "workspace");
    }
    if (cfg->max_tokens < 1) {
        cfg->max_tokens = 1;
    }
    if (cfg->max_tokens > 32768) {
        cfg->max_tokens = 32768;
    }
    /* 0 means unlimited; otherwise allow a generous 1..200.  A negative or
     * unparsable value falls back to the default. */
    if (cfg->max_steps < 0) {
        cfg->max_steps = 0;
    }
    if (cfg->max_steps > 200) {
        cfg->max_steps = 200;
    }
    if (cfg->temperature < 0.0) {
        cfg->temperature = 0.0;
    }
    if (cfg->temperature > 2.0) {
        cfg->temperature = 2.0;
    }
    if (cfg->connect_timeout_sec < 1) {
        cfg->connect_timeout_sec = 1;
    }
    if (cfg->request_timeout_sec < 1) {
        cfg->request_timeout_sec = 1;
    }
    return 0;
}

static int write_str(const char *ini, const char *section, const char *key,
                     const char *value)
{
    return WritePrivateProfileStringA(section, key, value ? value : "", ini)
               ? 0 : -1;
}

static int write_int(const char *ini, const char *section, const char *key,
                     int value)
{
    char tmp[32];
    /* Every value passed here is a clamped 32-bit int or a 0/1 flag, so %d
     * writes at most 12 bytes.  The fixed buffer cannot overflow. */
    sprintf(tmp, "%d", value);
    return write_str(ini, section, key, tmp);
}

int config_save(const Config *cfg, const char *ini_path)
{
    int ok = 1;
    char *temp;

    if (cfg == NULL || ini_path == NULL) {
        return -1;
    }
    /* Every write is checked: a read-only directory (the CD-ROM / network
     * share case the README warns about) used to fail completely silently. */
    ok = (write_str(ini_path, "api", "base", cfg->api_base) == 0) && ok;
    ok = (write_str(ini_path, "api", "key", cfg->api_key) == 0) && ok;
    ok = (write_str(ini_path, "api", "model", cfg->model) == 0) && ok;
    ok = (write_str(ini_path, "api", "system_prompt", cfg->system_prompt) == 0) && ok;
    ok = (write_str(ini_path, "api", "extra_headers", cfg->extra_headers) == 0) && ok;
    ok = (write_int(ini_path, "api", "max_tokens", cfg->max_tokens) == 0) && ok;
    /* a98_strfmt() grows its buffer instead of overflowing/clipping; a NaN
     * that slipped in from elsewhere must never be persisted as "nan". */
    temp = a98_isfinite(cfg->temperature)
               ? a98_strfmt("%.2f", cfg->temperature) : NULL;
    ok = (write_str(ini_path, "api", "temperature",
                    temp != NULL ? temp : "0.70") == 0) && ok;
    a98_free(temp);
    ok = (write_int(ini_path, "api", "stream", cfg->stream ? 1 : 0) == 0) && ok;

    ok = (write_str(ini_path, "agent", "workspace", cfg->workspace) == 0) && ok;
    ok = (write_int(ini_path, "agent", "allow_outside",
                    cfg->allow_outside ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "agent", "allow_shell",
                    cfg->allow_shell ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "agent", "full_access",
                    cfg->full_access ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "agent", "auto_confirm",
                    cfg->auto_confirm ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "emoji", "inline",
                    cfg->emoji_inline ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "net", "allow", cfg->net_allow ? 1 : 0) == 0) && ok;
    ok = (write_str(ini_path, "net", "allow_hosts", cfg->net_allow_hosts) == 0) && ok;
    ok = (write_int(ini_path, "net", "max_bytes", cfg->net_max_bytes) == 0) && ok;
    ok = (write_str(ini_path, "net", "search_url", cfg->net_search_url) == 0) && ok;
    ok = (write_str(ini_path, "net", "github_token", cfg->net_github_token) == 0) && ok;
    ok = (write_int(ini_path, "agent", "max_steps", cfg->max_steps) == 0) && ok;
    ok = (write_int(ini_path, "agent", "tool_call_fallback",
                    cfg->tool_call_fallback ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "agent", "read_cache",
                    cfg->read_cache ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "agent", "nudge_on_success",
                    cfg->nudge_on_success ? 1 : 0) == 0) && ok;
    ok = (write_str(ini_path, "agent", "tool_profile",
                    cfg->tool_profile) == 0) && ok;
    ok = (write_int(ini_path, "agent", "compact_threshold",
                    cfg->compact_threshold) == 0) && ok;
    ok = (write_int(ini_path, "agent", "compact_keep", cfg->compact_keep) == 0) && ok;
    ok = (write_int(ini_path, "agent", "connect_timeout",
                    cfg->connect_timeout_sec) == 0) && ok;
    ok = (write_int(ini_path, "agent", "request_timeout",
                    cfg->request_timeout_sec) == 0) && ok;

    ok = (write_int(ini_path, "tls", "verify", cfg->tls_verify ? 1 : 0) == 0) && ok;
    ok = (write_int(ini_path, "log", "level", cfg->log_level) == 0) && ok;
    return ok ? 0 : -1;
}

int config_ensure(Config *cfg, const char *ini_path)
{
    char exe_dir[A98_MAX_PATH];

    if (a98_path_exists(ini_path)) {
        return config_load(cfg, ini_path);
    }
    config_exe_dir(exe_dir, sizeof(exe_dir));
    config_defaults(cfg, exe_dir);
    return config_save(cfg, ini_path);
}
