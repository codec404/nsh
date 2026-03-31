#include "shell.h"
#include <dirent.h>

/*
 * Context-aware readline tab completion.
 *
 * Rules:
 *   - First word in a pipeline segment → command names (builtins + PATH)
 *   - Word after where / select / sort-by → table column names
 *   - Otherwise → default filename completion
 */

/* ── executable cache ────────────────────────────────────────────────────── */

static char **exe_cache          = NULL;
static int    exe_count          = 0;
static int    exe_cap            = 0;
static char   exe_path_snap[4096] = "";   /* PATH at last rebuild */

static void cache_push(const char *name)
{
    if (exe_count >= exe_cap) {
        int newcap = exe_cap ? exe_cap * 2 : 512;
        char **tmp = realloc(exe_cache, newcap * sizeof(char *));
        if (!tmp) return;
        exe_cache = tmp;
        exe_cap   = newcap;
    }
    exe_cache[exe_count++] = strdup(name);
}

static void build_exe_cache(void)
{
    for (int i = 0; i < exe_count; i++) free(exe_cache[i]);
    exe_count = 0;

    /* shell builtins */
    static const char *blt[] = {
        "cd", "exit", "pwd", "jobs", "fg", "bg", "replay", "shellenv", NULL };
    for (int i = 0; blt[i]; i++) cache_push(blt[i]);

    /* table commands */
    static const char *tbl[] = {
        "ls", "env", "history", "sessions",
        "where", "select", "sort-by", "first", "last",
        "count", "describe", "get", NULL };
    for (int i = 0; tbl[i]; i++) cache_push(tbl[i]);

    /* PATH executables */
    const char *path_env = getenv("PATH");
    if (!path_env) return;

    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *tok = strtok(path_copy, ":");
    while (tok) {
        DIR *d = opendir(tok);
        if (d) {
            struct dirent *ent;
            char full[PATH_MAX];
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                snprintf(full, sizeof(full), "%s/%s", tok, ent->d_name);
                if (access(full, X_OK) == 0)
                    cache_push(ent->d_name);
            }
            closedir(d);
        }
        tok = strtok(NULL, ":");
    }
}

/* Rebuild the cache only when PATH changes */
static void ensure_exe_cache(void)
{
    const char *cur = getenv("PATH");
    if (!cur) cur = "";
    if (exe_count > 0 && strcmp(cur, exe_path_snap) == 0) return;
    strncpy(exe_path_snap, cur, sizeof(exe_path_snap) - 1);
    exe_path_snap[sizeof(exe_path_snap) - 1] = '\0';
    build_exe_cache();
}

static int    cmd_gen_idx;

static char *command_generator(const char *text, int state)
{
    if (!state) { ensure_exe_cache(); cmd_gen_idx = 0; }

    size_t tlen = strlen(text);
    while (cmd_gen_idx < exe_count) {
        const char *name = exe_cache[cmd_gen_idx++];
        if (strncmp(name, text, tlen) == 0)
            return strdup(name);
    }
    return NULL;
}

/* ── column name completion ──────────────────────────────────────────────── */

static const char *ls_cols[]       = { "name", "size", "type", "perm", "modified", NULL };
static const char *history_cols[]  = { "cmdline", "exit_code", "duration_ms", "cwd", "ts", NULL };
static const char *sessions_cols[] = { "session_id", "start_ts", "end_ts", "count", NULL };
static const char *env_cols[]      = { "name", "value", NULL };

/* Heuristic: scan the buffer for the most recent producer keyword */
static const char **cols_for_line(const char *buf)
{
    if (strstr(buf, "history"))  return history_cols;
    if (strstr(buf, "sessions")) return sessions_cols;
    if (strstr(buf, "env"))      return env_cols;
    return ls_cols;
}

static const char **col_src;
static int          col_gen_idx;

static char *column_generator(const char *text, int state)
{
    if (!state) {
        col_src     = cols_for_line(rl_line_buffer);
        col_gen_idx = 0;
    }

    size_t tlen = strlen(text);
    while (col_src[col_gen_idx]) {
        const char *col = col_src[col_gen_idx++];
        if (strncmp(col, text, tlen) == 0)
            return strdup(col);
    }
    return NULL;
}

/* ── context detection ───────────────────────────────────────────────────── */

/*
 * Returns 1 if `start` is in command position — i.e., there is no
 * non-whitespace word between the start of the current pipeline segment
 * (after the last '|') and `start`.
 */
static int is_first_word(int start)
{
    int seg_start = 0;
    for (int i = start - 1; i >= 0; i--) {
        if (rl_line_buffer[i] == '|') { seg_start = i + 1; break; }
    }
    for (int i = seg_start; i < start; i++) {
        char c = rl_line_buffer[i];
        if (c != ' ' && c != '\t') return 0;
    }
    return 1;
}

/*
 * Returns 1 if the word immediately before `start` is a table consumer
 * (where / select / sort-by), meaning we should complete column names.
 */
static int is_after_consumer(int start)
{
    static const char *consumers[] = { "where", "select", "sort-by", NULL };

    /* skip trailing whitespace before cursor */
    int i = start - 1;
    while (i >= 0 && (rl_line_buffer[i] == ' ' || rl_line_buffer[i] == '\t')) i--;
    int end = i + 1;

    /* walk back over the preceding word */
    while (i >= 0 && rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t' &&
           rl_line_buffer[i] != '|') i--;
    int wstart = i + 1;
    int wlen   = end - wstart;

    for (int k = 0; consumers[k]; k++) {
        if ((int)strlen(consumers[k]) == wlen &&
            strncmp(rl_line_buffer + wstart, consumers[k], wlen) == 0)
            return 1;
    }
    return 0;
}

/* ── main completion entry point ─────────────────────────────────────────── */

static char **nsh_complete(const char *text, int start, int end)
{
    (void)end;
    rl_attempted_completion_over = 1;  /* suppress default file completion */

    /* column completion takes priority (handles "where colna<TAB>") */
    if (is_after_consumer(start))
        return rl_completion_matches(text, column_generator);

    /* path-like text (./foo, /usr, ~/bin) → always filename completion */
    if (text[0] == '.' || text[0] == '/' || text[0] == '~') {
        rl_attempted_completion_over = 0;
        return NULL;
    }

    if (is_first_word(start))
        return rl_completion_matches(text, command_generator);

    /* fall back to readline's built-in filename completion */
    rl_attempted_completion_over = 0;
    return NULL;
}

void complete_init(void)
{
    rl_attempted_completion_function = nsh_complete;
}
