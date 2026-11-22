/*
 * AIagent98 - pluggable skills ("procedural memory", Hermes-style).
 *
 * A skill is a SKILL.md file in <workspace>\skills\. Format:
 *   ---
 *   name: my-skill
 *   description: what this skill does
 *   ---
 *   # Markdown body (UTF-8) loaded in full when the skill is invoked.
 *
 * Only name + description are injected into the system prompt (progressive
 * disclosure); the body is fetched via the use_skill tool. The model can
 * create/update skills with save_skill. Skills are plain files, so adding or
 * removing one needs no rebuild.
 */
#ifndef A98_SKILLS_H
#define A98_SKILLS_H

#include "../core/base.h"
#include "../core/str.h"

typedef struct {
    char name[64];         /* sanitized [A-Za-z0-9_-] */
    char *description;     /* UTF-8, malloc'ed */
    char *body;            /* UTF-8, malloc'ed */
    char path[A98_MAX_PATH]; /* ACP on-disk path */
} Skill;

typedef struct {
    Skill *items;
    int count;
    int cap;
} SkillList;

void skills_init(SkillList *list);
void skills_free(SkillList *list);

/* Where skills live: <workspace>\skills\ (ACP). Returns 0 on success. */
int skills_dir(const char *workspace, char *out, size_t cap);

/* Scan the skills directory into `list` (cleared first). Returns count, or
 * -1 on a hard error. Missing directory is not an error (0 skills). */
int skills_scan(const char *workspace, SkillList *list);

/* Find by exact name. Returns NULL if absent. */
const Skill *skills_find(const SkillList *list, const char *name);

/* Create or update a skill. Validates/sanitizes name. Returns 0 on success. */
int skills_save(const char *workspace, const char *name,
                const char *description, const char *body,
                char *err, size_t errcap);

/* Delete a skill by name. Returns 0 on success. */
int skills_delete(const char *workspace, const char *name,
                  char *err, size_t errcap);

/* True if `name` is a safe skill identifier. */
int skills_valid_name(const char *name);

#endif /* A98_SKILLS_H */

