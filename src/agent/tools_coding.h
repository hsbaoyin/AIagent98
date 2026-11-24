/* AIagent98 - extra coding tools. */
#ifndef A98_TOOLS_CODING_H
#define A98_TOOLS_CODING_H

#include "tools.h"

extern const ToolDef tool_build;
extern const ToolDef tool_replace_across;
extern const ToolDef tool_outline;
extern const ToolDef tool_restore;
extern const ToolDef tool_snapshot;
extern const ToolDef tool_hexdump;
extern const ToolDef tool_toolchain_probe;

int tool_build_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_replace_across_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_outline_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_restore_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_snapshot_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_hexdump_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_toolchain_probe_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);

extern const char schema_build[];
extern const char schema_replace_across[];
extern const char schema_outline[];
extern const char schema_restore[];
extern const char schema_snapshot[];
extern const char schema_hexdump[];
extern const char schema_toolchain_probe[];

#endif /* A98_TOOLS_CODING_H */


