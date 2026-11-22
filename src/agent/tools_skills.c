/* AIagent98 - skill tools. See tools_skills.h. */
#include "tools_skills.h"
#include "agent.h"
#include "jsonutil.h"
#include "skills.h"

#include "../core/log.h"
#include "../core/utf8.h"
#include "../../third_party/cjson/cJSON.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static const char *arg_str(cJSON *args, const char *name, const char *def)
{
    const char *v = json_string(args, name);
    return v != NULL ? v : def;
}

char *skills_prompt_block(const char *workspace)
{
    SkillList list;
    Str s;
    int i;

    str_init(&s);
    skills_init(&list);
    if (skills_scan(workspace, &list) > 0) {
        str_append_cstr(&s,
            "\nAvailable skills (procedural memory). A skill is a reusable "
            "procedure saved as a file. To follow one, call use_skill with "
            "its name to load the full instructions before proceeding. To "
            "save what you learned, call save_skill.\n");
        for (i = 0; i < list.count; i++) {
            str_appendf(&s, "- %s: %s\n",
                        list.items[i].name,
                        (list.items[i].description && list.items[i].description[0])
                        ? list.items[i].description : "(no description)");
        }
    }
    skills_free(&list);
    return str_detach(&s);
}

/* ---- list_skills ----------------------------------------------------- */

const char schema_list_skills[] =
    "{\"type\":\"object\",\"properties\":{},\"required\":[]}";

int tool_list_skills_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    SkillList list;
    int i, n;
    (void)args; (void)err; (void)errcap;

    skills_init(&list);
    n = skills_scan(a->workspace, &list);
    if (n <= 0) {
        str_append_cstr(out, "(no skills available)\n");
        skills_free(&list);
        return 0;
    }
    str_appendf(out, "%d skill(s):\n", n);
    for (i = 0; i < list.count; i++) {
        str_appendf(out, "- %s: %s\n", list.items[i].name,
                    (list.items[i].description && list.items[i].description[0])
                    ? list.items[i].description : "(no description)");
    }
    skills_free(&list);
    return 0;
}
const ToolDef tool_list_skills = {
    "list_skills", "List available skills (procedural memory).",
    schema_list_skills, 0, tool_list_skills_run
};

/* ---- use_skill ------------------------------------------------------- */

const char schema_use_skill[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Skill name to load.\"}},"
    "\"required\":[\"name\"]}";

int tool_use_skill_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *name = arg_str(args, "name", NULL);
    SkillList list;
    const Skill *s;

    if (name == NULL || name[0] == '\0') {
        set_err(err, errcap, "name is required");
        return -1;
    }
    skills_init(&list);
    skills_scan(a->workspace, &list);
    s = skills_find(&list, name);
    if (s == NULL) {
        set_err(err, errcap, "no such skill: %s (use list_skills to see available skills)", name);
        skills_free(&list);
        return -1;
    }
    str_appendf(out, "# Skill: %s\n\n", s->name);
    if (s->description != NULL && s->description[0] != '\0')
        str_appendf(out, "%s\n\n", s->description);
    if (s->body != NULL && s->body[0] != '\0')
        str_append_cstr(out, s->body);
    else
        str_append_cstr(out, "(skill has no body)");
    log_info("use_skill: loaded '%s'", s->name);
    skills_free(&list);
    return 0;
}
const ToolDef tool_use_skill = {
    "use_skill", "Load a skill's full instructions into the conversation.",
    schema_use_skill, 0, tool_use_skill_run
};

/* ---- save_skill ------------------------------------------------------ */

const char schema_save_skill[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Short id (letters/digits/-/_).\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"One-line summary for the skill list.\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Full skill body (Markdown, UTF-8).\"}},"
    "\"required\":[\"name\",\"description\",\"content\"]}";

int tool_save_skill_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *name = arg_str(args, "name", NULL);
    const char *desc = arg_str(args, "description", "");
    const char *content = arg_str(args, "content", NULL);

    if (content == NULL) {
        set_err(err, errcap, "content is required");
        return -1;
    }
    if (skills_save(a->workspace, name, desc, content, err, errcap) != 0)
        return -1;
    str_appendf(out, "Skill '%s' saved. It is now available via use_skill and appears in the skill list.\n", name);
    log_info("save_skill: saved '%s'", name);
    return 0;
}
const ToolDef tool_save_skill = {
    "save_skill", "Create or update a reusable skill. Requires approval.",
    schema_save_skill, 1, tool_save_skill_run
};
