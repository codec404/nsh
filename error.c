#include "shell.h"

/* ── edit distance ───────────────────────────────────────────────────────── */

/* Levenshtein distance, capped at 4 (only used for short command names). */
static int edit_dist(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 32 || lb > 32 || abs(la - lb) > 3) return 4;

    int dp[33][33];
    for (int i = 0; i <= la; i++) dp[i][0] = i;
    for (int j = 0; j <= lb; j++) dp[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] != b[j-1]) ? 1 : 0;
            int sub  = dp[i-1][j-1] + cost;
            int del  = dp[i-1][j]   + 1;
            int ins  = dp[i][j-1]   + 1;
            dp[i][j] = sub < del ? (sub < ins ? sub : ins)
                                 : (del < ins ? del : ins);
        }
    }
    return dp[la][lb];
}

/* ── suggestions ─────────────────────────────────────────────────────────── */

/*
 * suggest_command_not_found — print a helpful hint when a command cannot
 * be found in PATH.  Checks for:
 *   1. Near-misses among builtins / table commands (edit distance ≤ 2)
 *   2. The executable existing in a common dir not in $PATH
 *   3. Generic PATH advice
 */
void suggest_command_not_found(const char *cmd)
{
    static const char *known[] = {
        /* shell builtins */
        "cd", "exit", "pwd", "jobs", "fg", "bg", "replay", "shellenv",
        /* table commands */
        "ls", "env", "history", "sessions", "where", "select", "sort-by",
        "first", "last", "count", "describe", "get",
        NULL
    };

    for (int i = 0; known[i]; i++) {
        if (edit_dist(cmd, known[i]) <= 2) {
            fprintf(stderr, "  hint: did you mean '%s'?\n", known[i]);
            return;
        }
    }

    /* check common directories that might not be in PATH */
    static const char *dirs[] = {
        "/usr/bin", "/usr/local/bin",
        "/opt/homebrew/bin", "/opt/homebrew/sbin",
        "/usr/sbin", "/sbin",
        NULL
    };
    char path[PATH_MAX];
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], cmd);
        if (access(path, X_OK) == 0) {
            fprintf(stderr,
                    "  hint: '%s' exists at %s but is not in $PATH\n",
                    cmd, path);
            return;
        }
    }

    fprintf(stderr,
            "  hint: check spelling or run 'echo $PATH' to inspect search dirs\n");
}

/*
 * suggest_on_error — print a hint after execvp() fails.
 * `file` is the resolved path; `err` is the saved errno.
 */
void suggest_on_error(const char *file, int err)
{
    switch (err) {
    case EACCES:
        fprintf(stderr, "  hint: permission denied — try: sudo %s\n", file);
        break;
    case ENOENT:
        fprintf(stderr, "  hint: no such file — check the path or spelling\n");
        break;
    case ENOEXEC:
        fprintf(stderr,
                "  hint: not executable — wrong format or missing shebang line\n");
        break;
    case EISDIR:
        fprintf(stderr, "  hint: that is a directory, not an executable\n");
        break;
    default:
        break;
    }
}
