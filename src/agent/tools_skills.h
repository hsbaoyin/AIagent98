/* AIagent98 - skill tools (procedural memory). */
#ifndef A98_TOOLS_SKILLS_H
#define A98_TOOLS_SKILLS_H

#include "tools.h"

extern const ToolDef tool_use_skill;
extern const ToolDef tool_save_skill;
extern const ToolDef tool_list_skills;

int tool_use_skill_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_save_skill_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_list_skills_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);

extern const char schema_use_skill[];
extern const char schema_save_skill[];
extern const char schema_list_skills[];

/* Build a short "Available skills" block for the system prompt (UTF-8),
 * listing each skill's name and description. Returns an empty string when
 * there are no skills. Caller owns the returned a98_strdup'd string. */
char *skills_prompt_block(const char *workspace);

#endif /* A98_TOOLS_SKILLS_H */

