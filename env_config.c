#include "shell.h"
#include <sys/event.h>
#include <dirent.h>
#include <time.h>

/*
 * .shellenv — per-directory environment configuration
 *
 * File format (lines processed top to bottom):
 *
 *   # comment
 *   [env]
 *   KEY=value               # setenv on enter, unsetenv on exit
 *   KEY=$EXISTING:new_part  # value is expanded at load time
 *
 *   [on_enter]
 *   echo "entered $(pwd)"   # arbitrary shell commands, run via nsh pipeline
 *
 *   [on_exit]
 *   echo "leaving"
 *
 * Sections can appear multiple times; lines before the first section header
 * are ignored.
 *
 * The env stack allows nesting: entering /a/b/proj applies proj's .shellenv
 * on top of /a/b's.  Leaving /a/b/proj unapplies exactly proj's vars.
 */

/* ── data structures ────────────────────────────────────────────────────── */

#define MAX_SHELLENV_VARS  128
#define MAX_SHELLENV_HOOKS 64
#define MAX_STACK          32

typedef struct {
    char *key;
    char *value;     /* value after expansion */
    char *old_value; /* previous value, or NULL if was unset — for unapply */
    int   was_set;   /* 1 if key existed before we set it */
} EnvVar;

typedef struct {
    char *cmdline;   /* one hook line */
} HookLine;

typedef struct {
    char     path[PATH_MAX];        /* directory this config belongs to */
    EnvVar   vars[MAX_SHELLENV_VARS];
    int      nvars;
    HookLine on_enter[MAX_SHELLENV_HOOKS];
    int      n_on_enter;
    HookLine on_exit[MAX_SHELLENV_HOOKS];
    int      n_on_exit;
    int      applied;               /* 1 if env vars are currently active */
} ShellEnv;

/* directory stack: stack[0] = outermost applied dir */
static ShellEnv *stack[MAX_STACK];
static int       stack_depth = 0;

int shellenv_depth(void) { return stack_depth; }

/* kqueue fd for watching .shellenv files */
static int kq_fd   = -1;
static int *kq_wds = NULL;  /* per-stack-entry watch descriptor (kqueue uses file fds) */
static int *kq_fds = NULL;  /* open file descriptors being watched */

/* ── parser ─────────────────────────────────────────────────────────────── */

typedef enum { SEC_NONE, SEC_ENV, SEC_ON_ENTER, SEC_ON_EXIT } Section;

static void trim_right(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t'))
        s[--n] = '\0';
}

/*
 * expand_value — expand $VAR references in a value string at parse time.
 * Returns a malloc'd string; caller must free.
 */
static char *expand_value(const char *val)
{
    char out[PATH_MAX * 4] = "";
    const char *p = val;

    while (*p) {
        if (*p == '$') {
            p++;
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            if (p > start) {
                char name[256];
                size_t n = (size_t)(p - start);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, start, n);
                name[n] = '\0';
                const char *ev = getenv(name);
                if (ev) strncat(out, ev, sizeof(out) - strlen(out) - 1);
            }
        } else {
            char tmp[2] = { *p++, '\0' };
            strncat(out, tmp, sizeof(out) - strlen(out) - 1);
        }
    }
    return strdup(out);
}

static ShellEnv *shellenv_parse(const char *dir)
{
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/.shellenv", dir);

    FILE *f = fopen(filepath, "r");
    if (!f) return NULL;

    ShellEnv *se = calloc(1, sizeof(ShellEnv));
    if (!se) { fclose(f); return NULL; }
    strncpy(se->path, dir, PATH_MAX - 1);

    char line[NSH_MAX_INPUT];
    Section sec = SEC_NONE;

    while (fgets(line, sizeof(line), f)) {
        trim_right(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        if (strcmp(line, "[env]")      == 0) { sec = SEC_ENV;      continue; }
        if (strcmp(line, "[on_enter]") == 0) { sec = SEC_ON_ENTER; continue; }
        if (strcmp(line, "[on_exit]")  == 0) { sec = SEC_ON_EXIT;  continue; }

        switch (sec) {
        case SEC_ENV: {
            char *eq = strchr(line, '=');
            if (!eq) break;
            if (se->nvars >= MAX_SHELLENV_VARS) break;
            *eq = '\0';
            char *key = line, *raw_val = eq + 1;
            /* strip leading whitespace from key */
            while (*key == ' ' || *key == '\t') key++;
            EnvVar *v   = &se->vars[se->nvars++];
            v->key      = strdup(key);
            v->value    = expand_value(raw_val);
            v->old_value = NULL;
            v->was_set  = 0;
            break;
        }
        case SEC_ON_ENTER:
            if (se->n_on_enter < MAX_SHELLENV_HOOKS) {
                /* skip leading whitespace */
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                se->on_enter[se->n_on_enter++].cmdline = strdup(s);
            }
            break;
        case SEC_ON_EXIT:
            if (se->n_on_exit < MAX_SHELLENV_HOOKS) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                se->on_exit[se->n_on_exit++].cmdline = strdup(s);
            }
            break;
        default:
            break;
        }
    }

    fclose(f);
    return se;
}

