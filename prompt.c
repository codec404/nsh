#include "shell.h"

/* ── git branch ─────────────────────────────────────────────────────────── */

/*
 * Walk up from cwd looking for .git/HEAD.
 * Fills `out` with branch name ("main") or short hash for detached HEAD.
 * Returns 1 on success, 0 if not inside a git repo.
 */
static int git_branch(char *out, size_t outlen)
{
    char dir[PATH_MAX];
    if (!getcwd(dir, sizeof(dir))) return 0;

    while (1) {
        char head[PATH_MAX];
        snprintf(head, sizeof(head), "%s/.git/HEAD", dir);

        FILE *f = fopen(head, "r");
        if (f) {
            char line[256] = {0};
            if (fgets(line, sizeof(line), f)) {
                fclose(f);
                size_t len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                    line[--len] = '\0';
                if (strncmp(line, "ref: refs/heads/", 16) == 0)
                    snprintf(out, outlen, "%s", line + 16);
                else
                    snprintf(out, outlen, "%.7s", line);  /* detached HEAD */
                return 1;
            }
            fclose(f);
            return 0;
        }

        /* walk up one directory */
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return 0;
}

/* ── make_prompt ─────────────────────────────────────────────────────────── */

/*
 * Format tokens (from g_config.prompt_format):
 *
 *   %u   username
 *   %w   cwd (~ substituted for $HOME)
 *   %g   git branch as " (branch)" or ""
 *   %j   background job count as " [N]" or ""
 *   %e   shellenv stack depth as " {N}" or ""
 *   %s   "$" — bold-red if last_status != 0, bold-green otherwise
 *   %%   literal %
 *
 * ANSI codes are wrapped in \001/\002 (RL_PROMPT_START/END_IGNORE) so
 * readline measures the visible width correctly.
 */

/* Wraps an ANSI escape sequence so readline ignores it in width calculations */
#define RL_ANSI(seq)  "\001" seq "\002"

char *make_prompt(void)
{
    /* ── gather data ── */
    const char *user = getenv("USER");
    if (!user) user = "nsh";

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");

    const char *home = getenv("HOME");
    char display_cwd[PATH_MAX];
    if (home && strncmp(cwd, home, strlen(home)) == 0 &&
        (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0'))
        snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    else
        snprintf(display_cwd, sizeof(display_cwd), "%s", cwd);

    char branch[128] = "";
    int  has_git   = git_branch(branch, sizeof(branch));
    int  jcount    = jobs_count();
    int  envdepth  = shellenv_depth();

    /* ── render format string ── */
    char buf[PATH_MAX + 512];
    int  bi   = 0;
    int  bmax = (int)sizeof(buf) - 1;

#define APPEND(str) do {                                \
    const char *_s = (str);                             \
    while (*_s && bi < bmax) buf[bi++] = *_s++;         \
} while (0)

    const char *fmt = g_config.prompt_format;
    char tmp[256];

    for (int i = 0; fmt[i] && bi < bmax; i++) {
        if (fmt[i] != '%') { buf[bi++] = fmt[i]; continue; }
        i++;
        switch (fmt[i]) {
        case 'u':
            APPEND(RL_ANSI("\033[1;32m"));
            APPEND(user);
            APPEND(RL_ANSI("\033[0m"));
            break;
        case 'w':
            APPEND(RL_ANSI("\033[1;34m"));
            APPEND(display_cwd);
            APPEND(RL_ANSI("\033[0m"));
            break;
        case 'g':
            if (has_git) {
                snprintf(tmp, sizeof(tmp), " (%s)", branch);
                APPEND(RL_ANSI("\033[0;35m"));
                APPEND(tmp);
                APPEND(RL_ANSI("\033[0m"));
            }
            break;
        case 'j':
            if (jcount > 0) {
                snprintf(tmp, sizeof(tmp), " [%d]", jcount);
                APPEND(RL_ANSI("\033[0;33m"));
                APPEND(tmp);
                APPEND(RL_ANSI("\033[0m"));
            }
            break;
        case 'e':
            if (envdepth > 0) {
                snprintf(tmp, sizeof(tmp), " {%d}", envdepth);
                APPEND(RL_ANSI("\033[0;36m"));
                APPEND(tmp);
                APPEND(RL_ANSI("\033[0m"));
            }
            break;
        case 's':
            if (last_status)
                APPEND(RL_ANSI("\033[1;31m") "$" RL_ANSI("\033[0m"));
            else
                APPEND(RL_ANSI("\033[1;32m") "$" RL_ANSI("\033[0m"));
            break;
        case '%':
            buf[bi++] = '%';
            break;
        default:
            buf[bi++] = '%';
            if (bi < bmax) buf[bi++] = fmt[i];
            break;
        }
    }
    buf[bi] = '\0';

#undef APPEND
#undef RL_ANSI

    return strdup(buf);
}
