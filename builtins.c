#include "shell.h"

static const char *BUILTINS[] = {
    "cd", "exit", "pwd", "jobs", "fg", "bg", "replay", "shellenv", "clear",
    "break", "continue", "return", "set", "unset",
    NULL
};

int is_builtin(const char *cmd)
{
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(cmd, BUILTINS[i]) == 0)
            return 1;
    return 0;
}

/* ── cd ─────────────────────────────────────────────────────────────────── */

static int builtin_cd(char **argv, int argc)
{
    char target[PATH_MAX];

    if (argc < 2) {
        const char *home = getenv("HOME");
        if (!home) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
        strncpy(target, home, PATH_MAX - 1);
        target[PATH_MAX - 1] = '\0';
    } else if (argc == 2 && strcmp(argv[1], "-") == 0) {
        const char *oldpwd = getenv("OLDPWD");
        if (!oldpwd) { fprintf(stderr, "cd: OLDPWD not set\n"); return 1; }
        strncpy(target, oldpwd, PATH_MAX - 1);
        target[PATH_MAX - 1] = '\0';
        printf("%s\n", target);
    } else {
        strncpy(target, argv[1], PATH_MAX - 1);
        target[PATH_MAX - 1] = '\0';
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        setenv("OLDPWD", cwd, 1);

    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if (getcwd(cwd, sizeof(cwd))) {
        setenv("PWD", cwd, 1);
        shellenv_cd_hook(cwd);   /* apply/unapply .shellenv for new directory */
    }

    return 0;
}

/* ── pwd ────────────────────────────────────────────────────────────────── */

static int builtin_pwd(void)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("pwd"); return 1; }
    printf("%s\n", cwd);
    return 0;
}

/* ── exit ───────────────────────────────────────────────────────────────── */

static int builtin_exit(char **argv, int argc)
{
    int code = last_status;
    if (argc >= 2)
        code = atoi(argv[1]);
    exit(code);
}

/* ── jobs ───────────────────────────────────────────────────────────────── */

static int builtin_jobs(void)
{
    jobs_print_all();
    return 0;
}

/* ── fg ─────────────────────────────────────────────────────────────────── */

/*
 * fg [%N]
 *
 * Brings a stopped or background job to the foreground.
 * Sends SIGCONT to the job's process group, gives it the terminal,
 * then waits for it as if it were a foreground job.
 */
static int builtin_fg(char **argv, int argc)
{
    Job *j = NULL;

    if (argc >= 2) {
        const char *spec = argv[1];
        if (spec[0] == '%') spec++;     /* %1 → 1 */
        j = jobs_find_num(atoi(spec));
    } else {
        j = jobs_last();
    }

    if (!j) {
        fprintf(stderr, "fg: no current job\n");
        return 1;
    }

    fprintf(stderr, "%s\n", j->cmdline ? j->cmdline : "");

    /* give the terminal to the job's process group */
    tcsetpgrp(STDIN_FILENO, j->pgid);

    /* resume if stopped */
    if (j->state == JOB_STOPPED) {
        j->ndone = 0;   /* reset so wait_foreground counts fresh */
        jobs_set_state(j, JOB_RUNNING);
        if (killpg(j->pgid, SIGCONT) < 0) {
            perror("fg: killpg SIGCONT");
            tcsetpgrp(STDIN_FILENO, shell_pgid);
            return 1;
        }
    }

    int status = wait_foreground(j);

    if (j->state == JOB_DONE)
        j->notified = 1;    /* reap_jobs will remove it */

    return status;
}

/* ── bg ─────────────────────────────────────────────────────────────────── */

/*
 * bg [%N]
 *
 * Resumes a stopped job in the background.
 * Does NOT give it the terminal — if it tries to read stdin it will stop
 * again with SIGTTIN.
 */
static int builtin_bg(char **argv, int argc)
{
    Job *j = NULL;

    if (argc >= 2) {
        const char *spec = argv[1];
        if (spec[0] == '%') spec++;
        j = jobs_find_num(atoi(spec));
    } else {
        j = jobs_last();
    }

    if (!j) {
        fprintf(stderr, "bg: no current job\n");
        return 1;
    }

    if (j->state != JOB_STOPPED) {
        fprintf(stderr, "bg: job %d is not stopped\n", j->num);
        return 1;
    }

    j->ndone = 0;
    jobs_set_state(j, JOB_RUNNING);

    fprintf(stderr, "[%d]+ %s &\n", j->num, j->cmdline ? j->cmdline : "");

    if (killpg(j->pgid, SIGCONT) < 0) {
        perror("bg: killpg SIGCONT");
        return 1;
    }

    return 0;
}

/* ── replay ─────────────────────────────────────────────────────────────── */

/*
 * replay [--session <id>] [--dry-run]
 *
 * Replays every command from a history session one by one.
 * With no --session flag, uses the current session (this shell's PID).
 * --dry-run prints commands without executing them.
 * For each command the user is shown the cmdline and prompted to run it
 * (Enter = yes, n = skip, q = quit).
 */