/* ── free ───────────────────────────────────────────────────────────────── */

static void shellenv_free(ShellEnv *se)
{
    if (!se) return;
    for (int i = 0; i < se->nvars; i++) {
        free(se->vars[i].key);
        free(se->vars[i].value);
        free(se->vars[i].old_value);
    }
    for (int i = 0; i < se->n_on_enter; i++) free(se->on_enter[i].cmdline);
    for (int i = 0; i < se->n_on_exit;  i++) free(se->on_exit[i].cmdline);
    free(se);
}

/* ── hook runner ────────────────────────────────────────────────────────── */

static void run_hooks(HookLine *hooks, int n)
{
    for (int i = 0; i < n; i++) {
        const char *cmd = hooks[i].cmdline;
        if (!cmd || !*cmd) continue;

        char mutable[NSH_MAX_INPUT];
        strncpy(mutable, cmd, sizeof(mutable) - 1);
        mutable[sizeof(mutable) - 1] = '\0';

        int nt = 0;
        char **toks = tokenize(mutable, &nt);
        if (!toks || nt == 0) { free_tokens(toks); continue; }

        Pipeline *p = parse_pipeline(toks, nt, cmd);
        free_tokens(toks);
        if (p) { execute_pipeline(p); free_pipeline(p); }
    }
}

/* ── apply / unapply ────────────────────────────────────────────────────── */

static void shellenv_apply(ShellEnv *se)
{
    if (!se || se->applied) return;

    for (int i = 0; i < se->nvars; i++) {
        EnvVar *v   = &se->vars[i];
        const char *cur = getenv(v->key);
        v->was_set  = (cur != NULL);
        v->old_value = cur ? strdup(cur) : NULL;
        setenv(v->key, v->value, 1);
    }

    se->applied = 1;
    run_hooks(se->on_enter, se->n_on_enter);
}

static void shellenv_unapply(ShellEnv *se)
{
    if (!se || !se->applied) return;

    /* run exit hooks before restoring env */
    run_hooks(se->on_exit, se->n_on_exit);

    /* restore in reverse order so nested overrides unwind cleanly */
    for (int i = se->nvars - 1; i >= 0; i--) {
        EnvVar *v = &se->vars[i];
        if (v->was_set)
            setenv(v->key, v->old_value, 1);
        else
            unsetenv(v->key);
        free(v->old_value);
        v->old_value = NULL;
        v->was_set   = 0;
    }

    se->applied = 0;
}

/* ── kqueue file watcher ────────────────────────────────────────────────── */

void shellenv_watch_init(void)
{
    kq_fd = kqueue();
    if (kq_fd < 0) {
        perror("kqueue");
        kq_fd = -1;
    }
    kq_wds = calloc(MAX_STACK, sizeof(int));
    kq_fds = calloc(MAX_STACK, sizeof(int));
    for (int i = 0; i < MAX_STACK; i++) kq_fds[i] = -1;
}

static void watch_add(int slot, const char *dir)
{
    if (kq_fd < 0) return;

    /* close previous watch on this slot */
    if (kq_fds[slot] >= 0) { close(kq_fds[slot]); kq_fds[slot] = -1; }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.shellenv", dir);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;   /* no .shellenv to watch */

    struct kevent ke;
    EV_SET(&ke, (uintptr_t)fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE,
           0, (void *)(intptr_t)slot);

    kevent(kq_fd, &ke, 1, NULL, 0, NULL);
    kq_fds[slot] = fd;
}

static void watch_remove(int slot)
{
    if (kq_fd < 0 || kq_fds[slot] < 0) return;
    close(kq_fds[slot]);
    kq_fds[slot] = -1;
}

