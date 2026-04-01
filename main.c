#include "shell.h"
#include <time.h>

int                   last_status  = 0;
pid_t                 shell_pgid   = 0;
pid_t                 last_bg_pgid = 0;
volatile sig_atomic_t got_sigchld  = 0;

/* ── signal handlers ────────────────────────────────────────────────────── */

static void sigchld_handler(int sig)
{
    (void)sig;
    got_sigchld = 1;
}

static sigjmp_buf ctrl_c_jump;
static void sigint_handler(int sig)
{
    (void)sig;
    siglongjmp(ctrl_c_jump, 1);
}

/* ── wall-clock timer ───────────────────────────────────────────────────── */

static struct timespec timer_start;

static void timer_begin(void)
{
    clock_gettime(CLOCK_MONOTONIC, &timer_start);
}

static int64_t timer_elapsed_ms(void)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (int64_t)(end.tv_sec  - timer_start.tv_sec)  * 1000
         + (int64_t)(end.tv_nsec - timer_start.tv_nsec) / 1000000;
}

/* ── shell initialisation ───────────────────────────────────────────────── */

static void shell_init(void)
{
    config_load();
    hist_open();
    execctx_init(&g_ctx);

    if (!isatty(STDIN_FILENO))
        return;

    while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
        kill(-shell_pgid, SIGTTIN);

    shell_pgid = getpid();
    setpgid(0, 0);
    shell_pgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGINT,  sigint_handler);
    signal(SIGQUIT, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    jobs_init();
    shellenv_init();
    line_editor_init();
    hist_seed_interactive();
}

/* ── brace depth counter ─────────────────────────────────────────────────── */

/*
 * Count net open braces in a token array so the REPL knows whether to keep
 * reading more lines before parsing (multi-line blocks).
 */
static int brace_depth(char **toks, int n)
{
    int depth = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], "{") == 0) depth++;
        else if (strcmp(toks[i], "}") == 0) depth--;
    }
    return depth;
}

/* ── execute one logical line ────────────────────────────────────────────── */

static void run_input(const char *input, int interactive)
{
    int ntokens = 0;
    char **tokens = tokenize((char *)input, &ntokens);
    if (!tokens || ntokens == 0) { free_tokens(tokens); return; }

    AstNode *ast = parse_program(tokens, ntokens);
    free_tokens(tokens);
    if (!ast) { last_status = 1; return; }

    timer_begin();
    last_status = execute_ast(ast, &g_ctx);
    int64_t elapsed = timer_elapsed_ms();

    if (interactive && g_config.prompt_show_ms >= 0 &&
        elapsed >= (int64_t)g_config.prompt_show_ms)
        fprintf(stderr, "\033[0;90m  took %lldms\033[0m\n", (long long)elapsed);

    free_ast(ast);
}

/* ── REPL ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    shell_init();

    /* ── script file mode ── */
    if (argc >= 2) {
        int status = execute_script_file(argv[1]);
        execctx_free(&g_ctx);
        hist_close();
        return status;
    }

    /* ── interactive / pipe mode ── */
    while (1) {
        if (sigsetjmp(ctrl_c_jump, 1) != 0) {
            write(STDOUT_FILENO, "\n", 1);
            last_status = 1;
        }

        if (got_sigchld) { got_sigchld = 0; reap_jobs(); }
        shellenv_check_reload();

        /* ── accumulate a complete statement (handles multi-line blocks) ── */
        char  *accum  = NULL;   /* heap-allocated accumulation buffer */
        size_t accum_len = 0;
        int    depth  = 0;
        int    first_line = 1;

        for (;;) {
            char *line = NULL;

            if (isatty(STDIN_FILENO)) {
                char *prompt;
                if (first_line) {
                    prompt = make_prompt();
                } else {
                    /* continuation prompt */
                    prompt = strdup("... ");
                }
                line = line_editor_read(prompt);
                free(prompt);

                if (!line) {
                    if (line_editor_was_interrupted()) {
                        write(STDOUT_FILENO, "\n", 1);
                        line_editor_clear_interrupt();
                        last_status = 1;
                        free(accum);
                        accum = NULL;
                        goto next_iteration;
                    }
                    /* EOF */
                    printf("\n");
                    line_editor_shutdown();
                    shellenv_shutdown();
                    hist_close();
                    execctx_free(&g_ctx);
                    free(accum);
                    return last_status;
                }

                if (line_editor_was_interrupted()) {
                    write(STDOUT_FILENO, "\n", 1);
                    line_editor_clear_interrupt();
                    free(line);
                    last_status = 1;
                    free(accum);
                    accum = NULL;
                    goto next_iteration;
                }
            } else {
                /* non-interactive (pipe / stdin redirect) */
                static char buf[NSH_MAX_INPUT];
                if (!fgets(buf, sizeof(buf), stdin)) {
                    free(accum);
                    return last_status;
                }
                size_t l = strlen(buf);
                if (l > 0 && buf[l-1] == '\n') buf[l-1] = '\0';
                line = buf;
            }

            /* append line to accumulation buffer with a newline separator */
            size_t line_len = strlen(line);
            accum = realloc(accum, accum_len + line_len + 2);
            memcpy(accum + accum_len, line, line_len);
            accum_len += line_len;
            accum[accum_len++] = '\n';
            accum[accum_len]   = '\0';

            if (isatty(STDIN_FILENO)) free(line);

            /* check if braces are balanced */
            int nt = 0;
            char **toks = tokenize(accum, &nt);
            depth = brace_depth(toks, nt);
            free_tokens(toks);

            if (depth <= 0) break;   /* complete statement */
            first_line = 0;
        }

        if (!accum || !accum[0]) { free(accum); goto next_iteration; }

        /* trim leading whitespace to check for comment/empty */
        const char *trimmed = accum;
        while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\n') trimmed++;

        if (*trimmed == '\0' || *trimmed == '#') {
            free(accum);
            goto next_iteration;
        }

        if (isatty(STDIN_FILENO))
            line_editor_add_history(trimmed);

        /* save for hist_add before run_input may mutate it */
        char saved[NSH_MAX_INPUT];
        strncpy(saved, trimmed, sizeof(saved) - 1);
        saved[sizeof(saved) - 1] = '\0';

        run_input(accum, isatty(STDIN_FILENO));
        free(accum);

        hist_add(saved, last_status, 0);

next_iteration:;
    }
}
