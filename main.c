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
    config_load();  /* read ~/.config/nsh/config.toml before anything else */
    hist_open();    /* open DB unconditionally — works in both interactive and script mode */

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
    signal(SIGINT,  sigint_handler);  /* Ctrl+C: jump back to REPL top */
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

/* ── REPL ───────────────────────────────────────────────────────────────── */

int main(void)
{
    shell_init();

    while (1) {
        /* Ctrl+C longjmps here — terminal ECHOCTL already printed "^C",
         * we just need a newline before the next prompt */
        if (sigsetjmp(ctrl_c_jump, 1) != 0) {
            write(STDOUT_FILENO, "\n", 1);
            last_status = 1;
        }

        if (got_sigchld) {
            got_sigchld = 0;
            reap_jobs();
        }
        shellenv_check_reload();    /* drain kqueue, hot-reload changed .shellenv */

        char *line;

        if (isatty(STDIN_FILENO)) {
            char *prompt = make_prompt();
            line = line_editor_read(prompt);
            free(prompt);

            if (!line) {
                if (line_editor_was_interrupted()) {
                    write(STDOUT_FILENO, "\n", 1);
                    line_editor_clear_interrupt();
                    last_status = 1;
                    continue;
                }
                printf("\n");
                line_editor_shutdown();
                shellenv_shutdown();
                hist_close();
                break;
            }

            if (line_editor_was_interrupted()) {
                write(STDOUT_FILENO, "\n", 1);
                line_editor_clear_interrupt();
                free(line);
                last_status = 1;
                continue;
            }
        } else {
            static char buf[NSH_MAX_INPUT];
            if (!fgets(buf, sizeof(buf), stdin))
                break;
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            line = buf;
        }

        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (*trimmed == '\0' || *trimmed == '#') {
            if (isatty(STDIN_FILENO)) free(line);
            continue;
        }

        if (isatty(STDIN_FILENO))
            line_editor_add_history(trimmed);

        int ntokens = 0;
        char **tokens = tokenize(trimmed, &ntokens);

        /* keep a copy of trimmed before free(line) for hist_add */
        char saved[NSH_MAX_INPUT];
        strncpy(saved, trimmed, sizeof(saved) - 1);
        saved[sizeof(saved) - 1] = '\0';

        if (isatty(STDIN_FILENO)) free(line);

        if (!tokens || ntokens == 0) {
            free_tokens(tokens);
            continue;
        }

        Pipeline *p = parse_pipeline(tokens, ntokens, saved);
        free_tokens(tokens);

        if (!p) {
            last_status = 1;
            hist_add(saved, last_status, 0);
            continue;
        }

        timer_begin();
        last_status = execute_pipeline(p);
        int64_t elapsed = timer_elapsed_ms();

        if (isatty(STDIN_FILENO) &&
            g_config.prompt_show_ms >= 0 &&
            elapsed >= (int64_t)g_config.prompt_show_ms)
            fprintf(stderr, "\033[0;90m  took %lldms\033[0m\n", (long long)elapsed);

        free_pipeline(p);
        hist_add(saved, last_status, elapsed);
    }

    return last_status;
}