/*
 * shellenv_check_reload — called from the REPL before each prompt.
 * Drains kqueue events and hot-reloads changed .shellenv files.
 */
void shellenv_check_reload(void)
{
    if (kq_fd < 0) return;

    struct kevent events[8];
    struct timespec timeout = { 0, 0 };   /* non-blocking */
    int n = kevent(kq_fd, NULL, 0, events, 8, &timeout);

    for (int i = 0; i < n; i++) {
        int slot = (int)(intptr_t)events[i].udata;
        if (slot < 0 || slot >= stack_depth) continue;

        ShellEnv *old_se = stack[slot];
        if (!old_se) continue;

        char dir[PATH_MAX];
        strncpy(dir, old_se->path, PATH_MAX - 1);

        fprintf(stderr, "\nnsh: .shellenv changed in %s — reloading\n", dir);

        /* unapply old, parse new, apply new */
        shellenv_unapply(old_se);
        shellenv_free(old_se);

        watch_remove(slot);

        ShellEnv *new_se = shellenv_parse(dir);
        stack[slot] = new_se;

        if (new_se) {
            shellenv_apply(new_se);
            watch_add(slot, dir);
        }
    }
}

/* ── cd hook ────────────────────────────────────────────────────────────── */

/*
 * shellenv_cd_hook — called by builtin_cd AFTER chdir() succeeds.
 *
 * Algorithm:
 *   1. Walk the stack top-down. Unapply (and pop) any entry whose path
 *      is no longer a prefix of the new cwd.
 *   2. Walk from the current stack top toward the new cwd, looking for
 *      a .shellenv in each intermediate directory that isn't already applied.
 *
 * This handles both descending (a → a/b/c) and ascending (a/b/c → a).
 */
void shellenv_cd_hook(const char *new_cwd)
{
    /* 1. Unapply entries that are no longer ancestors of new_cwd */
    while (stack_depth > 0) {
        ShellEnv *top = stack[stack_depth - 1];
        size_t plen = strlen(top->path);

        int still_under = (strncmp(new_cwd, top->path, plen) == 0 &&
                           (new_cwd[plen] == '/' || new_cwd[plen] == '\0'));
        if (still_under) break;

        shellenv_unapply(top);
        watch_remove(stack_depth - 1);
        shellenv_free(top);
        stack[--stack_depth] = NULL;
    }

    /* 2. Find the deepest already-applied ancestor */
    const char *base;
    if (stack_depth > 0)
        base = stack[stack_depth - 1]->path;
    else
        base = "/";

    /*
     * Collect path components between base and new_cwd that we haven't
     * checked yet, then apply each one's .shellenv (if it exists) in order.
     */
    char seg[PATH_MAX];
    strncpy(seg, new_cwd, PATH_MAX - 1);

    /* build a list of dirs to check from shallowest to deepest */
    char *dirs[PATH_MAX / 2];
    int   ndirs = 0;

    char tmp[PATH_MAX];
    strncpy(tmp, new_cwd, PATH_MAX - 1);

    while (1) {
        /* only check dirs deeper than base */
        if (strcmp(tmp, base) == 0 || strlen(tmp) <= strlen(base)) break;

        dirs[ndirs++] = strdup(tmp);

        /* go up one level */
        char *slash = strrchr(tmp, '/');
        if (!slash) break;
        if (slash == tmp) { tmp[1] = '\0'; }
        else              { *slash = '\0'; }

        if (ndirs >= (int)(PATH_MAX / 2)) break;
    }

    /* reverse: apply from shallowest to deepest */
    for (int i = ndirs - 1; i >= 0; i--) {
        if (stack_depth < MAX_STACK) {
            ShellEnv *se = shellenv_parse(dirs[i]);
            if (se) {
                stack[stack_depth] = se;
                shellenv_apply(se);
                watch_add(stack_depth, dirs[i]);
                stack_depth++;
            }
        }
        free(dirs[i]);
    }
}

/* ── init / shutdown ────────────────────────────────────────────────────── */

void shellenv_init(void)
{
    shellenv_watch_init();

    /* apply .shellenv for the starting directory */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        shellenv_cd_hook(cwd);
}

