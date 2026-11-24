#ifndef A98_TOOLS_H
#define A98_TOOLS_H

#include "../core/base.h"
#include "../core/str.h"
#include "../../third_party/cjson/cJSON.h"

struct Agent;

typedef struct ToolDef {
    const char *name;
    const char *description;
    const char *params_json;
    int needs_confirm;
    int (*run)(struct Agent *agent, cJSON *args, Str *out, char *err, size_t errcap);
} ToolDef;

/* Resolve a model-supplied path inside the workspace (or allow_outside).
 * Returns 0 on success. Shared by the built-in and the extended tools. */
int tools_resolve_path(struct Agent *a, const char *user, char *out, size_t cap,
                       char *err, size_t errcap);

const ToolDef *tools_all(int *count);
const ToolDef *tools_find(const char *name);

/* Whether `name` is advertised to the model under `profile`.
 * Profiles: "full", "coding" (default), "network", "docs".  This only
 * filters the tool list sent in the request; every tool remains callable
 * so that a saved session or a skill cannot lose access. */
int tools_profile_allows(const char *profile, const char *name);

/* Clear the unchanged-read cache (new conversation, loaded session, etc.). */
void tools_read_cache_clear(void);

/* True for tools that change workspace files (used by the read cache and the
 * "checks passed" nudge). */
int tools_tool_is_writer(const char *name);

/* Execute a tool by name. `args_json` may be empty. Returns 0 on success;
 * on failure returns -1 and puts a model-readable message in `err`. */
int tools_execute(struct Agent *agent, const char *name, const char *args_json,
                  Str *out, char *err, size_t errcap);

/* Human-readable confirm text. danger: 0=low, 1=medium, 2=high.
 * out receives a multi-line summary in UTF-8 (caller converts to ACP). */
void tools_format_confirm(const char *name, const char *args_json,
                         char *out, size_t outcap, int *danger);

#endif /* A98_TOOLS_H */
