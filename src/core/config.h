/*
 * AIagent98 - INI-backed configuration.
 */
#ifndef A98_CONFIG_H
#define A98_CONFIG_H

#include "base.h"
#include "path.h"

typedef struct {
    char api_base[512];
    char api_key[512];
    char model[128];
    char system_prompt[4096];
    char workspace[A98_MAX_PATH];
    char extra_headers[512];
    int max_tokens;
    double temperature;
    int stream;
    int tls_verify;
    int allow_outside;
    int allow_shell;
    /* DANGER: full system access. Implies allow_shell + allow_outside and
     * removes the shell command ban list. Off by default. */
    int full_access;
    /* Draw emoji as the pack's pictures in the transcript ([emoji] inline).
     * Off by default: the RichEdit OLE path faults on some Windows 9x
     * installs, and shortcode text is the safe fallback. */
    int emoji_inline;
    /* Network tools ([net]). Off by default: the agent must not reach the
     * network unless the user opts in. allow_hosts is a comma-separated
     * suffix list; empty means any host. max_bytes caps each response body. */
    int net_allow;
    char net_allow_hosts[512];
    int net_max_bytes;
    char net_search_url[512];
    char net_github_token[256];
    int max_steps;
    int connect_timeout_sec;
    int request_timeout_sec;
    int log_level;
} Config;

/* Fill in defaults. `exe_dir` may be NULL; workspace then defaults to
 * "workspace" relative to the current directory. */
void config_defaults(Config *cfg, const char *exe_dir);

/* Returns 0 on success, -1 on I/O error. Missing keys keep defaults. */
int config_load(Config *cfg, const char *ini_path);
int config_save(const Config *cfg, const char *ini_path);
int config_ensure(Config *cfg, const char *ini_path);

/* Resolve the default INI path next to the executable. */
void config_default_path(char *out, size_t cap);

void config_trim(char *s);

#endif /* A98_CONFIG_H */