void shellenv_shutdown(void)
{
    /* unapply in reverse order (deepest first) */
    for (int i = stack_depth - 1; i >= 0; i--) {
        shellenv_unapply(stack[i]);
        watch_remove(i);
        shellenv_free(stack[i]);
        stack[i] = NULL;
    }
    stack_depth = 0;

    if (kq_fd >= 0) { close(kq_fd); kq_fd = -1; }
    free(kq_wds); kq_wds = NULL;
    free(kq_fds); kq_fds = NULL;
}

/* ── env diff ───────────────────────────────────────────────────────────── */

/*
 * shellenv_diff — show what env vars are currently active from .shellenv
 * files, and optionally what WOULD change if we cd'd to `target_dir`.
 */
void shellenv_diff(const char *target_dir)
{
    /* show currently active vars */
    printf("\033[1mActive .shellenv vars:\033[0m\n");
    if (stack_depth == 0) {
        printf("  (none)\n");
    } else {
        for (int s = 0; s < stack_depth; s++) {
            ShellEnv *se = stack[s];
            if (!se || !se->applied || se->nvars == 0) continue;
            printf("  \033[1;34m%s/.shellenv\033[0m\n", se->path);
            for (int i = 0; i < se->nvars; i++) {
                EnvVar *v = &se->vars[i];
                printf("    %-20s = %s\n", v->key, v->value);
            }
        }
    }

    if (!target_dir) return;

    /* what WOULD change if we cd'd to target_dir */
    printf("\n\033[1mWould change on cd to %s:\033[0m\n", target_dir);

    ShellEnv *preview = shellenv_parse(target_dir);
    if (!preview || preview->nvars == 0) {
        printf("  (no .shellenv or no [env] section)\n");
        shellenv_free(preview);
        return;
    }

    for (int i = 0; i < preview->nvars; i++) {
        EnvVar *v = &preview->vars[i];
        const char *cur = getenv(v->key);

        if (!cur) {
            printf("  \033[1;32m+ %-20s = %s\033[0m  (new)\n",
                   v->key, v->value);
        } else if (strcmp(cur, v->value) != 0) {
            printf("  \033[1;33m~ %-20s   %s  →  %s\033[0m\n",
                   v->key, cur, v->value);
        } else {
            printf("    %-20s = %s  (unchanged)\n", v->key, v->value);
        }
    }
    shellenv_free(preview);
}

/* ── shellenv link ──────────────────────────────────────────────────────── */

/*
 * shellenv_link — create a .shellenv symlink in cwd pointing to `target`.
 * If target is NULL, creates an empty .shellenv template instead.
 */
int shellenv_link(const char *target)
{
    if (target) {
        /* resolve to absolute path */
        char real[PATH_MAX];
        if (!realpath(target, real)) {
            fprintf(stderr, "shellenv link: %s: %s\n", target, strerror(errno));
            return 1;
        }

        if (symlink(real, ".shellenv") < 0) {
            fprintf(stderr, "shellenv link: %s\n", strerror(errno));
            return 1;
        }
        printf("Linked .shellenv → %s\n", real);
    } else {
        /* create a template */
        FILE *f = fopen(".shellenv", "wx");   /* fail if exists */
        if (!f) {
            fprintf(stderr, "shellenv link: .shellenv already exists\n");
            return 1;
        }
        fputs("# nsh per-directory environment config\n"
              "# See: shellenv --help\n\n"
              "[env]\n"
              "# KEY=value\n\n"
              "[on_enter]\n"
              "# echo \"entered $(pwd)\"\n\n"
              "[on_exit]\n"
              "# echo \"leaving $(pwd)\"\n",
              f);
        fclose(f);
        printf("Created .shellenv template\n");
    }
    return 0;
}

/* ── show ───────────────────────────────────────────────────────────────── */

void shellenv_show(void)
{
    if (stack_depth == 0) {
        printf("No .shellenv files active.\n");
        return;
    }
    for (int s = 0; s < stack_depth; s++) {
        ShellEnv *se = stack[s];
        if (!se) continue;
        printf("\033[1;34m[%d] %s/.shellenv\033[0m  (%s)\n",
               s + 1, se->path, se->applied ? "applied" : "not applied");
        for (int i = 0; i < se->nvars; i++)
            printf("  %-20s = %s\n", se->vars[i].key, se->vars[i].value);
        if (se->n_on_enter)
            printf("  on_enter: %d hook(s)\n", se->n_on_enter);
        if (se->n_on_exit)
            printf("  on_exit:  %d hook(s)\n", se->n_on_exit);
    }
}
