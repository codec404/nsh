#include "shell.h"

/* ── PATH resolution ────────────────────────────────────────────────────── */

static int resolve_path(const char *cmd, char *out)
{
    if (cmd[0] == '/' || cmd[0] == '.') {
        if (access(cmd, X_OK) == 0) {
            strncpy(out, cmd, PATH_MAX - 1);
            out[PATH_MAX - 1] = '\0';
            return 1;
        }
        return 0;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char path_copy[PATH_MAX * 4];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir) {
        snprintf(out, PATH_MAX, "%s/%s", dir, cmd);
        if (access(out, X_OK) == 0)
            return 1;
        dir = strtok(NULL, ":");
    }
    return 0;
}

/* ── redirection ────────────────────────────────────────────────────────── */

static int apply_redirects(const Cmd *cmd)
{
    if (cmd->redir_in) {
        int fd = open(cmd->redir_in, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "nsh: %s: %s\n", cmd->redir_in, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    if (cmd->redir_out) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
        int fd = open(cmd->redir_out, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "nsh: %s: %s\n", cmd->redir_out, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}

/* ── child: reset signals and exec ─────────────────────────────────────── */

static void child_reset_signals(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static void exec_cmd(const Cmd *cmd)
{
    if (is_builtin(cmd->argv[0])) {
        int status = run_builtin(cmd->argv, cmd->argc);
        _exit(status);
    }

    char resolved[PATH_MAX];
    if (!resolve_path(cmd->argv[0], resolved)) {
        fprintf(stderr, "nsh: %s: command not found\n", cmd->argv[0]);
        suggest_command_not_found(cmd->argv[0]);
        _exit(127);
    }

    execvp(resolved, cmd->argv);
    int save_err = errno;
    fprintf(stderr, "nsh: %s: %s\n", cmd->argv[0], strerror(save_err));
    suggest_on_error(resolved, save_err);
    _exit(127);
}

/* ── shared wait loop ───────────────────────────────────────────────────── */

static int drain_pgid(pid_t pgid, pid_t *pids, int npids,
                      int *out_stopped, int *out_stop_sig)
{
    int result    = 0;
    int ndone     = 0;
    *out_stopped  = 0;
    *out_stop_sig = 0;

    while (ndone < npids) {
        int   wstatus;
        pid_t pid = waitpid(-pgid, &wstatus, WUNTRACED);

        if (pid < 0) {
            if (errno == EINTR)  continue;
            if (errno == ECHILD) break;
            perror("waitpid");
            break;
        }

        if (WIFSTOPPED(wstatus)) {
            *out_stopped  = 1;
            *out_stop_sig = WSTOPSIG(wstatus);
            ndone++;
            result = 128 + *out_stop_sig;
        } else if (WIFEXITED(wstatus)) {
            ndone++;
            if (pids[npids - 1] == pid)
                result = WEXITSTATUS(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
            ndone++;
            if (pids[npids - 1] == pid)
                result = 128 + WTERMSIG(wstatus);
        }
    }
    return result;
}

/* ── foreground wait ────────────────────────────────────────────────────── */

static int wait_new_fg(pid_t pgid, pid_t *pids, int npids, const char *cmdline)
{
    int stopped, stop_sig;
    int result = drain_pgid(pgid, pids, npids, &stopped, &stop_sig);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (stopped) {
        Job *j = jobs_add(pgid, pids, npids, cmdline);
        if (j) {
            jobs_set_state(j, JOB_STOPPED);
            fprintf(stderr, "\n[%d]+  Stopped\t\t%s\n",
                    j->num, cmdline ? cmdline : "");
        }
    }
    return result;
}

int wait_foreground(Job *j)
{
    int stopped, stop_sig;
    int result = drain_pgid(j->pgid, j->pids, j->npids, &stopped, &stop_sig);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (stopped) {
        jobs_set_state(j, JOB_STOPPED);
        fprintf(stderr, "\n[%d]+  Stopped\t\t%s\n",
                j->num, j->cmdline ? j->cmdline : "");
    } else {
        jobs_set_state(j, JOB_DONE);
        j->notified = 1;
    }
    return result;
}

/* ── external (fork/exec) pipeline ─────────────────────────────────────── */

/*
 * execute_external_pipeline — the Phase 1/2/3 fork+exec engine.
 * Takes an optional override_stdin fd; if >= 0, the first child's stdin
 * is replaced with it (used when serialising a table into an external cmd).
 */
static int execute_external_pipeline(Pipeline *p, int override_stdin)
{
    int npipes = p->ncmds - 1;

    int (*pipes)[2] = NULL;
    if (npipes > 0) {
        pipes = malloc(npipes * sizeof(int[2]));
        if (!pipes) { perror("malloc"); return 1; }
        for (int i = 0; i < npipes; i++) {
            if (pipe(pipes[i]) < 0) {
                perror("pipe");
                for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
                free(pipes);
                return 1;
            }
        }
    }

    pid_t *pids = malloc(p->ncmds * sizeof(pid_t));
    if (!pids) {
        perror("malloc");
        if (pipes) {
            for (int i = 0; i < npipes; i++) { close(pipes[i][0]); close(pipes[i][1]); }
            free(pipes);
        }
        return 1;
    }

    pid_t pgid = 0;

    for (int i = 0; i < p->ncmds; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            for (int j = 0; j < i; j++) waitpid(pids[j], NULL, 0);
            if (pipes) {
                for (int j = 0; j < npipes; j++) { close(pipes[j][0]); close(pipes[j][1]); }
                free(pipes);
            }
            free(pids);
            return 1;
        }

        if (pids[i] == 0) {
            /* ── child ── */
            pid_t my_pgid = (i == 0) ? getpid() : pgid;
            setpgid(0, my_pgid);
            if (!p->background && i == 0)
                tcsetpgrp(STDIN_FILENO, my_pgid);

            child_reset_signals();

            /* stdin: use override (table serialiser pipe) or regular pipe */
            if (i == 0 && override_stdin >= 0) {
                if (dup2(override_stdin, STDIN_FILENO) < 0) { perror("dup2"); _exit(1); }
                close(override_stdin);
            } else if (i > 0 && pipes) {
                if (dup2(pipes[i-1][0], STDIN_FILENO) < 0) { perror("dup2"); _exit(1); }
            }

            if (i < npipes && pipes)
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }

            if (pipes)
                for (int j = 0; j < npipes; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            if (override_stdin >= 0) close(override_stdin);

            if (apply_redirects(&p->cmds[i]) < 0) _exit(1);
            exec_cmd(&p->cmds[i]);
        }

        /* ── parent ── */
        if (i == 0) { pgid = pids[0]; setpgid(pids[0], pgid); }
        else          setpgid(pids[i], pgid);

        if (i > 0 && pipes)      close(pipes[i-1][0]);
        if (i < npipes && pipes) close(pipes[i][1]);
    }

    /* close the override fd in parent now that all children are forked */
    if (override_stdin >= 0) close(override_stdin);
    free(pipes);

    if (p->background) {
        last_bg_pgid = pgid;
        Job *j = jobs_add(pgid, pids, p->ncmds, p->cmdline);
        free(pids);
        if (j) { fprintf(stderr, "[%d] %d\n", j->num, (int)pgid); return 0; }
        return 1;
    }

    tcsetpgrp(STDIN_FILENO, pgid);
    int result = wait_new_fg(pgid, pids, p->ncmds, p->cmdline);
    free(pids);
    return result;
}

/* ── table pipeline ─────────────────────────────────────────────────────── */

/*
 * execute_table_pipeline — run the first `table_len` commands as in-memory
 * table operations, then either render (pure table) or serialize and hand
 * off to the remaining external commands (mixed pipeline).
 *
 * --json anywhere in the last table command's argv switches JSON output.
 */
static int execute_table_pipeline(Pipeline *p, int table_len)
{
    /* detect --json in last table cmd and build filtered argv */
    int use_json = 0;

    NshTable *tbl = NULL;
    for (int i = 0; i < table_len; i++) {
        Cmd *c = &p->cmds[i];

        /* build filtered argv: strip --json */
        char *fargv[MAX_ARGS + 1];
        int   fargc = 0;
        for (int j = 0; j < c->argc; j++) {
            if (strcmp(c->argv[j], "--json") == 0) { use_json = 1; continue; }
            fargv[fargc++] = c->argv[j];
        }
        fargv[fargc] = NULL;

        NshTable *next = run_table_cmd(fargv, fargc, tbl);
        table_free(tbl);
        tbl = next;
        if (!tbl) return 1;
    }

    /* ── pure table pipeline: render and return ── */
    if (table_len == p->ncmds) {
        Cmd *last = &p->cmds[table_len - 1];
        FILE *out = stdout;
        int   close_out = 0;

        if (last->redir_out) {
            int flags = O_WRONLY | O_CREAT | (last->append_out ? O_APPEND : O_TRUNC);
            int fd = open(last->redir_out, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "nsh: %s: %s\n", last->redir_out, strerror(errno));
                table_free(tbl);
                return 1;
            }
            out = fdopen(fd, "w");
            close_out = 1;
        }

        if (use_json) table_print_json(tbl, out);
        else          table_print(tbl, out);

        if (close_out) fclose(out);
        table_free(tbl);
        return 0;
    }

    /*
     * ── mixed pipeline: serialize table → pipe → external commands ──
     *
     * Fork a child writer that serialises the table to the write end of a
     * pipe.  The parent passes the read end to execute_external_pipeline
     * as stdin for the first external command.
     */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); table_free(tbl); return 1; }

    pid_t writer = fork();
    if (writer < 0) {
        perror("fork"); close(pipefd[0]); close(pipefd[1]); table_free(tbl); return 1;
    }

    if (writer == 0) {
        close(pipefd[0]);
        child_reset_signals();
        FILE *f = fdopen(pipefd[1], "w");
        if (f) { table_serialize(tbl, f); fclose(f); }
        else   { close(pipefd[1]); }
        table_free(tbl);
        _exit(0);
    }

    close(pipefd[1]);
    table_free(tbl);

    /* build a sub-Pipeline from the remaining (external) commands */
    Pipeline sub;
    sub.cmds       = p->cmds + table_len;
    sub.ncmds      = p->ncmds - table_len;
    sub.background = p->background;
    sub.cmdline    = p->cmdline;

    /* pipefd[0] becomes the override_stdin for the first external child;
     * execute_external_pipeline closes it after all children are forked */
    int result = execute_external_pipeline(&sub, pipefd[0]);

    waitpid(writer, NULL, 0);
    return result;
}

/* ── public dispatcher ──────────────────────────────────────────────────── */

int execute_pipeline(Pipeline *p)
{
    /* count leading table builtins */
    int table_len = 0;
    while (table_len < p->ncmds &&
           is_table_builtin(p->cmds[table_len].argv[0]))
        table_len++;

    if (table_len > 0)
        return execute_table_pipeline(p, table_len);

    /* fast path: single foreground builtin with no redirections */
    if (p->ncmds == 1 && !p->background) {
        Cmd *c = &p->cmds[0];
        if (is_builtin(c->argv[0]) && !c->redir_in && !c->redir_out)
            return run_builtin(c->argv, c->argc);
    }

    return execute_external_pipeline(p, -1);
}