static int builtin_replay(char **argv, int argc)
{
    int64_t session_id = (int64_t)getpid();   /* default: current session */
    int dry_run = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_id = (int64_t)atoll(argv[++i]);
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            session_id = (int64_t)atoll(argv[i]);
        }
    }

    int n = 0;
    char **cmds = hist_session_cmds(session_id, &n);
    if (!cmds || n == 0) {
        fprintf(stderr, "replay: no commands found for session %lld\n",
                (long long)session_id);
        free(cmds);
        return 1;
    }

    printf("Replaying %d commands from session %lld\n\n",
           n, (long long)session_id);

    int status = 0;
    for (int i = 0; i < n; i++) {
        printf("\033[1;33m[%d/%d]\033[0m  %s\n", i + 1, n, cmds[i]);

        if (dry_run) { free(cmds[i]); continue; }

        /* prompt: Enter=run, n=skip, q=quit */
        printf("  Run? [Y/n/q] ");
        fflush(stdout);

        char ch = 0;
        int  c;
        /* read first non-whitespace char */
        while ((c = getchar()) != EOF && (c == ' ' || c == '\t')) {}
        if (c != EOF) {
            ch = (char)c;
            /* drain rest of line */
            while (c != EOF && c != '\n') c = getchar();
        }

        if (ch == 'q' || ch == 'Q') {
            printf("  quit.\n");
            free(cmds[i]);
            i++;    /* free remaining */
            for (; i < n; i++) free(cmds[i]);
            break;
        }
        if (ch == 'n' || ch == 'N') {
            printf("  skipped.\n");
            free(cmds[i]);
            continue;
        }

        /* execute */
        int nt = 0;
        char *copy = strdup(cmds[i]);
        char **toks = tokenize(copy, &nt);
        free(copy);

        if (toks && nt > 0) {
            Pipeline *p = parse_pipeline(toks, nt, cmds[i]);
            free_tokens(toks);
            if (p) {
                status = execute_pipeline(p);
                free_pipeline(p);
            }
        } else {
            free_tokens(toks);
        }

        free(cmds[i]);
    }

    free(cmds);
    return status;
}

/* ── shellenv ───────────────────────────────────────────────────────────── */

/*
 * shellenv [subcommand] [args]
 *
 *   shellenv              — show active .shellenv files and their vars
 *   shellenv diff [dir]   — show active vars; with dir, show what would change
 *   shellenv link [src]   — symlink src as .shellenv in cwd; no src = template
 *   shellenv reload       — force reload all active .shellenv files
 */
static int builtin_shellenv(char **argv, int argc)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        shellenv_show();
        return 0;
    }

    if (strcmp(argv[1], "diff") == 0) {
        shellenv_diff(argc >= 3 ? argv[2] : NULL);
        return 0;
    }

    if (strcmp(argv[1], "link") == 0) {
        return shellenv_link(argc >= 3 ? argv[2] : NULL);
    }

    if (strcmp(argv[1], "reload") == 0) {
        /* force a reload by resetting the kqueue check */
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            shellenv_shutdown();
            shellenv_init();
        }
        return 0;
    }

    fprintf(stderr,
            "shellenv: unknown subcommand '%s'\n"
            "usage: shellenv [show|diff [dir]|link [src]|reload]\n",
            argv[1]);
    return 1;
}

/* ── clear ──────────────────────────────────────────────────────────────── */

/*
 * Run the real /usr/bin/clear to erase the screen, then tell readline
 * to redraw from the top.  Without this, our DEC cursor save/restore
 * (\0337/\0338) in the predictor would jump back to the now-gone old
 * cursor position and corrupt the display.
 */
static int builtin_clear(void)
{
    /* \033[2J  — erase entire screen
     * \033[3J  — erase scrollback buffer
     * \033[H   — move cursor to top-left
     * The next readline() call in the REPL will then draw the prompt there. */
    write(STDOUT_FILENO, "\033[2J\033[3J\033[H", 12);
    return 0;
}

/* ── dispatcher ─────────────────────────────────────────────────────────── */

static int builtin_break(void)    { nsh_unwind = UNWIND_BREAK;    return 0; }
static int builtin_continue(void) { nsh_unwind = UNWIND_CONTINUE; return 0; }

static int builtin_return(char **argv, int argc)
{
    nsh_retval = (argc > 1) ? atoi(argv[1]) : last_status;
    nsh_unwind = UNWIND_RETURN;
    return nsh_retval;
}

static int builtin_set(char **argv, int argc)
{
    if (argc < 3) { fprintf(stderr, "usage: set name value\n"); return 1; }
    setenv(argv[1], argv[2], 1);
    return 0;
}

static int builtin_unset(char **argv, int argc)
{
    if (argc < 2) { fprintf(stderr, "usage: unset name\n"); return 1; }
    unsetenv(argv[1]);
    return 0;
}

int run_builtin(char **argv, int argc)
{
    if (strcmp(argv[0], "cd")       == 0) return builtin_cd(argv, argc);
    if (strcmp(argv[0], "pwd")      == 0) return builtin_pwd();
    if (strcmp(argv[0], "exit")     == 0) return builtin_exit(argv, argc);
    if (strcmp(argv[0], "jobs")     == 0) return builtin_jobs();
    if (strcmp(argv[0], "fg")       == 0) return builtin_fg(argv, argc);
    if (strcmp(argv[0], "bg")       == 0) return builtin_bg(argv, argc);
    if (strcmp(argv[0], "replay")   == 0) return builtin_replay(argv, argc);
    if (strcmp(argv[0], "shellenv") == 0) return builtin_shellenv(argv, argc);
    if (strcmp(argv[0], "clear")    == 0) return builtin_clear();
    if (strcmp(argv[0], "break")    == 0) return builtin_break();
    if (strcmp(argv[0], "continue") == 0) return builtin_continue();
    if (strcmp(argv[0], "return")   == 0) return builtin_return(argv, argc);
    if (strcmp(argv[0], "set")      == 0) return builtin_set(argv, argc);
    if (strcmp(argv[0], "unset")    == 0) return builtin_unset(argv, argc);
    return 127;
}
